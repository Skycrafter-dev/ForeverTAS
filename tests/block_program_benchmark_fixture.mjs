import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import {fileURLToPath} from 'node:url';

// Generate the current editable workbench source, with the native benchmark's
// window and target. No runtime optimization depends on these names or shapes.
const [catalogPath, horizonText, countText, batchText, structure = 'workbench', targetStartText = horizonText] = process.argv.slice(2);
const horizon = Number(horizonText), count = Number(countText);
const targetStart = Number(targetStartText);
const batch = batchText === undefined || batchText === 'auto' ? undefined : Number(batchText);
if (!catalogPath || !Number.isInteger(horizon) || horizon <= 1000 || horizon % 10 ||
    !Number.isSafeInteger(count) || count <= 0 ||
    (batch !== undefined && (!Number.isSafeInteger(batch) || batch < 1)) ||
    !Number.isInteger(targetStart) || targetStart < 0 || targetStart > horizon || targetStart % 10) {
  throw Error('usage: node block_program_benchmark_fixture.mjs catalog.json horizon-ms candidates [batch-size]');
}
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const context = {window: {}};
vm.runInNewContext(fs.readFileSync(path.join(root, 'assets/blockly/workbench.js'), 'utf8'), context);
const catalog = JSON.parse(fs.readFileSync(catalogPath, 'utf8'));
const workspace = context.window.ForeverWorkbench.buildSearch(catalog, 'existing-events', 'speed-target');
const settings = new Map([
  ['iterations', count], ['simulation end ms', horizon], ['promote best', false],
  ['Inputs / first ms', 1000], ['Inputs / last ms', horizon - 10],
  ['Target / first ms', targetStart], ['Target / last ms', horizon]
]);
if (batch !== undefined) settings.set('batch size', batch);
const found = new Set();
function visit(node) {
  if (!node) return;
  if (['ft_data_local', 'ft_data_set'].includes(node.type) && settings.has(node.fields.name)) {
    const value = settings.get(node.fields.name);
    node.inputs.value.block.fields.value = typeof value === 'boolean' ? (value ? 'TRUE' : 'FALSE') : value;
    found.add(node.fields.name);
  }
  for (const input of Object.values(node.inputs || {})) { visit(input.block); visit(input.shadow); }
  visit(node.next?.block);
}
workspace.blocks.blocks.forEach(visit);
for (const name of settings.keys()) if (!found.has(name)) throw Error('Missing benchmark setting: ' + name);

const block = (id, inputs = {}, fields = {}) => ({type: 'ft_' + id.replaceAll('/', '_').replaceAll('-', '_'), fields,
  inputs: Object.fromEntries(Object.entries(inputs).map(([key, value]) => [key, {block: value}]))});
const num = value => block('values/number', {}, {value});
const text = value => block('values/text', {}, {value});
const scripts = workspace.blocks.blocks;
const evaluation = scripts.find(node => node.fields.name === 'Evaluate candidate');
const reference = name => ({...block('procedures/reference', {}, {name}), extraState: {name}});
function transform(node, edit) {
  if (!node) return;
  for (const input of Object.values(node.inputs || {})) { transform(input.block, edit); transform(input.shadow, edit); }
  transform(node.next?.block, edit);
  edit(node);
}
function appendEvaluation(statement) {
  let tail = evaluation?.inputs?.body?.block;
  if (!tail) throw Error('Missing mapped evaluation body');
  while (tail.next?.block) tail = tail.next.block;
  tail.next = {block: statement};
}
const entryStructure = structure === 'mapped-restart-history' ? 'mapped-restart'
  : structure === 'mapped-restore-history' ? 'mapped-restore' : structure;
