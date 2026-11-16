// Groups on a relay (node group_check.mjs [BASE], default the live one; `wrangler dev` gives http://127.0.0.1:8787):
// membership and invites, the events of a search (job, speed, best, overall best, stop, final count), a machine
// moved to a second search, a window back within its grace, the public pool's caps, credit.
import WebSocket from "ws";

const base = new URL(process.argv[2] ?? "https://forevertas.tmtas.exchange");
const http = (path) => new URL(path, base).toString();
const ws = (path) => { const url = new URL(path, base); url.protocol = base.protocol === "http:" ? "ws:" : "wss:"; return url.toString(); };
const post = async (path, body) => { const r = await fetch(http(path), { method: "POST", body: JSON.stringify(body) }); return { status: r.status, body: await r.json() }; };
const secret = () => crypto.randomUUID().replaceAll("-", "");
let failures = 0;
const expect = (what, ok) => { console.log(`${ok ? "ok  " : "FAIL"} ${what}`); if (!ok) ++failures; };

// a socket whose messages queue up, read in order (next: the next one; until: the next one that matches)
function open(path) {
  // (wrangler dev: each connection its own address; Cloudflare refuses the header from outside)
  const socket = new WebSocket(ws(path), base.protocol === "http:"
    ? { headers: { "CF-Connecting-IP": `10.0.${Math.floor(Math.random() * 250)}.${Math.floor(Math.random() * 250)}` } } : {});
  const queue = [], waiting = [];
  socket.on("message", (data) => { const text = data.toString(); if (text === "pong") return; const value = JSON.parse(text);
    waiting.length ? waiting.shift()(value) : queue.push(value); });
  socket.closed = new Promise((resolve) => socket.on("close", (code, reason) => resolve(`${code} ${reason}`)));
  socket.on("error", () => {});
  socket.next = (ms = 5000) => queue.length ? Promise.resolve(queue.shift())
    : new Promise((resolve) => {
      waiting.push(resolve);
      setTimeout(() => { const at = waiting.indexOf(resolve); if (at >= 0) { waiting.splice(at, 1); resolve(null); } }, ms);
    });
  socket.until = async (test, ms = 8000) => {
    const end = Date.now() + ms;
    for (;;) { const left = end - Date.now(); if (left <= 0) return null; const m = await socket.next(left); if (!m || test(m)) return m; }
  };
  socket.quiet = async (ms = 1000) => (await socket.next(ms)) === null;
  socket.ready = new Promise((resolve, reject) => { socket.on("open", resolve); socket.on("error", reject); });
  socket.json = (value) => socket.send(JSON.stringify(value));
  return socket;
}

const a = { member: secret(), secret: secret(), memberName: "Alice" };
const b = { member: secret(), secret: secret(), memberName: "Bob" };
const created = await post("/groups", { name: "Route 43", ...a });
expect(`a group is created (${created.status})`, created.status === 200 && /^[A-Z2-9]{8}-[0-9A-F]{12}$/.test(created.body.invite));
const id = created.body.id;
expect("a wrong invite is refused", (await post(`/groups/${id}/join`, { invite: `${id}-000000000000`, ...b })).status === 403);
expect("the invite lets Bob in", (await post(`/groups/${id}/join`, { invite: created.body.invite, ...b })).status === 200);
const q = (m) => `member=${m.member}&secret=${m.secret}`;
expect("a stranger cannot see the group", (await fetch(http(`/groups/${id}/status?member=x&secret=${secret()}`))).status === 403);

const hello = (version, lends = {}) => ({ type: "hello", version, packs: { "Stadium.pak": "aa" },
  gpus: [{ backend: "cuda", name: "T4" }], cpu: { name: "x", threads: 2 }, ...lends });
const machine = async (who, helloMessage) => { const m = open(`/groups/${id}/worker?${q(who)}`); await m.ready; m.json(helloMessage); return m; };
const job = { version: "9.9", packs: { "Stadium.pak": "aa", "Island.pak": "bb" }, request: { anything: 1 } };

const w1 = await machine(b, hello("9.9"));
const window1 = open(`/groups/${id}/client?${q(a)}`);
await window1.ready;
window1.json({ type: "search", id: "a".repeat(32), job });
expect("the search starts", (await window1.until((m) => m.event === "searching"))?.search === "a".repeat(32));
const job1 = await w1.next();
expect(`Bob's machine gets the job, its range and waiting period (${job1 && `slot ${job1.slot}, ${job1.cooldownMs} ms`})`,
  job1?.type === "job" && job1.job?.request?.anything === 1 && job1.slot === 0 && job1.cooldownMs >= 1000);
expect("the window hears of one machine", (await window1.until((m) => m.event === "machines" && m.count === 1)) !== null);
w1.json({ type: "speed", search: job1.search, perSecond: 5000 });
expect("and of its speed", (await window1.until((m) => m.event === "machines"))?.perSecond === 5000);
w1.json({ type: "best", search: job1.search, live: { score: 3 } });
expect("a machine's best reaches the window", (await window1.until((m) => m.event === "batch" && m.bests))?.bests?.[0]?.live?.score === 3);
window1.json({ type: "best", inputs: [1, 2], sample: { score: 3 } });
expect("the overall best goes down to the machine", (await w1.next())?.best?.sample?.score === 3);

