"""Exercise the bundled editor in Chromium, then validate its output in C++."""
import html
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def main():
    root, bridge, chromium = map(Path, sys.argv[1:4])
    physics_arguments = sys.argv[4:]
    catalog = subprocess.check_output([str(bridge), "--dump-catalog"], text=True, timeout=20)
    json.loads(catalog)
    bootstrap = r"""
const signal = () => ({listeners: [], connect(callback) { this.listeners.push(callback); }, fire() { this.listeners.forEach(callback => callback()); }});
const browserErrors = [];
window.addEventListener('error', event => browserErrors.push(event.error?.stack || event.message));
// Chromium's virtual clock advances timers without compositor frames. Keep
// Blockly's frame-batched events on that same clock in this headless test.
window.requestAnimationFrame = callback => setTimeout(() => callback(performance.now()), 5);
window.cancelAnimationFrame = clearTimeout;
window.qt = {webChannelTransport: {}};
window.QWebChannel = function(transport, boot) {
  const bridge = {
    catalogJson: CATALOG, workspaceRevision: 0, diagnosticsJson: '[]', editable: true, darkMode: false,
    workspaceJson: JSON.stringify({blocks: {languageVersion: 0, blocks: [
      {type: 'ft_flow_when_start', id: 'start'}]}}),
    workspaceJsonChanged: signal(), diagnosticsJsonChanged: signal(), editableChanged: signal(),
    darkModeChanged: signal(), viewerPointPicked: signal(), debugChanged: signal(), debugJson: '{}', running: false,
    setBreakpoints() {}, inspectProgram() {}, pauseProgram() {}, resumeProgram() {}, stopProgram() {},
    runWorkspace(text, revision, debug, callback) { callback(true); },
    applyWorkspace(text, revision, callback) { if (callback) callback(true); },
    selectedViewerTargetJson(kind, callback) { callback('{}'); },
    editorReady() { setTimeout(runTests, 25); }
  };
  window.testBridge = bridge;
  boot({objects: {foreverBridge: bridge}});
};
const pause = () => new Promise(resolve => setTimeout(resolve, 25));
const check = (test, message) => { if (!test) throw Error(message); };
async function runTests() {
  const result = document.createElement('pre'); result.id = 'test-result'; document.body.append(result);
  try {
    const ws = Blockly.getMainWorkspace();
    const make = (type, fields = {}) => {
      const b = ws.newBlock(type);
      for (const [key, value] of Object.entries(fields)) b.setFieldValue(value, key);
      b.initSvg(); b.render(); return b;
    };
    const definition = make('ft_procedures_define', {name: 'advance', parameters: 'ticks, offset'});
    const returnBlock = make('ft_procedures_return');
    const parameter = make('ft_data_get', {name: 'ticks'});
    returnBlock.getInput('value').connection.connect(parameter.outputConnection);
    definition.getInput('body').connection.connect(returnBlock.previousConnection);
    await pause();
    const call = make('ft_procedures_value', {name: 'advance', parameters: 'ticks, offset'});
    const setter = make('ft_data_set', {name: 'observed'});
    setter.getInput('value').connection.connect(call.outputConnection);
    ws.getBlockById('start').getInput('body').connection.connect(setter.previousConnection);
    await pause();
    check(call.getInput('arg0') && call.getInput('arg1'), 'Procedure arguments were not created.');
    call.getInputTargetBlock('arg0').setFieldValue(2, 'value');
    const argument = make('ft_values_number', {value: 7});
    call.getInput('arg1').connection.connect(argument.outputConnection);
    Blockly.Events.setGroup(true);
    definition.setFieldValue('offset, ticks', 'parameters');
    await pause();
    Blockly.Events.setGroup(false);
    check(call.getInputTargetBlock('arg0') === argument, 'Reordering parameters detached a real argument: ' +
      JSON.stringify({parameters: call.getFieldValue('parameters'), names: call.parameterNames_,
        arg0: call.getInputTargetBlock('arg0')?.id, arg1: call.getInputTargetBlock('arg1')?.id,
        expected: argument.id, parent: argument.getParent()?.id, definition: definition.getFieldValue('parameters'),
        errors: browserErrors}));
    check(Number(call.getInputTargetBlock('arg1').getFieldValue('value')) === 2, 'Reordering parameters lost a shadow value.');
    definition.setFieldValue('advance renamed', 'name');
    await pause();
    check(call.getFieldValue('name') === 'advance renamed', 'Renaming a definition left a stale call.');
    Blockly.Events.setGroup(true);
    definition.setFieldValue('offset, ticks, extra', 'parameters');
    await pause();
    Blockly.Events.setGroup(false);
    check(call.getInput('arg2'), 'Adding a parameter did not update calls.');
    ws.undo(false); await pause();
    check(!call.getInput('arg2') && definition.getFieldValue('parameters') === 'offset, ticks', 'Signature undo was not atomic.');
    ws.undo(true); await pause();
    check(call.getInput('arg2'), 'Signature redo failed.');
    definition.setFieldValue('offset, duration, extra', 'parameters');
    await pause();
    check(parameter.getFieldValue('name') === 'duration', 'Renaming a parameter left its body reporter stale.');
    const reference = make('ft_procedures_reference', {name: 'advance renamed'});
    const saved = Blockly.serialization.workspaces.save(ws);
    ws.clear();
    // Calls may deserialize before their definitions.
    saved.blocks.blocks.sort((a,b) => (a.type === 'ft_procedures_define') - (b.type === 'ft_procedures_define'));
    Blockly.serialization.workspaces.load(saved, ws);
    await pause();
    check(ws.getBlockById(reference.id).getFieldValue('name') === 'advance renamed', 'First-class block reference lost its procedure on reload.');
    const bridge = window.testBridge;
    bridge.running = true; bridge.editable = false;
    bridge.debugJson = JSON.stringify({block: setter.id, paused: true, timeMs: 20,
      variables: [{name: 'observed', value: '42'}], frames: [{name: 'advance', locals: [{name: 'duration', value: '2'}]}],
      state: [{name: 'time', value: '20'}]});
    bridge.debugChanged.fire(); bridge.editableChanged.fire();
    check(ws.isReadOnly(), 'Running programs still accept keyboard edits behind the read-only shield.');
    check(!document.getElementById('stepProgram').disabled && !document.getElementById('stopProgram').disabled,
      'Debugger controls are blocked while the workspace is read-only.');
    check(document.getElementById('runtimeValues').textContent.includes('duration') &&
          document.getElementById('runtimeValues').textContent.includes('42'), 'Runtime watches omit variables or call-local values.');
    let stepped = '';
    bridge.resumeProgram = kind => { stepped = kind; };
    document.getElementById('stepKind').value = 'over';
    document.getElementById('stepProgram').click();
    check(stepped === 'over', 'Step toolbar did not invoke the native debugger.');
    bridge.running = false; bridge.editable = true; bridge.editableChanged.fire();
    check(!ws.isReadOnly(), 'The editor stayed locked after the run ended.');
    const loaded = ws.getAllBlocks(false).find(b => b.type === 'ft_procedures_value');
    check(loaded && loaded.getFieldValue('name') === 'advance renamed' && loaded.getInput('arg2'), 'Procedure reload lost its name or signature.');
    check(Number(loaded.getInputTargetBlock('arg0').getFieldValue('value')) === 7, 'Reload lost a non-shadow argument.');
    check(Number(loaded.getInputTargetBlock('arg1').getFieldValue('value')) === 2, 'Reload lost the edited shadow.');
    check(!ws.getTopBlocks(false).some(b => b.isShadow()), 'Parameter edits leaked top-level shadow blocks.');
    const output = document.createElement('pre'); output.id = 'workspace-json';
    output.textContent = JSON.stringify(Blockly.serialization.workspaces.save(ws)); document.body.append(output);
    const disabled = make('ft_data_set', {name: 'disabled'});
    disabled.setDisabledReason(true, 'MANUALLY_DISABLED');
    const disabledInput = disabled.getInput('value').connection;
    const disabledValue = disabledInput.targetBlock();
    disabledInput.setShadowState(null);
    if (disabledValue) disabledValue.dispose();
    // Missing arguments inside a disabled command must not make it run.
    ws.getBlockById(setter.id).nextConnection.connect(disabled.previousConnection);
    await pause();
    const disabledOutput = document.createElement('pre'); disabledOutput.id = 'example-disabled';
    disabledOutput.textContent = JSON.stringify(Blockly.serialization.workspaces.save(ws)); document.body.append(disabledOutput);
    for (const name of ['empty', 'feedback', 'branches']) {
      ws.clear();
      Blockly.serialization.workspaces.load(window.foreverBlockExample(name), ws);
      await pause();
      const example = document.createElement('pre'); example.id = 'example-' + name;
      example.textContent = JSON.stringify(Blockly.serialization.workspaces.save(ws)); document.body.append(example);
    }
    const catalog = JSON.parse(bridge.catalogJson);
    check(!catalog.blocks.some(block => /^(mutate|search|objective)\//.test(block.id)), 'Legacy policies are still offered as primitives.');
    check(catalog.macros.length >= 18, 'Input, condition or target macroblocks are missing.');
    for (const macro of catalog.macros) {
      ws.clear();
      const entry = make('ft_flow_when_start');
      const insert = ws.getButtonCallback(`macro:${macro.id}`);
      check(typeof insert === 'function', 'Macroblocks category has no insertion action for ' + macro.id);
      insert();
      await pause();
      const sequence = ws.getTopBlocks(false).find(block => block !== entry);
      check(sequence && sequence.type !== 'ft_flow_when_start' && sequence.getDescendants(false).length > 2,
        'Macro inserted an opaque wrapper or another entry hat: ' + macro.id);
      entry.getInput('body').connection.connect(sequence.previousConnection);
      await pause();
      const output = document.createElement('pre'); output.id = 'macro-' + macro.id;
      output.textContent = JSON.stringify(Blockly.serialization.workspaces.save(ws)); document.body.append(output);
    }
    ws.clear();
    const prism = make('ft_targets_prism');
    bridge.selectedViewerTargetJson = (kind, callback) => callback(JSON.stringify({kind: 'prism',
      plane: 'xz', originX: 1, originY: 2, originZ: 3, depth: 5, polygon: '0,0;5,0;0,5'}));
    await pause(); prism.select(); await pause();
    document.getElementById('viewerPickButton').click(); await pause();
    const polygon = prism.getInputTargetBlock('polygon');
    check(polygon?.type === 'ft_targets_polygon_from_points' &&
      polygon.getDescendants(false).filter(block => block.type === 'ft_targets_point').length === 3,
      'Viewer prism import retained an opaque polygon string instead of three editable points.');
    // Inserting twice must not capture a procedure parameter or another macro's
    // temporary variables. Undo/redo must treat the whole insertion as one edit.
    ws.clear();
    const owner = make('ft_procedures_define', {name: 'user operation', parameters: 'inputs'});
    await pause();
    const insert = ws.getButtonCallback('macro:input-deletion');
    insert(); await pause();
    const first = ws.getTopBlocks(false).find(block => block !== owner);
    const firstIds = new Set(first.getDescendants(false).map(block => block.id));
    const locals = root => new Set(root.getDescendants(false).filter(block => block.type === 'ft_data_local').map(block => block.getFieldValue('name')));
    const firstLocals = locals(first);
    check(!firstLocals.has('inputs'), 'Macro overwrote a user procedure parameter.');
    insert(); await pause();
    const second = ws.getTopBlocks(false).find(block => block !== owner && block !== first);
    const secondLocals = locals(second);
    check(![...secondLocals].some(name => firstLocals.has(name)), 'Two expansions share temporary variables.');
    ws.undo(false); await pause();
    check(!ws.getBlockById(second.id) && [...firstIds].every(id => ws.getBlockById(id)), 'Undo did not remove exactly one expanded sequence.');
    ws.undo(true); await pause();
    check(ws.getBlockById(second.id), 'Redo did not restore the expanded sequence.');
    const savedMacros = Blockly.serialization.workspaces.save(ws);
    ws.clear(); Blockly.serialization.workspaces.load(savedMacros, ws); await pause();
    check(ws.getBlockById(first.id) && ws.getBlockById(second.id), 'Expanded sequences did not survive reload.');
    result.textContent = 'PASS';
  } catch (error) { result.textContent = 'FAIL: ' + (error.stack || error); }
}
""".replace("CATALOG", json.dumps(catalog))
    source = (root / "assets/blockly/index.html").read_text()
    source = source.replace('href="editor.css"', f'href="{(root / "assets/blockly/editor.css").as_uri()}"')
    source = source.replace('src="editor.js"', f'src="{(root / "assets/blockly/editor.js").as_uri()}"')
    source = source.replace('src="examples.js"', f'src="{(root / "assets/blockly/examples.js").as_uri()}"')
    source = source.replace("qrc:/blockly/third_party/blockly/", (root / "third_party/blockly").as_uri() + "/")
    source = source.replace('<script src="qrc:///qtwebchannel/qwebchannel.js"></script>', '<script>' + bootstrap + '</script>')
    with tempfile.TemporaryDirectory(prefix="forevertas-blockly-") as directory:
        folder = Path(directory)
        page = folder / "test.html"
        page.write_text(source)
        browser = subprocess.run([str(chromium), "--headless", "--no-sandbox", "--disable-gpu",
            "--allow-file-access-from-files", "--no-first-run", "--disable-background-networking",
            f"--user-data-dir={folder / 'profile'}", "--virtual-time-budget=5000", "--dump-dom", page.as_uri()],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
        match = re.search(r'<pre id="test-result">(.*?)</pre>', browser.stdout, re.S)
        verdict = html.unescape(match.group(1)) if match else "Editor did not finish its browser tests."
        if browser.returncode or verdict != "PASS":
            raise RuntimeError(verdict + "\n" + browser.stderr[-2500:])
        workspace = re.search(r'<pre id="workspace-json">(.*?)</pre>', browser.stdout, re.S)
        if not workspace:
            raise RuntimeError("Browser produced no workspace.")
        path = folder / "workspace.json"
        path.write_text(html.unescape(workspace.group(1)))
        subprocess.run([str(bridge), "--validate-workspace", str(path)], check=True, timeout=20)
        for name in ["disabled", "empty", "feedback", "branches"]:
            example = re.search(r'<pre id="example-' + name + r'">(.*?)</pre>', browser.stdout, re.S)
            if not example:
                raise RuntimeError("Browser did not build example: " + name)
            path.write_text(html.unescape(example.group(1)))
            subprocess.run([str(bridge), "--validate-workspace", str(path), *physics_arguments], check=True, timeout=45)
        for macro in json.loads(catalog)["macros"]:
            serialized = re.search(r'<pre id="macro-' + re.escape(macro["id"]) + r'">(.*?)</pre>', browser.stdout, re.S)
            if not serialized:
                raise RuntimeError("Browser did not expand macro: " + macro["id"])
            path.write_text(html.unescape(serialized.group(1)))
            subprocess.run([str(bridge), "--validate-workspace", str(path)], check=True, timeout=20)
    print("PASS Blockly: procedures, macro expansion, isolated names, undo/redo, reload, native validation")


if __name__ == "__main__":
    main()
