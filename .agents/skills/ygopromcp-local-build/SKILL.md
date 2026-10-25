---
name: ygopromcp-local-build
description: Build and verify this repository's graphical YGOPro client and MCP stdio transport in a local development runtime.
---

# Local client build and stdio verification

Use for changes to the native build, startup, stdio transport, or runtime
packaging. Read the [repository README](../../../README.md) first and inspect
`premake5.lua`, `gframe/premake5.lua`, and `.github/workflows/build.yml`
for the dependency mode actually selected.

Keep a record of source commit, compiler/thread model, generated project
options, ocgcore/Irrlicht/Lua revisions, and build outputs. CI may fetch
dependency heads independently of Git links. Supply the matching runtime
assets and launch with its directory as the working directory; a compiled
executable alone does not establish GUI or card-data readiness.

On Windows GNU/UCRT64, make the toolchain's `bin` directory available on
the build process's `PATH`. Distinguish missing DLLs or dependency sources
from repository defects. Do not alter global PATH to conceal a local build
problem. Verify `tools/list`, protocol-clean stdout, visible GUI startup,
stdin EOF/shutdown, and redirected/closed output behavior with the focused
Windows tests in `tests/`. The Irrlicht archive command-length workaround
must be recorded if used. Do not describe the portable Linux stdio-helper
check as a complete Linux graphical build.

If a test uses temporary `cards.cdb` or decks, label that limitation. Shut
down only the processes and loopback listeners this test started.
