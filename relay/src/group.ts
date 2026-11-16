// ForeverTAS@home's groups (docs/REMOTE.md, "Groups").
//
// A group is a coordinator (class Group: members, invites, searches, which
// machine searches for which search, each search's job and overall best,
// credit) and shards (class Shard: up to SHARD_SIZE machines' connections
// each). A searching window connects to the coordinator; a machine to the
// shard the coordinator places it in. Everything is an event: a machine that
// searches sends nothing until it has a best worth sending (at most one per
// waiting period), stops, or fails; the coordinator tells the window only
// what changed. Idle connections are kept by "ping"/"pong", which Cloudflare
// answers without waking anything.

export interface GroupEnv {
  GROUPS: DurableObjectNamespace;
  SHARDS: DurableObjectNamespace;
}

const SHARD_SIZE = 200;
const BEST_BUDGET_PER_SECOND = 20; // bests a search may receive a second, whatever its size
const MIN_COOLDOWN_MS = 1000;
const WINDOW_GRACE_MS = 60 * 1000; // a search whose window is gone stops after this
const STOP_GRACE_MS = 30 * 1000; // machines told to stop that never answer are freed after this
const MAX_SEARCHES_PER_MEMBER_PUBLIC = 2;
const MAX_MACHINES_PER_SEARCH_PUBLIC = 2000; // (a group of one's own has no cap: its searches share evenly)
const MAX_JOB_BYTES = 1_900_000; // (a stored value is at most 2 MB)
const MAX_MESSAGE_BYTES = 256 * 1024;
const OVERALL_BUDGET_PER_SECOND = 1000; // overall bests sent down a second per search (one per machine each)
const SHARD_BATCH_MS = 100; // a shard passes its machines' messages up this often, in one request

export function json(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "content-type": "application/json", "cache-control": "no-store" },
  });
}

export function randomHex(bytes: number): string {
  return Array.from(crypto.getRandomValues(new Uint8Array(bytes)), (b) => b.toString(16).padStart(2, "0")).join("");
}

async function sha256(text: string): Promise<string> {
  const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(text));
  return Array.from(new Uint8Array(digest), (b) => b.toString(16).padStart(2, "0")).join("");
}

function cleanName(name: unknown): string {
  const text = typeof name === "string" ? name.trim().replace(/\s+/g, " ") : "";
  return text.slice(0, 40) || "Someone";
}

interface Member {
  name: string;
  hash: string;
  owner?: boolean;
}

interface Credit {
  attempts: number;
  seconds: number;
}

interface Lends {
  gpus?: unknown[];
  cpu?: { threads?: number };
}

interface Machine {
  id: string;
  member: string;
  shard: number;
  hello: { version?: string; packs?: Record<string, string>; gpus?: unknown[]; cpu?: { threads?: number } } | null;
  search: string | null; // the search it is given to
  stopping: boolean; // told to stop (moved, the search ended): its final count is awaited
  since: number; // when it got the search
  perSecond: number; // its speed in that search (attempts a second, its word)
  failedFor: string[]; // searches whose job failed on it (not given again)
  busy?: boolean; // searching for an earlier place it lends to (its room, another group): given nothing meanwhile
}

interface Search {
  id: string;
  member: string;
  version: string;
  packs: Record<string, string>;
  slots: number; // the next machine's candidate range
  cooldownMs: number; // the waiting period last sent to its machines
  best: string | null; // the overall best (JSON), sent down to machines
  lost: number; // when its window went (0: there)
  stopping: number; // when it was told to stop (0: running)
}

const lendsSomething = (hello: Lends | null) => !!hello && ((hello.gpus?.length ?? 0) > 0 || (hello.cpu?.threads ?? 0) > 0);

function cooldownFor(machines: number): number {
  return Math.max(MIN_COOLDOWN_MS, Math.ceil((machines * 1000) / BEST_BUDGET_PER_SECOND));
}

// ---- the coordinator ----

export class Group implements DurableObject {
  constructor(private readonly state: DurableObjectState, private readonly env: GroupEnv) {
    this.state.setWebSocketAutoResponse(new WebSocketRequestResponsePair("ping", "pong"));
  }

  private groupId = "";

