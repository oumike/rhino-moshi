# Rhino Companion — Moshi client for the Xteink X4

> **Status: design.** This branch is a fresh start. Only the X4 board
> definition, the button driver and the flashing tool carry over from `main`.
> This README is the plan for review before any code is written.

Turn an [Xteink X4](https://www.xteink.com/products/xteink-x4) into a desk
console for coding agents running under [Moshi](https://getmoshi.app) and
[herdr](https://github.com/herdrdev/herdr) (or tmux). The X4 talks **directly to
the `moshi-hook` daemon** already running on your other machines. Nothing else
is installed on the host.

**Jobs**, in priority order:

1. **Monitor:** see every workspace and agent on a host, and which ones are
   working, blocked, done or idle, at a glance.
2. **Unblock:** approve or deny tool permissions, pick a plan-menu option, and
   answer single-choice questions.
3. **Nudge** (secondary): send Esc / interrupt, send a canned prompt, or type a
   short prompt on an on-screen keyboard.
4. **Usage:** show agent rate-limit windows (5h / weekly).

## Host setup

`moshi-hook serve` runs a gateway (HTTP + WebSocket) that listens on
`127.0.0.1:24543` by default. Only programs on the same machine can reach it.
For the X4 to reach it, bind it to an address the X4 can see, in
`~/.config/moshi/config.toml`:

```toml
[gateway]
listen = "192.168.1.20:24543"   # this machine's LAN or Tailscale address
```

Then run `brew services restart moshi-hook`.

> **Warning:** the gateway has **no authentication**. That's by design, because
> Moshi expects it to be reached only over SSH. Anyone who can reach that
> address can approve tool calls and type into your agents' terminals. Bind it
> to an interface on a network you trust, never `0.0.0.0` on shared Wi-Fi, and
> firewall the port to the X4's IP if you can.

## Constraints that shape the design

| Constraint | Consequence |
|---|---|
| The gateway speaks its full API: multi-KB workspace frames and agent-status frames with plan markdown. | The firmware parses with ArduinoJson **filters**, keeping only the fields it draws, and discards everything else while parsing. |
| ESP32-C3: ~320 KB RAM, no PSRAM, single core. | **One WebSocket** to **one host** at a time. Frame-size limits are enforced, and oversized frames are dropped with an on-screen notice rather than crashing. |
| An `/events` agent watch follows **one agent session**. | The device re-points its single agent watch at whichever agent needs it most (see below). |
| E-paper: ~1.7 s full refresh, partial refresh for small regions. | Redraws only when something a human cares about changes. Ephemeral "Thinking… (12s)" lines are throttled. |
| Six usable buttons plus power, no touch. | List navigation plus contextual soft keys. Typing uses an on-screen keyboard. |
| No discovery protocol in moshi-hook. | Hosts are configured on the device (name + address:port) through the setup portal or the on-screen keyboard. |

## How the firmware uses the gateway

```
 laptop                                   Xteink X4
┌──────────────────────────────┐
│ herdr / tmux ── agents       │        ┌───────────────────────┐
│ moshi-hook serve             │◀═══════│ /events WebSocket     │  one host
│   gateway 192.168.1.20:24543 │◀───────│ HTTP POSTs (answers)  │  at a time
└──────────────────────────────┘        └───────────────────────┘
 desktop: same, configured on the device, connected only when selected
```

| Purpose | Gateway call |
|---|---|
| Compatibility probe | `GET /v1/version`: check `capabilities` (need `events.watch.workspaces`, `events.watch.agent-status`, `approvals.answer`) |
| Workspace / agent tree | `/events` watch `{"workspaces": true, "usage": true}` |
| Pending approval, plan menu or question | the same socket's `agent` watch, `{"source", "session"}`, giving `agentStatus` frames |
| Approve / deny | `POST /v1/approvals/answer` with `actionId` |
| Plan menu choice | `POST /v1/plans/answer` (echoes title, plan and options from `agentStatus`) |
| Single-choice question | `POST /v1/questions/answer` (only when every question is single-select) |
| Esc / interrupt | `POST /v1/keys` `["Escape"]`, `["C-c"]` |
| Prompt (canned or typed) | `POST /v1/prompt` |
| Pick herdr session | `GET /v1/muxes`, then `mux=` on the watch |

### Following blocked agents with one watch

The workspace tree reports per-tab `agentStatus`, but the *content* of a
blocked agent's request (`pendingApproval`, `planMenu`, `pendingPrompt`) only
arrives on an agent watch. With one socket, the device points its agent watch
at:

1. the agent whose detail page is open, else
2. the agent that has been blocked longest, else
3. nothing.

After that agent is answered or unblocks, the watch moves to the next blocked
agent. The agents list still shows *every* blocked agent from the tree, and the
card screen shows "1 of N waiting".

### Safety rules

- **Never guess an answer.** Answers are sent with the exact `actionId`,
  `toolUseId`, menu or questions the gateway published. The gateway verifies
  the terminal screen before typing anything; a `409` means "changed", so the
  card is dropped as stale and never retried.
- **No automatic retries** for prompts or keys (per the gateway contract).
- **Answered elsewhere wins:** when `pendingApproval` or `pendingPrompt`
  disappears from the agent's status updates, the card closes immediately.
- Multi-select or multi-question forms are shown read-only, with "answer in
  terminal".

## Firmware

PlatformIO + Arduino, reusing `board.h`, `buttons.*`, GxEPD2 and ArduinoJson
from `main`. It adds a WebSocket client (e.g. `links2004/WebSockets`).
WiFiManager handles first-boot Wi-Fi setup and the first host entry.

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
│ ✓•docs       claude 2h │   │                        │   │                        │
│ · scratch    shell     │   │                        │   │                        │
├────────────────────────┤   ├────────────────────────┤   ├────────────────────────┤
│ Hosts│ Open │ Usage│Menu│   │ Back │ Deny │Approve│  │   │ Back │ Esc │Prompt│Keys│
└────────────────────────┘   └────────────────────────┘   └────────────────────────┘
```

- **Agents:** sorted attention-first (blocked, then done, then by
  `statusChangedAt`). An agent that finishes is re-sorted and marked unseen
  (`•`) until you open it. There's no pop-up.
- **Card:** takes over when the watched agent has a pending request, unless
  you're typing. Soft keys map to options. Plans and questions with more than
  three options use Up/Down to select and Confirm to send.
- **Agent detail:** status, title, latest status line, plus Esc / interrupt /
  prompt actions.
- **Prompt:** canned prompts stored on the device, plus "Type…", which opens
  the keyboard.
- **Keyboard:** a 4-row grid. Up/Down change rows, Left/Right move along a row,
  Confirm types, Back deletes, and holding Back cancels. Meant for short
  prompts.
- **Hosts:** configured hosts with reachability. Confirm switches hosts,
  closing the current connection first.
- **Usage:** per-agent rate-limit windows with reset times.
- **Settings:** rotation, button calibration, hosts, canned prompts, Wi-Fi
  reset.

### Refresh policy

- Partial refresh for list rows, the header and the cursor. Full refresh on
  screen changes and every N partials.
- Ephemeral status lines update at most every 10 s, and only with a partial
  refresh.
- Wi-Fi stays associated while connected. USB power is expected for all-day
  use.

## Risks to verify early

- **Frame sizes:** measure real `workspaces` and `agentStatus` frames
  (`websocat ws://<host>:24543/events`) against C3 heap. Plan text in
  `pendingPrompt` is the likely worst case, and `/v1/plans/answer` needs it
  echoed back.
- **Re-watch semantics:** confirm that sending a new `watch` with a different
  `agent` replaces the previous agent watch on the same socket.
- **Non-loopback binding:** confirm the gateway accepts requests when bound
  to a LAN address (no Host/Origin checks rejecting the X4).

## Build order

1. **Spike:** bind a gateway to the LAN and capture frames; check the three
   risks above.
2. **Firmware skeleton:** Wi-Fi, host config, `/v1/version` probe, `/events`
   connect, agents list.
3. **Cards:** approve/deny, plan, question.
4. **Detail, keys and prompts:** canned prompts, then the on-screen keyboard.
5. **Usage**, host switching, settings, refresh tuning.

## Decisions

- **No host-side software.** The X4 talks to stock `moshi-hook` directly. The
  user makes the gateway reachable and accepts that it has no authentication.
- **Multiple machines:** each runs its own moshi-hook. The device connects to
  one at a time and switches between them.
- **Finished agents:** re-sorted and marked as unseen, with no pop-up.
- **Rotate / calibrate:** device settings menu. `docs/flashing.md` must drop
  the old `rhino-companion` CLI commands.
