<div align="center">

<img src="docs/ygopromcp-logo.png" alt="ygopromcp" width="220"/>

# ygopromcp

**A ygopro GUI instance with an MCP (Model Context Protocol) control plane.**

A fork of [fluorohydride/ygopro](https://github.com/Fluorohydride/ygopro) that
exposes the running Yu-Gi-Oh! client as a JSON-RPC/MCP stdio server — an AI
agent (Claude Code, Codex, or any MCP client) reads the board state and plays
the game through the same event receivers a human click uses.

</div>

---

## What it does

The client connects to a YGOPro server (e.g. [srvpro](https://github.com/mycard/srvpro)),
joins a room, and an agent drives it tool-by-tool:

| Tool | Purpose |
|---|---|
| `ygo_state` | Board snapshot + pending prompt, **uniform** `Option N~M/TOTAL:` header + numbered options |
| `ygo_choose` | Answer a prompt: pick `k = randint(N, M)` option ids from `0..TOTAL-1` via `indices` |
| `ygo_log` | Drained narration: turns, phases, draws, summons, attacks, activations, chains, discards, LP events + one-line `[Card]` explains |
| `ygo_card` | Explain a card on one line (ydkshow-style) by name or code |
| `ygo_chat` / `ygo_chatlog` | Send / receive room chat (incl. server announcements) |
| `ygo_surrender` / `ygo_disconnect` | End the duel / leave the room |

Every prompt is rendered the same way:

```
Option 2~3/6:
0. Card A (Lv4) @Hand[0]
1. Card B (Lv4) @Hand[1]
2. Card C (Lv4) @Hand[2]
3. Card D (Lv4) @Hand[3]
4. Card E (Lv4) @Hand[4]
```

The agent never needs game logic: read the header, pick a random (or computed)
subset, send `indices`. Cancel is never an option — a cancelable prompt is
presented as `min=0` and an **empty** `indices=[]` cancels. Invalid choices are
not offered: level-sum (synchro) selects enumerate only valid combinations, and
single-card selects (`MSG_SELECT_UNSELECT_CARD`, used by custom XYZ procedures)
always respond with exactly one card.

The bridge is deliberately dumb about the *client*: choices come from the
game's own parsed state, responses go through the game's own builders
(`SetResponseSelectedCards`, `RespondZones`, `RespondSortOrder`), and a
monotonic STOC sequence counter gates every action so stale prompts are never
re-answered (the source of the classic "invalid operation" rejections).

## Building

Prerequisites (Linux; the fork builds with the same toolchain as upstream):

- C++17 compiler (gcc/clang)
- [premake5](https://github.com/premake/premake-core)
- libevent, sqlite3, Lua 5.3 (headers + lib)
- [mercury233/irrlicht](https://github.com/mercury233/irrlicht) at `./irrlicht`
- nlohmann/json (vendored via premake in this fork)
- submodules: `git submodule update --init --recursive`

```bash
git clone --recursive https://github.com/<you>/ygopromcp
cd ygopromcp
premake5 gmake --lua-include-dir=/usr/include/lua5.3 --lua-lib-name=lua5.3
make -C build config=release -j$(nproc)
# binary: bin/release/YGOPro
```

## Running

```bash
bin/release/YGOPro --mcp -k -n MyName \
  -h 127.0.0.1 -p 7911 -w RoomName -d MyDeck -j
```

| Flag | Meaning |
|---|---|
| `--mcp` | enable the MCP stdio server (takes over stdout; app output goes to stderr) |
| `-k` | keep the client open after a match |
| `-n` | player name |
| `-h` / `-p` | server host / port (default 7911) |
| `-w` | room password (room is created if absent; **rooms persist by name** — use fresh names) |
| `-d` | deck **name without the `.ydk` extension** (the client matches the deck list item) |
| `-j` | auto-join and go straight to the lobby |

Requires a YGOPro server (e.g. srvpro) on the target host. In a password room
the duel starts when the host clicks start; `system.conf` in this fork sets
`chkMAutoPos/chkSTAutoPos/chkRandomPos/chkAutoChain = 1` (auto-place + forced
auto-chain) — keep those for agent play.

## Hooking up an agent

MCP stdio client config (`.mcp.json`):

```json
{
  "mcpServers": {
    "ygopromcp": {
      "type": "stdio",
      "command": "/abs/path/to/YGOPro",
      "args": [
        "--mcp", "-k", "-n", "agent", "-h", "127.0.0.1", "-p", "7911",
        "-w", "room1", "-d", "example", "-j"
      ]
    }
  }
}
```

Agent loop (the entire strategy):

1. `ygo_state` → parse the `Option N~M/TOTAL:` header.
2. Decide `k = randint(N, M)` and pick `k` distinct option ids — or compute a
   real play from the board text + `ygo_card` explains.
3. `ygo_choose {"indices": [ids]}` (a single option may use `{"id": N}`).
4. Drain `ygo_log` for the narration; repeat.

Wire protocol: newline-delimited JSON-RPC 2.0 on stdin/stdout, with
`initialize` → `tools/list` → `tools/call` (standard MCP stdio handshake).
`YGOCLI_MCP_DEBUG=1` prints `[mcp]`/`[stoc]`/`[ctos]` traces to stderr.

## Testing

```bash
# start a server first, then (from ygocli/tests):
python3 tests/fork_aggro_play.py <room-name> <deck.ydk>
```

A pure-random agent plays full games against itself through the bridge; the
test passes when turns advance, LP moves and no deadlock/watchdog fires. A
per-run transcript is written to `/tmp/mcp_transcript.log` (every state read
and choose result, with timestamps).

## Caveats

- The bridge drives **network play only** (lobby → RPS → turn order → duel).
  Deck building, single-player and replay modes are out of scope; replay
  playback narrates the log but is untested.
- Tag duels and match (BO3) siding are unverified.
- A deck rejected by the server shows a modal popup the agent cannot click;
  the reason is surfaced in `ygo_log` as `[!] deck rejected: ...` — switch
  decks rather than waiting.
- Constrained combo selects (tribute etc.) can still be rejected by the core;
  the retry flow re-prompts, and the test watchdog surrenders unsolvable ones.
- Opponent hand contents and draw names are never exposed (count-only), even
  though the ygopro v1 protocol leaks draw codes to the client.
- See [TODO.md](TODO.md) for the full list of known and suspected issues.

## Credits

- [fluorohydride/ygopro](https://github.com/Fluorohydride/ygopro) — the game
  client this is forked from.
- [mycard/srvpro](https://github.com/mycard/srvpro) — the reference server used
  in testing.