  private async member(id: string | null, secret: string | null): Promise<Member | null> {
    if (!id || !secret) return null;
    const member = await this.state.storage.get<Member>(`member:${id}`);
    return member && member.hash === (await sha256(secret)) ? member : null;
  }

  private async isPublic(): Promise<boolean> {
    return (await this.state.storage.get<boolean>("public")) === true;
  }

  private shard(index: number): DurableObjectStub {
    return this.env.SHARDS.get(this.env.SHARDS.idFromName(`${this.groupId}/${index}`));
  }

  // A message to machines, through their shards (one request a shard).
  private async deliver(machines: Machine[], message: unknown): Promise<void> {
    const byShard = new Map<number, string[]>();
    for (const machine of machines) byShard.set(machine.shard, [...(byShard.get(machine.shard) ?? []), machine.id]);
    await Promise.all(Array.from(byShard, ([index, ids]) =>
      this.shard(index).fetch("https://shard/deliver", {
        method: "POST",
        body: JSON.stringify({ group: this.groupId, machines: ids, message }),
      })));
  }

  // A message to a search's window (none while it is gone).
  private toWindow(search: string, message: unknown): void {
    const text = JSON.stringify(message);
    for (const socket of this.windowsOf(search)) {
      try {
        socket.send(text);
      } catch {
        // closing: its close handler tidies up
      }
    }
  }

  // (after sleeping, a handler other than fetch learns which group this is)
  private async ensureId(): Promise<void> {
    if (!this.groupId) this.groupId = (await this.state.storage.get<string>("id")) ?? "";
  }

  private async machines(): Promise<Machine[]> {
    return Array.from((await this.state.storage.list<Machine>({ prefix: "machine:" })).values());
  }

  private async searches(): Promise<Search[]> {
    return Array.from((await this.state.storage.list<Search>({ prefix: "search:" })).values());
  }

  // The search's machines and their summed speed, told to its window at most once a second.
  private summaries = new Map<string, number>(); // search -> when the last went
  private summaryTimers = new Set<string>();
  private async summary(search: string): Promise<void> {
    if (this.summaryTimers.has(search)) return;
    const wait = Math.max(0, (this.summaries.get(search) ?? 0) + 1000 - Date.now());
    this.summaryTimers.add(search);
    setTimeout(async () => {
      this.summaryTimers.delete(search);
      this.summaries.set(search, Date.now());
      const of = (await this.machines()).filter((machine) => machine.search === search && !machine.stopping);
      this.toWindow(search, {
        event: "machines",
        count: of.length,
        perSecond: of.reduce((sum, machine) => sum + machine.perSecond, 0),
      });
    }, wait);
  }

  // What machines said, for each search's window: one message a batch.
  private outbox = new Map<string, { bests: unknown[]; finals: unknown[]; failed: unknown[]; left: string[] }>();
  private out(search: string) {
    let box = this.outbox.get(search);
    if (!box) this.outbox.set(search, (box = { bests: [], finals: [], failed: [], left: [] }));
    return box;
  }
  private flushOutbox(): void {
    for (const [search, box] of this.outbox) {
      this.toWindow(search, {
        event: "batch", search,
        ...(box.bests.length ? { bests: box.bests } : {}), ...(box.finals.length ? { finals: box.finals } : {}),
        ...(box.failed.length ? { failed: box.failed } : {}), ...(box.left.length ? { left: box.left } : {}),
      });
    }
    this.outbox.clear();
  }

  // An overall best goes down to the search's machines at most once a period (the latest one), the period longer
  // as the search has more machines: the relay sends the same number a second whatever its size.
  private broadcasts = new Map<string, number>();
  private broadcastTimers = new Set<string>();
  private async broadcastSoon(searchId: string): Promise<void> {
    if (this.broadcastTimers.has(searchId)) return;
    const of = (await this.machines()).filter((machine) => machine.search === searchId && !machine.stopping);
    const period = Math.max(MIN_COOLDOWN_MS, Math.ceil((of.length * 1000) / OVERALL_BUDGET_PER_SECOND));
    const wait = Math.max(0, (this.broadcasts.get(searchId) ?? 0) + period - Date.now());
    this.broadcastTimers.add(searchId);
    setTimeout(async () => {
      this.broadcastTimers.delete(searchId);
      this.broadcasts.set(searchId, Date.now());
      const search = await this.state.storage.get<Search>(`search:${searchId}`);
      if (!search?.best || search.stopping) return;
      const now = (await this.machines()).filter((machine) => machine.search === searchId && !machine.stopping);
      await this.deliver(now, { type: "overall", search: searchId, best: JSON.parse(search.best) });
    }, wait);
  }

