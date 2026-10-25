---
name: ygopromcp-local-duel
description: Test a visible YGOPro client's MCP deck controls and private loopback duel decisions, including multi-client isolation and cleanup.
---

# Private MCP deck and duel workflow

Use the tool names and input descriptions in [README.md](../../../README.md)
as an orientation, then read the launched client's `tools/list`. Each
graphical client has its own stdio pair and writable runtime. Configure
`local_host_only = 1` for GUI-hosted offline rooms, use a distinct loopback
port per room, and keep agent requests bound to their owning process.

For a deck-edit check, create/select a local deck in the visible editor,
then exercise list/open/search/add/remove/save. Verify card identity, section,
alias and copy limits; confirm unsaved edits are not discarded when opening
another deck. Finish manual drags/modals before MCP edits. Save only the
intended selected deck and compare its on-disk bytes or parsed contents.

For a duel check, connect host and guest with separate processes, then use
the returned `# options: min~max/total` header to select only valid IDs.
Never send `ygo_choose` as a read. A one-way decision may be auto-answered;
a rejected response can return a restored prompt; a public confirmation
panel can require an acknowledgment. For subset prompts verify required
count, invalid-count rejection and safe retry. Trace both players' decision
responses and actual terminal result. Treat a time cap or forced process
kill as a failure to complete the duel.

Record exact client/core/script/database revisions, fixture decks and rules,
both final MCP responses, process exit codes and listener release. A custom
rule, scripted opponent or fixture card database proves only its scoped flow.
Do not join public rooms or send game chat during automated verification.
Close task-owned clients afterward and leave unrelated user apps alone.
