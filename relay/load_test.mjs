// Groups under load (node load_test.mjs BASE [MACHINES] [SECONDS]): simulated machines join a group, a window
// starts a search, and every machine improves about 100 times a second at first (fewer as it goes on), sharing as
// a ForeverTAS machine does: only a best that beats the overall best it knows, at most one per waiting period (the
// latest at the period's end). Counts every message each second; at the end the best the window holds must be the
// best any machine found, and the relay's traffic must stay within the budget however many machines there are.
// For `wrangler dev` (one address): each connection claims its own CF-Connecting-IP.
import WebSocket from "ws";

const base = new URL(process.argv[2] ?? "http://127.0.0.1:8787");
const MACHINES = Number(process.argv[3] ?? 1000);
const SECONDS = Number(process.argv[4] ?? 20);
const BUDGET = 20; // the relay's bests a second per search
const http = (path) => new URL(path, base).toString();
const ws = (path) => { const url = new URL(path, base); url.protocol = base.protocol === "http:" ? "ws:" : "wss:"; return url.toString(); };
const post = async (path, body) => (await fetch(http(path), { method: "POST", body: JSON.stringify(body) })).json();
const secret = () => crypto.randomUUID().replaceAll("-", "");
const fakeIp = () => `10.${Math.floor(Math.random() * 250)}.${Math.floor(Math.random() * 250)}.${Math.floor(Math.random() * 250)}`;
// (wrangler dev: each connection its own address; Cloudflare refuses the header from outside)
const addressed = () => (base.protocol === "http:" ? { headers: { "CF-Connecting-IP": fakeIp() } } : {});
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

let errors = 0;
const counts = { fromMachines: 0, bests: 0, toMachines: 0, toWindow: 0, fromWindow: 0 };
const perSecond = [];
const tick = () => perSecond.push({ ...counts });

const owner = { member: secret(), secret: secret(), memberName: "Owner" };
const group = await post("/groups", { name: "load", ...owner });
const lender = { member: secret(), secret: secret(), memberName: "Lender" };
await post(`/groups/${group.id}/join`, { invite: group.invite, ...lender });
const q = (m) => `member=${m.member}&secret=${m.secret}`;
const job = { version: "load", packs: { "Stadium.pak": "aa" }, request: { note: "simulated" } };

// a simulated machine
function machine(index) {
  const socket = new WebSocket(ws(`/groups/${group.id}/worker?${q(lender)}`), addressed());
  const self = { best: 0, sent: 0, overall: 0, search: null, cooldownMs: 1000, iterations: 0, timer: null, nextSend: 0, done: false };
  const send = (value) => { counts.fromMachines++; if (value.type === "best") counts.bests++; socket.send(JSON.stringify(value)); };
  // its search: improvements at ~100 a second, slowing down; a best goes out at the end of its waiting period
  const work = () => {
    const started = Date.now();
    self.nextSend = started + Math.random() * self.cooldownMs; // (a random offset per machine)
    let speedSent = false;
    self.timer = setInterval(() => {
      const t = (Date.now() - started) / 1000;
      self.iterations += 500;
      const improvements = Math.random() < Math.exp(-t / 4) ? 1 : 0; // about 100 a second at first
      if (improvements) self.best += Math.random();
      if (Date.now() >= self.nextSend) {
        self.nextSend += self.cooldownMs;
        // its speed goes with its first slot (spread as bests are)
        if (!speedSent) {
          speedSent = true;
          send({ type: "speed", search: self.search, perSecond: 50000 });
        }
        if (self.best > self.overall && self.best > self.sent) {
          self.sent = self.best;
          send({ type: "best", search: self.search, live: { score: self.best, machine: index } });
        }
      }
    }, 10);
  };
  socket.on("message", (data) => {
    const text = data.toString();
    if (text === "pong") return;
    counts.toMachines++;
    const message = JSON.parse(text);
    if (message.type === "job") {
      self.search = message.search;
      self.cooldownMs = message.cooldownMs;
      if (message.best) self.overall = message.best.sample.score;
      work();
    } else if (message.type === "overall") {
      self.overall = Math.max(self.overall, message.best.sample.score);
    } else if (message.type === "cooldown") {
      // a new waiting period: the next send is picked again within it
      self.cooldownMs = message.cooldownMs;
      self.nextSend = Date.now() + Math.random() * self.cooldownMs;
    } else if (message.type === "stop") {
      clearInterval(self.timer);
      send({ type: "final", search: self.search, iterations: self.iterations, seconds: 1,
             ...(self.best > self.overall ? { live: { score: self.best, machine: index } } : {}) });
      self.done = true;
    }
  });
  socket.on("open", () => send({ type: "hello", version: "load", packs: { "Stadium.pak": "aa" },
                                 gpus: [{ backend: "cuda", name: "simulated" }], cpu: { name: "x", threads: 0 } }));
  socket.on("error", (error) => { if (!errors++) console.log(`a machine could not connect: ${error.message}`); });
  return { socket, self };
}

