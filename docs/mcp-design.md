# ygopromcp — MCP bridge design (v4)

The gframe (ygopro) client carries a JSON-RPC 2.0 MCP bridge on stdio. The
bridge is **always on** — no `--mcp` flag, no CLI parsing changes, no other
game/CLI modification. All fork changes that do not serve the bridge are
reverted (backed up first, so nothing is lost); only the bridge itself, its
inbound taps, its shared response builders, and its unconditional init/poll
hooks remain. The goal is minimal code that never conflicts during upstream
sync.

The bridge is an **external coroutine**: a blocking tool call (`ygo_connect`,
then `ygo_choose`) returns only when it is the agent's turn to make a
decision, as one text block containing everything the agent needs
(log → state → card explain → options). All tool outputs use this same text
format (JSON is only the MCP envelope; the agent reads the text field).

---

## 1. Protocol & interaction model

- Transport: newline-delimited JSON-RPC 2.0 on stdin/stdout.
- Agent → bridge: `initialize`, `tools/list`, `tools/call {name, arguments}`.
- Bridge → agent: one response per request
  (`{"jsonrpc":"2.0","id":...,"result":{"content":[{"type":"text","text":...}]}}`).
- **Blocking calls**: the bridge executes the action, then HOLDS the
  response until the next decision point, then returns the text block. If a
  decision is already pending when the call arrives, the response is
  immediate. NO timeout on this wait.
  - `ygo_connect` — blocks until the first decision point (section 3).
  - `ygo_choose` — blocks until the next decision point.
  - `ygo_surrender` — blocks: sends the surrender event and waits for the
    server's response (e.g. side-deck exchange in a match, or the
    win/lose message).
- **Immediate calls** (reach the same sync point without changing state):
  - `ygo_card` — no state change, responds immediately with the text block.
  - `ygo_chat` — responds immediately (incoming chat appears in # log).
  - `ygo_disconnect` — kills the connection; the bridge returns to the
    initial (disconnected) state, where `ygo_connect` must be called again.
- Threading: reader thread owns stdin; GUI thread executes requests and
  emits the decision-time response; network thread feeds the narration taps
  (publish-only, never block).
- The bridge drives the game through the SAME event receivers a human click
  would use; responses go through the game's own SendResponse paths so the
  ClearSelect/ClearCommandFlag cleanup runs. //Technically not "non blocking", ygo_card/surrender doesn't change state so it immediately reaches the same sync point. ygo_surrender do blocks, it sends event to server and wait to server response, e.g. side side exchange in match, or win/lose message. ygo_disconnect essentially kills the connection and it goes to the initial state where you have to call ygo_coonect.

**Seq gate** (deterministic prompt handling): a monotonic STOC counter
(`g_stoc_seq`) increments per STOC packet; a prompt is "decision time" only
when `seq != last_acted`. Acting stores `last_acted = seq` — EXCEPT when
acking a RETRY box (the same prompt is re-issued internally with no new
STOC packet, so the gate must stay open for the re-prompt). This
distinguishes "response accepted" (new seq) from "rejected / re-issued"
(same seq).

---

## 2. The text block (every tool response)

```
# log
<recent narration lines, terse — section 5>

# state
<board — section 4>

# card explain
<one-line explains for cards mentioned but not explained for N turns> //N configurable through env var
(absent when nothing to re-explain)

# options: min~max/total
0. <option>
1. <option>
...
(absent when no decision is required)
```

Example (first turn has no Draw Phase):

```
# log
T1 oppo
SP
M1
oppo summons 青眼白龙 H->M0
C1 oppo activates 青眼白龙(M0) [ignition]
C2 self activates 效果遮蔽者(H) [quick]
self discards 效果遮蔽者
C2 solved
C2 negates C1 青眼白龙
C1 青眼白龙 negated
chain resolve
EP
T2 self
DP
self draws 1
SP
M1
self summons 黑魔术师 H->M2
self 黑魔术师 attacks oppo 黑魔术师
battle 2500 vs 2500
self LP 8000 / oppo LP 8000
```

Word choice: minimize TOKENS, not characters — a proper English word costs
about the same as an abbreviation and is far easier to understand
(`attacks` not `A:`; `draws` not `dr`). Zone refs use `->` (`H->M0`); the
column (`M0`) identifies the slot. The activate line is about activating
the card's EFFECT; append the effect type (`[ignition]` / `[quick]` /
`[trigger]` / `[continuous]`...) when the client can derive it.

- `# log` — new narration since the last response (drained; each event seen
  once).