const w2 = await machine(b, hello("9.9"));
const job2 = await w2.next();
expect("a second machine: another range, the overall best with its job", job2?.type === "job" && job2.slot === 1 && job2.best?.sample?.score === 3);
const wrongVersion = await machine(b, hello("1.0"));
const nothing = await machine(b, hello("9.9", { gpus: [], cpu: { threads: 0 } }));
expect("machines of another version, or lending nothing, get no job", (await wrongVersion.quiet()) && (await nothing.quiet(200)));

// a machine busy elsewhere (its room, another group) is given nothing; free again, it is
const w3 = await machine(b, hello("9.9"));
w3.json({ type: "busy" });
const w3job = await w3.next(1500);
if (w3job?.type === "job") w3.json({ type: "final", search: w3job.search, iterations: 0, seconds: 0 });
expect("a machine busy elsewhere is given no job (or hands it back at once)", (await w3.quiet(1500)));
w3.close();

// a second search: one machine of the first stops, hands in its count and is given to it
const window2 = open(`/groups/${id}/client?${q(b)}`);
await window2.ready;
window2.json({ type: "search", id: "b".repeat(32), job: { ...job, packs: { "Stadium.pak": "aa" } } });
const asked = await Promise.race([w1.next(4000).then((m) => m && { m, s: w1 }), w2.next(4000).then((m) => m && { m, s: w2 })]);
expect(`a machine of Alice's search is stopped to move (${JSON.stringify(asked?.m)})`, asked?.m?.type === "stop" && asked.m.moved);
asked.s.json({ type: "final", search: asked.m.search, iterations: 1000, seconds: 2 });
expect("Alice's window gets its final count", (await window1.until((m) => m.event === "batch" && m.finals))?.finals?.[0]?.iterations === 1000);
expect("the machine gets Bob's search", (await asked.s.next())?.search === "b".repeat(32));
expect("each window hears of one machine", (await window1.until((m) => m.event === "machines" && m.count === 1)) !== null &&
  (await window2.until((m) => m.event === "machines" && m.count === 1)) !== null);

// Alice's window goes and comes back within its grace: the search carries on
window1.close();
await new Promise((r) => setTimeout(r, 1500));
const back = open(`/groups/${id}/client?${q(a)}`);
await back.ready;
back.json({ type: "search", id: "a".repeat(32), resume: true });
const resumed = await back.until((m) => m.event === "resumed" || m.event === "gone");
expect(`a window back within its grace carries on (${resumed?.event})`, resumed?.event === "resumed" && resumed.best?.sample?.score === 3);

// Bob stops: his machine hands in its count, the search ends, the machine goes back to Alice's
window2.json({ type: "stop" });
const stop = await asked.s.next();
expect("stop reaches the machine", stop?.type === "stop" && !stop.moved);
asked.s.json({ type: "final", search: stop.search, iterations: 500, seconds: 1 });
expect("Bob's search ends", (await window2.until((m) => m.event === "ended")) !== null);
expect("the machine is Alice's again", (await asked.s.next())?.search === "a".repeat(32));

// the public pool: two searches each at most
const carol = { member: secret(), secret: secret(), memberName: "Carol" };
expect("the public pool takes anyone", (await post("/groups/public/join", carol)).status === 200);
const answers = [], pools = [];
for (const search of ["c", "d", "e"].map((c) => c.repeat(32))) {
  const p = open(`/groups/public/client?member=${carol.member}&secret=${carol.secret}`);
  await p.ready;
  pools.push(p);
  p.json({ type: "search", id: search, job });
  answers.push(await p.until((m) => m.event === "searching" || m.event === "error"));
}
expect(`a third search in the public pool is refused (${answers.map((m) => m?.event).join(", ")})`,
  answers[0]?.event === "searching" && answers[1]?.event === "searching" && answers[2]?.event === "error");
for (const p of pools) p.json({ type: "stop" });

const status = await (await fetch(http(`/groups/${id}/status?${q(a)}`))).json();
const bob = status.credits.find((credit) => credit.name === "Bob");
expect(`status: ${status.machines} machines, ${status.searches} search, ${status.shards} shard, Bob ${bob?.attempts} attempts`,
  status.machines === 4 && status.searches === 1 && bob?.attempts === 1500 && status.owner === true);
expect("only the owner makes invites", (await post(`/groups/${id}/invite`, { member: b.member, secret: b.secret })).status === 403);
const renewed = await post(`/groups/${id}/invite`, { member: a.member, secret: a.secret });
const dave = { member: secret(), secret: secret(), memberName: "Dave" };
expect("the old invite stops working", renewed.status === 200 &&
  (await post(`/groups/${id}/join`, { invite: created.body.invite, ...dave })).status === 403);
for (const socket of [w1, w2, wrongVersion, nothing, back, window2, ...pools]) socket.close();
console.log(failures === 0 ? "groups work" : `${failures} failed`);
process.exit(failures === 0 ? 0 : 1);
