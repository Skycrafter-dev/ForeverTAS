"""Exercise the bundled editor in Chromium, then validate its output in C++."""
import html
import json
import os
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
window.addEventListener('unhandledrejection', event => browserErrors.push(event.reason?.stack || String(event.reason)));
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
    cancelViewerPointPick() {},
    sessionJson: '', sessionError: '', sessionFlushRequested: signal(),
    storeSession(text, callback) { this.sessionJson = text; callback(''); },
    finishSessionFlush(error) { this.lastFlushError = error; },
    openProgramFile(callback) { callback(''); },
    saveProgramFile(text, path, name, callback) { callback(JSON.stringify({path: path || '/tmp/program.json', title: name || 'program'})); },
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
    document.getElementById('blocksView').click();
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
    document.querySelector('[data-step="over"]').click();
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

    // Exercise the actual no-code path, then inspect the generated executable
    // graph. Form fields must modify the literal block, not detached settings.
    document.getElementById('configureView').click();
    document.getElementById('newSearch').click();
    check(document.getElementById('searchBuilder').open, 'New search did not open.');
    document.querySelector('#builderInputs [data-template="sky"]').click();
    document.querySelector('#builderTargets [data-template="speed-target"]').click();
    document.getElementById('createSearch').click(); await pause();
    while (window.foreverProgramTabs.busy) await pause();
    const local = name => ws.getAllBlocks(false).find(b => b.type === 'ft_data_local' && b.getFieldValue('name') === name);
    for (const [name, value] of [['Pass 1 / seed', 3749268317], ['Pass 2 / seed', 444721321], ['Pass 3 / seed', 4221481885],
      ['Pass 1 / maximum steering deletions',12], ['Pass 2 / maximum edits',12], ['Pass 3 / maximum steering insertions',5],
      ['Pass 3 / maximum accelerate hold ms',200], ['Pass 3 / maximum brake hold ms',300]]) {
      const source = local('Inputs / ' + name)?.getInputTargetBlock('value');
      check(source && Number(source.getFieldValue('value')) === value, 'Sky preset differs from screenshot: ' + name);
      check(document.querySelector(`[data-source="${source.id}"]`), 'Constant missing from configuration: ' + name);
    }
    const iterations = local('iterations').getInputTargetBlock('value');
    let setting = document.querySelector(`[data-source="${iterations.id}"]`);
    setting.value = '2'; setting.dispatchEvent(new Event('change')); await pause();
    check(Number(iterations.getFieldValue('value')) === 2, 'Form change did not edit Blockly.');
    ws.undo(false); await pause();
    check(Number(iterations.getFieldValue('value')) === 1000,
      'Form undo: value=' + iterations.getFieldValue('value') + ' events=' +
      JSON.stringify(ws.getUndoStack().slice(-5).map(e => [e.type,e.element,e.name,e.oldValue,e.newValue,e.group])));
    ws.undo(true); await pause(); check(Number(iterations.getFieldValue('value')) === 2, 'Form redo failed.');
    const skySection = ws.getAllBlocks(false).find(b => b.type === 'ft_flow_section' && b.getFieldValue('name') === "Sky's");
    check(skySection?.isCollapsed(), 'Preset is not collapsed by default.');
    const guided = document.createElement('pre'); guided.id = 'guided-sky';
    guided.textContent = JSON.stringify(Blockly.serialization.workspaces.save(ws)); document.body.append(guided);
    const dimensions = ws.scale;
    document.getElementById('blocksView').click(); document.getElementById('configureView').click(); await pause();
    check(ws.scale === dimensions, 'View changes reset zoom.');
    document.getElementById('projectAction').click();
    check(document.getElementById('projectMenu').open, 'Program menu cannot open.');
    const menu = document.querySelector('#projectMenu .menuContents').getBoundingClientRect();
    check(menu.width > 0 && menu.top >= document.getElementById('editorToolbar').getBoundingClientRect().top, 'Program menu is misplaced.');
    document.dispatchEvent(new KeyboardEvent('keydown', {key: 'Escape'}));
    check(!document.getElementById('projectMenu').open, 'Escape did not dismiss Program.');
    const settle = () => new Promise(resolve => setTimeout(resolve, 200));
    iterations.setFieldValue(3, 'value'); await settle();
    check(Number(document.querySelector(`[data-source="${iterations.id}"]`).value) === 3, 'Editing Blockly did not refresh the form.');
    setting = document.querySelector(`[data-source="${iterations.id}"]`);
    setting.value = ''; setting.dispatchEvent(new Event('input')); setting.dispatchEvent(new Event('change')); setting.blur(); await pause();
    check(document.getElementById('runProgram').disabled && document.getElementById('debugProgram').disabled &&
      document.querySelector(`[data-source="${iterations.id}"]`).value === '', 'Invalid field was silently discarded or could run old settings.');
    setting = document.querySelector(`[data-source="${iterations.id}"]`); setting.value = '2'; setting.dispatchEvent(new Event('change')); await settle();
    check(!document.getElementById('runProgram').disabled, 'Correcting a form value did not enable Run.');
    Blockly.Events.setGroup(true);
    const initializer = local('iterations'), expr = make('ft_math_add'), three = make('ft_values_number', {value: 3});
    iterations.unplug(); expr.getInput('a').connection.connect(iterations.outputConnection);
    expr.getInput('b').connection.connect(three.outputConnection); initializer.getInput('value').connection.connect(expr.outputConnection);
    Blockly.Events.setGroup(false); await settle();
    check(!document.querySelector(`[data-source="${iterations.id}"]`) && initializer.getInputTargetBlock('value') === expr,
      'The settings form exposed or flattened a computed initializer.');
    ws.undo(false); await settle();
    check(local('iterations').getInputTargetBlock('value').id === iterations.id, 'Undo did not restore a literal initializer.');
    const pass = ws.getAllBlocks(false).find(b => b.type === 'ft_flow_section' && b.getFieldValue('name').startsWith('Pass 1'));
    let passCard = document.querySelector(`[data-block="${pass.id}"]`);
    passCard.querySelector(':scope > .cardActions button[title="Move later"]').click(); await settle();
    check(pass.getPreviousBlock()?.getFieldValue('name').startsWith('Pass 2'), 'Reordering a pass changed cards but not source order.');
    ws.undo(false); await settle(); check(pass.getPreviousBlock()?.getFieldValue('name') !== 'Pass 2 · Existing-event perturbation', 'Pass reorder undo failed.');
    passCard = document.querySelector(`[data-block="${pass.id}"]`);
    const toggle = passCard.querySelector(':scope > .cardActions input[type="checkbox"]');
    toggle.checked = false; toggle.dispatchEvent(new Event('change')); await settle();
    check(!pass.isEnabled() && [...document.querySelector(`[data-block="${pass.id}"]`).querySelectorAll('[data-source]')].every(e => e.disabled), 'Disabling a section did not lock its configuration.');
    ws.undo(false); await settle();
    document.querySelector('#configurationActions [data-template="checkpoint-condition"]').click(); await settle();
    const conditions = ws.getTopBlocks(false).find(b => b.getFieldValue('name') === 'Accept state');
    check(conditions.getInputTargetBlock('body').type === 'ft_flow_section', 'Adding a condition did not connect it to the program.');
    document.querySelector('#configurationActions [data-template="point-target"]').click(); await settle();
    check(ws.getAllBlocks(false).some(b => b.type === 'ft_data_local' && b.getFieldValue('name') === 'Target / target point'), 'Target chooser did not replace the target source.');
    document.querySelector('#configurationActions [data-template="box-target"]').click(); await settle();
    const boxSize = local('Target / box size');
    check(boxSize && document.querySelector(`[data-source="${boxSize.getInputTargetBlock('value').getInputTargetBlock('x').id}"]`),
      'Box dimensions cannot be configured without editing its implementation.');
    document.querySelector('#configurationActions [data-template="prism-target"]').click(); await settle();
    const plane = local('Target / projection plane')?.getInputTargetBlock('value');
    const planeRow = [...document.querySelectorAll('.settingRow')].find(row => row.firstChild.textContent === 'Projection plane');
    check(plane && planeRow?.querySelectorAll('button[aria-pressed]').length === 3 && local('Target / point 1'), 'Prism geometry is missing normal configuration controls.');
    [...planeRow.querySelectorAll('button')].find(button => button.textContent === 'XY').click(); await settle();
    check(plane.getFieldValue('value') === 'xy', 'Projection-plane setting does not edit its source.');
    const edited = document.createElement('pre'); edited.id = 'guided-edited';
    check(ws.getAllBlocks(false).filter(b => b.type === 'ft_procedures_reference')
      .every(b => b.getFieldValue('name') === 'Try candidate'), 'Guided branch reference lost its name during Blockly round-trip.');
    edited.textContent = JSON.stringify(Blockly.serialization.workspaces.save(ws)); document.body.append(edited);
    document.getElementById('blocksView').click(); await pause();
    const nestedGroups = document.querySelectorAll('.blocklyToolboxCategoryGroup .blocklyToolboxCategoryGroup');
    check([...nestedGroups].every(e => getComputedStyle(e).paddingTop === '0px'), 'Macro submenu still has a toolbar-sized gap.');
    const canvas = document.getElementById('canvasPane').getBoundingClientRect(), footer = document.getElementById('runtimeDock').getBoundingClientRect();
    check(canvas.bottom <= footer.top + 1, 'Execution controls overlap the block canvas.');
    document.getElementById('configureView').click();
    // Every guided target is compiled from real serialized source as well.
    for (const macro of JSON.parse(bridge.catalogJson).macros.filter(m => m.category === 'Targets')) {
      const source = ForeverWorkbench.buildSearch(JSON.parse(bridge.catalogJson), 'existing-events', macro.id);
      const record = document.createElement('pre'); record.id = 'guided-' + macro.id;
      record.textContent = JSON.stringify(source); document.body.append(record);
    }
    let savedProject = '';
    const renameBeforeSave = window.foreverProgramTabs.rename(); await pause();
    document.getElementById('programName').value = 'Named search';
    document.querySelector('#renameProgramDialog button[value="rename"]').click(); await renameBeforeSave;
    let savedProgramName = '';
    bridge.saveProgramFile = (text, path, name, callback) => {
      savedProject = text; savedProgramName = name;
      callback(JSON.stringify({path: '/tmp/saved-program.json', title: name || 'saved-program'}));
    };
    document.querySelector('[data-project="save"]').click(); await settle();
    check(JSON.parse(savedProject).blocks.blocks.length === ws.getTopBlocks(false).length, 'Program Save did not send the visible source to the bridge.');
    check(savedProgramName === 'Named search', 'Renamed program name was not sent to the portable file.');
    bridge.openProgramFile = callback => callback(JSON.stringify({workspace: JSON.parse(savedProject), path: '/tmp/saved-program.json', title: 'Named search'}));
    document.querySelector('[data-project="open"]').click(); await settle();
    check(ws.getAllBlocks(false).some(b => b.type === 'ft_flow_section' && b.isCollapsed()), 'Program Open discarded folded sections.');
    const tabs = window.foreverProgramTabs, firstId = tabs.active().id;
    const firstValueId = local('iterations').getInputTargetBlock('value').id;
    ws.getBlockById(firstValueId).setFieldValue(42, 'value'); await settle();
    const modeBounds = () => ['configureView','blocksView','settingsSideToggle'].map(id => {
      const box = document.getElementById(id).getBoundingClientRect(); return [box.x, box.y, box.width, box.height];
    });
    const originalBounds = modeBounds();
    document.getElementById('blocksView').click(); await pause();
    check(JSON.stringify(modeBounds()) === JSON.stringify(originalBounds), 'Changing views moves the view buttons.');
    check(getComputedStyle(document.getElementById('configurationPanel')).display === 'none', 'Blocks unexpectedly opened settings.');
    ws.setScale(1.2); await settle();
    await tabs.open(window.foreverBlockExample('empty'), 'Second program');
    const secondId = tabs.active().id;
    Blockly.Events.setGroup(true);
    const secondLocal = make('ft_data_local', {name: 'second setting'}), secondValue = make('ft_values_number', {value: 7});
    secondLocal.getInput('value').connection.connect(secondValue.outputConnection);
    ws.getTopBlocks(false).find(b => b.type === 'ft_flow_when_start').getInput('body').connection.connect(secondLocal.previousConnection);
    Blockly.Events.setGroup(false); await settle(); ws.clearUndo();
    secondValue.setFieldValue(9, 'value'); await settle();
    const secondValueId = secondValue.id;
    await tabs.switchTo(firstId);
    check(ws.getBlockById(firstValueId).getFieldValue('value') === 42 && Math.abs(ws.scale - 1.2) < .001,
      'Switching programs lost source or zoom.');
    document.getElementById('undoProgram').click(); await settle();
    check(ws.getBlockById(firstValueId).getFieldValue('value') === 2, 'Undo used another program history.');
    document.getElementById('redoProgram').click(); await settle();
    check(ws.getBlockById(firstValueId).getFieldValue('value') === 42, 'Per-program redo failed.');
    await tabs.switchTo(secondId);
    document.getElementById('undoProgram').click(); await settle();
    check(ws.getBlockById(secondValueId).getFieldValue('value') === 7, 'Second program lost its own undo.');
    document.getElementById('redoProgram').click(); await settle();
    await tabs.switchTo(firstId); document.getElementById('configureView').click(); await settle();
    const draftInput = document.querySelector(`[data-source="${firstValueId}"]`);
    draftInput.value = '-'; draftInput.dispatchEvent(new Event('input', {bubbles: true}));
    draftInput.dispatchEvent(new Event('change')); await settle();
    await tabs.switchTo(secondId); await tabs.switchTo(firstId);
    check(document.querySelector(`[data-source="${firstValueId}"]`).value === '-' && document.getElementById('runProgram').disabled,
      'Unfinished setting: ' + JSON.stringify({value: document.querySelector(`[data-source="${firstValueId}"]`).value,
        disabled: document.getElementById('runProgram').disabled, drafts: tabs.active().ui.drafts}));
    await tabs.switchTo(secondId);
    await tabs.flush();
    result.textContent = 'PROGRESS: reload tabs';
    const savedSession = JSON.parse(bridge.sessionJson);
    check(savedSession.tabs.length >= 3 && savedSession.active === secondId && savedSession.tabs.find(t => t.id === firstId).ui.drafts,
      'Autosave discarded inactive programs or drafts.');
    // Restart the document manager through the same load/history path used at boot.
    await tabs.initialize();
    check(tabs.active().id === secondId && ws.getBlockById(secondValueId).getFieldValue('value') === 9, 'Session restore lost the active source.');
    document.getElementById('undoProgram').click(); await settle();
    check(ws.getBlockById(secondValueId).getFieldValue('value') === 7, 'Serialized undo history cannot be restored.');
    document.getElementById('redoProgram').click(); await settle();
    await tabs.flush();
    result.textContent = 'PROGRESS: close cancel';
    const beforeClose = JSON.parse(bridge.sessionJson).tabs.length;
    const waitForDialog = async () => {
      for (let i = 0; i < 50 && !document.getElementById('closeProgramDialog').open; ++i) await pause();
      check(document.getElementById('closeProgramDialog').open, 'Close confirmation did not open: ' +
        JSON.stringify({busy: tabs.busy, running: bridge.running, active: tabs.active()}));
    };
    let closing = tabs.closeTab(); await waitForDialog();
    document.querySelector('#closeProgramDialog button[value="cancel"]').click();
    result.textContent = 'PROGRESS: waiting for close cancel'; await closing;
    check(JSON.parse(bridge.sessionJson).tabs.length === beforeClose, 'Cancel closed an autosaved draft.');
    result.textContent = 'PROGRESS: close discard';
    closing = tabs.closeTab(); await waitForDialog(); document.querySelector('#closeProgramDialog button[value="close"]').click(); await closing;
    check(JSON.parse(bridge.sessionJson).tabs.length === beforeClose - 1, 'Explicit close did not remove the tab from autosave.');
    await tabs.reopen();
    result.textContent = 'PROGRESS: storage failure';
    check(tabs.active().id === secondId && ws.getBlockById(secondValueId).getFieldValue('value') === 9, 'Reopen lost a closed draft.');
    const saveMethod = bridge.storeSession;
    bridge.storeSession = (text, callback) => callback('Simulated storage failure');
    tabs.active().title += ' recovery'; tabs.changed();
    check(await tabs.flush() === 'Simulated storage failure' && document.getElementById('autosaveStatus').classList.contains('error'),
      'Storage failure was reported as a successful autosave.');
    bridge.storeSession = saveMethod; await tabs.flush();
    const sessionRecord = document.createElement('pre'); sessionRecord.id = 'session-json';
    sessionRecord.textContent = bridge.sessionJson; document.body.append(sessionRecord);
    const historyRecord = document.createElement('pre'); historyRecord.id = 'history-value'; historyRecord.textContent = secondValueId; document.body.append(historyRecord);
    check(!browserErrors.length, 'Asynchronous editor error: ' + browserErrors.join('\n'));
    const screenshotMode = SCREENSHOT_MODE;
    if (screenshotMode) {
      await tabs.switchTo(firstId);
      const pending = document.querySelector(`[data-source="${firstValueId}"]`);
      pending.value = '42'; pending.dispatchEvent(new Event('change')); await settle();
      document.getElementById(screenshotMode === 'configure' ? 'configureView' : screenshotMode === 'blocks' ? 'blocksView' : 'settingsSideToggle').click();
      bridge.darkMode = true; bridge.darkModeChanged.fire(); await settle();
      if (screenshotMode === 'menu') { document.getElementById('projectAction').click(); await pause(); }
    }
    result.textContent = 'PASS';
  } catch (error) { result.textContent = 'FAIL: ' + (error.stack || error); }
}
""".replace("CATALOG", json.dumps(catalog)).replace('SCREENSHOT_MODE', json.dumps(os.environ.get('FOREVERTAS_BLOCKLY_SNAPSHOT_MODE', '')))
    source = (root / "assets/blockly/index.html").read_text()
    source = source.replace('href="editor.css"', f'href="{(root / "assets/blockly/editor.css").as_uri()}"')
    source = source.replace('src="editor.js"', f'src="{(root / "assets/blockly/editor.js").as_uri()}"')
    source = source.replace('src="examples.js"', f'src="{(root / "assets/blockly/examples.js").as_uri()}"')
    source = source.replace('src="workbench.js"', f'src="{(root / "assets/blockly/workbench.js").as_uri()}"')
    source = source.replace('src="program_tabs.js"', f'src="{(root / "assets/blockly/program_tabs.js").as_uri()}"')
    source = source.replace("qrc:/blockly/third_party/blockly/", (root / "third_party/blockly").as_uri() + "/")
    source = source.replace('<script src="qrc:///qtwebchannel/qwebchannel.js"></script>', '<script>' + bootstrap + '</script>')
    with tempfile.TemporaryDirectory(prefix="forevertas-blockly-") as directory:
        folder = Path(directory)
        page = folder / "test.html"
        page.write_text(source)
        browser = subprocess.run([str(chromium), "--headless", "--no-sandbox", "--disable-gpu",
            "--allow-file-access-from-files", "--no-first-run", "--disable-background-networking",
            "--window-size=" + os.environ.get("FOREVERTAS_BLOCKLY_WINDOW", "1280,960"),
            *(["--screenshot=" + os.environ["FOREVERTAS_BLOCKLY_SCREENSHOT"]] if os.environ.get("FOREVERTAS_BLOCKLY_SCREENSHOT") else []),
            f"--user-data-dir={folder / 'profile'}", "--virtual-time-budget=15000", "--dump-dom", page.as_uri()],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=45)
        match = re.search(r'<pre id="test-result">(.*?)</pre>', browser.stdout, re.S)
        verdict = html.unescape(match.group(1)) if match else "Editor did not finish its browser tests."
        if browser.returncode or verdict != "PASS":
            raise RuntimeError(verdict + "\n" + browser.stderr[-2500:])
        session = html.unescape(re.search(r'<pre id="session-json">(.*?)</pre>', browser.stdout, re.S).group(1))
        value_id = html.unescape(re.search(r'<pre id="history-value">(.*?)</pre>', browser.stdout, re.S).group(1))
        restore_tests = r"""