if (['mapped-restore', 'mapped-horizon', 'mapped-horizon-events', 'mapped-history', 'mapped-restart'].includes(entryStructure)) {
  const candidate = scripts.find(node => node.fields.name === 'Try candidate');
  if (!candidate?.inputs?.body?.block) throw Error('Missing mapped benchmark script');
  const operation = entryStructure === 'mapped-restart' ? block('simulation/restart') : entryStructure === 'mapped-history'
    ? block('data/local', {value: block('simulation/history')}, {name: 'observed prefix'})
    : entryStructure === 'mapped-restore'
    ? block('simulation/restore', {snapshot: block('data/get', {}, {name: 'origin'})})
    : block('simulation/set-horizon', {time: entryStructure === 'mapped-horizon' ? block('simulation/horizon')
      : block('math/add', {a: block('simulation/horizon'), b: block('math/multiply', {a: num(10),
        b: block('math/add', {a: num(1), b: block('math/modulo', {
          a: block('data/get', {}, {name: 'candidate index'}), b: num(5)})})})})});
  operation.next = {block: candidate.inputs.body.block};
  candidate.inputs.body.block = operation;
  if (structure === 'mapped-horizon-events') {
    scripts.push(block('events/on-tick', {body: block('flow/if', {
      condition: block('conditions/greater', {a: block('simulation/horizon'),
        b: block('data/get', {}, {name: 'simulation end ms'})}),
      body: block('simulation/set-horizon', {time: block('data/get', {}, {name: 'simulation end ms'})})})}));
  }
  if (structure === 'mapped-restart-history' || structure === 'mapped-restore-history')
    appendEvaluation(block('data/local', {value: block('data/length', {list: block('simulation/history')})}, {name: 'sample count'}));
} else if (['nested-map-tail', 'nested-map-tick-events', 'nested-map-physics-tail'].includes(structure)) {
  const name = 'Return nested item';
  let body = block('procedures/return', {value: block('data/get', {}, {name: 'nested item'})});
  if (structure === 'nested-map-physics-tail') {
    const statements = [block('simulation/restart'), block('simulation/step'),
      block('data/set', {value: num(91)}, {name: 'nested private value'}),
      block('results/count'), block('results/publish', {score: num(1)}), body];
    for (let i = 0; i + 1 < statements.length; ++i) statements[i].next = {block: statements[i + 1]};
    body = statements[0];
  }
  scripts.push(block('procedures/define', {body}, {name, parameters: 'nested item'}));
  const mapped = block('data/local', {value: block('procedures/map', {
    function: block('data/item', {list: block('data/append', {list: block('data/list'), value: reference(name)}), index: num(1)}),
    list: block('data/numbers', {from: num(1), to: num(3), step: num(1)}), workers: num(2)})}, {name: 'nested output'});
  if (structure === 'nested-map-tick-events') {
    const start = scripts.find(node => node.type === 'ft_flow_when_start');
    const initialize = block('data/set', {value: block('values/boolean', {}, {value: 'false'})}, {name: 'nested events enabled'});
    initialize.next = {block: start.inputs.body.block};
    start.inputs.body.block = initialize;
    const enable = block('data/set', {value: block('values/boolean', {}, {value: 'true'})}, {name: 'nested events enabled'});
    const candidate = scripts.find(node => node.fields.name === 'Try candidate');
    enable.next = {block: candidate.inputs.body.block};
    candidate.inputs.body.block = enable;
    scripts.push(block('events/on-tick', {body: block('flow/if', {
      condition: block('data/get', {}, {name: 'nested events enabled'}), body: mapped})}));
  }
  else appendEvaluation(mapped);
} else if (structure === 'mapped-result-restore-history') {
  const observe = () => block('data/local', {value: block('data/length', {list: block('simulation/history')})}, {name: 'sample count'});
  appendEvaluation(observe());
  const candidate = scripts.find(node => node.fields.name === 'Try candidate');
  let inserted = 0;
  transform(candidate, node => {
    if (node.type !== 'ft_procedures_call' || node.fields.name !== 'Evaluate candidate') return;
    const restore = block('simulation/restore', {snapshot: block('results/snapshot')});
    restore.next = {block: observe()};
    const guarded = block('flow/if', {condition: block('results/has-result'), body: restore});
    guarded.next = node.next;
    node.next = {block: guarded};
    ++inserted;
  });
  if (inserted !== 1) throw Error('Missing candidate evaluation for result history restore');
} else if (structure === 'mapped-restart-indirect') {
  const name = 'Restart with edited inputs';
  scripts.push(block('procedures/define', {body: block('simulation/restart')}, {name, parameters: ''}));
  const candidate = scripts.find(node => node.fields.name === 'Try candidate');
  let inserted = 0;
  transform(candidate, node => {
    if (node.type !== 'ft_procedures_call' || node.fields.name !== 'Evaluate candidate') return;
    const original = {...node};
    Object.assign(node, block('procedures/do', {function: block('data/item', {
      list: block('data/append', {list: block('data/list'), value: reference(name)}), index: num(1)}), arguments: block('data/list')}));
    delete node.extraState;
    node.next = {block: original};
    ++inserted;
  });
  if (inserted !== 1) throw Error('Missing candidate evaluation for indirect restart');
} else if (['mapped-history-tail', 'mapped-history-events', 'mapped-history-indirect'].includes(structure)) {
  let sample = block('simulation/history');
  if (structure === 'mapped-history-indirect') {
    const name = 'Inspect sampled history';
    scripts.push(block('procedures/define', {body: block('procedures/return', {value: sample})}, {name, parameters: ''}));
    sample = block('procedures/apply', {function: block('data/item', {
      list: block('data/append', {list: block('data/list'), value: reference(name)}), index: num(1)}), arguments: block('data/list')});
  }
  const observation = block('data/local', {value: block('data/length', {list: sample})}, {name: 'sample count'});
  if (structure === 'mapped-history-events') {
    scripts.push(block('events/on-tick', {body: block('flow/if', {condition: block('conditions/equal', {
      a: block('math/modulo', {a: block('simulation/time'), b: num(2000)}), b: num(0)}), body: observation})}));
  } else {
    appendEvaluation(observation);
  }
} else if (structure === 'read-globals' || structure === 'shrinking-globals') {
  const start = scripts.find(node => node.type === 'ft_flow_when_start');
  const candidate = scripts.find(node => node.fields.name === 'Try candidate');
  if (!start?.inputs?.body?.block || !candidate?.inputs?.body?.block) throw Error('Missing mapped benchmark script');
  const payload = block('data/set', {value: block('data/numbers', {from: num(1), to: num(Math.max(70000, count)), step: num(1)})},
    {name: 'candidate lookup'});
  payload.next = {block: start.inputs.body.block};
  start.inputs.body.block = payload;
  const lookup = block('data/set', {value: block('data/item', {
    list: block('data/get', {}, {name: 'candidate lookup'}), index: block('data/get', {}, {name: 'candidate index'})
  })}, {name: 'candidate index'});
  lookup.next = {block: candidate.inputs.body.block};
  candidate.inputs.body.block = lookup;
  if (structure === 'shrinking-globals') {
    const read = block('data/set', {value: block('data/length', {
      list: block('data/get', {}, {name: 'candidate lookup'})})}, {name: 'prime value'});
    const step = block('simulation/step');
    read.next = {block: step};
    step.next = {block: block('procedures/return', {value: block('data/get', {}, {name: 'prime value'})})};
    scripts.push(block('procedures/define', {body: read}, {name: 'Prime imports', parameters: 'index'}));
    const prime = block('data/set', {value: block('procedures/map', {function: reference('Prime imports'),
      list: block('data/numbers', {from: num(1), to: num(64), step: num(1)}), workers: num(2)})}, {name: 'primed imports'});
    const shrink = block('data/set', {value: block('data/numbers', {from: num(1), to: num(count), step: num(1)})},
      {name: 'candidate lookup'});
    shrink.next = payload.next;
    payload.next = {block: prime};
    prime.next = {block: shrink};
  }
} else if (structure === 'shadowed-globals' || structure === 'overwritten-globals') {
  const start = scripts.find(node => node.type === 'ft_flow_when_start');
  if (!start?.inputs?.body?.block) throw Error('Missing benchmark start script');
  const name = structure === 'shadowed-globals' ? 'candidate index' : 'overwritten scratch';
  const shadow = block('data/set', {value: block('data/numbers', {from: num(1), to: num(70000), step: num(1)})}, {name});
  shadow.next = {block: start.inputs.body.block};
  start.inputs.body.block = shadow;
  if (structure === 'overwritten-globals') {
    const candidate = scripts.find(node => node.fields.name === 'Try candidate');
    if (!candidate?.inputs?.body?.block) throw Error('Missing mapped benchmark procedure');
    const reset = block('data/set', {value: num(0)}, {name});
    const change = block('data/change', {value: num(1)}, {name});
    reset.next = {block: change};
    change.next = {block: candidate.inputs.body.block};
    candidate.inputs.body.block = reset;
  }
} else if (structure === 'indirect' || structure === 'dynamic-globals') {
  scripts.forEach(script => transform(script, node => {
    if (!['ft_procedures_call', 'ft_procedures_value'].includes(node.type)) return;
    const reporter = node.type === 'ft_procedures_value';
    const fn = structure === 'dynamic-globals' && node.fields.name === 'Evaluate candidate'
      ? block('data/get', {}, {name: 'selected callback'})
      : block('data/item', {list: block('data/append', {list: block('data/list'), value: reference(node.fields.name)}), index: num(1)});
    node.type = reporter ? 'ft_procedures_apply' : 'ft_procedures_do';
    node.fields = {}; delete node.extraState;
    node.inputs = {function: {block: fn}, arguments: {block: block('data/list')}};
  }));
  if (structure === 'dynamic-globals') {
    scripts.push(block('procedures/define', {body: block('procedures/return', {
      value: block('data/length', {list: block('data/get', {}, {name: 'unrelated callback payload'})})})},
    {name: 'Unrelated callback', parameters: ''}));
    const start = scripts.find(node => node.type === 'ft_flow_when_start');
    if (!start?.inputs?.body?.block) throw Error('Missing benchmark start script');
    const payload = block('data/set', {value: block('data/numbers', {from: num(1), to: num(70000), step: num(1)})},
      {name: 'unrelated callback payload'});
    const selected = block('data/set', {value: reference('Evaluate candidate')}, {name: 'selected callback'});
    payload.next = {block: selected};
    selected.next = {block: start.inputs.body.block};
    start.inputs.body.block = payload;
  }
} else if (structure === 'procedure-chain') {
  const name = index => 'Read current speed ' + index;
  const call = index => ({...block('procedures/value', {}, {name: name(index), parameters: ''}),
    extraState: {name: name(index), parameters: ''}});
  let leaf;
  scripts.forEach(script => transform(script, node => {
    if (node.type !== 'ft_simulation_read' || node.fields.property !== 'speed' ||
        node.inputs.state.block.type !== 'ft_simulation_state') return;
    leaf = structuredClone(node);
    Object.assign(node, call(0));
  }));
  if (!leaf) throw Error('Missing current-speed expression for procedure chain');
  for (let index = 0; index < 8; ++index)
    scripts.push(block('procedures/define', {body: block('procedures/return', {
      value: index === 7 ? leaf : call(index + 1)})}, {name: name(index), parameters: ''}));
} else if (structure === 'broadcast') {
  scripts.push(block('events/on-message', {body: evaluation.inputs.body.block}, {name: 'evaluate'}));
  evaluation.inputs.body.block = block('events/send', {message: text('evaluate'), value: block('simulation/time')});
} else if (structure === 'checkpoint-events' || structure === 'conditional-events') {
  const body = block('data/local', {value: block('events/state')}, {name: 'observed event state'});
  if (structure === 'checkpoint-events') {
    scripts.push(block('events/on-checkpoint', {body}));
    scripts.push(block('events/on-finish', {body: structuredClone(body)}));
  } else {
    scripts.push(block('events/when', {body, condition: block('conditions/greater', {
      a: block('simulation/car-speed'), b: num(25)})}));
  }
} else if (structure === 'tick-events' || structure === 'loop-chunks') {
  const beforeEnd = () => block('conditions/less', {a: block('simulation/time'), b: num(horizon)});
  const publication = (event = false) => {
    const state = () => block(event ? 'events/state' : 'simulation/state');
    const speed = () => block('simulation/read', {state: state()}, {property: 'speed'});
    return block('flow/if', {condition: block('conditions/and', {
      a: block('conditions/greater-equal', {a: block('simulation/read', {state: state()}, {property: 'time'}), b: num(targetStart)}),
      b: block('conditions/or', {a: block('conditions/not', {value: block('results/has-result')}),
        b: block('conditions/greater', {a: speed(), b: block('results/best-score')})})}),
    body: block('results/publish', {score: speed()})});
  };
  let step = block('simulation/step');
  if (structure === 'loop-chunks') {
    if (targetStart < horizon) step.next = {block: publication()};
    step = block('flow/repeat', {count: block('math/min', {a: num(17), b: block('math/divide', {
      a: block('math/subtract', {a: num(horizon), b: block('simulation/time')}), b: block('simulation/tick-duration')})}), body: step});
  }
  const loop = block('flow/while', {condition: beforeEnd(), body: step});
  if (structure === 'tick-events') {
    scripts.push(block('events/on-tick', {body: publication(true)}));
  } else if (targetStart === horizon) loop.next = {block: publication()};
  if (targetStart === 0) {
    const initial = publication(); initial.next = {block: loop}; evaluation.inputs.body.block = initial;
  } else evaluation.inputs.body.block = loop;
} else if (structure !== 'workbench') throw Error('Unknown benchmark structure: ' + structure);
process.stdout.write(JSON.stringify(workspace) + '\n');