  async fetch(request: Request): Promise<Response> {
    const url = new URL(request.url);
    const parts = url.pathname.split("/").filter(Boolean);
    const [id, action] = parts.slice(-2);
    this.groupId = id;
    if (!(await this.state.storage.get("id"))) await this.state.storage.put("id", id);
    if (action === "create") {
      if (await this.state.storage.get("name")) return json({ error: "taken" }, 409);
      const body = (await request.json().catch(() => ({}))) as Record<string, unknown>;
      if (typeof body.member !== "string" || typeof body.secret !== "string" || body.secret.length < 16)
        return json({ error: "a group is created by a member (id and secret)" }, 400);
      const invite = randomHex(6).toUpperCase();
      await this.state.storage.put({
        name: cleanName(body.name),
        invite: await sha256(invite),
        [`member:${body.member}`]: { name: cleanName(body.memberName), hash: await sha256(body.secret), owner: true } satisfies Member,
      });
      return json({ id, name: cleanName(body.name), invite: `${id}-${invite}` });
    }
    if (id === "public" && !(await this.state.storage.get("name")))
      await this.state.storage.put({ name: "Public pool", public: true });
    if (!(await this.state.storage.get("name"))) return json({ error: "no such group" }, 404);

    // from a shard: a machine's message, or its connection's end
    if (action === "machines" && request.method === "POST") {
      const { items } = (await request.json()) as { items: { machine: string; message?: Record<string, unknown>; gone?: boolean }[] };
      for (const item of items) {
        if (item.gone) await this.machineGone(item.machine);
        else if (item.message) await this.fromMachine(item.machine, item.message);
      }
      this.flushOutbox();
      return json({});
    }
    if (action === "job" && request.method === "GET") {
      const job = await this.state.storage.get<string>(`job:${url.searchParams.get("search")}`);
      return job ? new Response(job) : json({ error: "no such search" }, 404);
    }

    const body = request.method === "POST" ? ((await request.json().catch(() => ({}))) as Record<string, unknown>) : {};
    if (action === "join" && request.method === "POST") {
      if (typeof body.member !== "string" || typeof body.secret !== "string" || body.secret.length < 16)
        return json({ error: "a member joins with an id and a secret" }, 400);
      const known = await this.state.storage.get<Member>(`member:${body.member}`);
      if (known && known.hash !== (await sha256(body.secret))) return json({ error: "not this member" }, 403);
      if (!known && !(await this.isPublic())) {
        const invite = typeof body.invite === "string" ? body.invite.split("-").pop() ?? "" : "";
        if ((await sha256(invite.toUpperCase())) !== (await this.state.storage.get<string>("invite")))
          return json({ error: "this invite is not the group's (any more)" }, 403);
      }
      await this.state.storage.put(`member:${body.member}`, {
        name: cleanName(body.memberName),
        hash: await sha256(body.secret),
        ...(known?.owner ? { owner: true } : {}),
      } satisfies Member);
      return json({ id, name: await this.state.storage.get<string>("name") });
    }
    if (action === "invite" && request.method === "POST") {
      const member = await this.member(body.member as string, body.secret as string);
      if (!member?.owner) return json({ error: "only the group's owner makes invites" }, 403);
      const invite = randomHex(6).toUpperCase();
      await this.state.storage.put("invite", await sha256(invite));
      return json({ invite: `${id}-${invite}` });
    }
    if (action === "leave" && request.method === "POST") {
      if (!(await this.member(body.member as string, body.secret as string))) return json({ error: "not a member" }, 403);
      await this.state.storage.delete(`member:${body.member}`);
      for (const socket of this.state.getWebSockets(`member:${body.member}`)) socket.close(4003, "left the group");
      for (const machine of await this.machines())
        if (machine.member === body.member) {
          await this.shard(machine.shard).fetch("https://shard/close", {
            method: "POST",
            body: JSON.stringify({ group: id, machine: machine.id, code: 4003, reason: "left the group" }),
          });
        }
      return json({});
    }
    // a machine to place: in the shard with the fewest machines (a new one when all are full)
    if (action === "place" && request.method === "POST") {
      const memberId = body.member as string;
      if (!(await this.member(memberId, body.secret as string))) return json({ error: "not a member of this group" }, 403);
      const shards = (await this.state.storage.get<number[]>("shards")) ?? [];
      let index = shards.findIndex((count) => count < SHARD_SIZE);
      if (index < 0) {
        index = shards.length;
        shards.push(0);
      }
      for (let i = 0; i < shards.length; ++i) if (shards[i] < shards[index]) index = i;
      shards[index] += 1;
      const count = ((await this.state.storage.get<number>("machineCount")) ?? 0) + 1;
      const machine: Machine = {
        id: `m${count}`, member: memberId, shard: index, hello: null, search: null, stopping: false, since: 0,
        perSecond: 0, failedFor: [],
      };
      await this.state.storage.put({ shards, machineCount: count, [`machine:${machine.id}`]: machine });
      return json({ machine: machine.id, shard: index });
    }

    const self = await this.member(url.searchParams.get("member"), url.searchParams.get("secret"));
    if (!self) return json({ error: "not a member of this group" }, 403);
    const memberId = url.searchParams.get("member")!;
    if (action === "status") {
      const members = await this.state.storage.list<Member>({ prefix: "member:" });
      const credits = await this.state.storage.list<Credit>({ prefix: "credit:" });
      const machines = await this.machines();
      return json({
        id,
        name: await this.state.storage.get<string>("name"),
        public: await this.isPublic(),
        owner: self.owner === true,
        members: (await this.isPublic()) ? undefined : members.size,
        machines: machines.filter((machine) => machine.hello).length,
        searches: (await this.searches()).filter((search) => !search.stopping).length,
        yours: machines.filter((machine) => machine.member === memberId).length,
        shards: ((await this.state.storage.get<number[]>("shards")) ?? []).length,
        credits: Array.from(credits, ([key, credit]) => ({
          name: members.get(`member:${key.slice(7)}`)?.name ?? "Someone who left",
          you: key.slice(7) === memberId,
          attempts: credit.attempts,
          hours: credit.seconds / 3600,
        })).sort((a, b) => b.attempts - a.attempts).slice(0, 100),
      });
    }
    if (action === "client") {
      const pair = new WebSocketPair();
      this.state.acceptWebSocket(pair[1], ["client", `member:${memberId}`]);
      pair[1].serializeAttachment({ member: memberId, search: null });
      return new Response(null, { status: 101, webSocket: pair[0] });
    }
    return json({ error: "not found" }, 404);
  }

