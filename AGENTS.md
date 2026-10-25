# Repository agent guidance

This is the `ygopromcp` graphical client plus an MCP stdio bridge. Read
[README.md](README.md) for the current `maintained` branch workflow and
[docs/mcp-design.md](docs/mcp-design.md) for historical protocol design.
Implementation and `tools/list` win when design prose differs.

## Work boundaries

- Preserve the visible YGOPro client, event receivers, deck-editor rules,
  inherited licenses/credits, and normal non-MCP GUI behavior.
- Keep distinct concerns in distinct changes: native build/stdio, recipient
  visibility, deck editing, and prompt/duel lifecycle. Test the relevant GUI
  path, not only a synthetic helper.
- Before changing hidden-information handling, trace actual recipient packets,
  public reveals, shuffles, and hidden-zone refreshes. Do not call an
  unreachable internal state a production leak.
- Do not silently replace an unknown card ID, artwork, effect text, script, or
  dependency revision with a different card/version.
- Card art, card databases, user decks, account data, and local test
  transcripts are runtime inputs, not source files to publish.

## MCP and local tests

One process owns one MCP stdin/stdout pair. Room ports are separate from MCP
stdio; for offline tests bind game listeners only to `127.0.0.1` with
`local_host_only = 1` in the test runtime. Give simultaneous clients
separate writable runtimes and distinct room ports. Keep mutable cache and
config files process-private; only proven immutable assets may be shared.

Use `tools/list` to verify schemas after changes. `ygo_choose` is an
action, including retry acknowledgment; do not use it for polling. Preserve
prompt-generation publication at the actual decision boundary, including
`MSG_RETRY` and a blocking `MSG_CONFIRM_CARDS` panel. Assert terminal MCP
responses and process/listener cleanup separately from a time cap.

Run focused tests from `tests/` with temporary decks/database and record
the executable, core, scripts and database revisions used. A result from
synthetic cards or custom LP rules must be labeled as such. Avoid public
rooms and unrelated user processes in regression tests; stop test-owned
clients after the run.

## Reusable workflows

- [Build and verify the local graphical client](.agents/skills/ygopromcp-local-build/SKILL.md).
- [Exercise a private MCP duel](.agents/skills/ygopromcp-local-duel/SKILL.md).

Existing submodule directories under `.agents/skills` are upstream content;
do not rewrite or replace them as part of documentation edits.