async function runRestoreTests() {
  const result = document.createElement('pre'); result.id = 'restore-result'; document.body.append(result);
  try {
    const tabs = window.foreverProgramTabs, ws = Blockly.getMainWorkspace();
    const bridge = window.testBridge, initial = JSON.parse(bridge.sessionJson);
    check(document.querySelectorAll('.programTab').length === initial.tabs.length, 'Restart changed open tabs.');
    check(ws.getBlockById(VALUE_ID).getFieldValue('value') === 9, 'Fresh process lost active source.');
    document.getElementById('undoProgram').click(); await new Promise(r => setTimeout(r, 200));
    check(ws.getBlockById(VALUE_ID).getFieldValue('value') === 7, 'Fresh process cannot undo restored history.');
    document.getElementById('redoProgram').click(); await new Promise(r => setTimeout(r, 200));
    check(ws.getBlockById(VALUE_ID).getFieldValue('value') === 9, 'Fresh process cannot redo restored history.');
    const other = initial.tabs.find(tab => Object.keys(tab.ui.drafts || {}).length);
    await tabs.switchTo(other.id);
    const field = [...document.querySelectorAll('#configurationCards input')].find(input => input.value === '-');
    check(field && !field.checkValidity() && document.getElementById('runProgram').disabled, 'Restart lost unfinished settings or enabled stale values.');
    const rename = tabs.rename(); await pause(); document.getElementById('programName').value = 'Renamed draft';
    document.querySelector('#renameProgramDialog button[value="rename"]').click(); await rename;
    await tabs.flush(); check(tabs.active().title === 'Renamed draft', 'Rename did not persist.');
    bridge.running = true; bridge.editable = false; bridge.editableChanged.fire();
    const before = tabs.active().id; await tabs.switchTo(initial.active);
    check(tabs.active().id === before && document.getElementById('undoProgram').disabled, 'Program switching changed a running source.');
    bridge.running = false; bridge.editable = true; bridge.editableChanged.fire();
    bridge.sessionFlushRequested.fire(); await new Promise(r => setTimeout(r, 200));
    check(bridge.lastFlushError === '', 'Application-close autosave was not acknowledged.');
    for (const tab of JSON.parse(bridge.sessionJson).tabs) {
      const closing = tabs.closeTab(tab.id);
      for (let i=0; i<50 && !document.getElementById('closeProgramDialog').open &&
           JSON.parse(bridge.sessionJson).tabs.some(t=>t.id===tab.id); ++i) await pause();
      if (document.getElementById('closeProgramDialog').open)
        document.querySelector('#closeProgramDialog button[value="close"]').click();
      await closing;
    }
    check(JSON.parse(bridge.sessionJson).tabs.length === 0 && !tabs.active() && document.getElementById('runProgram').disabled,
      'Closing all tabs resurrected a program or kept Run enabled.');
    await tabs.initialize(); check(!tabs.active(), 'Explicitly closed programs reopened after session restoration.');
    check(!browserErrors.length, browserErrors.join('\n')); result.textContent = 'PASS';
  } catch (error) { result.textContent = 'FAIL: ' + error.stack; }
}
""".replace('VALUE_ID', json.dumps(value_id))
        restored_bootstrap = bootstrap.replace("sessionJson: ''", 'sessionJson: ' + json.dumps(session).replace('<', '\\u003c'))
        restored_bootstrap = restored_bootstrap.replace('setTimeout(runTests, 25)', 'setTimeout(runRestoreTests, 25)') + restore_tests
        restored_page = folder / 'restore.html'
        restored_page.write_text(source.replace(bootstrap, restored_bootstrap))
        restored = subprocess.run([str(chromium), '--headless', '--no-sandbox', '--disable-gpu',
            '--allow-file-access-from-files', '--no-first-run', '--disable-background-networking',
            f'--user-data-dir={folder / "restored-profile"}', '--virtual-time-budget=7000', '--dump-dom', restored_page.as_uri()],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
        verdict = re.search(r'<pre id="restore-result">(.*?)</pre>', restored.stdout, re.S)
        if restored.returncode or not verdict or html.unescape(verdict.group(1)) != 'PASS':
            raise RuntimeError('Fresh-process restore: ' + (html.unescape(verdict.group(1)) if verdict else 'did not finish') + '\n' + restored.stderr[-1200:])

        corrupt_session = json.loads(session)
        corrupt_active = next(tab for tab in corrupt_session['tabs'] if tab['id'] == corrupt_session['active'])
        corrupt_active['undo'] = [None]
        corrupt_active['redo'] = [{'type': 'not-a-blockly-event'}]
        corrupt_history_tests = r"""
