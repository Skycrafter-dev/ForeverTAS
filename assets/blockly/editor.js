(() => {
  'use strict';

  let bridge = null;
  let workspace = null;
  let catalog = null;
  let revision = 0;
  let loadingWorkspace = false;
  let saveTimer = null;
  let lastSelectedBlockId = '';
  const definitionsByType = new Map();
  const categoriesById = new Map();
  let syncingProcedures = false;
  let debugSnapshot = {};
  let lastDebugBlock = '';
  const breakpoints = new Set();

  function parameterNames(text) {
    return text === '' ? [] : String(text).split(',').map(name => name.trim());
  }

  function isProcedureCall(block) {
    const id = definitionsByType.get(block.type)?.id;
    return id === 'procedures/call' || id === 'procedures/value';
  }

  function procedureChoices() {
    const block = this.getSourceBlock();
    if (!block) return [['my block', 'my block']];
    const owner = block.workspace.targetWorkspace || block.workspace;
    const names = new Set([block.getFieldValue('name') || 'my block', block.loadingProcedureName_]);
    for (const candidate of owner.getAllBlocks(false))
      if (definitionsByType.get(candidate.type)?.id === 'procedures/define') names.add(candidate.getFieldValue('name'));
    return [...names].filter(Boolean).map(name => [name, name]);
  }

  class ProcedureNameField extends Blockly.FieldDropdown {
    constructor() { super(procedureChoices); }
    doClassValidation_(value) {
      // Blockly otherwise validates against the options cached before the
      // field was attached. New/renamed/deserialized procedures are dynamic.
      this.getOptions(false);
      return super.doClassValidation_(value);
    }
  }

  function updateArguments(text) {
    const names = parameterNames(text);
    if (names.length > 64) return; // The native validator reports the limit.
    const old = this.parameterNames_ || [];
    if (JSON.stringify(old) === JSON.stringify(names)) return;
    const saved = old.map((name, index) => {
      const child = this.getInputTargetBlock(`arg${index}`);
      const shadow = this.getInput(`arg${index}`).connection.getShadowState(true);
      const connection = child && !child.isShadow() ? child.outputConnection : null;
      if (connection) child.unplug(false);
      this.removeInput(`arg${index}`);
      return {name, connection, shadow, used: false};
    });
    const numberType = catalog.blocks.find(block => block.id === 'values/number').type;
    names.forEach((name, index) => {
      const socket = this.appendValueInput(`arg${index}`).appendField(name || 'parameter');
      // Reordering keeps arguments attached to their names. A rename keeps the
      // positional value without stealing a surviving parameter's connection.
      let argument = saved.find(item => item.name === name && !item.used);
      if (!argument && saved[index] && !saved[index].used && !names.includes(saved[index].name)) argument = saved[index];
      if (argument) argument.used = true;
      socket.connection.setShadowState(argument?.shadow || {type: numberType, fields: {value: 0}});
      if (argument?.connection) socket.connection.connect(argument.connection);
    });
    this.parameterNames_ = names;
  }

  function syncProcedures(event) {
    if (syncingProcedures) return;
    syncingProcedures = true;
    const group = Blockly.Events.getGroup();
    if (event?.group) Blockly.Events.setGroup(event.group);
    try {
      const blocks = workspace.getAllBlocks(false);
      const changed = event?.blockId && workspace.getBlockById(event.blockId);
      const renamed = changed && definitionsByType.get(changed.type)?.id === 'procedures/define'
        && event.element === 'field' && event.name === 'name';
      const parametersChanged = changed && definitionsByType.get(changed.type)?.id === 'procedures/define'
        && event.element === 'field' && event.name === 'parameters';
      if (parametersChanged) {
        const before = parameterNames(event.oldValue), after = parameterNames(event.newValue);
        const renamedParameters = new Map();
        before.forEach((name, index) => {
          if (!after.includes(name) && after[index] && !before.includes(after[index])) renamedParameters.set(name, after[index]);
        });
        for (const child of changed.getDescendants(false)) {
          const id = definitionsByType.get(child.type)?.id;
          if (!['data/get', 'data/set', 'data/change', 'data/local', 'flow/for-each', 'flow/try'].includes(id)) continue;
          const name = renamedParameters.get(child.getFieldValue('name'));
          if (name) child.setFieldValue(name, 'name');
        }
      }
      const definitions = new Map(blocks.filter(block => definitionsByType.get(block.type)?.id === 'procedures/define')
        .map(block => [block.getFieldValue('name'), block]));
      for (const call of blocks.filter(block => isProcedureCall(block) || definitionsByType.get(block.type)?.id === 'procedures/reference')) {
        if (renamed && call.getFieldValue('name') === event.oldValue) call.setFieldValue(event.newValue, 'name');
        if (!isProcedureCall(call)) continue;
        const definition = definitions.get(call.getFieldValue('name'));
        if (!definition) continue;
        const parameters = definition.getFieldValue('parameters');
        if (call.getFieldValue('parameters') !== parameters) call.setFieldValue(parameters, 'parameters');
      }
    } finally { Blockly.Events.setGroup(group); syncingProcedures = false; }
  }

  function procedureContextMenu(options) {
    for (const [id, label] of [['procedures/call', 'Create command call'], ['procedures/value', 'Create reporter call'], ['procedures/reference', 'Use this block as a value']]) {
      options.push({text: label, enabled: !!bridge?.editable, callback: () => {
        const type = catalog.blocks.find(block => block.id === id).type;
        const call = workspace.newBlock(type);
        call.setFieldValue(this.getFieldValue('name'), 'name');
        if (call.getField('parameters')) call.setFieldValue(this.getFieldValue('parameters'), 'parameters');
        centerNewBlock(call);
      }});
    }
  }

  const search = document.getElementById('blockSearch');
  const clearSearch = document.getElementById('clearSearch');
  const searchResults = document.getElementById('searchResults');
  const diagnostics = document.getElementById('diagnostics');
  const loadingState = document.getElementById('loadingState');
  const readOnlyShield = document.getElementById('readOnlyShield');
  const viewerPickDock = document.getElementById('viewerPickDock');
  const viewerPickButton = document.getElementById('viewerPickButton');
  const viewerPickStatus = document.getElementById('viewerPickStatus');

  function parseJson(text, fallback) {
    try { return JSON.parse(text); } catch (_) { return fallback; }
  }

  function fieldFor(spec) {
    switch (spec.kind) {
      case 'number': return new Blockly.FieldNumber(Number(spec.defaultValue || 0));
      case 'integer': return new Blockly.FieldNumber(Number(spec.defaultValue || 0), undefined, undefined, 1);
      case 'boolean': return new Blockly.FieldCheckbox(String(spec.defaultValue).toLowerCase() === 'true');
      case 'enum': return new Blockly.FieldDropdown(spec.choices || []);
      default: return new Blockly.FieldTextInput(String(spec.defaultValue || ''));
    }
  }

  function appendLiteral(block, definition) {
    const input = block.appendDummyInput();
    const field = definition.fields[0];
    const unit = definition.id === 'values/meters' ? 'm'
      : definition.id === 'values/milliseconds' ? 'ms'
      : definition.id === 'values/degrees' ? '°'
      : definition.id === 'values/percent' ? '%' : '';
    input.appendField(fieldFor(field), field.key);
    if (unit) input.appendField(unit);
  }

  function appendRangeLiteral(block, definition) {
    const input = block.appendDummyInput();
    input.appendField('[');
    input.appendField(fieldFor(definition.fields[0]), 'minimum');
    input.appendField('…');
    input.appendField(fieldFor(definition.fields[1]), 'maximum');
    input.appendField(']');
  }

  function registerDefinitions() {
    for (const definition of catalog.blocks) {
      definitionsByType.set(definition.type, definition);
      Blockly.Blocks[definition.type] = {
        init() {
          const category = categoriesById.get(definition.category);
          this.setColour(category ? category.color : '#67736b');
          this.setInputsInline(definition.inputsInline !== false);

          if (definition.id === 'values/number-range' || definition.id === 'values/integer-range') {
            appendRangeLiteral(this, definition);
          } else if (definition.id.startsWith('values/') && definition.fields.length === 1) {
            appendLiteral(this, definition);
          } else {
            let header = null;
            const rowInputs = definition.inputsInline === false
              && definition.fields.length === 0
              && definition.inputs.length > 0;
            if (definition.label && !rowInputs) {
              header = this.appendDummyInput('header');
              header.appendField(definition.label);
            }
            for (const field of definition.fields) {
              if (!header) header = this.appendDummyInput('header');
              if (field.label) header.appendField(field.label);
              const call = definition.id === 'procedures/call' || definition.id === 'procedures/value';
              const editor = (call || definition.id === 'procedures/reference') && field.key === 'name' ? new ProcedureNameField() : fieldFor(field);
              header.appendField(editor, field.key);
              if (call && field.key === 'parameters') {
                editor.setVisible(false);
                editor.setValidator(function(text) {
                  this.getSourceBlock().updateArguments(text);
                  return text;
                });
              }
            }
            definition.inputs.forEach((input, index) => {
              const socket = this.appendValueInput(input.key);
              if (rowInputs && index === 0 && definition.label) {
                socket.appendField(definition.label);
              }
              if (input.checks && input.checks.length) socket.setCheck(input.checks);
              if (input.label) socket.appendField(input.label);
            });
            for (const statement of definition.statements) {
              const socket = this.appendStatementInput(statement.key);
              if (statement.checks && statement.checks.length) socket.setCheck(statement.checks);
              if (statement.label) socket.appendField(statement.label);
            }
          }

          if (definition.shape === 'reporter' || definition.shape === 'predicate') {
            this.setOutput(true, definition.outputChecks?.length ? definition.outputChecks : null);
          } else if (definition.shape === 'command' || definition.shape === 'control') {
            const checks = definition.statementChecks?.length ? definition.statementChecks : null;
            this.setPreviousStatement(true, checks);
            this.setNextStatement(true, checks);
          }
          this.setTooltip(definition.label || definition.id);
          if (definition.id === 'procedures/define') {
            this.customContextMenu = procedureContextMenu;
            this.setTooltip('Name this block and list its parameter names, separated by commas. Right-click to create a call. Use local variables and return inside the body.');
          }
          if (definition.shape === 'command' || definition.shape === 'control') {
            this.customContextMenu = options => options.push({
              text: breakpoints.has(this.id) ? 'Remove breakpoint' : 'Break before this block',
              enabled: true,
              callback: () => {
                if (breakpoints.has(this.id)) breakpoints.delete(this.id); else breakpoints.add(this.id);
                this.setWarningText(breakpoints.has(this.id) ? 'Breakpoint' : null, 'breakpoint');
                bridge.setBreakpoints([...breakpoints]);
                document.getElementById('inspectProgram').checked = true;
                bridge.inspectProgram(true);
              }
            });
          }
        }
      };
      if (definition.id === 'procedures/call' || definition.id === 'procedures/value' || definition.id === 'procedures/reference') {
        Object.assign(Blockly.Blocks[definition.type], {
          updateArguments,
          saveExtraState() { return {name: this.getFieldValue('name'), parameters: this.getFieldValue('parameters') || ''}; },
          loadExtraState(state) {
            this.loadingProcedureName_ = state.name || 'my block';
            this.setFieldValue(this.loadingProcedureName_, 'name');
            this.loadingProcedureName_ = null;
            if (this.getField('parameters')) this.setFieldValue(state.parameters || '', 'parameters');
          }
        });
      }
    }
  }

  function toolbox() {
    return {
      kind: 'categoryToolbox',
      contents: [...catalog.categories.map(category => ({
        kind: 'category',
        name: category.label,
        colour: category.color,
        contents: catalog.blocks
          .filter(block => block.category === category.id && block.toolboxVisible !== false)
          .map(block => ({kind: 'block', type: block.type}))
      })), {
        kind: 'category', name: 'Macroblocks', colour: '#bc6a36',
        contents: [...new Set((catalog.macros || []).map(macro => macro.category))].map(category => ({
          kind: 'category', name: category, colour: '#bc6a36',
          contents: [
            {kind: 'label', text: 'Insert an editable sequence'},
            ...(catalog.macros || []).filter(macro => macro.category === category)
              .map(macro => ({kind: 'button', text: macro.label, callbackKey: `macro:${macro.id}`}))
          ]
        }))
      }]
    };
  }

  function insertMacro(macro) {
    if (!workspace || !bridge?.editable) return;
    const source = JSON.parse(JSON.stringify(macro.stack));
    const walk = (node, visit) => {
      if (!node) return;
      visit(node);
      for (const input of Object.values(node.inputs || {})) {
        walk(input.block, visit);
        walk(input.shadow, visit);
      }
      walk(node.next?.block, visit);
    };
    const variable = type => ['ft_data_get', 'ft_data_set', 'ft_data_change', 'ft_data_local', 'ft_flow_for_each'].includes(type);
    const used = new Set(workspace.getAllBlocks(false).filter(block => variable(block.type)).map(block => block.getFieldValue('name')));
    for (const block of workspace.getAllBlocks(false))
      if (definitionForBlock(block)?.id === 'procedures/define')
        for (const name of parameterNames(block.getFieldValue('parameters'))) used.add(name);
    const names = new Set();
    walk(source, node => { if (variable(node.type)) names.add(node.fields.name); });
    let suffix = '';
    for (let number = 2; [...names].some(name => used.has(name + suffix)); ++number) suffix = ` ${number}`;
    walk(source, node => {
      delete node.id;
      delete node.x;
      delete node.y;
      if (variable(node.type)) node.fields.name += suffix;
    });
    const previousGroup = Blockly.Events.getGroup();
    Blockly.Events.setGroup(true);
    try {
      const first = Blockly.serialization.blocks.append(source, workspace, {recordUndo: true});
      first.setCommentText(`${macro.label}\n${macro.description}\nAll steps below are ordinary blocks. Temporary variables${suffix ? ` use suffix ${suffix.trim()}` : ' can be renamed'}.`);
      centerNewBlock(first);
    } finally {
      Blockly.Events.setGroup(previousGroup);
    }
    commitWorkspace();
  }

  const themes = new Map();

  function makeTheme(dark) {
    const key = dark ? 'dark' : 'light';
    if (themes.has(key)) return themes.get(key);
    const theme = Blockly.Theme.defineTheme(`forevertas-${key}`, {
      base: Blockly.Themes.Classic,
      componentStyles: dark ? {
        workspaceBackgroundColour: '#151917',
        toolboxBackgroundColour: '#171c19',
        toolboxForegroundColour: '#e7ece8',
        flyoutBackgroundColour: '#1a201c',
        flyoutForegroundColour: '#e7ece8',
        flyoutOpacity: 0.98,
        scrollbarColour: '#647068',
        scrollbarOpacity: 0.55,
        insertionMarkerColour: '#65d995',
        insertionMarkerOpacity: 0.42,
        cursorColour: '#65d995'
      } : {
        workspaceBackgroundColour: '#f4f5f2',
        toolboxBackgroundColour: '#f7f8f5',
        toolboxForegroundColour: '#202421',
        flyoutBackgroundColour: '#ffffff',
        flyoutForegroundColour: '#202421',
        flyoutOpacity: 0.98,
        scrollbarColour: '#9aa49c',
        scrollbarOpacity: 0.62,
        insertionMarkerColour: '#26734d',
        insertionMarkerOpacity: 0.38,
        cursorColour: '#26734d'
      },
      fontStyle: {family: 'Inter, Noto Sans, system-ui, sans-serif', weight: '500', size: 11},
      startHats: true
    });
    themes.set(key, theme);
    return theme;
  }

  function applyTheme(dark) {
    const isDark = Boolean(dark);
    document.documentElement.dataset.theme = isDark ? 'dark' : 'light';
    if (workspace) {
      workspace.setTheme(makeTheme(isDark));
      Blockly.svgResize(workspace);
    }
  }

  function injectWorkspace() {
    workspace = Blockly.inject('blocklyDiv', {
      toolbox: toolbox(),
      renderer: 'zelos',
      theme: makeTheme(Boolean(bridge?.darkMode)),
      media: 'qrc:/blockly/third_party/blockly/media/',
      trashcan: true,
      disable: true,
      sounds: false,
      move: {scrollbars: true, drag: true, wheel: true},
      zoom: {controls: true, wheel: true, startScale: 0.82, maxScale: 1.8, minScale: 0.45, scaleSpeed: 1.08, pinch: true},
      grid: {spacing: 24, length: 2, colour: '#354039', snap: false}
    });
    for (const macro of catalog.macros || [])
      workspace.registerButtonCallback(`macro:${macro.id}`, () => insertMacro(macro));
    workspace.addChangeListener(onWorkspaceEvent);
    window.addEventListener('resize', () => Blockly.svgResize(workspace));
  }

  function addDefaultShadows(block) {
    const definition = definitionsByType.get(block.type);
    if (!definition) return;
    for (const spec of definition.inputs || []) {
      if (!spec.defaultBlock) continue;
      const connection = block.getInput(spec.key)?.connection;
      if (!connection || connection.isConnected()) continue;
      const shadowDefinition = catalog.blocks.find(item => item.id === spec.defaultBlock);
      if (!shadowDefinition) continue;
      const shadow = workspace.newBlock(shadowDefinition.type);
      shadow.setShadow(true);
      if (shadowDefinition.id === 'values/number-range' ||
          shadowDefinition.id === 'values/integer-range') {
        const parts = String(spec.defaultValue || '').split(',', 2);
        if (parts.length === 2) {
          shadow.getField('minimum')?.setValue(parts[0]);
          shadow.getField('maximum')?.setValue(parts[1]);
        }
      } else {
        const valueField = shadow.getField('value');
        if (valueField && spec.defaultValue !== '') valueField.setValue(String(spec.defaultValue));
      }
      shadow.initSvg();
      shadow.render();
      connection.connect(shadow.outputConnection);
    }
  }

  function hydrateNewBlock(block) {
    addDefaultShadows(block);
    for (const child of block.getChildren(false)) addDefaultShadows(child);
  }

  function definitionForBlock(block) {
    return block ? definitionsByType.get(block.type) : null;
  }

  function selectedBlock() {
    if (lastSelectedBlockId && workspace) {
      const block = workspace.getBlockById(lastSelectedBlockId);
      if (block) return block;
    }
    const selected = Blockly.common?.getSelected?.();
    if (selected && selected.id && workspace?.getBlockById(selected.id) === selected) return selected;
    return null;
  }

  function pickerLabel(kind) {
    switch (kind) {
      case 'point': return 'Pick point in viewer';
      case 'rotation': return 'Use selected pose rotation';
      case 'box': return 'Use selected box';
      case 'prism': return 'Use selected prism';
      default: return 'Use viewer target';
    }
  }

  function updateViewerPicker() {
    const block = selectedBlock();
    const definition = definitionForBlock(block);
    const kind = definition?.viewerPicker || '';
    viewerPickDock.hidden = !kind;
    viewerPickButton.textContent = pickerLabel(kind);
    viewerPickButton.disabled = !kind || !bridge?.editable;
    if (!kind) viewerPickStatus.textContent = '';
  }

  function ensureDefaultInputBlock(parent, key) {
    const definition = definitionForBlock(parent);
    const spec = definition?.inputs?.find(item => item.key === key);
    if (!spec?.defaultBlock) return parent.getInputTargetBlock(key);
    const expected = catalog.blocks.find(item => item.id === spec.defaultBlock);
    if (!expected) return parent.getInputTargetBlock(key);
    let child = parent.getInputTargetBlock(key);
    if (child?.type === expected.type) return child;
    if (child) child.unplug(false);
    const connection = parent.getInput(key)?.connection;
    if (!connection) return null;
    child = workspace.newBlock(expected.type);
    child.setShadow(true);
    child.initSvg();
    child.render();
    connection.connect(child.outputConnection);
    addDefaultShadows(child);
    return child;
  }

  function setLiteralInput(parent, key, value) {
    const child = ensureDefaultInputBlock(parent, key);
    if (!child) return false;
    const field = child.getField('value');
    if (!field) return false;
    field.setValue(String(value));
    return true;
  }

  function applyViewerTarget(block, kind, data) {
    if (!block || !data) return false;
    hydrateNewBlock(block);
    if (kind === 'point') {
      return setLiteralInput(block, 'x', data.x)
        && setLiteralInput(block, 'y', data.y)
        && setLiteralInput(block, 'z', data.z);
    }
    if (kind === 'rotation') {
      return setLiteralInput(block, 'yaw', data.yawDegrees)
        && setLiteralInput(block, 'pitch', data.pitchDegrees)
        && setLiteralInput(block, 'roll', data.rollDegrees);
    }
    if (kind === 'box') {
      const center = ensureDefaultInputBlock(block, 'center');
      const size = ensureDefaultInputBlock(block, 'size');
      return center && size
        && setLiteralInput(center, 'x', data.centerX)
        && setLiteralInput(center, 'y', data.centerY)
        && setLiteralInput(center, 'z', data.centerZ)
        && setLiteralInput(size, 'x', data.sizeX)
        && setLiteralInput(size, 'y', data.sizeY)
        && setLiteralInput(size, 'z', data.sizeZ);
    }
    if (kind === 'prism') {
      const points = String(data.polygon || '').split(';').map(pair => pair.split(',').map(Number));
      if (points.length < 3 || points.some(point => point.length !== 2 || point.some(value => !Number.isFinite(value)))) return false;
      const origin = ensureDefaultInputBlock(block, 'origin');
      if (!origin) return false;
      block.setFieldValue(String(data.plane), 'plane');
      const source = (id, inputs = {}, fields = {}) => ({
        type: catalog.blocks.find(item => item.id === id).type, fields,
        inputs: Object.fromEntries(Object.entries(inputs).map(([key, value]) => [key, {block: value}]))
      });
      const number = value => source('values/number', {}, {value});
      let list = source('data/list');
      for (const [x, y] of points) list = source('data/append', {
        list, value: source('targets/point', {x: number(x), y: number(y), z: number(0)})
      });
      const socket = block.getInput('polygon').connection;
      socket.setShadowState(null);
      socket.targetBlock()?.dispose();
      const polygon = Blockly.serialization.blocks.append(source('targets/polygon-from-points', {points: list}), workspace, {recordUndo: true});
      socket.connect(polygon.outputConnection);
      return setLiteralInput(origin, 'x', data.originX)
        && setLiteralInput(origin, 'y', data.originY)
        && setLiteralInput(origin, 'z', data.originZ)
        && setLiteralInput(block, 'depth', data.depth);
    }
    return false;
  }

  function applyViewerTargetAtomically(block, kind, data) {
    loadingWorkspace = true;
    let applied = false;
    try {
      applied = applyViewerTarget(block, kind, data);
      block?.render?.();
    } finally {
      loadingWorkspace = false;
    }
    if (applied) {
      viewerPickStatus.textContent = data.name ? `Imported ${data.name}` : 'Viewer value imported';
      commitWorkspace();
    } else {
      viewerPickStatus.textContent = 'Could not apply viewer value to this block';
    }
    return applied;
  }

  function loadWorkspace(serializedText) {
    if (!workspace) return;
    const state = parseJson(serializedText, null);
    if (!state) return;
    loadingWorkspace = true;
    try {
      workspace.clear();
      Blockly.serialization.workspaces.load(state, workspace);
      syncProcedures();
      for (const block of workspace.getAllBlocks(false)) addDefaultShadows(block);
      Blockly.svgResize(workspace);
      lastSelectedBlockId = '';
      updateViewerPicker();
    } finally {
      loadingWorkspace = false;
    }
  }

  function onWorkspaceEvent(event) {
    if (loadingWorkspace || !bridge || !workspace) return;
    if (event && event.type === Blockly.Events.SELECTED) {
      lastSelectedBlockId = event.newElementId || '';
      updateViewerPicker();
      return;
    }
    if (event && (event.isUiEvent || event.type === Blockly.Events.VIEWPORT_CHANGE || event.type === Blockly.Events.TOOLBOX_ITEM_SELECT)) return;
    syncProcedures(event);
    if (event && event.type === Blockly.Events.BLOCK_CREATE) {
      for (const id of event.ids || []) {
        const block = workspace.getBlockById(id);
        if (block) hydrateNewBlock(block);
      }
    }
    clearTimeout(saveTimer);
    saveTimer = setTimeout(commitWorkspace, 140);
  }

  function commitWorkspace() {
    if (!bridge || !workspace || loadingWorkspace || !bridge.editable) return;
    const state = Blockly.serialization.workspaces.save(workspace);
    const json = JSON.stringify(state);
    ++revision;
    bridge.applyWorkspace(json, revision, accepted => {
      if (accepted === false) renderDiagnostics();
      else if (breakpoints.size) bridge.setBreakpoints([...breakpoints]);
      renderRuntime();
    });
  }

  function renderRuntime() {
    if (!bridge || !workspace) return;
    const running = Boolean(bridge.running), paused = running && debugSnapshot.paused;
    const program = workspace.getTopBlocks(false).some(block =>
      definitionForBlock(block)?.id === 'flow/when-start');
    document.getElementById('runProgram').disabled = running;
    document.getElementById('debugProgram').disabled = running || !program;
    document.getElementById('debugProgram').title = program ? 'Start paused before the first block' : 'Choose block program in the start hat';
    document.getElementById('pauseProgram').disabled = !running || paused || !program;
    document.getElementById('inspectProgram').disabled = !program;
    document.getElementById('resumeProgram').disabled = !paused;
    document.getElementById('stepProgram').disabled = !paused;
    document.getElementById('stopProgram').disabled = !running;
    document.getElementById('runtimeStatus').textContent = (paused ? 'Paused' : running ? 'Running' : debugSnapshot.finished ? 'Finished' : '')
      + (debugSnapshot.timeMs === undefined ? '' : ` · ${debugSnapshot.timeMs} ms`);
    workspace.highlightBlock(paused ? debugSnapshot.block || null : null);
    if (paused && debugSnapshot.block && debugSnapshot.block !== lastDebugBlock && workspace.getBlockById(debugSnapshot.block))
      workspace.centerOnBlock(debugSnapshot.block);
    lastDebugBlock = paused ? debugSnapshot.block : '';
    const inspector = document.getElementById('runtimeInspector');
    inspector.hidden = !document.getElementById('inspectProgram').checked && !paused;
    const values = document.getElementById('runtimeValues');
    const open = new Map([...values.querySelectorAll('details')].map(item => [item.dataset.key, item.open]));
    values.replaceChildren();
    const filter = document.getElementById('watchFilter').value.toLowerCase();
    const section = (key, title, rows, defaultOpen = false) => {
      const detail = document.createElement('details'); detail.dataset.key = key;
      detail.open = open.has(key) ? open.get(key) : defaultOpen;
      const summary = document.createElement('summary'); summary.textContent = title; detail.append(summary);
      for (const row of rows || []) {
        if (!`${row.name} ${row.value}`.toLowerCase().includes(filter)) continue;
        const div = document.createElement('div'); div.className = 'runtimeValue';
        const name = document.createElement('span'); name.textContent = row.name;
        const value = document.createElement('code'); value.textContent = row.value;
        div.append(name, value); detail.append(div);
      }
      values.append(detail);
    };
    section('globals', 'Program variables', debugSnapshot.variables, true);
    (debugSnapshot.frames || []).forEach((frame, i) => section(`frame${i}`, `${i + 1}. ${frame.name}`, frame.locals, true));
    section('state', 'Simulation state', debugSnapshot.state);
  }

  for (const [id, debug] of [['runProgram', false], ['debugProgram', true]]) {
    document.getElementById(id).addEventListener('click', () => {
      if (!bridge?.editable) return;
      clearTimeout(saveTimer);
      const json = JSON.stringify(Blockly.serialization.workspaces.save(workspace));
      if (debug) document.getElementById('inspectProgram').checked = true;
      bridge.runWorkspace(json, ++revision, debug, accepted => {
        if (!accepted) renderDiagnostics();
        if (accepted && breakpoints.size) bridge.setBreakpoints([...breakpoints]);
      });
    });
  }
  document.getElementById('pauseProgram').addEventListener('click', () => bridge.pauseProgram());
  document.getElementById('resumeProgram').addEventListener('click', () => bridge.resumeProgram('run'));
  document.getElementById('stepProgram').addEventListener('click', () => bridge.resumeProgram(document.getElementById('stepKind').value));
  document.getElementById('stopProgram').addEventListener('click', () => bridge.stopProgram());
  document.getElementById('inspectProgram').addEventListener('change', event => {
    bridge.inspectProgram(event.target.checked); renderRuntime();
  });
  document.getElementById('watchFilter').addEventListener('input', renderRuntime);

  function replaceProgram(state) {
    if (!bridge?.editable) return;
    // Keep the previous program in the undo history rather than discarding it
    // while switching examples or loading a file.
    Blockly.Events.setGroup(true);
    try {
      workspace.clear();
      for (const script of state.blocks?.blocks || []) Blockly.serialization.blocks.append(script, workspace, {recordUndo: true});
      syncProcedures();
      for (const item of workspace.getAllBlocks(false)) addDefaultShadows(item);
    } finally { Blockly.Events.setGroup(false); }
    breakpoints.clear(); bridge.setBreakpoints([]);
    clearTimeout(saveTimer); commitWorkspace();
    workspace.zoomToFit();
  }

  document.getElementById('projectAction').addEventListener('change', event => {
    const action = event.target.value; event.target.value = '';
    if (!bridge?.editable || !action) return;
    if (action === 'save') {
      bridge.saveProject(JSON.stringify(Blockly.serialization.workspaces.save(workspace)), () => {});
      return;
    }
    if (action === 'open' || action === 'library') {
      bridge.openProject(text => {
        if (!text || !bridge.editable) return;
        const state = JSON.parse(text);
        if (action === 'open') { replaceProgram(state); return; }
        const scripts = state.blocks?.blocks || [];
        const names = new Set(workspace.getAllBlocks(false)
          .filter(b => b.type === 'ft_procedures_define').map(b => b.getFieldValue('name')));
        const definitions = scripts.filter(b => b.type === 'ft_procedures_define');
        const conflict = definitions.find(b => names.has(b.fields?.name));
        if (conflict || !definitions.length) {
          diagnostics.textContent = conflict ? `A block named “${conflict.fields.name}” already exists.` : 'This file contains no reusable block definitions.';
          diagnostics.hidden = false; return;
        }
        const removeIds = value => {
          if (!value || typeof value !== 'object') return;
          if (value.type) delete value.id;
          Object.values(value).forEach(removeIds);
        };
        Blockly.Events.setGroup(true);
        try {
          for (const definition of definitions) { removeIds(definition); Blockly.serialization.blocks.append(definition, workspace, {recordUndo: true}); }
          syncProcedures();
        } finally { Blockly.Events.setGroup(false); }
        clearTimeout(saveTimer); commitWorkspace();
      });
      return;
    }
    replaceProgram(window.foreverBlockExample(action === 'new' ? 'empty' : action));
  });

  function renderDiagnostics() {
    if (!bridge) return;
    const items = parseJson(bridge.diagnosticsJson, []);
    if (!items.length) {
      diagnostics.hidden = true;
      diagnostics.textContent = '';
      diagnostics.classList.remove('info');
      return;
    }
    diagnostics.textContent = items.map(item => item.message).join(' · ');
    diagnostics.classList.toggle('info', items.every(item => item.severity !== 'error'));
    diagnostics.hidden = false;
  }

  function centerNewBlock(block) {
    block.initSvg();
    block.render();
    hydrateNewBlock(block);
    const metrics = workspace.getMetrics();
    const scale = workspace.scale || 1;
    const x = (metrics.viewLeft + metrics.viewWidth * 0.48) / scale;
    const y = (metrics.viewTop + metrics.viewHeight * 0.28) / scale;
    block.moveBy(x, y);
    block.select();
  }

  function updateSearch() {
    if (bridge && !bridge.editable) {
      searchResults.classList.remove('open');
      return;
    }
    const query = search.value.trim().toLowerCase();
    clearSearch.style.visibility = query ? 'visible' : 'hidden';
    searchResults.textContent = '';
    if (!query) {
      searchResults.classList.remove('open');
      return;
    }
    const matches = [...catalog.blocks.filter(block => block.toolboxVisible !== false),
      ...(catalog.macros || []).map(macro => ({...macro, isMacro: true}))]
      .filter(block => `${block.label} ${block.id} ${block.isMacro ? 'macroblock ' + block.description : ''}`.toLowerCase().includes(query))
      .slice(0, 30);
    for (const definition of matches) {
      const category = categoriesById.get(definition.category);
      const button = document.createElement('button');
      button.type = 'button';
      button.className = 'searchResult';
      const dot = document.createElement('span');
      dot.className = 'searchDot';
      dot.style.background = category?.color || '#6b776f';
      const label = document.createElement('span');
      label.textContent = definition.label;
      const meta = document.createElement('span');
      meta.className = 'searchMeta';
      meta.textContent = definition.isMacro ? `Macro · ${definition.category}` : category?.label || '';
      if (definition.isMacro) button.title = definition.description;
      button.append(dot, label, meta);
      button.addEventListener('click', () => {
        if (!bridge?.editable) return;
        if (definition.isMacro) insertMacro(definition);
        else centerNewBlock(workspace.newBlock(definition.type));
        search.value = '';
        updateSearch();
        workspace.getToolbox()?.clearSelection?.();
      });
      searchResults.appendChild(button);
    }
    if (!matches.length) {
      const empty = document.createElement('div');
      empty.className = 'searchResult';
      empty.textContent = 'No matching blocks';
      searchResults.appendChild(empty);
    }
    searchResults.classList.add('open');
  }

  search.addEventListener('input', updateSearch);
  search.addEventListener('keydown', event => {
    if (event.key === 'Escape') {
      search.value = '';
      updateSearch();
      search.blur();
    }
  });
  clearSearch.addEventListener('click', () => {
    search.value = '';
    updateSearch();
    search.focus();
  });
  document.addEventListener('keydown', event => {
    if ((event.ctrlKey || event.metaKey) && !event.altKey
        && event.key.toLowerCase() === 'f') {
      event.preventDefault();
      search.focus();
      search.select();
    }
  });
  viewerPickButton.addEventListener('click', () => {
    if (!bridge?.editable) return;
    const block = selectedBlock();
    const kind = definitionForBlock(block)?.viewerPicker || '';
    if (!block || !kind) return;
    if (kind === 'point') {
      viewerPickStatus.textContent = 'Click a track surface in the 3D viewer';
      bridge.requestViewerPointPick(block.id);
      return;
    }
    bridge.selectedViewerTargetJson(kind, text => {
      const data = parseJson(text, null);
      if (!data || !Object.keys(data).length) {
        viewerPickStatus.textContent = `No selected ${kind} target in the viewer`;
        return;
      }
      applyViewerTargetAtomically(block, kind, data);
    });
  });
  document.addEventListener('pointerdown', event => {
    if (!document.getElementById('searchDock').contains(event.target)) searchResults.classList.remove('open');
  });

  function setEditableState() {
    if (!bridge) return;
    const editable = Boolean(bridge.editable);
    workspace.setIsReadOnly(!editable);
    readOnlyShield.hidden = editable;
    search.disabled = !editable;
    document.getElementById('projectAction').disabled = !editable;
    clearSearch.disabled = !editable;
    updateViewerPicker();
    if (!editable) searchResults.classList.remove('open');
    renderRuntime();
  }

  function boot(channel) {
    try {
      bridge = channel.objects.foreverBridge;
      revision = Number(bridge.workspaceRevision || 0);
      applyTheme(Boolean(bridge.darkMode));
      catalog = parseJson(bridge.catalogJson, {categories: [], blocks: []});
      for (const category of catalog.categories) categoriesById.set(category.id, category);
      registerDefinitions();
      injectWorkspace();
      loadWorkspace(bridge.workspaceJson);
      renderDiagnostics();
      setEditableState();
      bridge.workspaceJsonChanged.connect(() => {
        revision = Number(bridge.workspaceRevision || revision);
        loadWorkspace(bridge.workspaceJson);
      });
      bridge.diagnosticsJsonChanged.connect(renderDiagnostics);
      bridge.editableChanged.connect(setEditableState);
      debugSnapshot = parseJson(bridge.debugJson, {});
      bridge.debugChanged.connect(() => { debugSnapshot = parseJson(bridge.debugJson, {}); renderRuntime(); });
      bridge.darkModeChanged.connect(() => applyTheme(Boolean(bridge.darkMode)));
      bridge.viewerPointPicked.connect((blockId, x, y, z) => {
        const block = workspace?.getBlockById(String(blockId));
        if (!block || definitionForBlock(block)?.viewerPicker !== 'point') return;
        applyViewerTargetAtomically(block, 'point', {x, y, z});
      });
      loadingState.hidden = true;
      bridge.editorReady();
    } catch (error) {
      const detail = error && (error.stack || error.message) ? (error.stack || error.message) : String(error);
      console.error('ForeverTAS block editor boot failed:', detail);
      loadingState.textContent = 'Block editor failed: ' + detail;
      loadingState.style.whiteSpace = 'pre-wrap';
      loadingState.style.padding = '24px';
      loadingState.style.placeItems = 'start';
    }
  }

  if (window.qt && qt.webChannelTransport) {
    new QWebChannel(qt.webChannelTransport, boot);
  } else {
    loadingState.textContent = 'Native block bridge unavailable.';
  }
})();
