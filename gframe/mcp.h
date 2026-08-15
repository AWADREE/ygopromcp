#ifndef YGO_MCP_H
#define YGO_MCP_H

#include <cstddef>

// MCP bridge: a JSON-RPC 2.0 control plane on stdin/stdout that lets an
// external agent drive the GUI through the SAME event receivers a human
// click would use. GUI code only calls MCP_Poll() (one line in
// Game::MainLoop) and MCP_Init()/MCP_Shutdown() (gframe.cpp).
//
// Inbound hooks (network thread -> bridge, publish-only, never block/act):
//   MCP_NotifyStoc()      - every STOC packet, after handling (duelclient.cpp)
//   MCP_NotifyGameMsg()   - every MSG_* inside STOC_GAME_MSG, before the
//                           client's own parse (duelclient.cpp)
// See mcp.cpp for the threading contract and the learned caveats.
namespace ygo {

// Called once at startup, always on (no --mcp flag), BEFORE Game::Initialize
// (the bridge must own stdout from the very start so the JSON-RPC channel
// never sees app output -- see gframe.cpp). Redirects the app's stdout to
// stderr and starts the stdin reader thread.
void MCP_Init();

// Called after MainLoop returns. Stops the reader thread.
void MCP_Shutdown();

// Called every frame from Game::MainLoop (GUI thread). Executes any pending
// MCP requests by synthesizing irrlicht events for the CURRENT event
// receiver, then writes the JSON-RPC responses.
void MCP_Poll();

// ---- inbound taps (called on the NETWORK thread; publish only) ----
// Fires at the end of DuelClient::HandleSTOCPacketLan's switch for every
// well-formed STOC packet (GAME_MSG included - ClientAnalyze already ran).
void MCP_NotifyStoc(unsigned char pktType);
// Fires at the top of DuelClient::ClientAnalyze with the raw MSG_* payload.
// Parses the display/event subset for the narration log. Must never block.
void MCP_NotifyGameMsg(const unsigned char* msg, size_t len);
// Fires when the server rejects a deck (STOC_ERROR_MSG / ERRMSG_DECKERROR)
// and the client shows the modal popup. Surfaces the popup text in the log
// so the agent sees why instead of waiting forever.
void MCP_NotifyDeckError(const wchar_t* msg);
// Fires on STOC_CHAT (network thread). Formats "<player>: <text>" and appends
// it to the same narration ring as MCP_NotifyGameMsg, so chat and duel
// narration share one bounded deque (drained under one lock).
void MCP_NotifyChat(const wchar_t* msg, int player);

} // namespace ygo

#endif // YGO_MCP_H
