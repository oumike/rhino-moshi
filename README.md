# Rhino Companion — Moshi client for the Xteink X4

> **Status: in development.** This branch is a fresh start. Only the X4 board
> definition, the button driver and the flashing tool carry over from `main`.
> This README is the design; the build order at the end tracks progress.

Turn an [Xteink X4](https://www.xteink.com/products/xteink-x4) into a desk
console for coding agents running under [Moshi](https://getmoshi.app) and
[herdr](https://github.com/herdrdev/herdr) (or tmux). The X4 talks **directly to
the `moshi-hook` daemon** already running on your other machines, over SSH, the
same way the Moshi app does. Nothing is installed on the host; you only
authorize the X4's SSH key.

**Jobs**, in priority order:

1. **Monitor:** see every workspace and agent on a host, and which ones are
   working, blocked, done or idle, at a glance.
2. **Unblock:** approve or deny tool permissions, pick a plan-menu option, and
   answer single-choice questions.
3. **Nudge** (secondary): send Esc / interrupt, send a canned prompt, or type a
   short prompt on an on-screen keyboard.
4. **Usage:** show agent rate-limit windows (5h / weekly).

## Host setup

Nothing is installed on the host, and moshi-hook keeps its default settings.
Its gateway stays on `127.0.0.1:24543`, unreachable from the network. Like the
Moshi app, the X4 reaches it through **SSH**: it logs in with its own key and
opens a tunnel (an SSH `direct-tcpip` channel, the same thing `ssh -W` does) to
`127.0.0.1:24543` on the host.

1. **Enable SSH** on the host if it isn't already (macOS: System Settings →
   General → Sharing → Remote Login).
2. **Authorize the X4's key.** On first boot the X4 generates an ed25519 key
   pair. The private half never leaves the device. Open the X4's settings page
   (see [Firmware](#firmware)), copy the `authorized_keys` line it shows, and
   append it to `~/.ssh/authorized_keys` on each host:

   ```
   restrict,port-forwarding,permitopen="127.0.0.1:24543",command="false" ssh-ed25519 AAAA… rhino-x4-3fa2
   ```

   The options limit this key to exactly one thing: a tunnel to the moshi-hook
   gateway. `restrict` turns off every OpenSSH feature (pty, agent/X11
   forwarding, other forwards). `port-forwarding` plus `permitopen` re-enable
   only the gateway tunnel, and `command="false"` makes any attempt to run a
   shell or command exit immediately. Requires OpenSSH 7.2+. A stolen X4 can do what the X4 already
   does, and nothing more. To revoke it, delete the line.
3. **Add the host on the X4:** a name, address, SSH port and user, entered on
   the settings page. Up to four hosts can be set up; Menu → Hosts on the X4
   switches between them.
4. **Confirm the host key** on first connect. The X4 shows the host's SHA256
   fingerprint. Compare it with `ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub`
   on the host, then press Trust. The key is pinned. If it later changes, the
   X4 refuses to connect and shows both fingerprints. It never re-trusts
   silently.

## Security model

| Threat | Mitigation |
|---|---|
| Someone on the LAN talks to moshi-hook | The gateway never leaves loopback. Only SSH is exposed, and SSH already guards the machine. |
| Network eavesdropping or tampering | All traffic runs inside SSH (encrypted and integrity-checked). |
| Spoofed host / man-in-the-middle | The host key is pinned on first use, and any change blocks the connection. |
| Lost or stolen X4 | The key is restricted to `permitopen="127.0.0.1:24543"`. Remove its `authorized_keys` line on each host. |
| Someone reads the key off the X4 | The private key lives in NVS. Enabling ESP32 flash encryption is optional hardening; without it, physical access is game over, and the restriction above bounds the damage. |
| Settings page abuse | The page is open on the LAN by default; set a page password on it to require HTTP Basic auth. The open setup access point runs only on a fresh install or with Back held at boot (physical access). The page shows the public key and accepts settings; the private key can't be read or exported. |

## Constraints that shape the design

| Constraint | Consequence |
|---|---|
| The gateway speaks its full API: multi-KB workspace frames and agent-status frames with plan markdown. | The firmware parses with ArduinoJson **filters**, keeping only the fields it draws, and discards everything else while parsing. |
| ESP32-C3: ~320 KB RAM, no PSRAM, single core. SSH (libssh + mbedTLS) takes a large share. | **One SSH session** to **one host** at a time, carrying one WebSocket channel plus short-lived HTTP channels. Frame-size limits are enforced, and oversized frames are dropped with an on-screen notice rather than crashing. |
| An `/events` agent watch follows **one agent session**. | The device re-points its single agent watch at whichever agent needs it most (see below). |
| E-paper: ~1.7 s full refresh, partial refresh for small regions. | Redraws only when something a human cares about changes. Ephemeral "Thinking… (12s)" lines are throttled. |
| Six usable buttons plus power, no touch. | List navigation plus contextual soft keys. Typing uses an on-screen keyboard. |
| No discovery protocol in moshi-hook. | Hosts are configured on the device through the settings page. |

## How the firmware uses the gateway

```
 laptop                                          Xteink X4
┌─────────────────────────────────────┐
│ herdr / tmux ── agents              │       ┌──────────────────────────┐
│ moshi-hook serve                    │       │ SSH client (libssh)      │
│   gateway 127.0.0.1:24543 ◀── sshd ◀════════│  ├ channel: /events WS   │  one host
│   (loopback only)        :22        │  SSH  │  └ channel: HTTP POSTs   │  at a time
└─────────────────────────────────────┘       └──────────────────────────┘
 desktop: same, configured on the X4, connected only when selected
```

Every gateway call below travels over a `direct-tcpip` channel to
`127.0.0.1:24543` inside the one SSH session. The `/events` WebSocket gets one
long-lived channel; each HTTP POST opens a short-lived channel, which costs
almost nothing on an established session.

| Purpose | Gateway call |
|---|---|
| Compatibility probe | `GET /v1/version`: check `capabilities` (need `events.watch.workspaces`, `events.watch.agent-status`, `approvals.answer`) |
| Workspace / agent tree | `/events` watch `{"workspaces": true, "usage": true}` |
| Pending approval, plan menu or question | the same socket's `agent` watch, `{"source", "session"}`, giving `agentStatus` frames |
| Approve / deny | `POST /v1/approvals/answer` with `actionId` |
| Plan menu choice | `POST /v1/plans/answer` (echoes title, plan and options from `agentStatus`) |
| Single-choice question | `POST /v1/questions/answer` (only when every question is single-select) |
| Keys (Esc, interrupt, Enter, Shift-Tab, Tab, Up, Down) | `POST /v1/keys` with one of `Escape`, `C-c`, `Enter`, `BTab`, `Tab`, `Up`, `Down` |
| Prompt (canned or typed) | `POST /v1/prompt` |
| Pick herdr session | `GET /v1/muxes`, then `mux=` on the watch |

### Following blocked agents with one watch

The workspace tree reports per-tab `agentStatus`, but the *content* of a
blocked agent's request (`pendingApproval`, `planMenu`, `pendingPrompt`) only
arrives on an agent watch. With one socket, the device points its agent watch
at:

1. the agent whose detail page or card is open, else
2. the agent that has been blocked longest, else
3. nothing.

After that agent is answered or unblocks, the watch moves to the next blocked
agent. If the watched agent has nothing answerable (dismissed, or a form the
X4 can't answer), the watch rotates through the blocked agents every 10 s. The
agents list still shows *every* blocked agent from the tree, and the card
header shows "1 of N".

### Safety rules

- **Never guess an answer.** Answers are sent with the exact `actionId`,
  `toolUseId`, menu or questions the gateway published. The gateway verifies
  the terminal screen before typing anything; a `409` means "changed", so the
  card is dropped as stale and never retried.
- **No automatic retries** for prompts or keys (per the gateway contract).
- **Answered elsewhere wins:** when `pendingApproval` or `pendingPrompt`
  disappears from the agent's status updates, the card closes immediately.
- **Only blocked agents have cards.** A request can outlive its dialog (an
  answered plan leaves an `ExitPlanMode` approval in the status while the
  agent works), so a card shows only while the agent is blocked, per either
  the `agentStatus` frame or the workspace tree. The tree matters because the
  frame's status reverts to "working" 30 minutes into a block.
- Question forms with up to 8 single-select questions are answered on the X4.
  Multi-select questions, larger forms, or requests missing a `toolUseId` or
  plan text are shown read-only, with "answer in terminal".

## Firmware

PlatformIO + Arduino, reusing `board.h`, `buttons.*`, GxEPD2 and ArduinoJson
from `main`. It adds:

- **[LibSSH-ESP32](https://github.com/ewpa/LibSSH-ESP32)** (libssh + mbedTLS,
  supports the C3) for the SSH session, ed25519 keys and `direct-tcpip`
  channels (`ssh_channel_open_forward`).
- A small HTTP/1.1 and WebSocket client written against a byte-stream
  interface, so it runs over an SSH channel instead of a `WiFiClient`.
  (Alternative: a loopback listener on the X4 that relays to the channel, so
  stock libraries work unchanged. The spike decides.)
- **Settings page:** a plain web page at `http://rhino-x4.local` on the LAN.
  It handles Wi-Fi credentials, shows the `authorized_keys` line, takes up to
  four host entries (name, address, port, user) and which one to connect to,
  shows and forgets each host's pinned key,
  sets orientation (portrait by default), edits the canned prompts (one per
  line, up to 20 of 300 characters), and sets an optional page password
  (HTTP Basic auth, user `admin`; none by default).
- **Setup mode:** with no saved Wi-Fi (fresh install), or with Back held at
  boot, the X4 runs an open access point `Rhino-X4-Setup` and serves the same
  page at `http://192.168.4.1`. There is no captive portal: join and browse
  there yourself. Saving restarts the X4 onto the saved network.
- **Backup/restore:** the settings page downloads and restores a JSON backup
  in the browser, and writes or reads one on the SD card at
  `/moshi/rhino-x4-backup.json`. A backup holds every setting (all hosts in
  `hosts`, plus the active one in `host` for older firmware), including the
  Wi-Fi and page passwords and the X4's SSH private key, so a restored X4
  needs no re-authorizing on hosts. The file must be kept private.

### Screens

```
 Agents (home)                 Card                          Agent detail
┌────────────────────────┐   ┌────────────────────────┐   ┌────────────────────────┐
│ laptop   5h 42% online │   │ laptop    1 of 3 ⧗ 5m  │   │ app-moshi / 1   claude │
│────────────────────────│   │────────────────────────│   │ Fix login redirect     │
│▶! app-moshi  claude 5m │   │ app-moshi · claude     │   │ opus · 42% context left│
│   Fix login redirect   │   │ Bash                   │   │────────────────────────│
│ ~ api        codex  45s│   │                        │   │ > Running tests…       │
│   Running tests…       │   │ touch /tmp/probe       │   │                        │
│ ✓•docs       claude 2h │   │                        │   │                        │
│ · scratch    shell     │   │                        │   │                        │
├────────────────────────┤   ├────────────────────────┤   ├────────────────────────┤
│ Menu │ Open │ Prev│Next │   │ Back │ Deny │Approve│  │   │ Back │Answer│Keys│Prompt│
└────────────────────────┘   └────────────────────────┘   └────────────────────────┘
```

- **Agents:** sorted attention-first (blocked, then done, then by
  `statusChangedAt`). An agent that finishes is re-sorted and marked unseen
  (`•`) until you open it. There's no pop-up. The header shows the host, its
  most-used rate-limit window and the connection state. Back opens the Menu
  (also from the connection status screen, so you can switch away from an
  unreachable host).
- **Card:** takes over from the agents list or detail when the watched agent
  has a pending request, unless dismissed. Back dismisses it until the agent
  blocks again (Confirm on the agent reopens it). Right jumps to the next
  blocked agent. A notice bar reports "Sent", "Changed on the host" (409) or
  "Answered on the host".

  | Card | Confirm | Left | Up/Down |
  |---|---|---|---|
  | Approval | Approve | Deny | Scroll |
  | Plan | Send option | Page the plan | Select option |
  | Question | Choose (Send on the last) | Previous question | Select option |
- **Agent detail:** status, title, model and context left, and the live
  activity line (redrawn at most every 10 s). Answer reopens a pending card,
  Keys and Prompt open the screens below (only for agents with a session).
- **Keys:** Esc, Ctrl-C, Enter, Shift-Tab, Tab, Up and Down. Confirm sends the
  selected key and stays on the screen, so Shift-Tab can be pressed again to
  cycle Claude's mode. The gateway doesn't report the mode, so the X4 can't
  show it.
- **Prompt:** "Type a prompt…", which opens the keyboard, then the canned
  prompts (edited on the settings page). Confirm sends a canned prompt; Right
  opens it in the keyboard to edit first.
- **Keyboard:** a 5-row grid: digits, three letter rows, then Shift, a
  symbols layer, space and Send. Up/Down change rows, Left/Right move along a
  row (both wrap), Confirm types, Back deletes, and holding Back cancels.
  Meant for short prompts.
- **Menu:** Usage, Hosts, Device, Flip screen and Restart. Cards don't take
  over the Menu and its screens.
- **Usage:** each account moshi-hook reports (agent, masked account label,
  when it was captured), with every window's use as a bar and its reset time.
  Up/Down scroll when there are more accounts than fit.
- **Hosts:** the configured hosts, marking the current one. Confirm switches,
  closing the current SSH session first and remembering the choice; on the
  current host it retries a failed connection. A new host shows its
  fingerprint for Trust.
- **Device:** settings page address, Wi-Fi network and signal, the X4's key
  fingerprint, battery voltage, free memory, uptime and build date.
- **Flip screen** turns the display upside down (saved; also on the settings
  page). Up/Down swap with it, so Up stays the upper key.
- Not on the device: regenerating the key (every host would have to
  re-authorize) and changing Wi-Fi, which is setup mode (Back held at boot).

### Refresh policy

- Partial refreshes (~0.6 s) for everything, including screen changes. A
  full refresh (~1.8 s, with a flash) clears the ghosting partials leave:
  after 15 partials at the next screen change, or after 30 anywhere except
  the keyboard.
- Frames go to the panel in one 20 MHz SPI burst rather than byte by byte.
- Buttons are read through a refresh, and presses made during one are replayed
  in order afterwards, so a burst of moves costs one refresh. They are dropped
  if the screen changed under them, or on a decision card.
- Ephemeral status lines update at most every 10 s.
- Wi-Fi stays associated while connected. USB power is expected for all-day
  use.

### Serial debugging

Over USB serial (115200), single characters drive the X4 without touching it:
`b` `c` `l` `r` `u` `d` press Back, Confirm, Left, Right, Up and Down, `h`
holds Back for a second, and `s` dumps the screen as base64 between
`SHOT <width> <height> <rotation>` and `END` (the panel's native 800×480
frame, 1 bit per pixel, set bits white).

## Risks to verify early

- **SSH on the C3:** heap left after Wi-Fi, a libssh session and one
  open channel, plus the time an ed25519/curve25519 handshake takes. LibSSH-ESP32
  supports the C3, but this app also needs GxEPD2's buffers and JSON parsing.
- **Frame sizes:** measure real `workspaces` and `agentStatus` frames against
  the heap left over. Plan text in `pendingPrompt` is the likely worst case,
  and `/v1/plans/answer` needs it echoed back.
- **Re-watch semantics:** confirm that sending a new `watch` with a different
  `agent` replaces the previous agent watch on the same socket.
- **Session-less client:** the X4 is a loopback client with no terminal, so
  it gets the gateway's loopback behavior (herdr by default, `mux=` to pin).
  Confirm that the answer endpoints resolve panes from hook state, as
  documented for loopback callers.

## Build order

1. ✅ **Spike:** use `ssh -W 127.0.0.1:24543 host` plus `websocat` to capture
   frames; on the X4, get libssh to open a channel and fetch `/v1/version`
   while measuring heap.
2. ✅ **Firmware skeleton:** Wi-Fi, key generation, settings page, host key
   pinning (Trust on the X4), SSH session, `/v1/version` probe, `/events`,
   agents list and a read-only agent detail.
3. ✅ **Cards:** approve/deny, plan, question.
4. ✅ **Detail, keys and prompts:** canned prompts, then the on-screen keyboard.
5. ✅ **Usage**, host switching, menu and device info, refresh tuning.

## Decisions

- **No host-side software.** The X4 talks to stock `moshi-hook` directly.
- **Authentication is SSH.** The gateway stays on loopback, and the X4 tunnels
  to it with its own ed25519 key, restricted with `restrict`, `permitopen` and a forced `command`. The key is
  generated on the device and the public half is shown on the settings page.
  Host keys are trusted on first use and then pinned.
- **Multiple machines:** each runs its own moshi-hook. The device connects to
  one at a time and switches between them.
- **Finished agents:** re-sorted and marked as unseen, with no pop-up.
- **Rotation:** on the settings page or Menu → Flip screen. **Button calibration:** none on the
  device; the ladder thresholds in `board.h` are measured on hardware.
  `docs/flashing.md` must drop the old `rhino-companion` CLI commands.
