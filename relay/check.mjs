// A room on the live relay (node check.mjs): a client and a worker exchange a message each way,
// a wrong secret is refused, and the worker is closed when the client leaves.
const base = "forevertas.tmtas.exchange";
const opened = await fetch(`https://${base}/rooms`, { method: "POST" });
const { code, secret } = await opened.json();
console.log("room", code, "(secret", secret.length, "chars)");
const wait = (socket, what) => new Promise((resolve, reject) => {
  socket.addEventListener(what, resolve, { once: true });
  socket.addEventListener("error", (e) => reject(new Error("socket error")), { once: true });
});
const next = (socket) => new Promise((resolve) => socket.addEventListener("message", (m) => resolve(JSON.parse(m.data)), { once: true }));
const client = new WebSocket(`wss://${base}/rooms/${code}/client?secret=${secret}`);
await wait(client, "open");
const joined = next(client);
const worker = new WebSocket(`wss://${base}/rooms/${code}/worker`);
await wait(worker, "open");
const event = await joined;
console.log("client saw", JSON.stringify(event));
const toClient = next(client);
worker.send(JSON.stringify({ type: "hello" }));
console.log("client received", JSON.stringify(await toClient));
const toWorker = next(worker);
client.send(JSON.stringify({ to: event.worker, message: { type: "job", job: "j1" } }));
console.log("worker received", JSON.stringify(await toWorker));
const intruder = new WebSocket(`wss://${base}/rooms/${code}/client?secret=wrong`);
const refused = await new Promise((r) => { intruder.addEventListener("error", () => r("refused")); intruder.addEventListener("open", () => r("ACCEPTED")); });
console.log("a client with a wrong secret:", refused);
const closing = new Promise((r) => worker.addEventListener("close", (e) => r(`${e.code} ${e.reason}`), { once: true }));
client.close(1000, "done");
console.log("worker closed with", await closing);
