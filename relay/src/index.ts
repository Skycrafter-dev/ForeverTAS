// The remote-searching relay (docs/REMOTE.md). Rooms (below) pair one client
// with its own machines; groups (class Group) pool lent machines for their
// members' searches (ForeverTAS@home).
//
// Rooms: a ForeverTAS client opens a room
// (POST /rooms: a pairing code and a secret only it knows) and connects to it;
// its workers join with the code. Colab, Kaggle and home computers accept no
// incoming connections, so both sides connect out to the room, which passes
// messages along: a worker's to the client as {from, message}, the client's
// {to, message} to that worker as the message alone (a worker never knows it
// is relayed). The room ends when its client leaves; one never joined is
// forgotten after ten minutes.

export interface Env {
  ROOMS: DurableObjectNamespace;
  GROUPS: DurableObjectNamespace;
  SHARDS: DurableObjectNamespace;
  // per address: new groups and memberships, connections
  LIMIT_NEW: RateLimit;
  LIMIT_CONNECT: RateLimit;
}

interface RateLimit {
  limit(options: { key: string }): Promise<{ success: boolean }>;
}

const limited = async (limit: RateLimit, request: Request) =>
  !(await limit.limit({ key: request.headers.get("CF-Connecting-IP") ?? "local" })).success;

// no 0/O, 1/I/L: a code read aloud or retyped comes out right
const CODE_ALPHABET = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
const CODE_LENGTH = 8;
const MAX_WORKERS = 32;
const UNCLAIMED_ROOM_MS = 10 * 60 * 1000;

function randomCode(): string {
  const bytes = crypto.getRandomValues(new Uint8Array(CODE_LENGTH));
  return Array.from(bytes, (b) => CODE_ALPHABET[b % CODE_ALPHABET.length]).join("");
}

function randomSecret(): string {
  const bytes = crypto.getRandomValues(new Uint8Array(24));
  return Array.from(bytes, (b) => b.toString(16).padStart(2, "0")).join("");
}

function json(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "content-type": "application/json", "cache-control": "no-store" },
  });
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url);
    if (url.pathname === "/" && request.method === "GET") {
      return new Response("ForeverTAS relay: rooms where a ForeverTAS client and its workers meet.\n");
    }
    if (url.pathname === "/rooms" && request.method === "POST") {
      for (let attempt = 0; attempt < 5; ++attempt) {
        const code = randomCode();
        const secret = randomSecret();
        const room = env.ROOMS.get(env.ROOMS.idFromName(code));
        const opened = await room.fetch("https://room/open", { method: "POST", body: JSON.stringify({ secret }) });
        if (opened.status === 200) return json({ code, secret });
      }
      return json({ error: "no free room" }, 503);
    }
    if (url.pathname === "/groups" && request.method === "POST") {
      if (await limited(env.LIMIT_NEW, request)) return json({ error: "too many new groups from here: wait a minute" }, 429);
      for (let attempt = 0; attempt < 5; ++attempt) {
        const id = randomCode();
        const group = env.GROUPS.get(env.GROUPS.idFromName(id));
        const created = await group.fetch(`https://group/groups/${id}/create`, { method: "POST", body: await request.clone().text() });
        if (created.status !== 409) return created;
      }
      return json({ error: "no free group id" }, 503);
    }
    const group = url.pathname.match(/^\/groups\/(public|[A-Z2-9]{8})\/(join|invite|leave|status|worker|client)$/);
    if (group) {
      const [, id, action] = group;
      const coordinator = env.GROUPS.get(env.GROUPS.idFromName(id));
      if (action === "join" && (await limited(env.LIMIT_NEW, request)))
        return json({ error: "too many joins from here: wait a minute" }, 429);
      if (action !== "worker" && action !== "client") return coordinator.fetch(request);
      if (request.headers.get("Upgrade")?.toLowerCase() !== "websocket")
        return json({ error: "a group's machines and searches connect with a WebSocket" }, 426);
      if (await limited(env.LIMIT_CONNECT, request)) return json({ error: "too many connections from here" }, 429);
      if (action === "client") return coordinator.fetch(request);
      // a machine: the coordinator places it in a shard, which holds its connection
      const placed = await coordinator.fetch(`https://group/groups/${id}/place`, {
        method: "POST",
        body: JSON.stringify({ member: url.searchParams.get("member"), secret: url.searchParams.get("secret") }),
      });
      if (!placed.ok) return placed;
      const { machine, shard } = (await placed.json()) as { machine: string; shard: number };
      const target = new URL(request.url);
      target.search = `?group=${id}&machine=${machine}`;
      return env.SHARDS.get(env.SHARDS.idFromName(`${id}/${shard}`)).fetch(new Request(target.toString(), request));
    }
    const match = url.pathname.match(/^\/rooms\/([A-Z2-9]{8})\/(client|worker)$/);
    if (match) {
      if (request.headers.get("Upgrade")?.toLowerCase() !== "websocket") {
        return json({ error: "a room is joined with a WebSocket" }, 426);
      }
      return env.ROOMS.get(env.ROOMS.idFromName(match[1])).fetch(request);
    }
    return json({ error: "not found" }, 404);
  },
};

