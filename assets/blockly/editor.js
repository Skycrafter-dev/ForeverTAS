(() => {
  'use strict';

  let bridge = null;
  let workspace = null;
  let programTabs = null;
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
  let workbench = null;
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
          // Blockly derives a collapsed label during a later render frame.
          // That presentation update must not become a second undo operation.
          const updateCollapsed = this.updateCollapsed.bind(this);
          this.updateCollapsed = () => {
            Blockly.Events.disable();
            try { updateCollapsed(); } finally { Blockly.Events.enable(); }
          };
          if (definition.id === 'flow/section') this.toString = () => this.getFieldValue('name') || 'Section';
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
        contents: [...new Set((catalog.macros || []).filter(macro => macro.category !== 'Macromacroblocks').map(macro => macro.category))].map(category => ({
          kind: 'category', name: category, colour: '#bc6a36',
          contents: [
            {kind: 'label', text: 'Insert an editable sequence'},
            ...(catalog.macros || []).filter(macro => macro.category === category)
              .map(macro => ({kind: 'button', text: macro.label, callbackKey: `macro:${macro.id}`}))
          ]
        }))
      }, {
        kind: 'category', name: 'Macromacroblocks', colour: '#bc6a36',
        contents: (catalog.macros || []).filter(macro => macro.category === 'Macromacroblocks')
          .map(macro => ({kind: 'button', text: macro.label, callbackKey: `macro:${macro.id}`}))
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
      const grouped = {type: 'ft_flow_section', fields: {name: macro.label}, collapsed: true, inputs: {body: {block: source}}};
      const first = Blockly.serialization.blocks.append(grouped, workspace, {recordUndo: true});
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
      collapse: true,
      sounds: false,
      move: {scrollbars: true, drag: true, wheel: true},
      zoom: {controls: true, wheel: true, startScale: 0.82, maxScale: 1.8, minScale: 0.45, scaleSpeed: 1.08, pinch: true},
      grid: {spacing: 24, length: 2, colour: '#354039', snap: false}
    });
    for (const macro of catalog.macros || [])
      workspace.registerButtonCallback(`macro:${macro.id}`, () => insertMacro(macro));
    workspace.addChangeListener(onWorkspaceEvent);
    window.addEventListener('resize', () => Blockly.svgResize(workspace));
    new ResizeObserver(() => {
      if (document.getElementById('canvasPane').clientWidth) Blockly.svgResize(workspace);
    }).observe(document.getElementById('canvasPane'));
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
      if (!setLiteralInput(block, 'plane', data.plane)) return false;
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

  function loadWorkspace(state) {
    if (!workspace) return;
    loadingWorkspace = true;
    Blockly.Events.disable();
    try {
      workspace.clear();
      Blockly.serialization.workspaces.load(state, workspace);
      syncProcedures();
      for (const block of workspace.getAllBlocks(false)) addDefaultShadows(block);
      Blockly.svgResize(workspace);
      lastSelectedBlockId = '';
      updateViewerPicker();
    } finally {
      Blockly.Events.enable();
      loadingWorkspace = false;
    }
  }

  function onWorkspaceEvent(event) {
    if (loadingWorkspace || !bridge || !workspace) return;
    if (event && event.type === Blockly.Events.SELECTED) {
      lastSelectedBlockId = event.newElementId || '';
      updateViewerPicker();
      programTabs?.changed();
      return;
    }
    if (event?.type === Blockly.Events.VIEWPORT_CHANGE) { programTabs?.changed(); return; }
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
    programTabs?.changed();
  }

  function commitWorkspace() {
    if (!bridge || !workspace || loadingWorkspace || !bridge.editable) return;
    const state = Blockly.serialization.workspaces.save(workspace);
    const json = JSON.stringify(state);
    const submittedRevision = ++revision, documentId = programTabs?.active()?.id;
    bridge.applyWorkspace(json, submittedRevision, accepted => {
      if (submittedRevision !== revision || programTabs?.active()?.id !== documentId) return;
      if (accepted === false) renderDiagnostics();
      else if (breakpoints.size) bridge.setBreakpoints([...breakpoints]);
      renderRuntime();
    });
    workbench?.render();
    programTabs?.changed();
  }

  function renderRuntime() {
    if (!bridge || !workspace) return;
    const running = Boolean(bridge.running), paused = running && debugSnapshot.paused;
    const program = workspace.getTopBlocks(false).some(block =>
      definitionForBlock(block)?.id === 'flow/when-start');
    const unavailable = running || !program || !programTabs?.active() || Boolean(programTabs.active().loadError);
    document.getElementById('runProgram').disabled = unavailable || Boolean(document.querySelector('#configurationPanel input:invalid'));
    document.getElementById('debugProgram').disabled = unavailable || Boolean(document.querySelector('#configurationPanel input:invalid'));
    document.getElementById('debugProgram').title = program ? 'Start paused before the first block' : 'Add a when run starts block';
    document.getElementById('pauseProgram').disabled = !running || paused || !program;
    document.getElementById('inspectProgram').disabled = !program;
    document.getElementById('resumeProgram').disabled = !paused;
    for (const button of document.querySelectorAll('[data-step]')) button.disabled = !paused;
    document.getElementById('stepMenu').querySelector('summary').setAttribute('aria-disabled', String(!paused));
    document.getElementById('stopProgram').disabled = !running;
    document.getElementById('runtimeStatus').textContent = (paused ? 'Paused' : running ? 'Running' : debugSnapshot.finished ? 'Finished' : '')
      + (debugSnapshot.timeMs === undefined ? '' : ` · ${debugSnapshot.timeMs} ms`);
    workspace.highlightBlock(paused ? debugSnapshot.block || null : null);
    if (paused && debugSnapshot.block && debugSnapshot.block !== lastDebugBlock && workspace.getBlockById(debugSnapshot.block)) {
      const active = workspace.getBlockById(debugSnapshot.block);
      workbench?.reveal(active);
    }
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
    programTabs?.render();
  }

  for (const [id, debug] of [['runProgram', false], ['debugProgram', true]]) {
    document.getElementById(id).addEventListener('click', async () => {
      if (!bridge?.editable || programTabs.busy) return;
      const documentId = programTabs.active()?.id;
      const invalid = document.querySelector('#configurationPanel input:invalid');
      if (invalid) { invalid.reportValidity(); return; }
      if (await programTabs.flush()) return;
      if (!bridge.editable || programTabs.busy || programTabs.active()?.id !== documentId) return;
      bridge.cancelViewerPointPick();
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
  for (const button of document.querySelectorAll('[data-step]')) button.addEventListener('click', () => {
    document.getElementById('stepMenu').open = false;
    bridge.resumeProgram(button.dataset.step);
  });
  document.getElementById('stopProgram').addEventListener('click', () => bridge.stopProgram());
  document.getElementById('inspectProgram').addEventListener('change', event => {
    bridge.inspectProgram(event.target.checked); renderRuntime();
  });
  document.getElementById('watchFilter').addEventListener('input', renderRuntime);

  function openProgram(state, title = 'Untitled', view = 'configure') {
    return programTabs.open(state, title, '', view);
  }

  function projectAction(action) {
    document.getElementById('projectMenu').open = false;
    if (!bridge?.editable || programTabs.busy || !action) return;
    if (action === 'save' || action === 'save-as') { programTabs.saveFile(action === 'save-as'); return; }
    if (action === 'rename') { programTabs.rename(); return; }
    if (action === 'close') { programTabs.closeTab(); return; }
    if (action === 'reopen') { programTabs.reopen(); return; }
    if (action === 'open' || action === 'library') {
      const documentId = programTabs.active()?.id;
      bridge.openProgramFile(text => {
        if (!text || !bridge.editable) return;
        const file = JSON.parse(text), state = file.workspace;
        if (action === 'open') { programTabs.open(state, file.title, file.path); return; }
        if (!documentId || documentId !== programTabs.active()?.id) return;
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
    openProgram(window.foreverBlockExample(action === 'new' ? 'empty' : action),
      ({new: 'Untitled', feedback: 'Feedback control', branches: 'Parallel branches'})[action], 'blocks');
  }
  for (const button of document.querySelectorAll('[data-project]'))
    button.addEventListener('click', () => projectAction(button.dataset.project));
  document.getElementById('newProgramTab').addEventListener('click', () => projectAction('new'));
  document.addEventListener('keydown', event => {
    if ((event.ctrlKey || event.metaKey) && !event.altKey && !document.querySelector('dialog[open]') && event.key.toLowerCase() === 'n') {
      event.preventDefault(); projectAction('new');
    }
  });

  function setView(view) {
    if (!['configure', 'blocks', 'split'].includes(view)) view = 'configure';
    document.getElementById('editorShell').dataset.view = view;
    document.getElementById('configureView').setAttribute('aria-pressed', String(view === 'configure'));
    document.getElementById('blocksView').setAttribute('aria-pressed', String(view === 'blocks'));
    document.getElementById('settingsSideToggle').setAttribute('aria-pressed', String(view === 'split'));
    if (view !== 'configure') requestAnimationFrame(() => Blockly.svgResize(workspace));
    programTabs?.changed();
  }
  document.getElementById('configureView').addEventListener('click', () => { setView('configure'); workbench?.render(); });
  document.getElementById('blocksView').addEventListener('click', () => setView('blocks'));
  document.getElementById('settingsSideToggle').addEventListener('click', () => { setView('split'); workbench?.render(); });
  document.addEventListener('pointerdown', event => {
    const menus = [...document.querySelectorAll('.popupMenu[open]')];
    if (menus.length && !menus.some(menu => menu.contains(event.target))) {
      for (const menu of menus) menu.open = false;
      if (event.target.closest('#canvasPane')) { event.preventDefault(); event.stopPropagation(); }
    }
  }, true);
  document.addEventListener('keydown', event => {
    if (event.key === 'Escape') for (const menu of document.querySelectorAll('.popupMenu[open]')) { menu.open = false; menu.querySelector('summary').focus(); }
  });
  document.addEventListener('click', event => {
    if (event.target.closest('summary[aria-disabled="true"]')) event.preventDefault();
  }, true);
  document.addEventListener('toggle', event => {
    const menu = event.target;
    if (!menu.matches?.('.popupMenu') || !menu.open) return;
    for (const other of document.querySelectorAll('.popupMenu[open]')) if (other !== menu) other.open = false;
    const content = menu.querySelector('.menuContents'), trigger = menu.querySelector('summary').getBoundingClientRect();
    content.style.position = 'fixed'; content.style.bottom = 'auto';
    content.style.maxHeight = Math.max(80, window.innerHeight - 16) + 'px';
    const size = content.getBoundingClientRect();
    content.style.left = Math.max(6, Math.min(trigger.left, window.innerWidth - size.width - 6)) + 'px';
    content.style.top = Math.max(6, Math.min(menu.id === 'stepMenu' ? trigger.top - size.height - 4 : trigger.bottom + 4,
      window.innerHeight - size.height - 6)) + 'px';
  }, true);
  document.addEventListener('keydown', event => {
    const menu = event.target.closest?.('.popupMenu[open]');
    if (!menu || !['ArrowDown', 'ArrowUp', 'Home', 'End'].includes(event.key)) return;
    const buttons = [...menu.querySelectorAll('.menuContents button:not(:disabled)')];
    if (!buttons.length) return;
    let next = buttons.indexOf(document.activeElement) + (event.key === 'ArrowUp' ? -1 : 1);
    if (event.key === 'Home') next = 0;
    if (event.key === 'End') next = buttons.length - 1;
    buttons[(next + buttons.length) % buttons.length].focus(); event.preventDefault();
  });

  function collectTabUi() {
    const drafts = {};
    for (const input of document.querySelectorAll('#configurationCards input[data-source]')) {
      if (input.type === 'checkbox') continue;
      const source = workspace.getBlockById(input.dataset.source)?.getField(input.dataset.field);
      if (source && (!input.checkValidity() || input.value !== String(source.getValue())))
        drafts[input.dataset.source + '/' + input.dataset.field] = {value: input.value, error: input.validationMessage};
    }
    return {view: document.getElementById('editorShell').dataset.view,
      scale: workspace.scale, scrollX: workspace.scrollX, scrollY: workspace.scrollY,
      cards: Object.fromEntries([...document.querySelectorAll('#configurationCards details[data-block]')].map(card => [card.dataset.block, card.open])),
      configScroll: document.getElementById('configurationPanel').scrollTop,
      selected: lastSelectedBlockId, breakpoints: [...breakpoints], drafts};
  }
  function restoreTabUi(ui) {
    document.getElementById('configurationCards').replaceChildren();
    workbench.render();
    for (const card of document.querySelectorAll('#configurationCards details[data-block]'))
      if (ui.cards && Object.hasOwn(ui.cards, card.dataset.block)) card.open = ui.cards[card.dataset.block];
    for (const input of document.querySelectorAll('#configurationCards input[data-source]')) {
      const draft = ui.drafts?.[input.dataset.source + '/' + input.dataset.field];
      if (draft) { input.value = draft.value; input.setCustomValidity(draft.error || ''); }
    }
    document.getElementById('configurationPanel').scrollTop = ui.configScroll || 0;
    setView(ui.view || 'configure');
    if (Number.isFinite(ui.scale)) workspace.setScale(Math.max(.45, Math.min(1.8, ui.scale)));
    if (Number.isFinite(ui.scrollX) && Number.isFinite(ui.scrollY)) workspace.scroll(ui.scrollX, ui.scrollY);
    const selected = ui.selected ? workspace.getBlockById(ui.selected) : null;
    selected?.select();
    renderRuntime();
  }
  function activateTab(doc) {
    clearTimeout(saveTimer);
    bridge.cancelViewerPointPick();
    search.value = ''; updateSearch();
    for (const menu of document.querySelectorAll('.popupMenu[open]')) menu.open = false;
    breakpoints.clear();
    for (const id of doc?.ui?.breakpoints || []) breakpoints.add(id);
    bridge.setBreakpoints([...breakpoints]);
    debugSnapshot = {}; lastDebugBlock = '';
    document.getElementById('inspectProgram').checked = false;
    bridge.inspectProgram(false);
    document.getElementById('emptyPrograms').hidden = Boolean(doc);
    commitWorkspace();
  }

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
      setView('blocks');
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
    search.disabled = !editable;
    for (const button of document.querySelectorAll('[data-project]')) button.disabled = !editable;
    document.getElementById('newSearch').disabled = !editable;
    clearSearch.disabled = !editable;
    updateViewerPicker();
    if (!editable) searchResults.classList.remove('open');
    renderRuntime();
    workbench?.render();
  }

  async function boot(channel) {
    try {
      bridge = channel.objects.foreverBridge;
      revision = Number(bridge.workspaceRevision || 0);
      applyTheme(Boolean(bridge.darkMode));
      catalog = parseJson(bridge.catalogJson, {categories: [], blocks: []});
      for (const category of catalog.categories) categoriesById.set(category.id, category);
      registerDefinitions();
      injectWorkspace();
      workbench = ForeverWorkbench.attach({workspace, bridge, catalog, changed: commitWorkspace,
        replace: openProgram, showBlocks: () => setView('blocks')});
      programTabs = ForeverProgramTabs.create({workspace, bridge, load: loadWorkspace,
        activated: activateTab, collectUi: collectTabUi, restoreUi: restoreTabUi});
      window.foreverProgramTabs = programTabs;
      await programTabs.initialize();
      workbench.render();
      renderDiagnostics();
      setEditableState();
      bridge.workspaceJsonChanged.connect(() => {
        revision = Number(bridge.workspaceRevision || revision);
        const state = JSON.stringify(Blockly.serialization.workspaces.save(workspace));
        if (state !== bridge.workspaceJson) openProgram(JSON.parse(bridge.workspaceJson), 'Recovered program');
        workbench.render();
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