  // ---- a window's messages ----

  async webSocketMessage(socket: WebSocket, data: string | ArrayBuffer): Promise<void> {
    if (typeof data !== "string") return;
    await this.ensureId();
    if (data.length > MAX_JOB_BYTES + 64 * 1024) {
      socket.close(4009, "message too large");
      return;
    }
    let message: Record<string, unknown>;
    try {
      message = JSON.parse(data);
    } catch {
      return;
    }
    const attachment = socket.deserializeAttachment() as { member: string; search: string | null };
    const say = (what: unknown) => socket.send(JSON.stringify(what));
    if (message.type === "search") {
      const id = typeof message.id === "string" ? message.id.slice(0, 64) : "";
      if (!/^[0-9a-f]{16,64}$/.test(id)) return say({ event: "error", message: "a search needs an id" });
      const existing = await this.state.storage.get<Search>(`search:${id}`);
      if (message.resume === true) {
        // the window back within its grace: the search carries on
        if (!existing || existing.member !== attachment.member || existing.stopping) return say({ event: "gone", search: id });
        existing.lost = 0;
        await this.state.storage.put(`search:${id}`, existing);
        this.attach(socket, attachment, id);
        say({ event: "resumed", search: id, best: existing.best ? JSON.parse(existing.best) : null });
        await this.summary(id);
        return;
      }
      if (existing) return say({ event: "error", message: "that search exists" });
      const job = message.job as Record<string, unknown> | undefined;
      const text = job ? JSON.stringify(job) : "";
      if (!job || text.length > MAX_JOB_BYTES) return say({ event: "error", message: "the search is missing or too large" });
      if (await this.isPublic()) {
        const mine = (await this.searches()).filter((search) => search.member === attachment.member && !search.stopping);
        if (mine.length >= MAX_SEARCHES_PER_MEMBER_PUBLIC)
          return say({ event: "error", message: `at most ${MAX_SEARCHES_PER_MEMBER_PUBLIC} searches each in the public pool` });
      }
      const search: Search = {
        id, member: attachment.member, version: String(job.version ?? ""),
        packs: (job.packs ?? {}) as Record<string, string>, slots: 0, cooldownMs: MIN_COOLDOWN_MS, best: null,
        lost: 0, stopping: 0,
      };
      await this.state.storage.put({ [`search:${id}`]: search, [`job:${id}`]: text });
      this.attach(socket, attachment, id);
      say({ event: "searching", search: id });
      await this.scheduleSoon(); // (machines still saying hello are given in the same pass)
      return;
    }
    const id = attachment.search;
    if (!id) return;
    const search = await this.state.storage.get<Search>(`search:${id}`);
    if (!search) return;
    if (message.type === "best") {
      // an overall best the window checked: every machine of the search learns it
      const best = JSON.stringify({ inputs: message.inputs, sample: message.sample });
      if (best.length > MAX_MESSAGE_BYTES) return;
      search.best = best;
      await this.state.storage.put(`search:${id}`, search);
      await this.broadcastSoon(id);
      return;
    }
    if (message.type === "stop") await this.stopSearch(search);
  }