interface Attachment {
  role: "client" | "worker";
  id: string;
}

export class Room implements DurableObject {
  constructor(private readonly state: DurableObjectState, private readonly env: Env) {}

  async fetch(request: Request): Promise<Response> {
    const url = new URL(request.url);
    if (url.pathname === "/open") {
      if (await this.state.storage.get("secret")) return json({ error: "taken" }, 409);
      const { secret } = (await request.json()) as { secret: string };
      await this.state.storage.put({ secret, workers: 0 });
      await this.state.storage.setAlarm(Date.now() + UNCLAIMED_ROOM_MS);
      return json({});
    }
    const secret = (await this.state.storage.get<string>("secret")) ?? "";
    if (!secret) return json({ error: "no such room" }, 404);
    const role = url.pathname.endsWith("/client") ? "client" : "worker";
    const pair = new WebSocketPair();
    const [outside, inside] = [pair[0], pair[1]];
    if (role === "client") {
      if (url.searchParams.get("secret") !== secret) return json({ error: "not this room's client" }, 403);
      // a client that reconnects replaces the old connection
      for (const old of this.state.getWebSockets("client")) old.close(4000, "replaced");
      this.state.acceptWebSocket(inside, ["client"]);
      inside.serializeAttachment({ role, id: "client" } satisfies Attachment);
      await this.state.storage.deleteAlarm();
      // the workers already there
      for (const worker of this.state.getWebSockets("worker")) {
        const { id } = worker.deserializeAttachment() as Attachment;
        inside.send(JSON.stringify({ event: "joined", worker: id }));
      }
    } else {
      if (this.state.getWebSockets("worker").length >= MAX_WORKERS) return json({ error: "the room is full" }, 429);
      const count = ((await this.state.storage.get<number>("workers")) ?? 0) + 1;
      await this.state.storage.put("workers", count);
      const id = `w${count}`;
      this.state.acceptWebSocket(inside, ["worker", id]);
      inside.serializeAttachment({ role, id } satisfies Attachment);
      for (const client of this.state.getWebSockets("client")) client.send(JSON.stringify({ event: "joined", worker: id }));
    }
    return new Response(null, { status: 101, webSocket: outside });
  }

  async webSocketMessage(socket: WebSocket, data: string | ArrayBuffer): Promise<void> {
    if (typeof data !== "string") return;
    const { role, id } = socket.deserializeAttachment() as Attachment;
    if (role === "client") {
      let envelope: { to?: string; message?: unknown };
      try {
        envelope = JSON.parse(data);
      } catch {
        return;
      }
      if (!envelope.to || envelope.message === undefined) return;
      for (const worker of this.state.getWebSockets(envelope.to)) worker.send(JSON.stringify(envelope.message));
      return;
    }
    let message: unknown;
    try {
      message = JSON.parse(data);
    } catch {
      return;
    }
    const clients = this.state.getWebSockets("client");
    for (const client of clients) client.send(JSON.stringify({ from: id, message }));
    if (clients.length === 0) socket.send(JSON.stringify({ type: "error", message: "the client is away" }));
  }

  async webSocketClose(socket: WebSocket, code: number, reason: string): Promise<void> {
    const { role, id } = socket.deserializeAttachment() as Attachment;
    if (role === "worker") {
      for (const client of this.state.getWebSockets("client")) client.send(JSON.stringify({ event: "left", worker: id }));
      return;
    }
    if (reason === "replaced") return;
    // the client left: the room ends (its workers are told, its code is free again)
    for (const worker of this.state.getWebSockets("worker")) worker.close(4001, "the client left");
    await this.state.storage.deleteAll();
  }

  async webSocketError(socket: WebSocket): Promise<void> {
    await this.webSocketClose(socket, 1011, "error");
  }

  async alarm(): Promise<void> {
    // a room never joined by its client
    if (this.state.getWebSockets("client").length === 0) {
      for (const worker of this.state.getWebSockets()) worker.close(4001, "the client never came");
      await this.state.storage.deleteAll();
    }
  }
}

export { Group, Shard } from "./group";
