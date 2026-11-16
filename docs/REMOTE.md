# Remote searching

A search can run on other computers as well as this one: your own machines
(a Google Colab or Kaggle notebook, a rented GPU, another computer of yours),
and machines other people lend through groups and the public pool
(ForeverTAS@home). The window stays where it is; only the attempts run
elsewhere. Every search feature works remotely: exact and approximate
physics, every target, condition, pass and search algorithm, and promotion.

Everything is in **Edit > Preferences > Remote**.

## This computer

The tab lists the graphics cards ForeverTAS can search on (a green dot: ready)
and the processor with its thread count. **What it lends to groups** sets
what the computer lends: its graphics card, and how many CPU threads.

## Your own machines

Turn on **Let machines join with a code**: ForeverTAS opens a room on the
relay and shows its pairing code, with the command to run on the other
machine. Each machine that joins is listed with what it lends (each GPU, its
CPU threads); every ticked part searches for every search started here,
beside this computer's devices.

- **Google Colab:** **Open in Colab** opens the notebook of this version.
  It asks for the code (`CODES`) and the Packs folder in your Google Drive
  (`PACKS`), then fetches the worker kit, ForeverTAS and the GPU module for
  the card it gets.
- **Kaggle:** `worker/kaggle.ipynb`, with the Packs in a private dataset.
- **Docker** (a rented GPU, a server): `worker/Dockerfile`, with the Packs
  mounted at `/packs`:

  ```sh
  docker build -t forevertas-worker worker/
  docker run --rm --gpus all -v /path/to/TmForever/Packs:/packs:ro forevertas-worker CODES
  ```

- **Any computer with ForeverTAS:**
  `ForeverTAS --worker CODES --packs DIR [--name NAME] [--lend gpu,cpu] [--cpu-threads N]`.

`CODES` is everything the machine searches for, separated by commas: a room's
code, groups' invites, `public`. A machine lends its GPUs and CPU threads by
default (all threads but two for each GPU it feeds; a machine with no GPU
lends its CPU).

A machine in your room is a device of the search like a local GPU: it takes
its candidates from the search's one sequence, adopts the search's shared
best when promoting, and a custom target ranks every attempt in order across
all devices. Results do not depend on where the attempts ran: a search on a
remote CPU gives the result of the same search here, on Linux and Windows
alike.

## Groups and the public pool

People lend their computers to a **group**; a member's search uses the
group's machines as one more device. A community route can go to the
**public pool** (anyone lends, anyone searches); a competitive one stays in a
group of its own.

- **Members, no accounts.** Each ForeverTAS makes itself a member id and a
  secret once (kept in its settings), and has a name you pick (**Your name in
  groups**): the one groups show for your credit.
- **Groups.** **New group** makes one and shows its invite, one string to
  share (`GROUPID-KEY`). Whoever has it joins with **Join a group**. The owner
  can make a **New invite**, which retires the old one. The public pool is the
  group `public`: everyone can join it.
- **Lending.** **Lend this computer** on a group. ForeverTAS runs one worker
  process for every group it lends to, with the groups' addresses in its
  environment (never on its command line), in a sandbox: on Linux bubblewrap
  when it is installed (the system read-only, an empty home folder but for
  the Packs, the GPU modules and the app); on Windows an AppContainer (it
  reads the Packs, the app and its GPU modules, writes only its own folder,
  and reaches the internet). A GPU that does not work inside the sandbox is
  not lent; the CPU still is. Lending waits while this computer runs a search
  of its own on its GPU.
- **Searching with a group.** **Search with it** on a group: the group is one
  device of every search started here, whose machines come and go while it
  runs. A search never waits for a group: until a machine is given, it runs
  on its other devices (or waits, if it has none).
- **One machine, several places.** A machine searches for one place at a
  time, in this order: its own room, then its groups (in the order given),
  the public pool last. While it searches for one, the later ones are told it
  is busy; a search of an earlier one stops a later one's (its count and best
  handed in) and takes the machine.
- **Connections.** Lent machines and windows connect again after a drop (the
  network, a relay restart), and a running search carries on.
- **Credit.** **Who lent most**: the relay counts, per member, the attempts
  their machines reported and the hours they searched.

### What lending shares

A search's job carries its replay or challenge, its inputs and settings to the
machines that run it: the group members' machines, or anyone's in the public
pool. The window says so where the public pool is joined.

### What is checked

In exact physics, a best from another machine counts (and goes to the other
machines) only once it re-simulates here with its score. In approximate
physics a best's score is the twin's: it counts as the machine reports it
(the twin gives the same score on every machine, CPU or GPU, Linux or
Windows), and the search's end checks the result on exact physics, as every
remote result. A dishonest machine can make an approximate search chase a
wrong best for a while; it can never make a wrong best the search's result.
Attempt counts and credit are the machines' own word.

