#ifndef YGO_MCP_H
#define YGO_MCP_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

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

// Called after MainLoop returns or initialization fails. Stops the reader
// thread even when stdin remains open or a request is waiting.
void MCP_Shutdown();

// Called every frame from Game::MainLoop (GUI thread). Executes any pending
// MCP requests by synthesizing irrlicht events for the CURRENT event
// receiver, then writes the JSON-RPC responses.
void MCP_Poll();

// Local Streamable HTTP is opt-in. The endpoint includes an ephemeral access
// token; it changes on every enable and is never written to system.conf.
bool MCP_HTTP_Start(uint16_t port, std::string& error);
bool MCP_BridgeIsReady();
void MCP_HTTP_Stop();
bool MCP_HTTP_IsRunning();
unsigned MCP_HTTP_ActiveRequestCount();
std::string MCP_HTTP_Endpoint();
// Returns and clears an unexpected listener failure for the GUI to report.
std::string MCP_HTTP_TakeFailure();
#ifdef YGOPROMCP_HTTP_PROBE
void MCP_HTTP_ProbeBreakListener();
#endif

// HTTP workers submit bounded jobs here; only MCP_Poll executes game tools.
std::string MCP_InvokeHttp(const std::string& request,
                           const std::function<bool()>& connection_alive);
void MCP_CancelHttpRequest(const std::string& request_id_json);
void MCP_CancelAllHttpRequests();

// ---- inbound taps (called on the NETWORK thread; publish only) ----
// Fires at the end of DuelClient::HandleSTOCPacketLan's switch for every
// well-formed STOC packet. Only completed decision messages advance the
// generation; ClientAnalyze has already parsed their GUI choice state.
void MCP_NotifyStoc(unsigned char pktType, unsigned char gameMsgType);
// MSG_CONFIRM_CARDS can wait inside ClientAnalyze for a panel OK before the
// outer STOC callback returns. Publish that decision after the panel is ready.
void MCP_NotifyConfirmPanelReady();
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
