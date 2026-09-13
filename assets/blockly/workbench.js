// Configuration is a view of literal source nodes, never a second settings model.
window.ForeverWorkbench = (() => {
  const clone = value => JSON.parse(JSON.stringify(value));
  const type = id => 'ft_' + id.replace(/[^a-z0-9]/g, '_');
  const block = (id, inputs = {}, fields = {}) => ({type: type(id), fields,
    inputs: Object.fromEntries(Object.entries(inputs).map(([key, value]) => [key, {block: value}]))});
  const chain = nodes => {
    for (let i = 0; i < nodes.length - 1; ++i) nodes[i].next = {block: nodes[i + 1]};
    return nodes[0];
  };
  const num = value => block('values/number', {}, {value});
  const flag = value => block('values/boolean', {}, {value: value ? 'TRUE' : 'FALSE'});
  const get = name => block('data/get', {}, {name});
  const set = (name, value, local = false) => block(local ? 'data/local' : 'data/set', {value}, {name});
  const binary = (id, a, b) => block(id, {a, b});
  const branch = (condition, body) => block('flow/if', {condition, body: chain(body)});
  const section = (name, body) => ({...block('flow/section', {body}, {name}), collapsed: true});
  const call = (name, reporter = false) => ({...block(reporter ? 'procedures/value' : 'procedures/call', {}, {name, parameters: ''}),
    extraState: {name, parameters: ''}});
  const reference = name => ({...block('procedures/reference', {}, {name}), extraState: {name}});
  const walk = (node, visit) => {
    if (!node) return;
    visit(node);
    for (const socket of Object.values(node.inputs || {})) { walk(socket.block, visit); walk(socket.shadow, visit); }
    walk(node.next?.block, visit);
  };
  const variable = node => ['ft_data_local','ft_data_get','ft_data_set','ft_data_change','ft_flow_for_each'].includes(node.type);
  function macroSource(macro, prefix = '', fixedWindow = false) {
    const source = clone(macro.stack);
    walk(source, node => {
      delete node.id; delete node.x; delete node.y;
      if (fixedWindow && macro.category === 'Inputs' && node.type === type('data/local')) {
        if (node.fields.name === 'first ms') node.inputs.value = {block: num(1000)};
        if (node.fields.name === 'last ms') node.inputs.value = {block: num(5990)};
      }
      if (variable(node)) node.fields.name = prefix + node.fields.name;
    });
    return section(macro.label, source);
  }
  function procedure(name, body) {
    return block('procedures/define', {body: chain(body)}, {name, parameters: ''});
  }
  function sourceForCondition(macro, prefix, horizon = 6000) {
    let source = clone(macro.stack);
    if (macro.id === 'time-condition') walk(source, node => {
      if (node.type === type('data/local') && node.fields.name === 'last ms') node.inputs.value = {block: num(horizon)};
    });
    // The per-tick example becomes a predicate in the guided search. Its loop
    // is owned by the target; the same comparison remains visible source.
    if (macro.id === 'require-each-tick') source = chain([
      set('minimum speed (m/s)', num(0), true),
      set('condition', binary('conditions/greater-equal', block('simulation/car-speed'), get('minimum speed (m/s)')), true)
    ]);
    let tail = source; while (tail.next?.block) tail = tail.next.block;
    tail.next = {block: branch(block('conditions/not', {value: get('condition')}), [block('procedures/return', {value: flag(false)})])};
    walk(source, node => { delete node.id; if (variable(node)) node.fields.name = prefix + node.fields.name; });
    return section(macro.label, source);
  }
  function buildSearch(catalog, inputId, targetId) {
    const find = id => { const result = catalog.macros.find(item => item.id === id); if (!result) throw Error('Unknown template: ' + id); return result; };
    const input = find(inputId), target = find(targetId);
    if (!['Inputs','Macromacroblocks'].includes(input.category) || target.category !== 'Targets') throw Error('Choose an input pass and a target.');
    const horizon = inputId === 'sky' ? 10510 : 6000;
    const targetSource = macroSource(target, 'Target / ');
    const maximize = ['speed-target','direction-target','stunt-target'].includes(targetId);
    const publications = [];
    walk(targetSource, node => {
      if (node.type === type('data/local') && node.fields.name === 'Target / first ms') node.inputs.value = {block: num(0)};
      if (node.type === type('data/local') && node.fields.name === 'Target / last ms') node.inputs.value = {block: num(horizon)};
      if (node.type === type('flow/forever')) {
        const check = branch(block('conditions/not', {value: call('Accept state', true)}),
          [block('results/clear'), block('procedures/return', {value: flag(false)})]);
        check.next = {block: node.inputs.body.block}; node.inputs.body.block = check;
      }
      // Only publish an improvement across candidates, not just within one.
      if (node.type === type('results/publish-snapshot')) publications.push(node);
    });
    for (const node of publications) {
        const publication = clone(node); delete publication.next;
        const score = clone(node.inputs.score.block);
        node.type = type('flow/if'); node.fields = {};
        node.inputs = {condition: {block: binary('conditions/or', block('conditions/not', {value: block('results/has-result')}),
          binary(maximize ? 'conditions/greater' : 'conditions/less', score, block('results/best-score')))}, body: {block: publication}};
    }
    const item = (list, index) => block('data/item', {list, index: num(index)});
    const candidateScore = () => item(get('candidate result'), 1);
    const candidateSnapshot = () => item(get('candidate result'), 2);
    const improves = () => binary('conditions/or', block('conditions/not', {value: block('results/has-result')}),
      binary('conditions/or', binary('conditions/or',
        binary('conditions/and', get('maximize score'), binary('conditions/greater', candidateScore(), block('results/best-score'))),
        binary('conditions/and', block('conditions/not', {value: get('maximize score')}), binary('conditions/less', candidateScore(), block('results/best-score')))),
        binary('conditions/and', binary('conditions/equal', candidateScore(), block('results/best-score')),
          binary('conditions/less', block('inputs/count', {inputs: block('simulation/snapshot-inputs', {snapshot: candidateSnapshot()})}),
            block('inputs/count', {inputs: block('simulation/snapshot-inputs', {snapshot: block('results/snapshot')})})))));
    const consume = block('flow/for-each', {list: get('candidate results'), body: chain([
      block('results/count'),
      branch(block('data/has-value', {value: get('candidate result')}), [branch(improves(),
        [block('results/publish-snapshot', {snapshot: candidateSnapshot(), score: candidateScore()})])])
    ])}, {name: 'candidate result'});
    const iteration = [block('simulation/restore', {snapshot: get('origin')}),
      block('simulation/replace-inputs', {inputs: get('baseline inputs')}),
      set('jobs this batch', get('batch size')),
      branch(binary('conditions/equal', get('jobs this batch'), num(0)), [set('jobs this batch', block('runtime/batch-size'))]),
      branch(block('conditions/not', {value: get('run until stopped')}),
        [set('jobs this batch', binary('math/min', get('jobs this batch'), binary('math/subtract', get('iterations'), block('results/iterations'))))]),
      set('candidate results', block('procedures/map', {
        function: reference('Try candidate'),
        list: block('data/numbers', {from: binary('math/add', block('results/iterations'), num(1)),
          to: binary('math/add', block('results/iterations'), get('jobs this batch')), step: num(1)}),
        workers: block('runtime/workers')})), consume,
      branch(binary('conditions/and', get('promote best'), block('results/has-result')),
        [set('baseline inputs', block('simulation/snapshot-inputs', {snapshot: block('results/snapshot')}))])];
    const search = [set('run until stopped', flag(false), true), set('iterations', num(1000), true), set('simulation end ms', num(horizon), true),
      set('seed', num(1), true), set('promote best', flag(true), true), set('batch size', num(0), true),
      set('maximize score', flag(maximize), true), set('skip unchanged inputs', flag(true), true),
      block('simulation/set-horizon', {time: get('simulation end ms')}), block('math/seed', {value: get('seed')}),
      set('origin', block('simulation/snapshot'), true), set('baseline inputs', block('simulation/inputs'), true), block('results/clear'),
      call('Evaluate candidate'),
      block('flow/while', {condition: binary('conditions/or', get('run until stopped'),
        binary('conditions/less', block('results/iterations'), get('iterations'))), body: chain(iteration)})];
    const scripts = [block('flow/when-start', {body: section('Search', chain(search))}),
      procedure('Mutate inputs', [macroSource(input, 'Inputs / ', true)]),
      procedure('Accept state', [block('procedures/return', {value: flag(true)})]),
      procedure('Evaluate candidate', [targetSource]),
      block('procedures/define', {body: chain([
        block('results/add-count', {amount: get('candidate index')}), call('Mutate inputs'),
        branch(binary('conditions/and', get('skip unchanged inputs'),
          binary('conditions/equal', block('simulation/inputs'), get('baseline inputs'))),
          [block('procedures/return', {value: block('values/none')})]),
        call('Evaluate candidate'),
        branch(block('results/has-result'), [block('procedures/return', {value: block('data/append', {
          list: block('data/append', {list: block('data/list'), value: block('results/best-score')}),
          value: block('results/snapshot')})})]),
        block('procedures/return', {value: block('values/none')})
      ])}, {name: 'Try candidate', parameters: 'candidate index'})];
    scripts.forEach((script, index) => { script.x = 30 + index * 380; script.y = 35; });
    return {blocks: {languageVersion: 0, blocks: scripts}};
  }

  function attach({workspace, bridge, catalog, changed, replace, showBlocks}) {
    const panel = document.getElementById('configurationPanel');
    const cards = document.getElementById('configurationCards');
    const definitions = new Map(catalog.blocks.map(item => [item.type, item]));
    const isSection = node => node?.type === type('flow/section');
    const element = (tag, text, className) => { const e = document.createElement(tag); if (text) e.textContent = text; if (className) e.className = className; return e; };
    const button = (text, action, title = text) => {
      const e = element('button', text); e.type = 'button'; e.title = title;
      e.addEventListener('click', action); return e;
    };
    function transaction(action) {
      if (!bridge.editable) return;
      const group = Blockly.Events.getGroup(); Blockly.Events.setGroup(true);
      try { action(); } finally { Blockly.Events.setGroup(group); }
      changed(); render();
    }
    function reveal(node) {
      showBlocks();
      for (let parent = node; parent; parent = parent.getParent()) parent.setCollapsed(false);
      workspace.centerOnBlock(node.id); node.select();
    }
    function literals(node, label = '') {
      if (!node) return [];
      const definition = definitions.get(node.type);
      if (!definition) return [];
      if ((definition.id.startsWith('values/') || definition.id === 'inputs/action-name' || definition.id === 'targets/plane') && definition.fields.length) {
        return definition.fields.map(spec => ({node, spec, label: label + (definition.fields.length > 1 ? ' ' + spec.key : '')}));
      }
      // These are data constructors, not calculations. Any computed component
      // makes the initializer an expression and keeps it out of the form.
      if (!['targets/point','targets/size','targets/rotation','targets/direction','targets/box','targets/prism'].includes(definition.id)) return [];
      const groups = definition.inputs.map(input => literals(node.getInputTargetBlock(input.key), (label ? label + ' · ' : '') + input.key));
      return groups.every(items => items.length) ? groups.flat() : [];
    }
    const labelFor = node => (node.getFieldValue('name') || 'Value').split(' / ').pop();
    function setting(owner, binding) {
      const row = element('label', '', 'settingRow');
      const originalName = labelFor(owner);
      const labels = {'first ms':'Minimum time (ms)', 'last ms':'Maximum time (ms)', 'absolute steering':'Steering perturbation',
        'steering is offset':'Steering value mode', 'minimum edits':'Minimum event count', 'maximum edits':'Maximum event count'};
      const name = (labels[originalName] || originalName.replace(/^./, c => c.toUpperCase())) + (binding.label ? ' · ' + binding.label : '');
      row.append(element('span', name));
      let input;
      const field = binding.node.getField(binding.spec.key);
      const mode = originalName === 'absolute steering' ? [['Delta','FALSE'],['Absolute','TRUE']]
        : originalName === 'steering is offset' ? [['Absolute','FALSE'],['Offset','TRUE']] : null;
      const editable = bridge.editable && !owner.getInheritedDisabled() && owner.isEnabled();
      if (binding.spec.kind === 'enum' || mode) {
        input = element('div', '', 'choiceButtons');
        for (const [label, value] of mode || binding.spec.choices || []) {
          const choice = button(label, () => transaction(() => field.setValue(value)));
          choice.setAttribute('aria-pressed', String(field.getValue() === value));
          choice.disabled = !editable; input.append(choice);
        }
      } else {
        input = element('input');
        const boolean = binding.spec.kind === 'boolean';
        const numeric = ['number','integer'].includes(binding.spec.kind);
        input.type = boolean ? 'checkbox' : 'text';
        if (boolean) input.checked = field.getValue() === 'TRUE'; else input.value = field.getValue();
        if (numeric) input.inputMode = 'decimal';
        if (numeric) input.required = true;
        input.disabled = !editable;
        input.setAttribute('aria-label', name);
        input.dataset.source = binding.node.id; input.dataset.field = binding.spec.key; input.dataset.owner = owner.id;
        const validate = () => {
          const value = Number(input.value);
          const valid = !numeric || (input.value.trim() !== '' && Number.isFinite(value) &&
            (binding.spec.kind !== 'integer' || Number.isInteger(value)));
          input.setCustomValidity(valid ? '' : binding.spec.kind === 'integer' ? 'Enter a whole number.' : 'Enter a finite number.');
          return valid;
        };
        input.addEventListener('input', () => {
          validate();
          const disabled = Boolean(bridge.running) || Boolean(panel.querySelector('input:invalid'));
          document.getElementById('runProgram').disabled = disabled;
          document.getElementById('debugProgram').disabled = disabled;
        });
        input.addEventListener('change', () => {
          if (!validate()) { input.reportValidity(); return; }
          transaction(() => field.setValue(boolean ? (input.checked ? 'TRUE' : 'FALSE') : input.value));
        });
        input.addEventListener('blur', () => setTimeout(render, 0));
      }
      row.append(input);
      const link = button('↗', event => { event.preventDefault(); reveal(owner); }, 'Show this setting in Blocks');
      link.className = 'sourceLink'; row.append(link); return row;
    }
    function reorder(node, direction) {
      const previous = node.getPreviousBlock(), next = node.getNextBlock();
      const first = direction < 0 ? previous : node, second = direction < 0 ? node : next;
      if (!first || !second || !isSection(first) || !isSection(second)) return;
      transaction(() => {
        const connection = first.previousConnection.targetConnection;
        const after = second.getNextBlock();
        second.unplug(false); first.unplug(false);
        if (after) after.unplug(false);
        if (connection) connection.connect(second.previousConnection);
        second.nextConnection.connect(first.previousConnection);
        if (after) first.nextConnection.connect(after.previousConnection);
      });
    }
    function card(owner, savedOpen) {
      const detail = element('details', '', 'configurationCard');
      detail.dataset.block = owner.id; detail.open = savedOpen.get(owner.id) ?? true;
      const sourceTitle = isSection(owner) || owner.type === type('procedures/define') ? owner.getFieldValue('name') : 'Program settings';
      const title = ({'Mutate inputs':'Input passes', 'Evaluate candidate':'Target', 'Accept state':'Conditions'})[sourceTitle] || sourceTitle;
      const summary = element('summary', title); detail.append(summary);
      const actions = element('div', '', 'cardActions');
      if (isSection(owner)) {
        const enabled = element('label', 'Enabled '), check = element('input'); check.type = 'checkbox';
        check.checked = owner.isEnabled(); check.disabled = !bridge.editable;
        check.addEventListener('change', () => transaction(() => owner.setDisabledReason(!check.checked, 'MANUALLY_DISABLED')));
        enabled.append(check); actions.append(enabled);
        for (const [text, action, tooltip] of [['↑', () => reorder(owner, -1), 'Move earlier'], ['↓', () => reorder(owner, 1), 'Move later'],
          ['Remove', () => transaction(() => owner.dispose(true)), 'Remove this section']]) {
          const control = button(text, action, tooltip);
          control.disabled = !bridge.editable || (text === '↑' && !isSection(owner.getPreviousBlock())) || (text === '↓' && !isSection(owner.getNextBlock()));
          actions.append(control);
        }
      }
      actions.append(button('Edit logic', () => reveal(owner)));
      if (isSection(owner) || owner.type === type('procedures/define')) actions.append(button(owner.isCollapsed() ? 'Expand blocks' : 'Collapse blocks',
        () => transaction(() => owner.setCollapsed(!owner.isCollapsed()))));
      detail.append(actions);
      const regular = element('div'), advanced = element('details', '', 'advancedSettings');
      advanced.append(element('summary', 'More source settings'));
      let count = 0, advancedCount = 0;
      const scan = (node, nested = false) => {
        for (let current = node; current; current = current.getNextBlock()) {
          if (isSection(current)) { detail.append(card(current, savedOpen)); continue; }
          if (current.type === type('data/local')) {
            const found = literals(current.getInputTargetBlock('value'));
            for (const binding of found) (nested ? advanced : regular).append(setting(current, binding));
            count += found.length; if (nested) advancedCount += found.length;
          }
          for (const socket of definitions.get(current.type)?.statements || [])
            scan(current.getInputTargetBlock(socket.key), true);
        }
      };
      scan(owner.getInputTargetBlock('body'));
      detail.append(regular); if (advancedCount) detail.append(advanced);
      if (!count && !detail.querySelector('.configurationCard')) detail.append(element('p', 'No fixed settings here. Edit logic to change expressions.', 'settingsHint'));
      return detail;
    }
    function render() {
      for (const e of panel.querySelectorAll('[data-write]')) e.disabled = !bridge.editable;
      if (cards.contains(document.activeElement) && document.activeElement.tagName === 'INPUT' && bridge.editable) return;
      // Keep an unfinished field visible after blur instead of silently
      // restoring its old source value. A replaced program may discard it.
      if (bridge.editable && [...cards.querySelectorAll('input:invalid')].some(input =>
        workspace.getBlockById(input.dataset.owner)?.getInputTargetBlock('value')?.getDescendants(false).some(node => node.id === input.dataset.source))) return;
      const scroll = panel.scrollTop;
      const savedOpen = new Map([...cards.querySelectorAll('details[data-block]')].map(item => [item.dataset.block, item.open]));
      cards.replaceChildren();
      for (const owner of workspace.getTopBlocks(true)) {
        if (!['ft_flow_when_start','ft_procedures_define','ft_flow_section'].includes(owner.type) && !owner.type.startsWith('ft_events_')) continue;
        const first = owner.getInputTargetBlock('body');
        if (owner.type === type('flow/when-start') && isSection(first) && !first.getNextBlock()) cards.append(card(first, savedOpen));
        else cards.append(card(owner, savedOpen));
      }
      panel.scrollTop = scroll;
    }
    const findDefinition = name => workspace.getTopBlocks(false).find(b => b.type === type('procedures/define') && b.getFieldValue('name') === name);
    function add(macro) {
      const definition = findDefinition(macro.category === 'Conditions' ? 'Accept state' : 'Mutate inputs');
      if (!definition) { document.getElementById('newSearch').click(); return; }
      transaction(() => {
        const used = workspace.getAllBlocks(false).map(b => b.getFieldValue('name') || '');
        let serial = 1, prefix;
        do { prefix = `${macro.label} ${serial++} / `; } while (used.some(name => name.startsWith(prefix)));
        const horizonBlock = workspace.getAllBlocks(false).find(b => b.type === type('data/local') && b.getFieldValue('name') === 'simulation end ms')?.getInputTargetBlock('value');
        const horizon = Number(horizonBlock?.getFieldValue('value')) || 6000;
        const source = macro.category === 'Conditions' ? sourceForCondition(macro, prefix, horizon) : macroSource(macro, prefix, true);
        const inserted = Blockly.serialization.blocks.append(source, workspace, {recordUndo: true});
        let tail = definition.getInputTargetBlock('body');
        if (macro.category === 'Conditions') {
          while (tail?.getNextBlock() && tail.getNextBlock().type !== type('procedures/return')) tail = tail.getNextBlock();
          const end = tail?.type === type('procedures/return') ? tail : tail?.getNextBlock();
          const connection = end?.previousConnection.targetConnection || tail?.nextConnection || definition.getInput('body').connection;
          if (end) end.unplug(false); connection.connect(inserted.previousConnection);
          if (end) inserted.nextConnection.connect(end.previousConnection);
        } else {
          while (tail?.getNextBlock()) tail = tail.getNextBlock();
          (tail?.nextConnection || definition.getInput('body').connection).connect(inserted.previousConnection);
        }
      });
    }
    function menu(container, label, items, action) {
      const details = element('details', '', 'popupMenu'); details.append(element('summary', label));
      const contents = element('div', '', 'menuContents');
      for (const item of items) {
        const choice = button(item.label, () => { details.open = false; if (bridge.editable) action(item); });
        choice.dataset.write = ''; choice.dataset.template = item.id;
        choice.disabled = !bridge.editable; contents.append(choice);
      }
      details.append(contents); container.append(details);
    }
    const actions = document.getElementById('configurationActions');
    menu(actions, 'Add input pass', catalog.macros.filter(m => ['Inputs','Macromacroblocks'].includes(m.category)), add);
    menu(actions, 'Add condition', catalog.macros.filter(m => m.category === 'Conditions'), add);
    menu(actions, 'Change target', catalog.macros.filter(m => m.category === 'Targets'), macro => {
      const current = findDefinition('Evaluate candidate');
      if (!current) { document.getElementById('newSearch').click(); return; }
      transaction(() => {
        const replacement = buildSearch(catalog, 'existing-events', macro.id).blocks.blocks
          .find(node => node.fields.name === 'Evaluate candidate').inputs.body.block;
        const maximize = workspace.getAllBlocks(false).find(b => b.type === type('data/local') && b.getFieldValue('name') === 'maximize score')?.getInputTargetBlock('value');
        if (maximize?.type === type('values/boolean')) maximize.setFieldValue(
          ['speed-target','direction-target','stunt-target'].includes(macro.id) ? 'TRUE' : 'FALSE', 'value');
        const horizon = workspace.getAllBlocks(false).find(b => b.type === type('data/local') && b.getFieldValue('name') === 'simulation end ms')?.getInputTargetBlock('value');
        if (horizon?.getField('value')) walk(replacement, node => {
          if (node.type === type('data/local') && node.fields.name === 'Target / last ms') node.inputs.value = {block: num(Number(horizon.getFieldValue('value')))};
        });
        current.getInputTargetBlock('body')?.dispose(false);
        const target = Blockly.serialization.blocks.append(replacement, workspace, {recordUndo: true});
        current.getInput('body').connection.connect(target.previousConnection);
      });
    });
    const dialog = document.getElementById('searchBuilder');
    let inputId = 'existing-events', targetId = 'speed-target';
    for (const [id, items, select] of [
      ['builderInputs', catalog.macros.filter(m => ['Inputs','Macromacroblocks'].includes(m.category)), id => { inputId = id; }],
      ['builderTargets', catalog.macros.filter(m => m.category === 'Targets'), id => { targetId = id; }]
    ]) {
      const parent = document.getElementById(id);
      for (const item of items) {
        const choice = button(item.label, () => {
          select(item.id); for (const sibling of parent.children) sibling.setAttribute('aria-pressed', String(sibling === choice));
        }, item.description);
        choice.dataset.template = item.id; choice.setAttribute('aria-pressed', String(item.id === inputId || item.id === targetId)); parent.append(choice);
      }
    }
    document.getElementById('newSearch').addEventListener('click', () => { if (bridge.editable) dialog.showModal(); });
    document.getElementById('cancelSearchBuilder').addEventListener('click', () => dialog.close());
    document.getElementById('createSearch').addEventListener('click', () => {
      if (!bridge.editable) return;
      const title = catalog.macros.find(m => m.id === inputId).label + ' · ' + catalog.macros.find(m => m.id === targetId).label;
      replace(buildSearch(catalog, inputId, targetId), title); dialog.close(); render();
    });
    return {render, reveal, literals};
  }
  return {attach, buildSearch, macroSource};
})();