### Worth knowing

- Google Colab and Kaggle are meant for interactive use, and their terms
  limit some long, unattended or distributed uses. Lending one to the public
  pool for hours may put its account at risk: read their terms first.
- A machine searches only for the same ForeverTAS version as the window.
  Each release splits the public pool until lenders update; the window and
  the worker kit update themselves.

## How it works

### Pieces

- **Worker:** `ForeverTAS --worker`, the app without its window
  (`src/app/remote/remote_worker.*`). It runs the search devices it is sent
  on its own GPUs and CPU, with its own copy of the Packs.
- **Hub:** the window's side (`src/app/remote/remote_hub.*`): the workers
  that joined, and a search device run on one of them or on a group.
  `RemoteSearch` (`remote_search.*`) is what the Remote tab drives.
- **Relay** (`relay/`, a Cloudflare Worker at `forevertas.tmtas.exchange`;
  `FOREVERTAS_RELAY` names another): both sides connect out to it, since
  notebooks and home computers accept no incoming connections. Rooms pass
  messages between a window and its machines; groups schedule lent machines.

No game file travels. A job names the environment packs of the window by
content hash; a machine whose packs differ refuses it, and the relay gives a
group's search only to machines whose packs agree. A machine runs only the
window's ForeverTAS version: a search must run the same everywhere.

### Rooms

`POST /rooms` gives a pairing code and a secret; the window connects to
`/rooms/CODE/client` with the secret, its machines to `/rooms/CODE/worker`.
The room passes a machine's messages to the window as `{from, message}` and
the window's `{to, message}` to that machine. It ends when its window leaves;
a room never joined by its window is forgotten after ten minutes.

| From | Message | What |
| --- | --- | --- |
| worker | `hello` | version, GPUs, CPU (name, threads), machine name, environment pack hashes |
| window | `job` | the search request, the replay or challenge, the device's share (limit, offset, stride) |
| worker | `claim` | the next block of candidate ids, asked a block ahead |
| window | `claimed` | the first id of that block |
| window | `baseline` | a better shared best to start from, pushed |
| worker | `live`, `statistics`, `progress` | the device's best (inputs, score, iteration), its counts, its stages |
| worker | `attempt` | a custom target's attempt, for the search to rank in order |
| window | `stop`, `cancel` | the search ends |
| worker | `result` | the device's final best and counts |
| worker | `error` | why the job failed or was cancelled |

Over the internet a round trip is 20 to 200 ms while a GPU batch is about
10 ms, so the worker asks for claims a block ahead and keeps the last
baseline it was pushed: the GPU never waits for the network. A best carries
no car state, physics snapshot or timeline: the window rebuilds them by
simulating the inputs.

### Groups

The relay's groups (`relay/src/group.ts`) are built so that neither the
window's load nor the relay's traffic grows with the number of machines.

- **The coordinator** (one Durable Object per group) keeps the members and
  invites, each search's job and overall best, which machine searches for
  which search, and credit. A searching window connects to it.
- **Shards** (Durable Objects of up to 200 machines each) hold the machines'
  connections. A machine connects to the shard the coordinator places it in;
  a shard fetches a search's job once for all its machines and passes their
  messages up in one request every 100 ms.

A search, step by step:

1. **Start.** The window sends the job once. The coordinator gives the search
   machines (a free machine goes to the search with the fewest; a new search
   takes machines from the one with the most until the shares are even: a
   machine moved hands in its count and takes the other job).
2. **Candidates.** Each part a machine lends (each GPU, its CPU threads) gets
   its own range of candidate numbers (`GroupSearch`,
   `src/app/remote/group_search.*`) and searches on its own, as a ForeverTAS
   started there would. Nothing is claimed while it runs.
3. **Silence.** A machine reports its speed once it has warmed up (and again
   when it doubles or halves), then sends nothing until it has a best worth
   sending. The window's attempt counter is the machines' speed times their
   time, corrected by the counts they hand in.
4. **Bests.** A machine sends a best only if it beats the search's overall
   best it knows, at most once per waiting period: at the end of a period it
   sends its latest best, if that still beats the overall one (improvements
   in between are superseded, not lost). The period is the search's machine
   count divided by a budget of 20 bests a second, at least one second, with
   a random offset per machine.
5. **Overall best.** The window checks one best at a time, always the best
   waiting, on its own thread with the map kept loaded. A new overall best
   goes down to every machine of the search (at most once a period, longer
   as the search has more machines): with promotion on, a machine adopts it
   as its baseline when it beats its own; either way it sends nothing that
   does not beat it.