  // A window's connection serves one search at a time.
  private attach(socket: WebSocket, attachment: { member: string; search: string | null }, id: string): void {
    attachment.search = id;
    socket.serializeAttachment(attachment);
  }

  private windowsOf(search: string): WebSocket[] {
    return this.state.getWebSockets("client").filter(
      (socket) => (socket.deserializeAttachment() as { search: string | null }).search === search);
  }

  private async stopSearch(search: Search): Promise<void> {
    if (search.stopping) return;
    search.stopping = Date.now();
    await this.state.storage.put(`search:${search.id}`, search);
    const machines = await this.machines();
    const of = machines.filter((machine) => machine.search === search.id);
    for (const machine of of) {
      machine.stopping = true;
      await this.state.storage.put(`machine:${machine.id}`, machine);
    }
    await this.deliver(of, { type: "stop", search: search.id });
    if (of.length === 0) await this.dropSearch(search.id);
    else await this.state.storage.setAlarm(Date.now() + STOP_GRACE_MS);
  }

  private async dropSearch(id: string): Promise<void> {
    await this.state.storage.delete([`search:${id}`, `job:${id}`]);
    this.flushOutbox(); // (its machines' last words before it ends)
    this.toWindow(id, { event: "ended", search: id });
    await this.schedule();
  }

  async webSocketClose(socket: WebSocket): Promise<void> {
    await this.ensureId();
    const { search: id } = socket.deserializeAttachment() as { search: string | null };
    if (!id) return;
    const search = await this.state.storage.get<Search>(`search:${id}`);
    if (!search || search.stopping) return;
    // the window is gone: its search waits for it a while
    search.lost = Date.now();
    await this.state.storage.put(`search:${id}`, search);
    await this.state.storage.setAlarm(Date.now() + WINDOW_GRACE_MS);
  }

  async webSocketError(socket: WebSocket): Promise<void> {
    await this.webSocketClose(socket);
  }

  async alarm(): Promise<void> {
    await this.ensureId();
    const now = Date.now();
    let next = 0;
    for (const search of await this.searches()) {
      if (search.lost && !search.stopping) {
        if (now - search.lost >= WINDOW_GRACE_MS) await this.stopSearch(search);
        else next = Math.max(next, search.lost + WINDOW_GRACE_MS);
      }
      if (search.stopping) {
        if (now - search.stopping >= STOP_GRACE_MS) {
          // machines that never answered are freed
          for (const machine of await this.machines())
            if (machine.search === search.id) await this.freeMachine(machine, search.id, 0);
          await this.dropSearch(search.id);
        } else next = Math.max(next, search.stopping + STOP_GRACE_MS);
      }
    }
    if (next) await this.state.storage.setAlarm(next);
  }

