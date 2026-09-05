// These are editable visual programs, not native runtime implementations.
window.foreverBlockExample = name => {
  const block = (id, inputs = {}, fields = {}) => {
    const node = {type: 'ft_' + id.replace(/[^a-z0-9]/g, '_'), fields,
      inputs: Object.fromEntries(Object.entries(inputs).map(([key, value]) => [key, {block: value}]))};
    if (['procedures/call', 'procedures/value', 'procedures/reference'].includes(id))
      node.extraState = {name: fields.name, parameters: fields.parameters || ''};
    return node;
  };
  const chain = blocks => {
    for (let i = 0; i + 1 < blocks.length; ++i) blocks[i].next = {block: blocks[i + 1]};
    return blocks[0];
  };
  const stack = (id, inputs, body, fields = {}) => block(id, {...inputs, body: chain(body)}, fields);
  const num = value => block('values/number', {}, {value});
  const get = name => block('data/get', {}, {name});
  const set = (name, value) => block('data/set', {value}, {name});
  const binary = (id, a, b) => block(id, {a, b});
  const input = (action, value) => block('simulation/set-input', {
    time: block('simulation/time'), action: block('inputs/action-name', {}, {value: action}), value
  });
  const step = ticks => ticks === 1 ? block('simulation/step') : stack('flow/repeat', {count: num(ticks)}, [block('simulation/step')]);
  const start = body => stack('flow/when-start', {}, body);
  const x = () => block('math/component', {value: block('simulation/car-position')}, {axis: 'x'});
  let scripts;
  if (name === 'feedback') {
    const goal = binary('math/add', x(), num(10));
    const steering = block('math/clamp', {
      value: binary('math/multiply', binary('math/subtract', get('target x'), x()), num(1000)),
      minimum: num(-65536), maximum: num(65536)
    });
    scripts = [start([
      set('target x', goal), input('accelerate', num(1)),
      stack('flow/repeat', {count: num(100)}, [
        block('procedures/call', {arg0: get('target x')}, {name: 'steer toward', parameters: 'target x'}), step(1)
      ]), block('results/publish', {score: block('simulation/car-speed')})
    ]), stack('procedures/define', {}, [input('steer', block('math/floor', {value: steering}))], {name: 'steer toward', parameters: 'target x'}),
    stack('events/on-checkpoint', {}, [set('last checkpoint', block('simulation/checkpoint-count'))])];
  } else if (name === 'branches') {
    const candidates = block('procedures/map', {
      function: block('procedures/reference', {}, {name: 'drive candidate'}),
      list: block('data/numbers', {from: num(-65536), to: num(65536), step: num(32768)})
    });
    const better = binary('conditions/or', block('conditions/not', {value: block('results/has-result')}),
      binary('conditions/greater', block('simulation/car-speed'), block('results/best-score')));
    scripts = [start([
      set('candidates', candidates), stack('flow/for-each', {list: get('candidates')}, [
        block('simulation/restore', {snapshot: get('candidate')}), block('results/count'),
        stack('flow/if', {condition: better}, [block('results/publish', {score: block('simulation/car-speed')})])
      ], {name: 'candidate'})
    ]), stack('procedures/define', {}, [
      input('accelerate', num(1)), input('steer', get('steering')), step(100),
      block('procedures/return', {value: block('simulation/snapshot')})
    ], {name: 'drive candidate', parameters: 'steering'})];
  } else if (name === 'empty') scripts = [block('flow/when-start')];
  else throw new Error('Unknown example program');
  scripts.forEach((script, i) => { script.x = 50 + i * 410; script.y = 50; });
  return {blocks: {languageVersion: 0, blocks: scripts}};
};