- `# state` — the current board snapshot; stable for a given prompt instance
  so consumers can key retry/rejection tracking on it.
- `# card explain` — for every card mentioned but not explained within the
  last N turns (default 3), one explain line (section 6); the recency
  counter advances per response round.
- `# options: min~max/total` — present only when the agent must decide
  (section 7). Option ids are 0-based and match the listing.

---

## 3. Tools

| Tool | Params | Response | Acts? |
|---|---|---|---|
| `ygo_connect` | `host`, `port`, `password`, `name` (nickname), `deck` (path) | **blocks** until the first decision point, then text block | YES |
| `ygo_choose` | `id` (int **or** list[int]) | **blocks** until the next decision point, then text block | YES |
| `ygo_card` | `name` (string: name or code) | immediate text block; explain lines added under `# card explain` | no |
| `ygo_chat` | `text` (string) | immediate text block (`Sent: ...` in # log) | YES |
| `ygo_surrender` | — | immediate text block (`Surrendered.` in # log) | YES |
| `ygo_disconnect` | — | immediate text block (`Left the room.` in # log) | YES |

Removed as tools (their content is in the text block): `ygo_state`,
`ygo_log`, `ygo_chatlog`.

`ygo_connect` flow (all automatic): set the nickname, join the room, upload
the deck, click ready, then poll the start button (if host) until the game
starts or the flow fails. It returns at the FIRST decision point:
- on failure — connect failed / kicked / deck rejected (cannot ready) /
  timeout, etc.: the text block carries the reason in `# log`
  (`[!] deck rejected: ...`, `[!] connect failed`, ...) and no options;
- on game start — the text block is exactly log + state + card explain +
  options, where state is the pre-duel RPS session (nearly everything
  empty, but the same structure), card explain is empty, and options is the
  RPS choice.

`ygo_choose` contract:
- Single param `id`: an int (single option) or a list of ints (subset).
- Option ids ALWAYS start at 0; negative ids are invalid:
  `Error: invalid choice (range 0..N)`.
- A prompt with exactly ONE way to answer — `min~max/1` (one option total,
  min >= 1) — is auto-answered by the bridge (no agent round-trip); the
  agent stays blocked until a real decision. `2~N/2` and similar are NOT
  auto-answered (order/count matters there).
- Legacy aliases `-1`/`-2` are removed.

---

## 4. Board/state format

### 4.1 Field model

The field is a **5×5 matrix plus an fzone column** — each player's
field-spell zone is the fzone cell of their own backrow row, part of the
same table (putting it elsewhere is confusing). The exact grid matters
because some cards have row/column-related effects. The board is rendered
as a markdown table (easier for the agent to read).

```
|      | col0 | col1 | col2 | col3 | col4 | fzone |
| row0 | [SZ] | [SZ] | [SZ] | [SZ] | [SZ] | [FZ]  |  oppo backrow
| row1 | [MZ] | [MZ] | [MZ] | [MZ] | [MZ] | -     |  oppo monster row
| row2 | -    | [EMZ]| -    | [EMZ]| -    | -     |  shared EMZ row
| row3 | [MZ] | [MZ] | [MZ] | [MZ] | [MZ] | -     |  self monster row
| row4 | [SZ] | [SZ] | [SZ] | [SZ] | [SZ] | [FZ]  |  self backrow
```

- Backrow (SZ) slots **0 and 4 double as the Pendulum zones (PZ)** — they
  are the same thing.
- **Row2 col1 and col3 are the Extra Monster Zones (EMZ)** — shared by
  both players, so every card there is tagged `(self)` / `(oppo)`. Row2
  col0/col2/col4 have NO zones (the matrix is a 5×5 with three holes).
  The EMZ columns are FIXED sequences (mirrored per player):
  - (row2, col1) = EMZ seq 5 — player 0's `mzone[0][5]`, player 1's
    mirrored `mzone[1][6]`;
  - (row2, col3) = EMZ seq 6 — player 0's `mzone[0][6]`, player 1's
    mirrored `mzone[1][5]`.
  Render whichever side occupies the zone (both cannot), tagged by owner.
- **fzone column**: the field-spell zone — oppo's field spell in row0's
  fzone cell, self's in row4's fzone cell.

### 4.2 Board text

```
T2 self turn, M1

oppo lp8000
  deck: 真红眼黑龙(up), ?*13
  extra: 异色眼灵摆龙(up)*2, ?*14
  hand: 真红眼黑龙(up)*2, ?*13
  grave: 真红眼黑龙*2, 真红眼黑龙*40
  remove: 真红眼黑龙*2, ?(down)*2

|      | col0 | col1 | col2 | col3 | col4 | fzone |
| row0 | -    | 死者苏生 | 死者苏生(down) | ?(down) | -    | 黑色花园 |
| row1 | -    | 青眼白龙(def0) | 黑魔术师(def) | ?(down) | -    | -      |
| row2 | -    | S:P 小夜骑士(oppo,dis) | -    | I:P莱娜(self) | -    | -      |
| row3 | -    | 阿不思的烙印 | -    | 阿不思的烙印(down) | 阿不思的烙印(def) | -      |
| row4 | -    | 死者苏生 | 死者苏生(down) | 死者苏生(down) | -    | 山铜结界 |

self lp8000
  deck: 真红眼黑龙*2, 异色眼灵摆龙*2, ...
  extra: 异色眼灵摆龙(up)*2, 异色眼灵摆龙*14
  hand: 真红眼黑龙(up)*2, 异色眼灵摆龙*13
  grave: 真红眼黑龙*2, 真红眼黑龙*40
  remove: 真红眼黑龙*2, 真红眼黑龙(down)*2
```

- Turn/phase line: `T<n> <self|oppo> turn, <phase>`; phases `DP SP M1 BP M2 EP`.
- Block headers: `oppo lp<lp>` / `self lp<lp>`; piles in the order deck,
  extra, hand, grave, remove.
- Unknown card name → `?`.
- Dedup: `name*N` (N copies); a single copy is bare `name`.
- Parentheses carry the card's SPECIAL state relative to its normal state:
  - deck / extra / hand: face-down is **normal**; `(up)` = face-up
    (revealed — pendulum in extra, revealed hand card, top deck under
    天变地异).
  - grave / remove: face-up is **normal**; `(down)` = face-down. Your own
    face-down removed card keeps its name (`真红眼黑龙(down)`); the
    opponent's is `?(down)`.
  - field monster: face-up attack is **normal**; `(def)` = face-up defense;
    `(down)` = face-down. (Face-down attack no longer exists.) A face-down
    card revealed once keeps its name (`真红眼黑龙(down)`).
  - field S/T: face-up is **normal**; `(down)` = face-down.
  - stat changes vs printed text: `(★6)` = level changed, `(atk1500)` =
    attack changed, `(def0)` = defense changed, etc.
  - `(dis)` = disabled (a UI-showable state).
  - EMZ cards are tagged `(self)` / `(oppo)`.
- Chains come from the narration log lines, not the board.

---

## 5. Narration log lines (terse, detail kept)

`self`/`oppo` replace player numbers. Location codes: `D` deck, `H` hand,
`G` grave, `R` removed, `E` extra, `M<c>` mzone column, `S<c>` szone column,
`F` fzone, `P` pzone, `O` overlay. A move is `SRC->DST` (e.g. `H->M0`).
Words, not abbreviations: minimize tokens, keep full English verbs.

| MSG_ | Line |
|---|---|
| MSG_START | `Duel start.` |
| MSG_NEW_TURN | `T<n> <self|oppo>` |
| MSG_NEW_PHASE | `DP` / `SP` / `M1` / `BP` / `M2` / `EP` (battle sub-phases collapse to `BP`) |
| MSG_LPUPDATE | `self LP <me> / oppo LP <opp>` (combined line, on any LP change) |
| MSG_DAMAGE | `<self|oppo> takes <v> damage` |
| MSG_RECOVER | `<self|oppo> recovers <v>` |
| MSG_PAY_LPCOST | `<self|oppo> pays <v>` |
| MSG_DRAW | `<self|oppo> draws <n>` (count only — opponent's codes never echoed) |
| MSG_MOVE | `<self|oppo> discards <name>` (H->G), `<self|oppo> <name> destroyed`, `... banished`, `... moved SRC->DST` |
| MSG_SUMMONING | `<self|oppo> summons <name> SRC->M<c>` |
| MSG_SPSUMMONING | `<self|oppo> special summons <name> SRC->M<c>` |
| MSG_FLIPSUMMONING | `<self|oppo> flip summons <name>` |
| MSG_SUMMONED / SPSUMMONED / FLIPSUMMONED | `<name> summoned` / `<name> special summoned` / `<name> flip summoned` |
| MSG_CHAINING | `C<n> <self|oppo> activates <name>(<loc>) [<effect type>]` |
| MSG_CHAINED | `chained` (implicit in the next C<n> line; may be omitted) |
| MSG_CHAIN_SOLVING | `C<n> solving` |
| MSG_CHAIN_SOLVED | `C<n> solved` |
| MSG_CHAIN_END | `chain resolve` |
| MSG_CHAIN_NEGATED / DISABLED | `C<n> <self|oppo> negates C<m> <name>` then `C<m> <name> negated|disabled` |
| MSG_ATTACK | `<self|oppo> <name> attacks <self|oppo> <name>` or `... attacks directly` |
| MSG_BATTLE | `battle <atk> vs <atk>` (card names already in the attack line) |
| MSG_HINT | `[!] <desc>` / `<desc>` / `announces race <race>` / `announces attribute <att>` / `announces <name>` / `announces number <n>` / `shows <name>` |
| MSG_WIN | `<self|oppo> wins` or `Draw` |
| MSG_SHUFFLE_DECK/HAND/EXTRA | `<self|oppo> shuffles deck|hand|extra` |
| MSG_SWAP_GRAVE_DECK | `<self|oppo> swaps grave and deck` |
| MSG_REVERSE_DECK | `<self|oppo> reverses deck` |
| MSG_FIELD_DISABLED | `field disabled 0x<hex>` |
| MSG_TOSS_COIN/DICE | `<self|oppo> tosses coin: H,T` / `<self|oppo> tosses dice: 1,2` |
| MSG_HAND_RES | `<self|oppo> hand result <n>` |
| MSG_RETRY | `[!] response rejected — re-answering prompt` |
| STOC_DUEL_END | `[!] game over — OK required` |
| deck error | `[!] deck rejected: <text>` |
| incoming chat | `[chat] <player>: <text>` |

Prompts are NOT narrated (they are the `# options` section). MSG_RETRY
re-enters ClientAnalyze with the last prompt, so prompts are never
double-narrated. Log ring cap: 256 lines.

---

## 6. Card explain (ygo_card + # card explain)

```
[Card] <code> <name> | <Type list> | <race> <att> | ★<lv> | <atk>/<def> | <effect text>
```

Real example (天变地异, Continuous Spell):

```
[Card] 62966332 天变地异 | Spell/Continuous | - | - | - | 只要这张卡在场上存在，双方玩家把卡组上下翻转过来进行决斗。
```

- Types (`/`-joined): Monster|Spell|Trap, Normal, Effect, Fusion, Ritual,
  Synchro, Xyz, Pendulum, Link, Spirit, Union, Dual, Tuner, Toon, Flip,
  TrapMonster, Quick-Play, Continuous, Equip, Field, Counter.
- Monster line: `★lv` (normal), `R<rank>` (Xyz), `L<rank> (<n> arrows)`
  (Link); `?` for unknown atk/def; Pendulum adds ` | Scale <l>/<r>`.
- Non-monster: `| - | - | -`.
- Effect text: cdb main text, newlines collapsed, one line.
- Lookup: exact numeric code first, else substring match (name in query OR
  query in name, up to 20 hits, sorted by code). `No card found: <q>`.
- The `# card explain` section lists cards mentioned but not explained
  within the last N turns (N = YGOCLI_EXPLAIN_ROUNDS env var, default 3).

---

## 7. Options — the decision surface

Header inside the block: `# options: min~max/total`, then `0..total-1`
option lines. Pick a subset of size k in [min,max] (list id), or one option
(int id). Cancelable prompts render as min=0 (empty list = cancel), only
when min<=1; min>=2 ignores the cancelable flag (mandatory).

Auto-answer: `min~max/1` (one option total, min >= 1) is answered
automatically — the single option is picked and the agent stays blocked.
No other shape auto-answers.

Per-MSG renderers:

| MSG_ | Header | Options | Wire |
|---|---|---|---|
| IDLECMD / BATTLECMD | `1~1/N` | per card: summon / special-summon / set-monster / set-spell / reposition / attack / activate; then `to battle phase` (IDLE) / `to main phase 2` (BATTLE); `shuffle deck`; `pass` | button click (game handler sends) |
| CARD / TRIBUTE / COUNTER | `min~max/N` | each selectable card `name @Loc[seq] [selected]` | toggles; auto-sends at select_max; empty = cancel if min 0 |
| UNSELECT_CARD | `0|1~1/N` | each selectable card | toggle ONE card (core hardcodes 1,1) |
| SUM | `1~1/N` (N combos, cap 20) | each VALID level-sum combo `name + name (sum S)` | combo mask |
| OPTION | `0|1~1/N` | each effect option desc | option index (or IDLE/BATTLE bit encoding for multi-activations) |
| YESNO / EFFECTYN | `1~1/2` | `yes` / `no` | button click |
| CHAIN | `1~1/N` | `decline`, then per chainable card `chain with name` | activatable index (decline = -1 wire) |
| PLACE / DISFIELD | `min~max/N` | each selectable zone `self|oppo <loc>[<seq>]` | zone bitmask; auto-sends at select_min |
| POSITION | `1~1/4` | face-up attack / face-down defense / face-up defense / face-down attack | position int, synchronous |
| CONFIRM_CARDS | `1~1/1` | `OK` | OK click |
| ANNOUNCE_RACE | `k~k/N` | each visible race | checked boxes -> bitmask at k |
| ANNOUNCE_ATTRIB | `k~k/N` | each visible attribute | checked boxes -> bitmask at k |
| ANNOUNCE_CARD | `1~1/N` | each declarable name | card code |
| ANNOUNCE_NUMBER | `1~1/N` | each number | number |
| SORT_CARD | `N~N/N` | each card `name @Loc[seq]` | the indices order IS the answer |

Special windows (rendered in options regardless of the seq gate):
- **Message box** (HINT_MESSAGE / RETRY / CONFIRM_CARDS / DUEL_END / deck
  error): `message: <text>` + `1~1/1` + `0. OK`. Boxes block the network
  thread on a local signal; there is NO protocol ack — OK is the only way;
  a missed ack freezes the client. Being `1~1/1`, boxes are auto-acked by
  the auto-answer rule and never reach the agent.
- **RPS** (pre-duel hand select or in-duel MSG_ROCK_PAPER_SCISSORS):
  `1~1/3`: rock / paper / scissors.
- **Turn order**: `1~1/2`: go first / go second.
- **Multi-activation option window** (wOptions, window-driven, no STOC
  message): options; answered with the IDLE/BATTLE bit encoding
  (IDLE = `(index<<16)+5`, BATTLE = `index<<16`).

---

## 8. Chat

- `ygo_chat`: UTF-8 input decoded codepoint-by-codepoint; 4-byte codepoints
  become U+FFFD; truncated at 255 UTF-16 units + NUL.
- Incoming chat appears in the `# log` section as `[chat] <player>: <text>`.
  May be empty in srvpro password rooms (server drops player chat there);
  random-duel rooms relay it.

---

## 9. ygo_surrender / ygo_disconnect

- `ygo_surrender` — sends CTOS_SURRENDER (accepted by the server gate in
  any state).
- `ygo_disconnect` — leaves the room (in-duel leave / lobby cancel /
  StopClient fallback).

---

## 10. Code footprint (revert policy)

Keep (MCP-serving): mcp.cpp/h, duelclient.cpp/h (inbound taps + chat
capture + debug logging), client_field.h + event_handler.cpp (shared
toggle/response builders: ToggleSelectCard, ClickSelectCounter,
RespondZones, RespondSortOrder), game.cpp MCP_Poll hook, gframe.cpp
always-on init, tests, .mcp.json, TODO.md, docs/.

Revert (backed up first): drawing.cpp animation speedup, image_utility.cpp
and CGUITTFont.cpp `lock()` changes, irrUString.h debug-break, premake/
miniaudio fix, CI deletion, game.cpp LoadExpansions + SetWindowsScale
changes, .gitignore additions.

Keep as build-required: the **ocgcore submodule bump** — upstream's own
gframe/replay.h uses `SEED_COUNT` from `ocgcore/common.h`, which only
exists after the bump ("move defines from ocgapi.h to common.h"); upstream's
pinned commit does not build. Verified: everything else reverts and builds.

---

## 11. Known caveats (accepted by design)

- Boxes auto-ack via the `1~1/1` auto-answer rule. If the core rejects a
  unique answer (constrained selects), MSG_RETRY increments seq each round,
  so an auto-answered unique prompt re-answers on the re-issue — accepted
  per design (no guard).
- Never respond outside a prompt (silently dropped); the text block IS the
  state.
- curMsg stays at the last prompt type after answering — "decision time" is
  (curMsg, seq vs last_acted), not curMsg alone.
- Responses defer while `fadingList` is non-empty (clicks dropped during
  fades) — long animations can delay the decision-time response.
- Opponent draw codes exist in client memory (ygopro v1 leak) but are never
  rendered.
- Auto-place/auto-chain rely on system.conf chkMAutoPos/chkSTAutoPos/
  chkRandomPos/chkAutoChain = 1.