  // ---- machines (through their shards) ----

  private async fromMachine(id: string, message: Record<string, unknown>): Promise<void> {
    const machine = await this.state.storage.get<Machine>(`machine:${id}`);
    if (!machine) return;
    if (message.type === "hello") {
      machine.hello = {
        version: String(message.version ?? ""), packs: (message.packs ?? {}) as Record<string, string>,
        gpus: (message.gpus ?? []) as unknown[], cpu: (message.cpu ?? {}) as { threads?: number },
      };
      await this.state.storage.put(`machine:${id}`, machine);
      void this.scheduleSoon();
      return;
    }
    if (message.type === "busy" || message.type === "free") {
      machine.busy = message.type === "busy";
      await this.state.storage.put(`machine:${id}`, machine);
      if (!machine.busy) void this.scheduleSoon();
      return;
    }
    const searchId = typeof message.search === "string" ? message.search : "";
    if (!searchId || machine.search !== searchId) return;
    if (message.type === "speed") {
      machine.perSecond = Math.max(0, Number(message.perSecond) || 0);
      await this.state.storage.put(`machine:${id}`, machine);
      await this.summary(searchId);
      return;
    }
    if (message.type === "best") {
      this.out(searchId).bests.push({ machine: id, live: message.live });
      return;
    }
    if (message.type === "final" || message.type === "error") {
      if (message.type === "error") machine.failedFor = [...machine.failedFor.slice(-7), searchId];
      const iterations = Math.max(0, Number(message.iterations) || 0);
      const seconds = Math.max(0, Number(message.seconds) || 0);
      if (message.type === "final")
        this.out(searchId).finals.push({ machine: id, iterations, ...(message.live ? { live: message.live } : {}) });
      else this.out(searchId).failed.push({ machine: id, iterations, message: String(message.message ?? "") });
      await this.freeMachine(machine, searchId, iterations, seconds);
      const search = await this.state.storage.get<Search>(`search:${searchId}`);
      if (search?.stopping && !(await this.machines()).some((other) => other.search === searchId)) {
        await this.dropSearch(searchId);
        return;
      }
      await this.schedule();
    }
  }

  // A machine is a search's no more: its credit counted (its final count, or speed x time when it vanished).
  private async freeMachine(machine: Machine, searchId: string, iterations: number, seconds?: number): Promise<void> {
    const elapsed = machine.since ? (Date.now() - machine.since) / 1000 : 0;
    const counted = iterations || machine.perSecond * elapsed;
    const credit = (await this.state.storage.get<Credit>(`credit:${machine.member}`)) ?? { attempts: 0, seconds: 0 };
    credit.attempts += counted;
    credit.seconds += seconds ?? elapsed;
    machine.search = null;
    machine.stopping = false;
    machine.since = 0;
    machine.perSecond = 0;
    await this.state.storage.put({ [`credit:${machine.member}`]: credit, [`machine:${machine.id}`]: machine });
    await this.summary(searchId);
  }

  private async machineGone(id: string): Promise<void> {
    const machine = await this.state.storage.get<Machine>(`machine:${id}`);
    if (!machine) return;
    await this.state.storage.delete(`machine:${id}`);
    const shards = (await this.state.storage.get<number[]>("shards")) ?? [];
    if (shards[machine.shard]) shards[machine.shard] -= 1;
    await this.state.storage.put("shards", shards);
    if (machine.search) {
      const searchId = machine.search;
      this.out(searchId).left.push(id);
      await this.freeMachine({ ...machine }, searchId, 0);
      await this.state.storage.delete(`machine:${id}`);
      const search = await this.state.storage.get<Search>(`search:${searchId}`);
      if (search?.stopping && !(await this.machines()).some((other) => other.search === searchId)) {
        await this.dropSearch(searchId);
        return;
      }
    }
    void this.scheduleSoon();
  }

  // A burst of events (a thousand machines joining at once) makes one scheduling pass, not one each.
  private scheduled: Promise<void> | null = null;
  private scheduleSoon(): Promise<void> {
    if (!this.scheduled) {
      this.scheduled = new Promise<void>((resolve) => setTimeout(resolve, 100)).then(async () => {
        this.scheduled = null;
        await this.schedule();
      });
    }
    return this.scheduled;
  }

