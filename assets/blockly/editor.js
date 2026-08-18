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
              header.appendField(fieldFor(field), field.key);
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
            this.setOutput(true, definition.outputChecks || null);
          } else if (definition.shape === 'command' || definition.shape === 'control') {
            const checks = definition.statementChecks?.length ? definition.statementChecks : null;
            this.setPreviousStatement(true, checks);
            this.setNextStatement(true, checks);
          }
          this.setTooltip(definition.label || definition.id);
        }
      };
    }
  }

  function toolbox() {
    return {
      kind: 'categoryToolbox',
      contents: catalog.categories.map(category => ({
        kind: 'category',
        name: category.label,
        colour: category.color,
        contents: catalog.blocks
          .filter(block => block.category === category.id && block.toolboxVisible !== false)
          .map(block => ({kind: 'block', type: block.type}))
      }))
    };
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
      sounds: false,
      move: {scrollbars: true, drag: true, wheel: true},
      zoom: {controls: true, wheel: true, startScale: 0.82, maxScale: 1.8, minScale: 0.45, scaleSpeed: 1.08, pinch: true},
      grid: {spacing: 24, length: 2, colour: '#354039', snap: false}
    });
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
      const origin = ensureDefaultInputBlock(block, 'origin');
      if (!origin) return false;
      block.setFieldValue(String(data.plane), 'plane');
      return setLiteralInput(origin, 'x', data.originX)
        && setLiteralInput(origin, 'y', data.originY)
        && setLiteralInput(origin, 'z', data.originZ)
        && setLiteralInput(block, 'depth', data.depth)
        && setLiteralInput(block, 'polygon', data.polygon);
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
    });
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
    const matches = catalog.blocks
      .filter(block => block.toolboxVisible !== false)
      .filter(block => block.shape !== 'hat')
      .filter(block => `${block.label} ${block.id}`.toLowerCase().includes(query))
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
      meta.textContent = category?.label || '';
      button.append(dot, label, meta);
      button.addEventListener('click', () => {
        if (!bridge?.editable) return;
        const block = workspace.newBlock(definition.type);
        centerNewBlock(block);
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
    readOnlyShield.hidden = editable;
    search.disabled = !editable;
    clearSearch.disabled = !editable;
    updateViewerPicker();
    if (!editable) searchResults.classList.remove('open');
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