6. **End.** Stop goes down to every machine; each hands in its count, and its
   best if it still beats the overall one. Credit is the counts handed in
   (speed times time for a machine that vanished).

A machine that dies is noticed when its connection closes. A search whose
window is gone stops after 60 s, unless the window comes back and carries
on; machines that never answer a stop are freed after 30 s. Idle connections
are kept by `ping` / `pong`, which Cloudflare answers without waking the
relay.

Limits: in the public pool, two searches per member and 2000 machines per
search; per address, 20 new groups or memberships and 600 connections a
minute; messages up to 256 KB, jobs up to 1.9 MB.

Unlike rooms, a group's results are not reproducible run to run (machines
come and go), and a custom target is ranked per machine: each sends only its
improvements.

### The relay's group API

| Request | What |
| --- | --- |
| `POST /groups` `{name, member, secret, memberName}` | a new group: `{id, invite}` |
| `POST /groups/ID/join` `{invite, member, secret, memberName}` | membership (`public`: no invite) |
| `POST /groups/ID/invite` `{member, secret}` | the owner's new invite |
| `POST /groups/ID/leave` `{member, secret}` | membership ends |
| `GET /groups/ID/status?member&secret` | name, machines, searches, shards, credits |
| `WS /groups/ID/worker?member&secret` | a lent machine (placed in a shard) |
| `WS /groups/ID/client?member&secret` | a searching window (the coordinator) |

A window sends `{type: "search", id, job}` (`{type: "search", id, resume:
true}` when it connects again), `{type: "best", sample}` (a best it checked,
sent down to the machines) and `{type: "stop"}`. It gets `searching`,
`machines {count, perSecond}` (at most once a second), `batch {bests, finals,
failed, left}` (one message a batch), `resumed`, `gone` (the search ended
while it was away), `ended` and `error`.

A machine sends `hello`, `speed {search, perSecond}`, `best {search, live}`,
`final {search, iterations, seconds, live?}`, `error`, and `busy` / `free`
(searching for an earlier place). It gets `job {search, slot, cooldownMs,
best, job}`, `overall {search, best}`, `cooldown {search, cooldownMs}` and
`stop {search, moved?}`.

### The worker kit

`worker/forevertas_worker.py` (standard library only) fetches a release of
ForeverTAS (`--version`, default the latest) and the GPU module for the card
it finds, unpacks the AppImage (notebooks and containers have no FUSE) and
runs `ForeverTAS --worker`. The notebooks and the Dockerfile run it.
`release.py publish` uploads the script and both notebooks with each
release, and the release notes link the Colab notebook of that version.

For developers, `--release FOLDER` runs a build's files from a folder (the
AppImage, `updates.json` and the modules, as `release.py build` writes them)
instead of a published release, and `--app` runs a given ForeverTAS.

### Checks

- `forevertas-remote-tests PACKS REPLAY [APP]`: a search on a remote worker
  gives exactly the result of the same search here (score, inputs, winning
  iteration and count), GPU or CPU, exact and approximate; searches on
  several devices run to their end; a worker with other packs refuses. Modes
  (environment): `FTAS_REMOTE_RELAY` (through a room on the relay),
  `FTAS_REMOTE_CPU`, `FTAS_REMOTE_AS_APP` (the device as the window makes
  it), `FTAS_REMOTE_EXTERNAL` (a worker started on another computer),
  `FTAS_REMOTE_GROUP` (lent machines on a new group: `FTAS_REMOTE_LEND`,
  `FTAS_REMOTE_APPROXIMATE`), `FTAS_REMOTE_GROUP_EXTERNAL`,
  `FTAS_REMOTE_LENDING` (the window's side, through a dropped connection),
  `FTAS_REMOTE_MULTI` (one machine in a group and the public pool) and, on
  Windows, `FTAS_REMOTE_SANDBOX_PROBE`.
- `relay/check.mjs` (rooms) and `relay/group_check.mjs [BASE]` (groups:
  membership and invites, a search's events, moved machines, a window back
  within its grace, the public pool's caps, credit), on the live relay or on
  `npx wrangler dev` (`http://127.0.0.1:8787`).
- `relay/load_test.mjs BASE MACHINES SECONDS`: simulated machines improving
  about 100 times a second at first, sharing as the app's do. With 200, 1000
  and 3000 machines on `wrangler dev` (and 500 on the live relay), the relay
  received 20 to 28 bests a second (budget 20), the window 8 to 25 messages
  a second whatever the count, and the window's best was the best any
  machine found.