  // Free machines to the searches with the fewest (their version, packs that agree); then even shares, by moving
  // machines from the searches with the most (a moved machine stops, hands in its count, and is placed again).
  private async schedule(): Promise<void> {
    const searches = (await this.searches()).filter((search) => !search.stopping);
    const machines = await this.machines();
    const cap = (await this.isPublic()) ? MAX_MACHINES_PER_SEARCH_PUBLIC : Number.POSITIVE_INFINITY;
    const fits = (machine: Machine, search: Search) => {
      const hello = machine.hello;
      if (!hello || !lendsSomething(hello) || hello.version !== search.version || machine.busy) return false;
      if (machine.failedFor.includes(search.id)) return false;
      for (const [pack, hash] of Object.entries(search.packs))
        if (hello.packs?.[pack] !== undefined && hello.packs[pack] !== hash) return false;
      return true;
    };
    const share = new Map(searches.map((search) => [search.id, 0]));
    for (const machine of machines)
      if (machine.search && !machine.stopping && share.has(machine.search))
        share.set(machine.search, share.get(machine.search)! + 1);
    const given = new Map<string, Machine[]>();
    for (const machine of machines) {
      if (machine.search || !machine.hello) continue;
      const fitting = searches.filter((search) => fits(machine, search) && share.get(search.id)! < cap);
      if (fitting.length === 0) continue;
      fitting.sort((a, b) => share.get(a.id)! - share.get(b.id)!);
      const search = fitting[0];
      machine.search = search.id;
      machine.since = Date.now();
      share.set(search.id, share.get(search.id)! + 1);
      given.set(search.id, [...(given.get(search.id) ?? []), machine]);
    }
    for (const [searchId, list] of given) {
      const search = searches.find((s) => s.id === searchId)!;
      const cooldownMs = cooldownFor(share.get(searchId)!);
      // (sent again once it is off by half: the budget holds while a search fills up)
      const changed = cooldownMs * 2 >= 3 * search.cooldownMs || cooldownMs * 3 <= 2 * search.cooldownMs;
      const slots: Record<string, number> = {};
      for (const machine of list) slots[machine.id] = search.slots++;
      if (changed) search.cooldownMs = cooldownMs;
      const puts: Record<string, unknown> = { [`search:${searchId}`]: search };
      for (const machine of list) puts[`machine:${machine.id}`] = machine;
      await this.state.storage.put(puts);
      // each machine its job (its shard fetches the job once), its candidate range and the waiting period
      const byShard = new Map<number, { machine: string; slot: number }[]>();
      for (const machine of list)
        byShard.set(machine.shard, [...(byShard.get(machine.shard) ?? []), { machine: machine.id, slot: slots[machine.id] }]);
      await Promise.all(Array.from(byShard, ([index, assigned]) =>
        this.shard(index).fetch("https://shard/assign", {
          method: "POST",
          body: JSON.stringify({
            group: this.groupId, search: searchId, assigned, cooldownMs: search.cooldownMs,
            best: search.best ? JSON.parse(search.best) : null,
          }),
        })));
      if (changed) {
        const others = machines.filter((machine) => machine.search === searchId && !machine.stopping && !list.includes(machine));
        await this.deliver(others, { type: "cooldown", search: searchId, cooldownMs: search.cooldownMs });
      }
      await this.summary(searchId);
    }
    // even shares: a machine moves from a search with at least two more than another that it fits
    for (const poor of searches) {
      const candidates = machines.filter((machine) =>
        machine.search && !machine.stopping && share.get(machine.search)! >= share.get(poor.id)! + 2 && fits(machine, poor));
      if (candidates.length === 0) continue;
      candidates.sort((a, b) => share.get(b.search!)! - share.get(a.search!)!);
      const machine = candidates[0];
      share.set(machine.search!, share.get(machine.search!)! - 1);
      machine.stopping = true;
      await this.state.storage.put(`machine:${machine.id}`, machine);
      await this.deliver([machine], { type: "stop", search: machine.search, moved: true });
      await this.summary(machine.search!);
    }
  }
}

// ---- a shard: machines' connections ----