async function runCorruptHistoryTests() {
  const result = document.createElement('pre'); result.id = 'corrupt-history-result'; document.body.append(result);
  try {
    const tabs = window.foreverProgramTabs, ws = Blockly.getMainWorkspace();
    check(ws.getBlockById(VALUE_ID)?.getFieldValue('value') === 9,
      'Corrupt undo metadata prevented valid program source from loading.');
    check(!tabs.active().loadError, 'Corrupt undo metadata was treated as a source load failure.');
    check(document.getElementById('undoProgram').disabled && document.getElementById('redoProgram').disabled,
      'Corrupt undo metadata was not discarded.');
    check(document.getElementById('programNotice').textContent.includes('Undo/redo history could not be restored'),
      'Discarded corrupt undo metadata was not reported.');
    check(!browserErrors.length, browserErrors.join('\n')); result.textContent = 'PASS';
  } catch (error) { result.textContent = 'FAIL: ' + error.stack; }
}
""".replace('VALUE_ID', json.dumps(value_id))
        corrupt_bootstrap = bootstrap.replace(
            "sessionJson: ''", 'sessionJson: ' + json.dumps(json.dumps(corrupt_session)).replace('<', '\\u003c'))
        corrupt_bootstrap = corrupt_bootstrap.replace(
            'setTimeout(runTests, 25)', 'setTimeout(runCorruptHistoryTests, 25)') + corrupt_history_tests
        corrupt_page = folder / 'corrupt-history.html'
        corrupt_page.write_text(source.replace(bootstrap, corrupt_bootstrap))
        corrupt = subprocess.run([str(chromium), '--headless', '--no-sandbox', '--disable-gpu',
            '--allow-file-access-from-files', '--no-first-run', '--disable-background-networking',
            f'--user-data-dir={folder / "corrupt-history-profile"}', '--virtual-time-budget=5000', '--dump-dom', corrupt_page.as_uri()],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
        corrupt_verdict = re.search(r'<pre id="corrupt-history-result">(.*?)</pre>', corrupt.stdout, re.S)
        if corrupt.returncode or not corrupt_verdict or html.unescape(corrupt_verdict.group(1)) != 'PASS':
            raise RuntimeError('Corrupt-history restore: ' +
                (html.unescape(corrupt_verdict.group(1)) if corrupt_verdict else 'did not finish') + '\n' + corrupt.stderr[-1200:])

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
        for name in ['sky', 'edited'] + [m['id'] for m in json.loads(catalog)['macros'] if m['category'] == 'Targets']:
            match = re.search(r'<pre id="guided-' + name + r'">(.*?)</pre>', browser.stdout, re.S)
            if not match:
                raise RuntimeError('Missing guided source: ' + name)
            path.write_text(html.unescape(match.group(1)))
            subprocess.run([str(bridge), '--validate-workspace', str(path), *(physics_arguments if name == 'sky' else [])], check=True, timeout=60)
    print("PASS Blockly: procedures, macro expansion, configuration, menus, independent tabs/history, fresh-process restoration, drafts, close/cancel/reopen, autosave failures, native validation")


if __name__ == "__main__":
    main()
