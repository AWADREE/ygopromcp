# Local MCP in the game

This branch adds a Streamable HTTP MCP endpoint to the existing stdio bridge.
The HTTP transport implements the 2025-11-25 MCP protocol. The existing
newline-delimited stdio transport remains available for clients that launch
the game as a subprocess; its protocol version remains 2024-11-05.

1. Start the game normally and choose **Local MCP** on the home screen.
2. Enter an unused local port (the default is 8765) and select **Enable MCP**.
3. After the status says **Listening on this PC**, select **Copy connection URL**.
   Register that complete URL as a Streamable HTTP MCP server in the chat
   client's MCP settings, then open a fresh chat and confirm that `ygo_guide`
   appears in its tools. Pasting the URL into chat text alone does not register
   a server or grant tool access. The client should use POST with
   `application/json` and advertise both
   `application/json` and `text/event-stream` in `Accept`.
4. Select **Disable MCP** to close the listener. The port is released, and the
   old URL stops working. Enabling again produces a new URL.

The listener is off each time the game starts. Only the port preference is
saved. The URL contains a temporary access token, so share it only with a
client you trust; it changes on each enable and is not written to settings.
One client can initialize per enable. The initialize response supplies an
`Mcp-Session-Id` header; that client must send it with later requests and
notifications. To switch clients, disable and re-enable MCP to issue fresh
access and session tokens.
It listens only on `127.0.0.1`, rejects browser `Origin` headers, and does not
accept LAN connections. It is separate from the game's room port (17924).
There is no separate MCP server to start when using this home-screen control.
The UI reports a bind error if another process already owns the chosen port.

The server's initialize response includes concise use instructions, and
`ygo_guide` supplies read-only overview, decks, duel, and limits topics. An
agent can create an empty deck through `ygo_deck_create`, search/add cards in
the visible deck editor, and save it without a shell or manual deck file. The
agent can then join a private room hosted by a separate human game client.
The server controls only the game process that exposes the URL; it cannot
launch or close other clients, run research clients over time, or see hidden
opponent cards. A new URL after each enable must be registered again by the
MCP client. These limits are reported by `ygo_guide` as well.

Theme selection is under **Settings** on the home screen and under the game's
existing System settings tab. Switching back to Classic restores the Classic
skin and layout. Local MCP availability and port are independent of theme.

HTTP requests have a 16 KiB header limit, 1 MiB JSON body limit, four socket
workers, three active request slots and an eight-job GUI queue. The fourth
socket worker remains available for cancellation and quick capacity errors.
Game tools execute on the GUI thread.
`GET` and server-sent event subscriptions are not offered. A disconnected
client does not cancel its submitted game action; send
`notifications/cancelled` to cancel a pending request. Disabling the listener
cancels its remaining HTTP jobs and closes its sockets.