export class Shard implements DurableObject {
  private jobs = new Map<string, string>(); // search -> its job (kept while this shard is awake)

  constructor(private readonly state: DurableObjectState, private readonly env: GroupEnv) {
    this.state.setWebSocketAutoResponse(new WebSocketRequestResponsePair("ping", "pong"));
  }

  private coordinator(group: string): DurableObjectStub {
    return this.env.GROUPS.get(this.env.GROUPS.idFromName(group));
  }

  private socketOf(machine: string): WebSocket | undefined {
    return this.state.getWebSockets(machine)[0];
  }

  private send(socket: WebSocket | undefined, text: string): void {
    try {
      socket?.send(text);
    } catch {
      // closing
    }
  }

  async fetch(request: Request): Promise<Response> {
    const url = new URL(request.url);
    if (request.headers.get("Upgrade")?.toLowerCase() === "websocket") {
      const group = url.searchParams.get("group") ?? "", machine = url.searchParams.get("machine") ?? "";
      const pair = new WebSocketPair();
      this.state.acceptWebSocket(pair[1], [machine]);
      pair[1].serializeAttachment({ group, machine });
      return new Response(null, { status: 101, webSocket: pair[0] });
    }
    const body = (await request.json()) as Record<string, unknown>;
    if (url.pathname === "/deliver") {
      const text = JSON.stringify(body.message);
      for (const machine of body.machines as string[]) this.send(this.socketOf(machine), text);
      return json({});
    }
    if (url.pathname === "/assign") {
      // the job, fetched once from the coordinator for every machine of this shard
      const search = body.search as string;
      let job = this.jobs.get(search);
      if (job === undefined) {
        const answer = await this.coordinator(body.group as string).fetch(
          `https://group/groups/${body.group}/job?search=${encodeURIComponent(search)}`);
        if (!answer.ok) return json({ error: "no such search" }, 404);
        job = await answer.text();
        if (this.jobs.size >= 8) this.jobs.delete(this.jobs.keys().next().value!);
        this.jobs.set(search, job);
      }
      for (const { machine, slot } of body.assigned as { machine: string; slot: number }[]) {
        this.send(this.socketOf(machine), `{"type":"job","search":${JSON.stringify(search)},"slot":${slot},` +
          `"cooldownMs":${Number(body.cooldownMs)},"best":${JSON.stringify(body.best ?? null)},"job":${job}}`);
      }
      return json({});
    }
    if (url.pathname === "/close") {
      this.socketOf(body.machine as string)?.close(Number(body.code) || 4000, String(body.reason ?? ""));
      return json({});
    }
    return json({ error: "not found" }, 404);
  }

  async webSocketMessage(socket: WebSocket, data: string | ArrayBuffer): Promise<void> {
    if (typeof data !== "string") return;
    if (data.length > MAX_MESSAGE_BYTES) {
      socket.close(4009, "message too large");
      return;
    }
    let message: unknown;
    try {
      message = JSON.parse(data);
    } catch {
      return;
    }
    const { group, machine } = socket.deserializeAttachment() as { group: string; machine: string };
    this.queue(group, { machine, message });
  }

  async webSocketClose(socket: WebSocket): Promise<void> {
    const { group, machine } = socket.deserializeAttachment() as { group: string; machine: string };
    this.queue(group, { machine, gone: true });
  }

  // What its machines say goes up in one request a batch (the coordinator sees this shard a few times a second at
  // most, however many machines it holds).
  private pending: { machine: string; message?: unknown; gone?: boolean }[] = [];
  private pendingGroup = "";
  private flushing: ReturnType<typeof setTimeout> | null = null;
  private queue(group: string, item: { machine: string; message?: unknown; gone?: boolean }): void {
    this.pendingGroup = group;
    this.pending.push(item);
    if (this.flushing) return;
    this.flushing = setTimeout(async () => {
      this.flushing = null;
      const items = this.pending;
      this.pending = [];
      await this.coordinator(this.pendingGroup).fetch(`https://group/groups/${this.pendingGroup}/machines`, {
        method: "POST",
        body: JSON.stringify({ items }),
      });
    }, SHARD_BATCH_MS);
  }

  async webSocketError(socket: WebSocket): Promise<void> {
    await this.webSocketClose(socket);
  }
}