console.log(`group ${group.id}: ${MACHINES} machines joining`);
const joining = Date.now();
const machines = [];
for (let i = 0; i < MACHINES; ++i) {
  machines.push(machine(i));
  if (i % 50 === 49) await sleep(50);
}
await Promise.all(machines.map((m) => new Promise((r) => (m.socket.readyState === 1 ? r() : (m.socket.on("open", r), m.socket.on("error", r))))));
if (errors) console.log(`${errors} of ${MACHINES} machines could not connect`);
const joined = Date.now();

// the window: takes every best that beats its overall best (as its check would), and sends it down
const window = new WebSocket(ws(`/groups/${group.id}/client?${q(owner)}`), addressed());
let overall = 0, machinesSeen = 0, finals = 0, ended = false;
window.on("message", (data) => {
  counts.toWindow++;
  const message = JSON.parse(data.toString());
  if (message.event === "machines") machinesSeen = Math.max(machinesSeen, message.count);
  if (message.event === "ended") ended = true;
  if (message.event !== "batch") return;
  finals += (message.finals ?? []).length;
  // the window checks only the best of what came (one check), and sends it down if it beats its overall best
  const candidates = [...(message.bests ?? []), ...(message.finals ?? [])].map((item) => item.live).filter(Boolean);
  const top = candidates.reduce((best, live) => (!best || live.score > best.score ? live : best), null);
  if (top && top.score > overall) {
    overall = top.score;
    counts.fromWindow++;
    window.send(JSON.stringify({ type: "best", inputs: [], sample: { score: overall } }));
  }
});
await new Promise((r) => window.on("open", r));
window.send(JSON.stringify({ type: "search", id: secret() + secret().slice(0, 8), job }));
counts.fromWindow++;
const before = { ...counts };
const clock = setInterval(tick, 1000);
await sleep(SECONDS * 1000);
window.send(JSON.stringify({ type: "stop" }));
const stopAt = Date.now();
while (!ended && Date.now() - stopAt < 60000) await sleep(100);
clearInterval(clock);
tick();

const truth = Math.max(...machines.map((m) => m.self.best));
const rows = perSecond.map((row, i) => {
  const previous = i === 0 ? before : perSecond[i - 1];
  return Object.fromEntries(Object.keys(row).map((key) => [key, row[key] - previous[key]]));
});
console.log(`joined in ${((joined - joining) / 1000).toFixed(1)} s; machines in the search: ${machinesSeen}`);
console.log("second  from machines  (bests)  to machines  to window  from window");
rows.forEach((row, i) => console.log(`${String(i + 1).padStart(6)}  ${String(row.fromMachines).padStart(13)}  ${String(row.bests).padStart(7)}  ${String(row.toMachines).padStart(11)}  ${String(row.toWindow).padStart(9)}  ${String(row.fromWindow).padStart(11)}`));
const searching = rows.slice(0, SECONDS); // (before the stop)
const peakBests = Math.max(...searching.map((row) => row.bests));
const peakWindow = Math.max(...searching.map((row) => row.toWindow));
const peakMachines = Math.max(...searching.map((row) => row.fromMachines));
console.log(`machines: ${MACHINES}; finals ${finals}; ended ${ended}; while searching, a second at most: ${peakBests} bests ` +
            `(budget ${BUDGET}), ${peakMachines} messages from machines, ${peakWindow} to the window`);
console.log(`best found by any machine ${truth.toFixed(6)}, the window's ${overall.toFixed(6)}: ${truth === overall ? "the same" : "DIFFERENT"}`);
const ok = ended && truth === overall && peakBests <= BUDGET * 2;
console.log(ok ? "the relay stays within its budget and nothing is lost" : "FAILED");
for (const m of machines) m.socket.close();
window.close();
process.exit(ok ? 0 : 1);
