# TODO / Known problems

Status of the ygopromcp MCP bridge. Verified = observed in real games or
read directly from the core/script source. Unsure = plausible, needs a test.

## Verified problems

1. **Constrained combo selects can still reject arbitrary picks.**
   - `MSG_SELECT_SUM` (synchro materials) — handled: the bridge enumerates only
     valid level-sum combinations (`MCP_SumText`).
   - `MSG_SELECT_UNSELECT_CARD` (custom XYZ material procedures) — handled: the
     core hardcodes `check_response(size,1,1)`, so the bridge responds with
     exactly one card and the payload min/max are ignored.
   - `MSG_SELECT_TRIBUTE` and other selects whose validity depends on card-text
     constraints (e.g. "material must be X") can still list cards the core
     rejects → MSG_RETRY → the retry-box → re-prompt loop. Not enumerable
     without script logic; the random test's watchdog surrenders such games.
     Residual flake: roughly 1 in several runs.

2. **The deck-check popup cannot be dismissed by MCP.**
   `STOC_ERROR_MSG`/`ERRMSG_DECKERROR` shows an irrlicht `env->addMessageBox`
   modal (not `wMessage`). The MCP surfaces the text in the `# log` section
   (`[!] deck rejected: ...`), but there is no way for the agent to click the
   box's OK — it stays on screen (harmless to the bridge, but the agent must
   give up / switch decks). A future `ygo_dismiss`-style action that finds the
   box window and clicks its OK would close the loop.

3. **`MSG_SELECT_SUM` mode 1 (unbounded count, variable-level cards) is not
   enumerated.** The bridge falls back to the generic card list; random picks
   may be rejected. Rare (used by few effects).

4. **Multi-activation option window** (`wOptions` opened from an IDLE/BATTLE
   "activate" click) is rendered unconditionally (window-driven) and answered
   with the IDLE/BATTLE bit encoding (`index<<16` / `+5`) mirroring
   `SetResponseSelectedOption`. Continuous-trap *operations* (`EDESC_OPERATION`)
   are filtered out of the option list like the game's button handler — if a
   card offers both an operation and a reset, the reset branch is not offered.

5. **`ygo_choose` ids are 0-based only** (`int` or `list[int]`); negative ids
   are rejected. Legacy `-1`/`-2` aliases are gone. A cancelable prompt is
   canceled with an empty `id=[]` (rendered as `min=0`).

6. **ygopro v1 leaks opponent draw codes** in the `MSG_DRAW` payload. The
   bridge deliberately logs opponent draws as count-only and the state shows
   the opponent hand as a face-down count. The codes are still in the client's
   memory — if a stricter policy is wanted, the tap could strip them too.

7. **Incoming chat may be empty in srvpro password rooms** unless
   `mycard.enabled=true` (server policy drops player chat there). Use random
   duel rooms (`S`/`M`/`T`) if chat must be tested.

8. **`ygo_connect` takes a full deck path** (`deck` argument) and loads it via
   `deckManager.LoadCurrentDeck` — the old extensionless `-d` name matching no
   longer applies.

9. **MCP_Poll defers while `fadingList` is non-empty** (the game drops clicks
   during fades). Long animation sequences can delay a request up to the 30s
   reader-thread cap.

## Unsure (need testing, not done)

1. **Replay mode** — the taps fire for replay playback too (`ClientAnalyze` is
   shared), so the bridge narrates replays. Never tested.
2. **Tag duels** — `MCP_MeOpp` assumes `selftype < 2`; tag (0-3) player naming
   and the field render are unverified.
3. **Match (BO3) flow** — after game 1 the client enters the deck-builder
   siding UI, which the MCP cannot drive. With srvpro `reconnect` disabled
   (this fork's config), verify how a match-mode room behaves after a game.
4. **`YGOCLI_EXPLAIN_ROUNDS`** semantics ("round" = a tool response round) are
   implemented but not explicitly verified.
5. **`ygo_card` name lookup** is substring-based over the loaded card table —
   ambiguous names can return multiple/wrong cards; numeric codes are exact.
6. **`ygo_chat` UTF-8** — 4-byte codepoints are replaced with U+FFFD; long
   messages are truncated at 255 UTF-16 units. Untested.
7. **Windows build** — only Linux is exercised here; the `-d` open-deck path in
   gframe.cpp uses platform-specific path building.
8. **The residual non-blocking "invalid operation" messages** (1-2 per game) —
   mostly fixed by the pending-prompt gate; the remaining ones are suspected to
   be a stale-prompt race (agent acts on a prompt already answered) — worth
   instrumenting `MCP_NotifyStoc`/`MSG_RETRY` correlation if it ever blocks.
