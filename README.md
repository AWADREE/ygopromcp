<div align="center">

<img src="docs/assets/ygopromcp-banner.png" alt="YGOPROMCP logo banner with cyan and gold circuit lines and stylized cards" width="100%"/>

# ygopromcp

**A YGOPro graphical client with stdio and optional localhost MCP control.**

Based on [Fluorohydride/ygopro](https://github.com/Fluorohydride/ygopro) through
[ZisIsNotZis/ygopromcp](https://github.com/ZisIsNotZis/ygopromcp). This
fork keeps the visible game and deck editor: an MCP client sends
actions to the same client instance a person can see.

</div>

## Current scope

This source includes the Gilded and Classic theme selector, native first-run
game-data setup, card-data updates, and local MCP controls. The packaged
launcher starts the client; stdio MCP remains available through its `--mcp`
mode. Localhost HTTP MCP is optional and enabled inside the game. See
[local MCP setup](docs/mcp-local-http.md) for transport and security details.
A game room uses a separate network port. These features have bounded local
tests. The reviewed Windows x64 package was built with full audio support and
exercised in isolated local runs; audible playback itself was not verified.

## Get a working local client

Clone the `main` branch and its Git submodules:

```sh
git clone --recurse-submodules --branch main https://github.com/AWADREE/ygopromcp.git
cd ygopromcp
```

A source checkout alone is not a ready-to-play installation. The reviewed
Windows package includes the native launcher, required DLLs, original
functional textures and licensed fonts. On first run, the launcher can fetch
and validate a pinned compatible English card database and scripts without
overwriting existing user data. Card artwork is not included. See
[Windows packaging](docs/windows-release-packaging.md) for the package and
source requirements. Do not infer a card's identity from a missing-image
placeholder.

### Build from source

Premake generates the GNU Make project in `build/`; `premake5.lua` and
`.github/workflows/build.yml` list the dependency variants and options.
The build needs a C++14 compiler, Premake 5, GNU Make for gmake builds,
and the dependency source trees or installed development libraries selected by
Premake (including ocgcore, modified Irrlicht, Lua, libevent, SQLite,
and graphics/audio libraries). nlohmann/json 3.12.0 is vendored in
`vendor/nlohmann` and does not need a separate package. Recursive Git submodules
do **not** provide every dependency source used by CI. In particular, CI
fetches ocgcore and Irrlicht separately and currently fetches Lua 5.4.8;
do not assume its floating dependency heads equal the tested local set.

On a prepared MSYS2 UCRT64/GNU Make Windows tree, the locally exercised
build step was:

```sh
premake5 gmake
mingw32-make -C build config=Release -j4
```

Use the matching UCRT64 toolchain and put its `bin` directory on the
**build process's** `PATH`, including when invoking GCC from PowerShell.
Otherwise `cc1plus.exe` can fail to find `libmpc-3.dll` even when the MSYS2
package is installed. The tested build needed an Irrlicht static-archive
response-file workaround for Windows command-length limits; a fresh checkout
is not guaranteed to build unmodified with the two commands above. Keep
dependency versions and any local workaround recorded with a build. The
release executable is under `bin/release/` (on Windows, `YGOPro.exe`).

The Windows GNU/UCRT64 build, visible GUI, and local MCP flows were exercised.
A full MSVC build and full Linux graphical build were not executed for this
tested source revision. The portable stdio helper was separately compiled and
tested on Linux; that is narrower than a full Linux client build.

## MCP setup and tools

For the Windows package, run `Play YGOPro.cmd` once to complete first-run
game-data setup. Then use `MCP YGOPro.cmd` as the stdio client's command.
Equivalently, launch the packaged `YGOProUpdateLauncher.exe` with `--mcp`.
The launcher starts the graphical client and sets its game working directory
to the package folder. The exact MCP-client configuration syntax varies; this
is the usual shape for a client that accepts an executable and arguments:

```json
{
  "mcpServers": {
    "ygopromcp": {
      "type": "stdio",
      "command": "C:\\path\\to\\YGOProUpdateLauncher.exe",
      "args": ["--mcp"]
    }
  }
}
```

Run one stdio MCP client per game process. `Play YGOPro.cmd` starts the
ordinary game; the optional localhost HTTP MCP control is enabled separately
on its home screen.
The server handles `initialize`, `ping`, and `tools/list`; initialization reports MCP
protocol version `2024-11-05`. Call `tools/list` for the authoritative
input schemas of the executable you actually launched. For example,
after the MCP initialization handshake:

```json
{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"ygo_deck_list","arguments":{}}}
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"ygo_connect","arguments":{"host":"127.0.0.1","port":"7911","password":"PrivateRoom","name":"Player1","deck":"deck/example.ydk"}}}
```

The second call assumes a private local room already exists and that
`deck/example.ydk` belongs to this client's runtime. MCP SDKs usually
construct these envelopes for you.

| Tool | Arguments | Result |
|---|---|---|
| `ygo_deck_list` | none | List locally available decks and editor state |
| `ygo_deck_open` | `deck` name; optional `category` (default `local`) | Open a listed deck in the visible editor |
| `ygo_deck_search` | `query` text | Apply the editor's search filter; show up to 20 matches |
| `ygo_deck_add`, `ygo_deck_remove` | integer `code`, `section` = `main`/`extra`/`side`, optional `count` = 1–3 | Edit the selected local deck using editor rules |
| `ygo_deck_save` | none | Save the currently selected existing local deck |
| `ygo_connect` | `host`, `port` (schema: string), `password`, optional `name`, `deck` **file path** | Join/upload/ready; block until first decision or failure |
| `ygo_choose` | `id`: zero-based integer or array of integers | Answer a decision, then block for the next one |
| `ygo_card` | `name`: name fragment or numeric code as text | Explain a card |
| `ygo_surrender` | none | Surrender and wait for server response |
| `ygo_disconnect` | none | Leave and wait for room/transport cleanup |
| `ygo_chat` | `text` | Send room chat; use only in a room where chat is intended |

`ygo_choose` accepts both an integer and an integer array in the
implementation, although the currently advertised `inputSchema` labels
`id` as integer. Clients that validate schemas strictly may need a
single-selection prompt or a bridge/schema correction before using array
choices. Do not assume multi-selection works through every MCP client.

Deck tools act in the normal graphical editor at the main menu. Open refuses
to discard unsaved edits. Add/remove/save require a selected existing local
deck; add enforces known-card, token, section, alias/copy, and deck-size
rules. Finish a manual drag or modal dialog before issuing a deck tool.
`ygo_deck_save` does not create an arbitrary new file: create a deck in the
GUI first, then select it. Save only when you intend to change that file.

A blocking duel response contains narration, state, card explanations, and,
when a decision is pending, a section like:

```text
# options: 1~2/3
0. first legal option
1. second legal option
2. third legal option
```

Choose one ID or an array with the allowed count. `id=[]` cancels only
where the prompt explicitly allows zero picks. One-way prompts are answered
automatically. Never use `ygo_choose` as a state-read or send speculative
IDs: it performs an action. After a core rejection, `MSG_RETRY` republishes
the restored prompt. Multi-card `MSG_CONFIRM_CARDS` panels publish an
acknowledgment decision when the panel becomes ready. The client tracks
prompt generations so an already answered prompt is not reoffered merely
because its previous message type remains set. Read the new options after
every returned call rather than reusing an earlier ID.

For protocol details, see [docs/mcp-design.md](docs/mcp-design.md). Some
historical design prose in that document predates the latest implementation;
the source and `tools/list` control actual behavior.

## Private local rooms and multiple clients

For a graphical host confined to this computer, set
`local_host_only = 1` in **that runtime's** `system.conf` before starting
that client (restart it if already running), then open the normal Create Host screen. The game listener then binds to
`127.0.0.1` and LAN broadcast is disabled. The default `0` is for normal
LAN hosting. Give each room a distinct available port. A second client
connects using `host: "127.0.0.1"`, that port, the room password/name and
its own deck file path. Both clients can then drive their own stdio session
through `ygo_connect`/`ygo_choose`.

Use a separate process and writable runtime directory for **each client**:
`system.conf`, `deck/`, replay/log/output files, and image caches must
not race. Read-only assets can be shared only if the workflow really keeps
them immutable. Keep each process's stdin/stdout pair with its own agent;
do not share a global MCP pipe. Start with one room/two clients, then scale
within measured CPU and memory limits. Bounded tests used two simultaneous
rooms/four clients with distinct loopback ports; that establishes isolation
for those runs, not a general speedup guarantee. Close each owned client
after disconnect and confirm its listener has released the port.

## Card data, art, and visibility

Card names and effect text come from the local card database. Scripts and
core behavior must match the database/card IDs. Full-face card images are separate local assets. The current image loader
checks `expansions/pics/<card-id>.jpg` before `pics/<card-id>.jpg`; it does
not fetch images on demand. A missing or cropped image is not proof that a
card is absent. Verify exact card IDs and image sources, and validate a JPEG
can actually decode before placing it in the higher-priority expansion path:
a corrupt readable overlay can mask a valid base image. Do not substitute a
different card. The source repository does not redistribute third-party
card artwork or a complete current database.
Some prerelease cards lack an official English printed full-card image;
their database text and art provenance need separate checks.

The opponent's hidden cards are shown according to recipient network
packets. An explicitly public reveal may remain named until the client
receives a shuffle/hidden-zone refresh. Tests exercised player seats and
selected recipient-packet cases; spectator/replay visibility was not fully
validated. Do not treat a locally forged internal card state as proof of a
reachable information leak.

## Testing, limits, and troubleshooting

Tests under `tests/` include offline Windows stdio, deck editor, recipient
visibility, loopback-host and disconnect cases. The Windows tests require
an interactive desktop and a built executable; read each test's header and
arguments before running. For example, after a release build in a prepared
runtime:

```sh
python tests/windows_mcp_stdio_smoke.py
```

Controlled local runs on this source also exercised English GUI
deck search/save/reopen, an ordinary 8000-LP duel reaching an actual result,
a custom-rule 100-LP duel, both sides' returned MCP calls and clean listener
release, plus a scripted eight-activation Herald of Perfection payment
sequence. These are scoped functional fixtures, not tournament legality,
competitive consistency, or universal card-script coverage. Replay playback,
tag/match siding, full MSVC and full Linux GUI runs remain unverified.

If the MCP client sees no tools, check that it launched `MCP YGOPro.cmd`
or `YGOProUpdateLauncher.exe --mcp` with stdio pipes and sent `initialize`
then `tools/list`.
Keep stdout protocol-clean; `YGOCLI_MCP_DEBUG=1` sends transport/prompt
traces to stderr. If the GUI opens with missing names/art, inspect its
working directory and compatible `cards.cdb`, `script/`, `textures/` and
image paths. If a deck is rejected, the reason is surfaced as
`[!] deck rejected: ...`; correct the deck/room rules rather than waiting
on the modal. If a request remains blocked, check whether the peer or the
visible client is waiting at RPS, turn order, an acknowledgment panel, or a
duel prompt. Use `ygo_disconnect` for a room exit; close task-owned clients
after recording results.

## Visual direction

The [visual style note](docs/visual-style.md) records the original banner's
representative palette and circuit-line treatment for possible future UI
work. This documentation update does not change the client theme.

## Credits and license

This fork retains the original [license](LICENSE) and source history. Credit
goes to [Fluorohydride/ygopro](https://github.com/Fluorohydride/ygopro),
[ZisIsNotZis/ygopromcp](https://github.com/ZisIsNotZis/ygopromcp), and
[mycard/srvpro](https://github.com/mycard/srvpro), the reference server used
in testing.
