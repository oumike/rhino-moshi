# Rhino Companion — Moshi client for the Xteink X4

> **Status: design.** This branch is a fresh start. Only the X4 board
> definition, the button driver and the flashing tool carry over from `main`.
> Everything below is the plan and is open for review before any code is written.

Turn an [Xteink X4](https://www.xteink.com/products/xteink-x4) into a desk
console for coding agents running under [Moshi](https://getmoshi.app) and
[herdr](https://github.com/herdrdev/herdr) (or tmux). It's for keeping an
eye on what your agents are doing and unblocking them without reaching for the
terminal or phone.

**Primary jobs**, in priority order:

1. **Monitor:** see every workspace and agent on a host, and which ones are
   working, blocked, done or idle, at a glance.
2. **Unblock:** approve or deny tool permissions, pick a plan-menu option, and
   answer single-choice questions.
3. **Nudge** (secondary): send Esc / interrupt, send a canned prompt, or type a
   short prompt on an on-screen keyboard.
4. **Usage:** show agent rate-limit windows (5h / weekly).

## Constraints that shape the design

| Constraint | Consequence |
|---|---|
| The moshi-hook gateway (`127.0.0.1:24543`) is **loopback-only, no auth**; Moshi clients reach it over SSH. | The ESP32-C3 can't realistically run SSH, so a small **host service** runs next to moshi-hook and bridges to the LAN. |
| ESP32-C3: ~320 KB RAM, no PSRAM, single core. | The device must not parse full `/events` frames, workspace trees or transcripts. The host service sends **small, pre-digested view models**. |
| E-paper: ~1.7 s full refresh, partial refresh for small regions. | The screen only redraws when something a human cares about changes. Agent status changes are coalesced; `ephemeral` "Thinking… (12s)" lines are throttled or dropped. |
| Six usable buttons plus power (4 under the screen, 2 on the right edge), no touch. | Everything is list navigation plus contextual soft keys. Typing uses an on-screen keyboard. |
| Several hosts, but **one connection at a time** | The device picks one host and holds a single connection to it, then switches explicitly. |

## Architecture

```
 host A (laptop)                                         Xteink X4
┌──────────────────────────────────────────┐
│ herdr / tmux ── agents (claude, codex…)  │
│        │ hooks                           │
│ moshi-hook serve  127.0.0.1:24543        │
│        ▲  /events WS, /v1/* HTTP         │
│        │  (loopback)                     │
│ rhino-host  0.0.0.0:24580 ◀═════════════════════ WebSocket (device-initiated,
│   • watches workspaces, usage            │        token auth)
│   • per-blocked-agent agentStatus watch  │       ┌──────────────────────┐
│   • builds view models, sends answers    │       │ host list (mDNS)     │
│   • advertises _rhino._tcp via mDNS      │       │ one live connection  │
└──────────────────────────────────────────┘       └──────────────────────┘
 host B (desktop) — same stack, discoverable, not connected until chosen
```

### Why a "smart" host service

Two other designs were considered and rejected:

- **Thin proxy:** the device would speak the gateway API directly. That puts
  JSON parsing of multi-KB workspace trees on the C3 and couples the firmware
  to every additive API change.
- **Expose the gateway directly** (socat, Tailscale): the gateway has no auth by
  design, and it can type into terminals.

The host service owns everything protocol-shaped: capability checks, staleness
(`409`), which agent session a card belongs to, and text truncation and
wrapping hints. The firmware owns rendering and input only. When the gateway
API changes, only Python changes.

### Connection direction

The **device connects to the host** (the reverse of `main`):

- It enables "one connection at a time, switchable": the device chooses.
- The host never needs to know the device's IP.
- A sleeping device costs the host nothing. The host service runs whether or
  not a device is attached.

Each host runs `rhino-host`, which advertises `_rhino._tcp` over mDNS (name,
port, protocol version). The device lists discovered hosts plus any it has
already paired, so a manually added IP still works when mDNS is blocked.

### Pairing and auth

The LAN listener can approve tool calls and type into terminals, so it
requires a token:

1. Choose an unpaired host on the device. It asks `rhino-host` to start pairing.
2. `rhino-host` prints a 6-digit code in its log/terminal (`rhino-host pair`
   shows it on demand).
3. Enter the code with the on-screen keyboard. The host returns a long random
   per-device token, stored in device NVS keyed by host ID.
4. Every later connection presents the token. `rhino-host devices` lists
   paired devices and `rhino-host revoke <id>` revokes one.

Traffic is plain `ws://` on the LAN. The token prevents casual misuse but not
sniffing. TLS on the C3 is possible but costs RAM; it's deferred (see open
questions).

## Host service: `rhino-host`

Python 3.11+, standard library plus one WebSocket library. It runs as a
launchd/systemd user service alongside `moshi-hook`.

### Upstream (to moshi-hook, loopback)

| Purpose | Gateway call |
|---|---|
| Compatibility probe | `GET /v1/version`: require the `approvals.answer` and `events.watch.workspaces` capabilities |
| Workspace / agent tree | `/events` watch `{workspaces: true, usage: true}` |
| Pending approval, plan menu or question for an agent | A second `/events` connection per **blocked** (or focused-on-device) agent, watching `{agent: {source, session}}` |
| Approve / deny | `POST /v1/approvals/answer` with `actionId` |
| Plan menu choice | `POST /v1/plans/answer` (echoes the full menu and plan from `agentStatus`) |
| Single-choice question | `POST /v1/questions/answer` (only when every question is single-select) |
| Esc / interrupt | `POST /v1/keys` `["Escape"]`, `["C-c"]` |
| Prompt (canned or typed) | `POST /v1/prompt` |
| Herdr session choice | `GET /v1/muxes`, `mux=` on the watch |

Agent watches are opened when a tab turns `blocked`, and while the device is
viewing that agent's detail page. They're closed shortly after the agent leaves
both states, which keeps upstream sockets bounded to "agents that need you".

### Downstream (to the device)

These are JSON frames over one WebSocket. Every frame is a complete
replacement for its slot (no diffs), sized for the C3, with strings
pre-truncated.

```jsonc
// host → device
{ "t": "hello", "host": "laptop", "proto": 1, "caps": ["usage", "keys", "prompt"] }
{ "t": "agents", "rev": 41, "items": [
    { "id": "wB:t1", "ws": "app-moshi", "agent": "claude", "st": "blocked",
      "title": "Fix login redirect loop", "age": 312, "ctx": 42 },
    { "id": "wC:t2", "ws": "api", "agent": "codex", "st": "working",
      "title": "Add pagination", "age": 45, "line": "Running tests…" } ] }
{ "t": "card", "id": "c17", "agent": "wB:t1", "kind": "approval",   // approval|plan|question|notice
  "title": "Bash", "body": "touch /tmp/probe",
  "options": ["Deny", "Approve"] }
{ "t": "card_clear", "id": "c17", "why": "answered|stale|elsewhere" }
{ "t": "usage", "items": [ { "agent": "claude", "label": "Max",
    "windows": [ { "l": "5h", "pct": 42, "resets": 9300 } ] } ] }
{ "t": "result", "req": 5, "ok": false, "msg": "Changed on screen; not sent" }

// device → host
{ "t": "answer", "req": 5, "card": "c17", "option": 1 }
{ "t": "keys",   "req": 6, "agent": "wC:t2", "keys": ["Escape"] }
{ "t": "prompt", "req": 7, "agent": "wC:t2", "text": "run the tests" }
{ "t": "view",   "agent": "wC:t2" }   // device is looking at this agent; keep its watch open
```

The card queue lives on the host, ordered blocked-longest-first. The device
shows one card at a time, plus a count of how many are waiting.

### Safety rules

- **Never guess an answer.** Every answer is relayed with the exact
  `actionId`, `toolUseId` or menu that the gateway published. The gateway's
  on-screen verification and `409` are the backstop. A `409` clears the card
  as stale and never retries.
- **No automatic retries** for prompts or keys (per the gateway contract).
- **Answered elsewhere wins.** When the agentStatus frame drops
  `pendingApproval` or `pendingPrompt`, the card is cleared immediately with
  `why: "elsewhere"`.
- Questions with multi-select or more than one question are shown read-only,
  with "answer in terminal".

## Firmware

Built on PlatformIO + Arduino, reusing `board.h`, `buttons.*`, GxEPD2 and
ArduinoJson from `main`. WiFiManager handles first-boot Wi-Fi setup.

### Screens

```
 Agents (home)                 Card                          Agent detail
┌────────────────────────┐   ┌────────────────────────┐   ┌────────────────────────┐
│ laptop      ●  5h 42%  │   │ laptop    1 of 3 ⧗ 5m  │   │ app-moshi / 1   claude │
│────────────────────────│   │────────────────────────│   │ Fix login redirect     │
│▶! app-moshi  claude 5m │   │ app-moshi · claude     │   │ blocked 5m · ctx 42%   │
│   Fix login redirect   │   │ Bash                   │   │────────────────────────│
│ ~ api        codex  45s│   │                        │   │ Running tests…         │
│   Running tests…       │   │ touch /tmp/probe       │   │                        │
│ ✓ docs       claude 2h │   │                        │   │                        │
│ · scratch    shell     │   │                        │   │                        │
│                        │   │                        │   │                        │
├────────────────────────┤   ├────────────────────────┤   ├────────────────────────┤
│ Hosts│ Open │ Usage│Menu│   │ Back │ Deny │Approve│  │   │ Back │ Esc │Prompt│Keys│
└────────────────────────┘   └────────────────────────┘   └────────────────────────┘
```

- **Agents:** sorted attention-first (blocked, then done, then by
  `statusChangedAt`). Up/Down move the selection; the header shows host,
  connection and a compact usage figure.
- **Card:** takes over when a card arrives, unless the user is typing. Soft
  keys map to options; plans and questions with more than three options use
  Up/Down to select and Confirm to send.
- **Agent detail:** status, title, latest status line, plus Esc / interrupt /
  prompt actions.
- **Prompt:** a list of canned prompts (synced from host config) plus "Type…",
  which opens the keyboard.
- **Keyboard:** a 4-row grid. Up/Down change rows, Left/Right move along a row,
  Confirm types, Back deletes. A hold on Back cancels, and a soft key toggles
  symbols and sends. Long text is out of scope; this is for "run the tests",
  not essays.
- **Hosts:** discovered + paired hosts. Confirm switches (closing the current
  connection first) or starts pairing.
- **Usage:** per-agent rate-limit windows with reset times.

### Refresh policy

- Partial refresh for list rows, the header, and the selection cursor. Full
  refresh on screen changes and every N partials (ghosting control).
- Agent `line` text (ephemeral status) updates at most every 10 s, and never
  triggers a full refresh.
- Battery: Wi-Fi stays associated while connected. Light sleep is used between
  button events. USB power is expected for all-day use, as on `main`.

## Repository layout (planned)

```
firmware/        PlatformIO project (board.h, buttons.* carried over)
host/            rhino-host Python package + tests (fake gateway, fake device)
tools/           xteink-flash (carried over)
docs/            flashing.md (carried over), protocol.md (device↔host)
```

## Build order

1. **Host service against a fake device:** gateway client, view models,
   answer relay, pairing, mDNS. Test against a recorded or fake moshi-hook
   gateway.
2. **Firmware skeleton:** Wi-Fi, mDNS browse, WebSocket client, agents list.
3. **Cards:** approve/deny, plan, question.
4. **Detail, keys and prompts:** canned prompts, then the on-screen keyboard.
5. **Usage screen**, host switching polish, refresh tuning.

## Open questions

- **TLS** between device and host: is it worth the RAM, or is the token plus
  a trusted LAN enough for v1?
- **Remote hosts:** should `rhino-host` on one machine be able to front
  another machine's gateway via moshi-hook's `/hosts/<name>/` SSH bridge, so
  the device only needs one pairing? That would conflict with "one connection
  at a time" only in spirit.
- **Notices:** should `done` transitions raise a brief card ("api finished"),
  or only re-sort the list?
- **Calibrate/rotate:** `docs/flashing.md` still references the old
  `rhino-companion` CLI commands; they need equivalents (likely device menu
  items).
