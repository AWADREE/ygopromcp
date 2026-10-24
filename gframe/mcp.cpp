// MCP bridge implementation. See mcp.h.
//
// Protocol: newline-delimited JSON-RPC 2.0. Tools: ygo_connect, ygo_choose,
// ygo_card, ygo_chat, ygo_surrender, ygo_disconnect, and ygo_deck_* (the former ygo_state /
// ygo_log / ygo_chatlog tools were folded into the text block). ping is the
// standard MCP liveness method. Only the network-play path (join room, lobby,
// RPS, turn-order, duel prompts) and the graphical deck editor are supported;
// other game modes are out of scope for the MCP mode.
//
// Threading: a reader thread owns stdin and writes responses to a saved
// stdout fd (the app's stdout is redirected to stderr at startup). All game
// state access happens on the GUI thread inside MCP_Poll(), which executes
// each request by invoking the CURRENT irrlicht event receiver with
// synthesized events -- the same boundary a human click crosses.
//
// Inbound model (two taps in duelclient.cpp, publish-only):
//   - MCP_NotifyStoc  (end of HandleSTOCPacketLan): monotonic STOC sequence
//     counter + last op. The sequence is THE way to tell "my response was
//     accepted" (new seq) from "still pending / MSG_RETRY re-issued the same
//     prompt with identical state" (same seq) -- state diffing CANNOT detect
//     rejections because MSG_RETRY re-runs the last message verbatim.
//   - MCP_NotifyGameMsg (top of ClientAnalyze): raw MSG_* payload for the
//     narration log. Display/event messages are parsed here; prompts are NOT
//     narrated (they surface as the choice list in ygo_state). MSG_RETRY
//     re-enters ClientAnalyze with the last PROMPT message, so prompts never
//     get double-narrated.
//
// Learned caveats (all surfaced in comments at their sites):
//   * ygo_choose(id=-1) MEANS pass/decline in a duel prompt -- it acts, it
//     is NOT a read. Never use it to inspect state (use ygo_state).
//   * Message boxes (HINT_MESSAGE / RETRY / CONFIRM_CARDS / DUEL_END) block
//     the network thread on a local signal; the server does NOT wait for an
//     ack (the response gate in netserver.cpp drops non-CTOS_RESPONSE
//     packets), so there is no protocol ack -- clicking OK is the only way.
//     A missed ack parks the network thread and the whole client freezes
//     (socket buffer fills). ygo_choose therefore acks any visible box
//     regardless of the requested id.
//   * A response sent when the server is not waiting is silently dropped --
//     never respond outside a prompt; the seq gate makes the prompt-pending
//     state deterministic instead of heuristic.
//   * ygopro v1 leaks opponent draw codes in MSG_DRAW -- never echo them
//     (log count only, like the GUI).
//   * curMsg stays at the last prompt type after answering until the next
//     message arrives -- "pending prompt" is derived from (curMsg, seq vs
//     last-acted-seq), not from curMsg alone.
//   * Responses must go through DuelClient::SendResponse() (or the game's
//     SetResponse* builders) so its switch runs the ClearSelect/
//     ClearCommandFlag cleanup; a raw SendBufferToServer(CTOS_RESPONSE)
//     leaks stale selection state into the next prompt.
//   * POSITION/PLACE/ANNOUNCE windows are closed only by their click
//     handlers or the fade-completion send; the programmatic paths here hide
//     them explicitly (HideElement(..., false) = no fade, no fade-send).
//   * MCP_Poll defers while fadingList is non-empty: ClientField::OnEvent
//     drops clicks during fades.
//   * Auto-place/auto-chain rely on system.conf chkMAutoPos/chkSTAutoPos/
//     chkRandomPos/chkAutoChain = 1 (set in this fork).
//   * Constrained selects (XYZ materials, some tributes) list cards the
//     engine then rejects; hidden constraints are not bridge-visible.
//     Mitigations (pick-shift, same-level preference, watchdog) live in
//     ygocli/tests, not here.
//   * ygo_chatlog can be empty in srvpro password rooms: the server drops
//     player chat there (mycard.enabled=false); random-duel rooms relay it.
//   * The reader thread caps each request at 30 s -- agents must poll
//     within the cap.

#include "mcp.h"
#include "config.h"
#include "game.h"
#include "client_field.h"
#include "client_card.h"
#include "duelclient.h"
#include "data_manager.h"
#include "deck_manager.h"
#include "file_system.h"
#include "bufferio.h"
#include "mcp_stdio.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <cstring>
#include <cwchar>
#include <csignal>
#include <string>
#include <deque>
#include <vector>
#include <set>
#include <atomic>
#include <sstream>
#include <thread>
#include <future>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <IEventReceiver.h>
#ifdef _WIN32
#include <Windows.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#endif

// globals owned by other translation units, used by the bridge
void ClickButton(irr::gui::IGUIElement* btn);   // gframe.cpp, global scope

namespace ygo {

using json = nlohmann::json;

void UpdateDeck();                              // menu_handler.cpp

// ---------------------------------------------------------------------------
// request queue
// ---------------------------------------------------------------------------

struct MCPJob {
    int id = 0;
    std::string method;
    json params;
    bool done = false;
    bool ok = false;
    json result;   // "result" payload on success
    std::string error_msg;   // JSON-RPC error message on failure
    bool blocking = false;   // ygo_connect/ygo_choose/ygo_surrender: held until the next decision point
    std::mutex mtx;
    std::condition_variable cv;
};

static MCPStdio g_stdio;
static std::atomic<bool> g_running{ false };
static std::thread g_thread;
#ifdef _WIN32
static HANDLE g_reader_handle = nullptr;
#endif
static std::mutex g_qmtx;
static std::deque<MCPJob*> g_queue;

static std::string MCP_Execute(MCPJob* job);
static void dbg(const char* s) { if (std::getenv("YGOCLI_MCP_DEBUG")) { fprintf(stderr, "[mcp] %s\n", s); fflush(stderr); } }

// Tools held until the next decision, or until disconnect finishes. The
// reader thread and executor both consult this blocking set.
static bool MCP_IsBlockingTool(const std::string& name) {
    return name == "ygo_connect" || name == "ygo_choose"
        || name == "ygo_surrender" || name == "ygo_disconnect";
}

static MCPJob* MCP_PopJob() {
    std::lock_guard<std::mutex> lk(g_qmtx);
    if (g_queue.empty()) return nullptr;
    MCPJob* job = g_queue.front();
    g_queue.pop_front();
    return job;
}

static bool MCP_DisconnectQueued() {
    std::lock_guard<std::mutex> lk(g_qmtx);
    if (g_queue.empty()) return false;
    const MCPJob* job = g_queue.front();
    return job->method == "tools/call" && job->params.is_object()
        && job->params.value("name", "") == "ygo_disconnect";
}

static void MCP_HandleLine(const std::string& line) {
    if (line.empty()) return;
    json req;
    try {
        req = json::parse(line);
    } catch (...) {
        return;
    }
    if (!req.is_object() || !req.contains("id")) { dbg("reader: bad req"); return; }
    dbg("reader: got request");
    auto* job = new MCPJob;
    job->id = req.value("id", 0);
    job->method = req.value("method", "");
    if (req.contains("params")) job->params = req["params"];
    // blocking tools (coroutine): the response waits for a decision point
    if (job->method == "tools/call" && job->params.contains("name")) {
        std::string n = job->params.value("name", "");
        if (MCP_IsBlockingTool(n))
            job->blocking = true;
    }
    {
        std::lock_guard<std::mutex> lk(g_qmtx);
        g_queue.push_back(job);
    }
    dbg("reader: queued request");
    {
        // Blocking calls (coroutine model) wait until the GUI thread
        // completes them at the next decision point -- NO timeout.
        // Non-blocking calls keep the 30 s safety cap.
        std::unique_lock<std::mutex> lk(job->mtx);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!job->done && g_running && (job->blocking || std::chrono::steady_clock::now() < deadline))
            job->cv.wait_for(lk, std::chrono::milliseconds(25));
    }
    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = job->id;
    if (job->ok) {
        resp["result"] = job->result;
    } else {
        resp["error"] = {{"code", -32603}, {"message", job->error_msg.empty() ? "internal error" : job->error_msg}};
    }
    if (g_running)
        g_stdio.Write(resp.dump() + "\n");
    {
        std::lock_guard<std::mutex> lk(g_qmtx);
        auto queued = std::find(g_queue.begin(), g_queue.end(), job);
        if (queued != g_queue.end()) g_queue.erase(queued);
    }
    delete job;
}

static void MCP_ReaderThread() {
    std::string pending;
    char buffer[4096];
    while (g_running) {
        const auto newline = pending.find('\n');
        if (newline != std::string::npos) {
            const std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            if (!line.empty()) MCP_HandleLine(line);
            continue;
        }
        const int count = g_stdio.Read(buffer, sizeof(buffer));
        if (count < 0) break;
        if (count > 0) pending.append(buffer, static_cast<size_t>(count));
    }
    g_running = false;
}

// ---------------------------------------------------------------------------
// inbound taps (network thread, publish-only; see header comment)
// ---------------------------------------------------------------------------

// forward decls (helpers are defined further down)
static std::string U8(const wchar_t* w);
static const char* MCP_PhaseShort(uint16_t ph);
static const char* MCP_EffectType(uint32_t code);

static std::atomic<uint64_t> g_stoc_seq{ 0 };    // monotonic STOC counter
static std::atomic<uint16_t> g_phase{ 0 };       // current phase (from MSG_NEW_PHASE)
static std::atomic<int> g_cur_turn_player{ 0 };  // current turn player, local view (from MSG_NEW_TURN)
static uint32_t g_last_summon_code = 0;          // card code from the last SUMMONING family (for SUMMONED lines)
static uint8_t g_solving_chain = 0;              // chain currently being solved (from MSG_CHAIN_SOLVING)

static std::mutex g_log_mtx;                     // guards g_log + g_mentioned
static std::deque<std::string> g_log;            // narration ring (drained by the text block)
static std::set<uint32_t> g_mentioned;           // card codes mentioned since last block
static std::set<uint32_t> g_force_explain;       // one-shot explicit explain requests (ygo_card)

// ygo_connect flow state (GUI thread + taps)
enum class ConnectPhase { NONE, JOIN, READY, START, DONE };
static ConnectPhase g_connect_phase = ConnectPhase::NONE;
static bool g_connect_err_set = false;
static std::string g_connect_err;
static bool g_connect_deck_err = false;          // set by MCP_NotifyDeckError during connect
static std::chrono::steady_clock::time_point g_connect_t0;

// A held call waiting for the next decision or a completed disconnect
// (GUI thread only).
static MCPJob* g_active_job = nullptr;
static bool g_disconnect_pending = false;
static std::chrono::steady_clock::time_point g_disconnect_t0;

static void MCP_Log(const std::string& line) {
    std::lock_guard<std::mutex> lk(g_log_mtx);
    g_log.push_back(line);
    if (g_log.size() > 256)
        g_log.pop_front();
}
static void MCP_Mention(uint32_t code) {
    if (!code) return;
    std::lock_guard<std::mutex> lk(g_log_mtx);
    g_mentioned.insert(code & 0x7fffffff);
}

void MCP_NotifyStoc(unsigned char pktType) {
    g_stoc_seq.fetch_add(1, std::memory_order_relaxed);
    if (pktType == STOC_DUEL_END)
        MCP_Log("[!] game over — OK required");
}

// The server rejected a deck (lflist/rule/count) and the client showed the
// modal popup. Surface the text in the log so the agent knows why.
void MCP_NotifyDeckError(const wchar_t* msg) {
    MCP_Log("[!] deck rejected: " + U8(msg));
    if (g_connect_phase != ConnectPhase::NONE)
        g_connect_deck_err = true;
}

// Incoming chat (STOC_CHAT): "<player>: <text>" into the shared narration
// ring (design doc §8). Runs on the network thread, publish-only.
void MCP_NotifyChat(const wchar_t* msg, int player) {
    char buf8[LEN_CHAT_MSG * 4];
    BufferIO::EncodeUTF8(msg, buf8);
    std::string who;
    if (player == 8) who = "Server";              // system custom message
    else if (player >= 4) who = "System";
    else if (player == 0 || player == 1) {
        char n8[128];
        BufferIO::EncodeUTF8(player == 0 ? mainGame->dInfo.hostname : mainGame->dInfo.clientname, n8);
        who = n8;
    } else who = "P" + std::to_string(player);
    MCP_Log("[chat] " + who + ": " + buf8);
}

// Narration log: parse the display/event subset of the MSG_* stream on the
// network thread, BEFORE the client's own case mutates state. Field layouts
// are copied verbatim from ClientAnalyze's own cases (single source of
// truth). Prompt messages are deliberately not narrated here -- they are the
// choice surface in ygo_state.
void MCP_NotifyGameMsg(const unsigned char* msg, size_t len) {
    if (len < 1 || !mainGame) return;
    const unsigned char* p = msg + 1;
    const unsigned char* end = msg + len;
    auto rd8 = [&]() -> uint8_t { return p < end ? *p++ : 0; };
    auto rd16 = [&]() -> uint16_t { uint16_t v = 0; if (p + 2 <= end) std::memcpy(&v, p, 2); p += 2; return v; };
    auto rd32 = [&]() -> int32_t { int32_t v = 0; if (p + 4 <= end) std::memcpy(&v, p, 4); p += 4; return v; };
    auto name = [&](uint32_t code) -> std::string {
        return U8(dataManager.GetName(code));
    };
    // protocol player -> local view (0 = self, 1 = oppo)
    auto who = [&](int pl) -> std::string {
        return mainGame->LocalPlayer(pl) == 0 ? "self" : "oppo";
    };
    // short zone ref: D/H/G/R/E/F/P/O; M<c>/S<c> carry the sequence column.
    auto loc = [](int l) -> const char* {
        switch (l) {
        case LOCATION_DECK: return "D";
        case LOCATION_HAND: return "H";
        case LOCATION_MZONE: return "M";
        case LOCATION_SZONE: return "S";
        case LOCATION_GRAVE: return "G";
        case LOCATION_REMOVED: return "R";
        case LOCATION_EXTRA: return "E";
        case LOCATION_OVERLAY: return "O";
        case LOCATION_FZONE: return "F";
        case LOCATION_PZONE: return "P";
        default: return "?";
        }
    };
    auto locseq = [&](int l, int seq) -> std::string {
        const char* z = loc(l);
        if (l == LOCATION_MZONE || l == LOCATION_SZONE)
            return std::string(z) + std::to_string(seq);
        return z;
    };
    // find a card by code in the player's visible zones (source lookup for
    // summons; the tap runs BEFORE the client moves the card).
    auto find_card = [&](int ctrl, uint32_t code) -> ClientCard* {
        auto& f = mainGame->dField;
        for (auto* c : f.hand[ctrl]) if (c->code == code) return c;
        for (auto* c : f.mzone[ctrl]) if (c && c->code == code) return c;
        for (auto* c : f.szone[ctrl]) if (c && c->code == code) return c;
        for (auto* c : f.grave[ctrl]) if (c->code == code) return c;
        for (auto* c : f.remove[ctrl]) if (c->code == code) return c;
        for (auto* c : f.extra[ctrl]) if (c->code == code) return c;
        return nullptr;
    };
    switch (msg[0]) {
    case MSG_START: {
        MCP_Log("Duel start.");
        break;
    }
    case MSG_NEW_TURN: {
        uint8_t pl = rd8();
        // dInfo.turn is incremented by the client's own case AFTER this tap.
        g_cur_turn_player.store(mainGame->LocalPlayer(pl));
        MCP_Log("T" + std::to_string(mainGame->dInfo.turn + 1) + " " + who(pl));
        break;
    }
    case MSG_NEW_PHASE: {
        uint16_t ph = rd16();
        g_phase.store(ph);
        MCP_Log(MCP_PhaseShort(ph));
        break;
    }
    case MSG_LPUPDATE: {
        rd8();  // player
        rd32(); // value
        // combined line on any LP change (design doc Q3)
        MCP_Log("self LP " + std::to_string(mainGame->dInfo.lp[0])
                + " / oppo LP " + std::to_string(mainGame->dInfo.lp[1]));
        break;
    }
    case MSG_DAMAGE: {
        uint8_t pl = rd8();
        int32_t v = rd32();
        MCP_Log(who(pl) + " takes " + std::to_string(v) + " damage");
        break;
    }
    case MSG_RECOVER: {
        uint8_t pl = rd8();
        int32_t v = rd32();
        MCP_Log(who(pl) + " recovers " + std::to_string(v));
        break;
    }
    case MSG_PAY_LPCOST: {
        uint8_t pl = rd8();
        int32_t v = rd32();
        MCP_Log(who(pl) + " pays " + std::to_string(v));
        break;
    }
    case MSG_DRAW: {
        uint8_t pl = rd8();
        uint8_t cnt = rd8();
        // Draw names: the core sends the OPPONENT's drawn codes (the classic
        // ygopro v1 leak) but the LOCAL player's draws carry code 0 (the
        // client learns its hand via a later UPDATE_DATA). We never echo the
        // opponent's codes (privacy) -- log the count only, both sides.
        if (cnt == 0) break;
        for (int i = 0; i < cnt; ++i) rd32();
        MCP_Log(who(pl) + " draws " + std::to_string((int)cnt));
        break;
    }
    case MSG_MOVE: {
        uint32_t code = (uint32_t)rd32();
        uint8_t pc = rd8(), pl = rd8(), ps = rd8(), pp = rd8();
        uint8_t cc = rd8(), cl = rd8(), cs = rd8(), cp = rd8();
        int32_t reason = rd32();
        code &= 0x7fffffff;
        if (!code || (pl == cl && ps == cs)) break;
        MCP_Mention(code);
        std::string who_s = who(pl);
        std::string card = name(code);
        if (reason & REASON_DESTROY) {
            MCP_Log(who_s + " " + card + " destroyed");
        } else if (cl == LOCATION_REMOVED && pl != cl) {
            MCP_Log(who_s + " " + card + " banished");
        } else if (pl == LOCATION_HAND && cl == LOCATION_GRAVE) {
            MCP_Log(who_s + " discards " + card);
        } else {
            MCP_Log(who_s + " " + card + " moved "
                    + locseq(pl, ps) + "->" + locseq(cl, cs));
        }
        break;
    }
    case MSG_SUMMONING:
    case MSG_SPSUMMONING:
    case MSG_FLIPSUMMONING: {
        uint32_t code = (uint32_t)rd32() & 0x7fffffff;
        uint8_t ctrl = rd8();
        uint8_t loccode = rd8();
        uint8_t seq = rd8();
        g_last_summon_code = code;
        MCP_Mention(code);
        std::string who_s = who(ctrl);
        std::string verb = msg[0] == MSG_SUMMONING ? " summons "
            : (msg[0] == MSG_SPSUMMONING ? " special summons " : " flip summons ");
        // source: the card's zone before the client processes the summon
        std::string src;
        ClientCard* c = find_card(mainGame->LocalPlayer(ctrl), code);
        if (c && c->location != loccode) src = locseq(c->location, c->sequence) + "->";
        else src = "?->";
        MCP_Log(who_s + verb + name(code) + " " + src + locseq(loccode, seq));
        break;
    }
    // The SUMMONED family payload carries no code (the client's own case
    // prints a fixed system string); the card name comes from the preceding
    // SUMMONING/SPSUMMONING/FLIPSUMMONING message (design doc §5).
    case MSG_SUMMONED: MCP_Log(name(g_last_summon_code) + " summoned"); break;
    case MSG_SPSUMMONED: MCP_Log(name(g_last_summon_code) + " special summoned"); break;
    case MSG_FLIPSUMMONED: MCP_Log(name(g_last_summon_code) + " flip summoned"); break;
    case MSG_CHAINING: {
        uint32_t code = (uint32_t)rd32() & 0x7fffffff;
        uint8_t pcc = rd8(), pcl = rd8(), pcs = rd8();
        rd8(); // subs
        rd8(); rd8(); rd8(); // cc, cl, cs
        rd32(); // desc
        rd8();  // ct
        MCP_Mention(code);
        size_t n = mainGame->dField.chains.size() + 1;
        std::string et = MCP_EffectType(code);
        MCP_Log("C" + std::to_string(n) + " " + who(pcc) + " activates "
                + name(code) + "(" + locseq(pcl, pcs) + ")"
                + (et.empty() ? "" : " " + et));
        break;
    }
    case MSG_CHAINED: {
        rd8(); // ct -- the link was just added; the CHAINING line already told it
        break;
    }
    case MSG_CHAIN_SOLVING: {
        uint8_t ct = rd8();
        g_solving_chain = ct;
        MCP_Log("C" + std::to_string((int)ct) + " solving");
        break;
    }
    case MSG_CHAIN_SOLVED: {
        uint8_t ct = rd8();
        MCP_Log("C" + std::to_string((int)ct) + " solved");
        break;
    }
    case MSG_CHAIN_END: {
        MCP_Log("chain resolve");
        break;
    }
    case MSG_CHAIN_NEGATED:
    case MSG_CHAIN_DISABLED: {
        uint8_t ct = rd8();   // the NEGATED chain
        std::string card;
        if (ct >= 1 && (size_t)ct <= mainGame->dField.chains.size()) {
            auto& ch = mainGame->dField.chains[ct - 1];
            ClientCard* c = ch.chain_card;
            card = c ? name(c->code) : name(ch.code);
            MCP_Mention(c ? c->code : ch.code);
        }
        // negator = the chain currently being solved (the one whose effect
        // negated); fall back to the last chain in the vector.
        size_t negator = g_solving_chain ? g_solving_chain : mainGame->dField.chains.size();
        std::string neg_who;
        if (negator >= 1 && negator <= mainGame->dField.chains.size())
            neg_who = (mainGame->dField.chains[negator - 1].controler == 0) ? "self" : "oppo";
        const char* verb = (msg[0] == MSG_CHAIN_NEGATED) ? "negates" : "disables";
        const char* suffix = (msg[0] == MSG_CHAIN_NEGATED) ? "negated" : "disabled";
        if (!card.empty()) {
            MCP_Log("C" + std::to_string(negator) + " " + neg_who + " " + verb
                    + " C" + std::to_string((int)ct) + " " + card);
            MCP_Log("C" + std::to_string((int)ct) + " " + card + " " + suffix);
        }
        break;
    }
    case MSG_ATTACK: {
        uint8_t ca = rd8(); uint8_t la = rd8(); uint8_t sa = rd8(); rd8();
        uint8_t cd = rd8(); uint8_t ld = rd8(); uint8_t sd = rd8(); rd8();
        std::string an = "", dn = "";
        auto* a = mainGame->dField.GetCard(mainGame->LocalPlayer(ca), la, sa);
        if (a && a->code) { an = name(a->code); MCP_Mention(a->code); }
        if (ld) {
            auto* d = mainGame->dField.GetCard(mainGame->LocalPlayer(cd), ld, sd);
            if (d && d->code) { dn = name(d->code); MCP_Mention(d->code); }
        }
        MCP_Log(who(ca) + " " + an + " attacks "
                + (ld ? who(cd) + " " + dn : "directly"));
        break;
    }
    case MSG_BATTLE: {
        rd8(); rd8(); rd8(); rd8(); // ca, la, sa, pad
        int32_t aatk = rd32(); rd32(); // aatk, adef
        rd8(); // da
        rd8(); rd8(); rd8(); rd8(); // cd, ld, sd, pad
        int32_t datk = rd32(); rd32(); // datk, ddef
        rd8(); // dd
        MCP_Log("battle " + std::to_string(aatk) + " vs " + std::to_string(datk));
        break;
    }
    case MSG_HINT: {
        uint8_t type = rd8();
        rd8(); // player
        int32_t data = rd32();
        switch (type) {
        case HINT_MESSAGE:
            // Blocking message box: the network thread parks on actionSignal
            // until OK -- auto-acked by MCP_AutoAnswer (1~1/1).
            MCP_Log("[!] " + U8(dataManager.GetDesc(data)));
            break;
        case HINT_OPSELECTED:
            MCP_Log(U8(dataManager.GetDesc(data)));
            break;
        case HINT_RACE:
            MCP_Log("announces race " + U8(dataManager.FormatRace(data).c_str()));
            break;
        case HINT_ATTRIB:
            MCP_Log("announces attribute " + U8(dataManager.FormatAttribute(data).c_str()));
            break;
        case HINT_CODE: {
            std::string n = name((uint32_t)data);
            if (!n.empty() && n != "?") { MCP_Log("announces " + n); MCP_Mention((uint32_t)data); }
            break;
        }
        case HINT_NUMBER:
            MCP_Log("announces number " + std::to_string(data));
            break;
        case HINT_CARD: {
            std::string n = name((uint32_t)data);
            if (!n.empty() && n != "?") { MCP_Log("shows " + n); MCP_Mention((uint32_t)data); }
            break;
        }
        default:
            break;
        }
        break;
    }
    case MSG_WIN: {
        uint8_t pl = rd8();
        rd8(); // reason
        if (pl == 2) MCP_Log("Draw");
        else MCP_Log(who(pl) + " wins");
        break;
    }
    case MSG_SHUFFLE_DECK: {
        uint8_t pl = rd8();
        MCP_Log(who(pl) + " shuffles deck");
        break;
    }
    case MSG_SHUFFLE_HAND: {
        uint8_t pl = rd8();
        MCP_Log(who(pl) + " shuffles hand");
        break;
    }
    case MSG_SHUFFLE_EXTRA: {
        uint8_t pl = rd8();
        MCP_Log(who(pl) + " shuffles extra");
        break;
    }
    case MSG_SWAP_GRAVE_DECK: {
        uint8_t pl = rd8();
        MCP_Log(who(pl) + " swaps grave and deck");
        break;
    }
    case MSG_REVERSE_DECK: {
        uint8_t pl = rd8();
        MCP_Log(who(pl) + " reverses deck");
        break;
    }
    case MSG_FIELD_DISABLED: {
        uint32_t disabled = (uint32_t)rd32();
        char buf[16];
        snprintf(buf, sizeof buf, "%x", disabled);
        MCP_Log(std::string("field disabled 0x") + buf);
        break;
    }
    case MSG_TOSS_COIN:
    case MSG_TOSS_DICE: {
        uint8_t pl = rd8();
        uint8_t cnt = rd8();
        std::ostringstream out;
        out << who(pl) << " tosses " << (msg[0] == MSG_TOSS_COIN ? "coin" : "dice") << ":";
        for (int i = 0; i < cnt; ++i) {
            uint8_t r = rd8();
            out << (i ? "," : "") << (msg[0] == MSG_TOSS_COIN ? (r ? "H" : "T") : std::to_string((int)r));
        }
        MCP_Log(out.str());
        break;
    }
    case MSG_HAND_RES: {
        uint8_t pl = rd8();
        uint8_t res = rd8();
        MCP_Log(who(pl) + " hand result " + std::to_string((int)res));
        break;
    }
    case MSG_RETRY: {
        // Response rejected; the box is auto-acked, then ClientAnalyze
        // re-runs the last PROMPT (which is not narrated -> no duplicates).
        MCP_Log("[!] response rejected — re-answering prompt");
        break;
    }
    default:
        break;
    }
}// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static std::string U8(const wchar_t* w) {
    if (!w) return "";
    char buf[2048];
    BufferIO::EncodeUTF8(w, buf);
    return std::string(buf);
}

static irr::gui::IGUIElement* MCP_El(int id) {
    if (!mainGame || !mainGame->env || !mainGame->env->getRootGUIElement()) return nullptr;
    return mainGame->env->getRootGUIElement()->getElementFromId(id, true);
}

// Synthesize a button-click event and feed it to the CURRENT event receiver
// synchronously (same code path a human click would take, minus irrlicht's
// frame-queued delivery).
static void MCP_Click(irr::gui::IGUIElement* el) {
    if (!el || !mainGame || !mainGame->device) return;
    irr::SEvent evt;
    evt.EventType = irr::EET_GUI_EVENT;
    evt.GUIEvent.EventType = irr::gui::EGET_BUTTON_CLICKED;
    evt.GUIEvent.Caller = el;
    auto* recv = mainGame->device->getEventReceiver();
    if (std::getenv("YGOCLI_MCP_DEBUG"))
        fprintf(stderr, "[mcp] click id=%d\n", el->getID());
    if (recv) recv->OnEvent(evt);
}

static std::string MCP_CardName(ClientCard* c) {
    if (!c) return "?";
    std::string name = U8(dataManager.GetName(c->code));
    if (c->level) {
        name += " (Lv" + std::to_string(c->level & 0xff) + ")";
    }
    return name;
}

static const char* MCP_LocName(int loc) {
    switch (loc) {
    case LOCATION_DECK: return "Deck";
    case LOCATION_HAND: return "Hand";
    case LOCATION_MZONE: return "MZone";
    case LOCATION_SZONE: return "SZone";
    case LOCATION_GRAVE: return "Grave";
    case LOCATION_REMOVED: return "Removed";
    case LOCATION_EXTRA: return "Extra";
    case LOCATION_OVERLAY: return "Overlay";
    case LOCATION_FZONE: return "FZone";
    case LOCATION_PZONE: return "PZone";
    default: return "Field";
    }
}

// Effect type tag for the activate line (design doc §5: [ignition]/[quick]/
// [trigger]/[continuous]... when the client can derive it). Only card-type
// categories are derivable client-side; monster effect categories
// (ignition/trigger/quick) are not exposed in the card data.
static const char* MCP_EffectType(uint32_t code) {
    CardData cd;
    if (!dataManager.GetData(code, &cd)) return "";
    if (cd.type & TYPE_QUICKPLAY) return "[quick]";
    if (cd.type & TYPE_COUNTER) return "[counter]";
    if (cd.type & TYPE_CONTINUOUS) return "[continuous]";
    if (cd.type & TYPE_FIELD) return "[field]";
    if (cd.type & TYPE_EQUIP) return "[equip]";
    return "";
}

// Monster position label (FA = face-up attack, etc.).
static const char* MCP_PosMonster(int pos) {
    switch (pos) {
    case POS_FACEUP_ATTACK: return "FA";
    case POS_FACEDOWN_ATTACK: return "DA";
    case POS_FACEUP_DEFENSE: return "FD";
    case POS_FACEDOWN_DEFENSE: return "DD";
    default: return "?";
    }
}
// S/T position label: face-up / face-down.
static const char* MCP_PosST(int pos) {
    switch (pos) {
    case POS_FACEUP: return "UP";
    case POS_FACEDOWN: return "FD";
    case POS_FACEUP_ATTACK: return "UP";
    case POS_FACEDOWN_ATTACK: return "FD";
    default: return "?";
    }
}

static std::string MCP_TypeName(uint32_t type) {
    std::vector<std::string> parts;
    if (type & TYPE_MONSTER) parts.push_back("Monster");
    else if (type & TYPE_SPELL) parts.push_back("Spell");
    else if (type & TYPE_TRAP) parts.push_back("Trap");
    if (type & TYPE_NORMAL) parts.push_back("Normal");
    if (type & TYPE_EFFECT) parts.push_back("Effect");
    if (type & TYPE_FUSION) parts.push_back("Fusion");
    if (type & TYPE_RITUAL) parts.push_back("Ritual");
    if (type & TYPE_SYNCHRO) parts.push_back("Synchro");
    if (type & TYPE_XYZ) parts.push_back("Xyz");
    if (type & TYPE_PENDULUM) parts.push_back("Pendulum");
    if (type & TYPE_LINK) parts.push_back("Link");
    if (type & TYPE_SPIRIT) parts.push_back("Spirit");
    if (type & TYPE_UNION) parts.push_back("Union");
    if (type & TYPE_DUAL) parts.push_back("Dual");
    if (type & TYPE_TUNER) parts.push_back("Tuner");
    if (type & TYPE_TOON) parts.push_back("Toon");
    if (type & TYPE_FLIP) parts.push_back("Flip");
    if (type & TYPE_TRAPMONSTER) parts.push_back("TrapMonster");
    if (type & TYPE_QUICKPLAY) parts.push_back("Quick-Play");
    if (type & TYPE_CONTINUOUS) parts.push_back("Continuous");
    if (type & TYPE_EQUIP) parts.push_back("Equip");
    if (type & TYPE_FIELD) parts.push_back("Field");
    if (type & TYPE_COUNTER) parts.push_back("Counter");
    if (parts.empty()) return "?";
    std::string out = parts[0];
    for (size_t i = 1; i < parts.size(); ++i) out += "/" + parts[i];
    return out;
}

// One-line card explain, ydkshow-style:
//   [Card] <code> <name> | <type> | <race> <att> | <lv> | <atk>/<def> | <effect>
static std::string MCP_CardLine(uint32_t code) {
    auto& table = dataManager.GetDataTable();
    auto it = table.find(code);
    if (it == table.end()) return "";
    const CardDataC& d = it->second;
    std::string name = U8(dataManager.GetName(code));
    if (name.empty() || name == "?") return "";
    std::ostringstream out;
    out << "[Card] " << code << " " << name << " | " << MCP_TypeName(d.type);
    if (d.type & TYPE_MONSTER) {
        out << " | " << U8(dataManager.FormatRace(d.race).c_str()) << " " << U8(dataManager.FormatAttribute(d.attribute).c_str());
        std::string atk = d.attack < 0 ? "?" : std::to_string(d.attack);
        if (d.type & TYPE_LINK) {
            int arrows = 0;
            for (int i = 0; i < 8; ++i) if (d.link_marker & (1u << i)) arrows++;
            out << " | L" << (d.level & 0xff) << " | " << atk << " (" << arrows << " arrows)";
        } else if (d.type & TYPE_XYZ) {
            out << " | R" << (d.level & 0xff) << " | " << atk << "/" << (d.defense < 0 ? "?" : std::to_string(d.defense));
        } else {
            out << " | ★" << (d.level & 0xff) << " | " << atk << "/" << (d.defense < 0 ? "?" : std::to_string(d.defense));
        }
        if (d.type & TYPE_PENDULUM)
            out << " | Scale " << d.lscale << "/" << d.rscale;
    } else {
        out << " | - | - | -";
    }
    // The card's main effect text lives in CardString::text (loaded from
    // the cdb's texts.desc column); desc[n] are the str1..16 slots.
    std::string desc;
    CardString cs;
    if (dataManager.GetString(code, &cs))
        desc = U8(cs.text.c_str());
    if (!desc.empty()) {
        // collapse newlines to keep the explain on one line
        std::string flat;
        flat.reserve(desc.size());
        for (char ch : desc) {
            if (ch == '\n' || ch == '\r') flat += ' ';
            else flat += ch;
        }
        // trim trailing spaces
        while (!flat.empty() && flat.back() == ' ') flat.pop_back();
        if (!flat.empty()) out << " | " << flat;
    }
    return out.str();
}

// ---------------------------------------------------------------------------
// choice surface
// ---------------------------------------------------------------------------

enum class MCActionKind { CLICK, CARD_COMMAND, POS, TOGGLE_CARD, PLACE, CHAIN_CARD, CHAIN_DECLINE, OPTION, SUM, ANNOUNCE_RACE, ANNOUNCE_ATTRIB, ANNOUNCE_CARD, ANNOUNCE_NUMBER };

struct MCAction {
    MCActionKind kind = MCActionKind::CLICK;
    irr::gui::IGUIElement* elem = nullptr;   // button to click
    ClientCard* card = nullptr;              // menu_card to set before the click / card to toggle
    int pos = 0;                             // position value for POS actions
    uint64_t mask = 0;                       // SUM subset bitmask over selectsum_all
};

static std::vector<MCAction> g_actions;   // rebuilt on every execute
static std::set<uint32_t> g_mention_ctx;  // card codes seen while rendering state (per read)

static void MCP_AddClick(int id) { g_actions.push_back({MCActionKind::CLICK, MCP_El(id), nullptr}); }
static void MCP_AddClickEl(irr::gui::IGUIElement* el) { g_actions.push_back({MCActionKind::CLICK, el, nullptr}); }
static void MCP_AddCardCmd(ClientCard* card, irr::gui::IGUIElement* btn) { g_actions.push_back({MCActionKind::CARD_COMMAND, btn, card}); }
static void MCP_AddToggleCard(ClientCard* card) { g_actions.push_back({MCActionKind::TOGGLE_CARD, nullptr, card}); }

static void MCP_RunAction(const MCAction& a) {
    if (a.kind == MCActionKind::CARD_COMMAND) {
        if (a.card) mainGame->dField.menu_card = a.card;
        // MCP always commands a SPECIFIC card, never the location-list flow:
        // the menu buttons (activate/sset/mset/...) branch on list_command,
        // which the GUI right-click ShowMenu sets to 0 for HAND/MZONE/SZONE
        // cards. The MCP bridge bypasses ShowMenu, so list_command can be
        // stale (e.g. COMMAND_ACTIVATE left by a previous chain panel at
        // duelclient.cpp:1891); with a stale non-zero value, BUTTON_CMD_ACTIVATE
        // enters the location-list branch and shows an EMPTY
        // "请选择要发动效果的卡" window (the card sits in SZone while the
        // branch scans command_location). Reset the location-list state so the
        // direct single-card response path is always taken.
        mainGame->dField.list_command = 0;
        mainGame->dField.command_location = 0;
        mainGame->dField.command_controler = 0;
        MCP_Click(a.elem);
    } else if (a.kind == MCActionKind::POS) {
        // The position buttons only stage the response and rely on the fade
        // animation to send it; complete it synchronously here and hide the
        // window (no fade) -- otherwise it lingers until the next message.
        DuelClient::SetResponseI(a.pos);
        DuelClient::SendResponse();
        if (mainGame->wPosSelect && mainGame->wPosSelect->isVisible())
            mainGame->HideElement(mainGame->wPosSelect, false);
    } else if (a.kind == MCActionKind::TOGGLE_CARD) {
        // Shared with the real field-click handler: toggles selection and
        // auto-sends via the game's own logic when the selection is complete
        // (see ClientField::ToggleSelectCard). No byte packing here.
        mainGame->dField.ToggleSelectCard(a.card);
    } else if (a.kind == MCActionKind::PLACE) {
        // Field-zone selection: replicate the mouse-click toggle; the last
        // required zone auto-sends (fade-signal otherwise).
        auto& f = mainGame->dField;
        unsigned flag = (unsigned)a.pos;
        if (!(flag & f.selectable_field)) return;
        f.selected_field |= flag;
        f.select_min--;
        if (f.select_min == 0) {
            // Pack via the game-owned zone responder (single source of truth).
            f.RespondZones();
            DuelClient::SendResponse();
            f.ShowCancelOrFinishButton(0);
        }
    } else if (a.kind == MCActionKind::CHAIN_CARD) {
        // Overlay chain pick: respond with the activatable index of the
        // chosen card (chain responses are ints, not card lists).
        auto& f = mainGame->dField;
        ClientCard* target = a.card;
        int index = -1;
        for (size_t i = 0; i < f.activatable_cards.size(); ++i) {
            if (f.activatable_cards[i] == target) { index = (int)i; break; }
        }
        if (index < 0) return;
        DuelClient::SetResponseI(index);
        if (mainGame->wCardSelect && mainGame->wCardSelect->isVisible())
            mainGame->HideElement(mainGame->wCardSelect, false);
        f.ClearChainSelect();
        DuelClient::SendResponse();
    } else if (a.kind == MCActionKind::CHAIN_DECLINE) {
        // Chain question: decline (-1). Direct send (no fade dependency).
        if (mainGame->wQuery && mainGame->wQuery->isVisible())
            mainGame->HideElement(mainGame->wQuery, false);
        mainGame->dField.ClearChainSelect();
        DuelClient::SetResponseI(-1);
        DuelClient::SendResponse();
    } else if (a.kind == MCActionKind::OPTION) {
        // Effect option: MSG_SELECT_OPTION responds with the option index;
        // a multi-activation opened from IDLE/BATTLE needs the activatable
        // index with the IDLE/BATTLE bit encoding (same as the game's own
        // SetResponseSelectedOption -- shared via EncodeOptionResponse).
        auto& f = mainGame->dField;
        int idx = a.pos;
        if (idx < 0 || (size_t)idx >= f.select_options.size()) return;
        DuelClient::SetResponseI(f.EncodeOptionResponse(idx));
        if (mainGame->wOptions && mainGame->wOptions->isVisible())
            mainGame->HideElement(mainGame->wOptions, false);
        DuelClient::SendResponse();
    } else if (a.kind == MCActionKind::SUM) {
        // Level-sum select (MSG_SELECT_SUM, e.g. synchro materials): respond
        // with the must cards + the chosen candidates. The response is
        // [count][must seqs][chosen seqs]; the core validates the exact sum.
        auto& f = mainGame->dField;
        uint64_t mask = a.mask;
        for (size_t i = 0; i < f.selectsum_all.size(); ++i) {
            if (mask & (1ull << i)) {
                f.selectsum_all[i]->is_selected = true;
                f.selected_cards.push_back(f.selectsum_all[i]);
            }
        }
        f.SetResponseSelectedCards();
        f.ShowCancelOrFinishButton(0);
        if (mainGame->wCardSelect && mainGame->wCardSelect->isVisible())
            mainGame->HideElement(mainGame->wCardSelect, false);
        DuelClient::SendResponse();
    } else if (a.kind == MCActionKind::ANNOUNCE_RACE) {
        auto& f = mainGame->dField;
        auto* box = mainGame->chkRace[a.pos];
        box->setChecked(!box->isChecked());
        int rac = 0, count = 0, filter = 0x1;
        for (int i = 0; i < RACES_COUNT; ++i, filter <<= 1)
            if (mainGame->chkRace[i]->isChecked()) { rac |= filter; ++count; }
        if (count == f.announce_count) {
            DuelClient::SetResponseI(rac);
            DuelClient::SendResponse();
            if (mainGame->wANRace && mainGame->wANRace->isVisible())
                mainGame->HideElement(mainGame->wANRace, false);
        }
    } else if (a.kind == MCActionKind::ANNOUNCE_ATTRIB) {
        auto& f = mainGame->dField;
        auto* box = mainGame->chkAttribute[a.pos];
        box->setChecked(!box->isChecked());
        int att = 0, count = 0, filter = 0x1;
        for (int i = 0; i < 7; ++i, filter <<= 1)
            if (mainGame->chkAttribute[i]->isChecked()) { att |= filter; ++count; }
        if (count == f.announce_count) {
            DuelClient::SetResponseI(att);
            DuelClient::SendResponse();
            if (mainGame->wANAttribute && mainGame->wANAttribute->isVisible())
                mainGame->HideElement(mainGame->wANAttribute, false);
        }
    } else if (a.kind == MCActionKind::ANNOUNCE_CARD) {
        auto& f = mainGame->dField;
        if (a.pos < 0 || (size_t)a.pos >= f.ancard.size()) return;
        mainGame->lstANCard->setSelected(a.pos);
        DuelClient::SetResponseI(f.ancard[a.pos]);
        DuelClient::SendResponse();
        if (mainGame->wANCard && mainGame->wANCard->isVisible())
            mainGame->HideElement(mainGame->wANCard, false);
    } else if (a.kind == MCActionKind::ANNOUNCE_NUMBER) {
        if (a.pos < 0 || a.pos >= mainGame->cbANNumber->getItemCount()) return;
        mainGame->cbANNumber->setSelected(a.pos);
        DuelClient::SetResponseI(a.pos);
        DuelClient::SendResponse();
        if (mainGame->wANNumber && mainGame->wANNumber->isVisible())
            mainGame->HideElement(mainGame->wANNumber, false);
    } else {
        MCP_Click(a.elem);
    }
}

// Choice suffix for a card: " @Hand[3]", " @MZone[2] FD (ch1)". seq
// disambiguates same-name duplicates; position/chain mirror the GUI.
static std::string MCP_ChoiceSuffix(ClientCard* c) {
    if (!c) return "";
    std::ostringstream out;
    out << " @" << MCP_LocName(c->location) << "[" << (int)c->sequence << "]";
    if (c->location == LOCATION_MZONE)
        out << " " << MCP_PosMonster(c->position);
    else if (c->location == LOCATION_SZONE)
        out << " " << MCP_PosST(c->position);
    // chain involvement (GUI chain arrows)
    int n = 1;
    for (auto& ch : mainGame->dField.chains) {
        if (ch.controler == c->controler && ch.location == c->location && ch.sequence == c->sequence) {
            out << " (ch" << n << ")";
            return out.str();
        }
        ++n;
    }
    return out.str();
}

// Uniform prompt header for EVERY prompt type: "Option N~M/TOTAL:" where
// N~M = how many options to pick (min~max, inclusive) and TOTAL = the total
// number of listed options. Agents only need to parse this one line, then
// pick a random subset of size randint(N, M) from options 0..TOTAL-1.
static std::string MCP_OptionHeader(int min, int max, int total) {
    return "Option " + std::to_string(min) + "~" + std::to_string(max) + "/" + std::to_string(total) + ":\n";
}

// Cancel is NEVER an MCP option (the GUI's cancel was a misclick-withdrawal;
// an agent never misclicks). A cancelable prompt is translated to min=0 (an
// EMPTY selection = cancel), but only when min<=1; for min>=2 the cancelable
// flag is ignored (the select is effectively mandatory).
static int MCP_AdjMin(int min, bool cancelable) {
    if (cancelable && min <= 1) return 0;
    return min;
}

// Battle-phase / main-phase command list: every (card, command) pair.
// Single choice: pick exactly ONE option (including pass, when the end_phase
// flag is set).
static std::string MCP_CommandText() {
    auto& f = mainGame->dField;
    std::ostringstream out;
    int n = 0;
    auto add = [&](const std::vector<ClientCard*>& list, const char* cmd, irr::gui::IGUIButton* btn) {
        for (auto* c : list) {
            if (c->code) g_mention_ctx.insert(c->code & 0x7fffffff);
            out << n << ". " << MCP_CardName(c) << MCP_ChoiceSuffix(c) << " " << cmd << "\n";
            MCP_AddCardCmd(c, btn);
            ++n;
        }
    };
    if (mainGame->dInfo.curMsg == MSG_SELECT_IDLECMD || mainGame->dInfo.curMsg == MSG_SELECT_BATTLECMD) {
        add(f.summonable_cards, "summon", mainGame->btnSummon);
        add(f.spsummonable_cards, "special-summon", mainGame->btnSPSummon);
        add(f.msetable_cards, "set-monster", mainGame->btnMSet);
        add(f.ssetable_cards, "set-spell", mainGame->btnSSet);
        add(f.reposable_cards, "reposition", mainGame->btnRepos);
        add(f.attackable_cards, "attack", mainGame->btnAttack);
    }
    add(f.activatable_cards, "activate", mainGame->btnActivate);
    if (mainGame->dInfo.curMsg == MSG_SELECT_IDLECMD && mainGame->btnBP && mainGame->btnBP->isVisible()) {
        out << n << ". to battle phase\n";
        MCP_AddClickEl(mainGame->btnBP);
        ++n;
    }
    if (mainGame->dInfo.curMsg == MSG_SELECT_BATTLECMD && mainGame->btnM2 && mainGame->btnM2->isVisible()) {
        out << n << ". to main phase 2\n";
        MCP_AddClickEl(mainGame->btnM2);
        ++n;
    }
    if (mainGame->btnShuffle && mainGame->btnShuffle->isVisible()) {
        out << n << ". shuffle deck\n";
        MCP_AddClickEl(mainGame->btnShuffle);
        ++n;
    }
    if (mainGame->btnEP && mainGame->btnEP->isVisible()) {
        // end phase = the payload's end_phase flag (btnEP visibility)
        out << n << ". pass\n";
        MCP_AddClickEl(mainGame->btnEP);
        ++n;
    }
    return MCP_OptionHeader(1, 1, n) + out.str();
}

// SELECT_CARD-family: enumerate ALL selectable cards (list or field mode) as
// toggle actions. No cancel/finish options -- the agent's subset is the
// answer; an empty subset (indices=[]) cancels a cancelable prompt.
// unselect: MSG_SELECT_UNSELECT_CARD mode -- always respond with exactly ONE
// card (core check_response hardcodes 1,1).
static std::string MCP_CardsText(bool unselect = false) {
    auto& f = mainGame->dField;
    std::ostringstream out;
    int min = MCP_AdjMin(f.select_min, f.select_cancelable);
    int max = f.select_max;
    if (unselect) {
        // UNSELECT: cancel/finish flag permits the -1 (cancel) response
        min = f.select_cancelable ? 0 : 1;
        max = 1;
    }
    for (size_t i = 0; i < f.selectable_cards.size(); ++i) {
        auto* c = f.selectable_cards[i];
        if (c->code) g_mention_ctx.insert(c->code & 0x7fffffff);
        out << i << ". " << MCP_CardName(c) << MCP_ChoiceSuffix(c)
            << (c->is_selected ? " [selected]" : "") << "\n";
        MCP_AddToggleCard(c);
    }
    return MCP_OptionHeader(min, max, (int)f.selectable_cards.size()) + out.str();
}

// MSG_SELECT_SUM (level-sum select, e.g. synchro materials): the core accepts
// ONLY subsets whose sum equals select_sumval (mode 0). A random pick of
// cards almost never sums right -> MSG_RETRY loop. So the bridge enumerates
// every VALID combination (must cards + candidates whose opParam values sum
// to the target) and presents each combination as ONE option -- the agent's
// uniform random pick is then always accepted.
static void MCP_SumDfs(const std::vector<ClientCard*>& cards, size_t idx, int remaining,
                       int chosen, int min, int max, uint64_t mask,
                       std::vector<std::pair<std::string, uint64_t>>& out) {
    if (chosen >= min && remaining == 0) {
        std::string label;
        for (size_t i = 0; i < cards.size(); ++i)
            if (mask & (1ull << i))
                label += (label.empty() ? "" : " + ") + MCP_CardName(cards[i]);
        out.push_back({label, mask});
        return;
    }
    if (idx >= cards.size() || chosen >= max) return;
    for (size_t i = idx; i < cards.size(); ++i) {
        int o1 = cards[i]->opParam & 0xffff;
        int o2 = (cards[i]->opParam >> 16) & 0xffff;
        if (o2 & 0x8000) { o1 = (int)(cards[i]->opParam & 0x7fffffff); o2 = 0; }
        if (o1 > 0 && remaining >= o1)
            MCP_SumDfs(cards, i + 1, remaining - o1, chosen + 1, min, max,
                       mask | (1ull << i), out);
        if (o2 > 0 && o2 != o1 && remaining >= o2)
            MCP_SumDfs(cards, i + 1, remaining - o2, chosen + 1, min, max,
                       mask | (1ull << i), out);
    }
}

static std::string MCP_SumText() {
    auto& f = mainGame->dField;
    int must_sum = 0;
    std::string must_label;
    for (auto* c : f.selected_cards) {          // must cards (from the payload)
        must_sum += (int)(c->opParam & 0xffff);
        must_label += (must_label.empty() ? "" : " + ") + MCP_CardName(c);
    }
    int acc = f.select_sumval - must_sum;
    int min = f.select_min > 0 ? f.select_min : 1;
    int max = f.select_max > 0 ? f.select_max : (int)f.selectsum_all.size();
    std::vector<std::pair<std::string, uint64_t>> combos;
    MCP_SumDfs(f.selectsum_all, 0, acc, 0, min, max, 0, combos);
    std::ostringstream out;
    int n = 0;
    for (auto& c : combos) {
        if (n >= 20) break;
        out << n << ". " << (must_label.empty() ? "" : must_label + " + ") << c.first
            << " (sum " << (acc + must_sum) << ")\n";
        g_actions.push_back({MCActionKind::SUM, nullptr, nullptr, 0, c.second});
        ++n;
    }
    return MCP_OptionHeader(1, 1, n) + out.str();
}

// SELECT_OPTION: pick ONE option (all listed, no 5-button cap); the response
// is the option index. Cancelable => min 0 (empty = cancel).
static std::string MCP_OptionsText() {
    auto& f = mainGame->dField;
    std::ostringstream out;
    for (size_t i = 0; i < f.select_options.size(); ++i) {
        out << i << ". " << U8(dataManager.GetDesc(f.select_options[i])) << "\n";
        g_actions.push_back({MCActionKind::OPTION, nullptr, nullptr, (int)i});
    }
    return MCP_OptionHeader(MCP_AdjMin(1, f.select_cancelable), 1, (int)f.select_options.size()) + out.str();
}

static std::string MCP_YesNoText() {
    MCP_AddClickEl(mainGame->btnYes);
    MCP_AddClickEl(mainGame->btnNo);
    return MCP_OptionHeader(1, 1, 2) + "0. yes\n1. no\n";
}

static std::string MCP_FieldPosText() {
    // SELECT_POSITION: choose a position (sent synchronously).
    g_actions.push_back({MCActionKind::POS, nullptr, nullptr, POS_FACEUP_ATTACK});
    g_actions.push_back({MCActionKind::POS, nullptr, nullptr, POS_FACEDOWN_DEFENSE});
    g_actions.push_back({MCActionKind::POS, nullptr, nullptr, POS_FACEUP_DEFENSE});
    g_actions.push_back({MCActionKind::POS, nullptr, nullptr, POS_FACEDOWN_ATTACK});
    return MCP_OptionHeader(1, 1, 4) + "0. face-up attack\n1. face-down defense\n2. face-up defense\n3. face-down attack\n";
}

// SELECT_PLACE / SELECT_DISFIELD: pick field zones (bits of selectable_field).
static std::string MCP_PlaceText() {
    auto& f = mainGame->dField;
    std::ostringstream out;
    int n = 0;
    auto addz = [&](int ctrl, int loc, int seq) {
        unsigned flag = ClientField::ZoneFlag(ctrl, loc, seq);
        if (!(f.selectable_field & flag)) return;
        out << n << ". " << (ctrl ? "oppo" : "self") << " " << MCP_LocName(loc) << "[" << seq << "]\n";
        g_actions.push_back({MCActionKind::PLACE, nullptr, nullptr, (int)flag});
        ++n;
    };
    for (int ctrl = 0; ctrl < 2; ++ctrl) {
        for (int i = 0; i < 7; ++i) addz(ctrl, LOCATION_MZONE, i);
        for (int i = 0; i < 8; ++i) addz(ctrl, LOCATION_SZONE, i);
    }
    int min = MCP_AdjMin(f.select_min, f.select_cancelable);
    return MCP_OptionHeader(min, f.select_min, n) + out.str();
}

// ANNOUNCE_RACE: check announce_count race checkboxes.
static std::string MCP_AnnounceRaceText() {
    auto& f = mainGame->dField;
    std::ostringstream out;
    int n = 0;
    for (int i = 0; i < RACES_COUNT; ++i) {
        if (mainGame->chkRace[i]->isVisible()) {
            out << n << ". " << U8(dataManager.GetSysString(DataManager::STRING_ID_RACE + i)) << "\n";
            g_actions.push_back({MCActionKind::ANNOUNCE_RACE, nullptr, nullptr, i});
            ++n;
        }
    }
    return MCP_OptionHeader((int)f.announce_count, (int)f.announce_count, n) + out.str();
}

// ANNOUNCE_ATTRIB: check announce_count attribute checkboxes.
static std::string MCP_AnnounceAttribText() {
    auto& f = mainGame->dField;
    std::ostringstream out;
    int n = 0;
    for (int i = 0; i < 7; ++i) {
        if (mainGame->chkAttribute[i]->isVisible()) {
            out << n << ". " << U8(dataManager.GetSysString(DataManager::STRING_ID_ATTRIBUTE + i)) << "\n";
            g_actions.push_back({MCActionKind::ANNOUNCE_ATTRIB, nullptr, nullptr, i});
            ++n;
        }
    }
    return MCP_OptionHeader((int)f.announce_count, (int)f.announce_count, n) + out.str();
}

// ANNOUNCE_CARD: pick a card name from the declarable list.
static std::string MCP_AnnounceCardText() {
    auto& f = mainGame->dField;
    std::ostringstream out;
    int n = 0;
    for (int i = 0; i < mainGame->lstANCard->getItemCount(); ++i) {
        out << n << ". " << U8(mainGame->lstANCard->getListItem(i)) << "\n";
        g_actions.push_back({MCActionKind::ANNOUNCE_CARD, nullptr, nullptr, i});
        ++n;
    }
    return MCP_OptionHeader(1, 1, n) + out.str();
}

// ANNOUNCE_NUMBER: pick a number from the combo.
static std::string MCP_AnnounceNumberText() {
    std::ostringstream out;
    int n = 0;
    for (int i = 0; i < mainGame->cbANNumber->getItemCount(); ++i) {
        out << n << ". " << mainGame->cbANNumber->getItemData(i) << "\n";
        g_actions.push_back({MCActionKind::ANNOUNCE_NUMBER, nullptr, nullptr, i});
        ++n;
    }
    return MCP_OptionHeader(1, 1, n) + out.str();
}

static bool MCP_IsPrompt(short curMsg) {
    switch (curMsg) {
    case MSG_SELECT_BATTLECMD:
    case MSG_SELECT_IDLECMD:
    case MSG_SELECT_EFFECTYN:
    case MSG_SELECT_YESNO:
    case MSG_SELECT_OPTION:
    case MSG_SELECT_CARD:
    case MSG_SELECT_UNSELECT_CARD:
    case MSG_SELECT_TRIBUTE:
    case MSG_SELECT_SUM:
    case MSG_SELECT_CHAIN:
    case MSG_SELECT_PLACE:
    case MSG_SELECT_DISFIELD:
    case MSG_SELECT_POSITION:
    case MSG_SELECT_COUNTER:
    case MSG_SORT_CARD:
    case MSG_ANNOUNCE_RACE:
    case MSG_ANNOUNCE_ATTRIB:
    case MSG_ANNOUNCE_CARD:
    case MSG_ANNOUNCE_NUMBER:
    case MSG_CONFIRM_CARDS:
        return true;
    default:
        return false;
    }
}

// Render the pending prompt's choice list (fills g_actions).
static std::string MCP_PromptText(short curMsg) {
    Game* g = mainGame;
    // The option window (wOptions) can appear from MSG_SELECT_OPTION OR from
    // a multi-activation inside IDLE/BATTLE (a card with several effects,
    // e.g. "remove or banish"). Render it whenever visible.
    if (g->wOptions && g->wOptions->isVisible() && !g->dField.select_options.empty())
        return MCP_OptionsText();
    switch (curMsg) {
    case MSG_SELECT_OPTION: return MCP_OptionsText();
    case MSG_SELECT_YESNO:
    case MSG_SELECT_EFFECTYN:
        if (g->wQuery && g->wQuery->isVisible()) return MCP_YesNoText();
        break;
    case MSG_SELECT_CARD:
    case MSG_SELECT_TRIBUTE:
    case MSG_SELECT_COUNTER:
        // List window or field mode: both are the same selectable_cards set.
        if (!g->dField.selectable_cards.empty()) return MCP_CardsText();
        break;
    case MSG_SELECT_SUM:
        // Level-sum select: valid combinations are enumerated (mode 0).
        if (!g->dField.selectsum_all.empty()) return MCP_SumText();
        break;
    case MSG_SELECT_UNSELECT_CARD: {
        // UNSELECT is ALWAYS a single-card pick: the core's response check is
        // hardcoded check_response(size, 1, 1) regardless of the payload's
        // min/max bytes (custom XYZ material selection via SelectSubgroup/
        // SelectUnselect sends min=max=2). The game's own toggle sends after
        // one pick; so render min=max=1 (min 0 when cancelable/finishable)
        // and let the toggle path respond.
        if (!g->dField.selectable_cards.empty()) return MCP_CardsText(true);
        break;
    }
    case MSG_SELECT_CHAIN: {
        // Chain prompt: the response is an INT (index into activatable_descs
        // or -1), NOT a card list. Forced chains are auto-answered by the
        // client (chkAutoChain in system.conf) -- nothing to do here.
        // All options: decline, or chain with each chainable card.
        if (g->wQuery && g->wQuery->isVisible()
            || (g->wCardSelect && g->wCardSelect->isVisible())) {
            std::ostringstream out;
            int n = 0;
            out << n << ". decline\n";
            g_actions.push_back({MCActionKind::CHAIN_DECLINE, nullptr, nullptr, 0});
            ++n;
            auto& f = g->dField;
            for (size_t i = 0; i < f.activatable_cards.size(); ++i) {
                auto* c = f.activatable_cards[i];
                if (!c || (f.activatable_descs[i].second & 0x8000)) continue;  // EDESC_OPERATION
                if (c->code) g_mention_ctx.insert(c->code & 0x7fffffff);
                out << n << ". chain with " << MCP_CardName(c) << MCP_ChoiceSuffix(c) << "\n";
                g_actions.push_back({MCActionKind::CHAIN_CARD, nullptr, c, (int)i});
                ++n;
            }
            return MCP_OptionHeader(1, 1, n) + out.str();
        }
        break;
    }
    case MSG_SELECT_PLACE:
    case MSG_SELECT_DISFIELD:
        return MCP_PlaceText();
    case MSG_SELECT_POSITION:
        return MCP_FieldPosText();
    case MSG_CONFIRM_CARDS:
        if (g->wCardSelect && g->wCardSelect->isVisible()) {
            MCP_AddClickEl(g->btnSelectOK);
            return MCP_OptionHeader(1, 1, 1) + "0. OK\n";
        }
        break;
    case MSG_SELECT_BATTLECMD:
    case MSG_SELECT_IDLECMD:
        return MCP_CommandText();
    case MSG_ANNOUNCE_RACE:
        if (g->wANRace && g->wANRace->isVisible()) return MCP_AnnounceRaceText();
        break;
    case MSG_ANNOUNCE_ATTRIB:
        if (g->wANAttribute && g->wANAttribute->isVisible()) return MCP_AnnounceAttribText();
        break;
    case MSG_ANNOUNCE_CARD:
        if (g->wANCard && g->wANCard->isVisible()) return MCP_AnnounceCardText();
        break;
    case MSG_ANNOUNCE_NUMBER:
        if (g->wANNumber && g->wANNumber->isVisible()) return MCP_AnnounceNumberText();
        break;
    case MSG_SORT_CARD:
        // SORT: choose the card order via indices (respond in sort order).
        if (!g->dField.selectable_cards.empty()) {
            std::ostringstream out;
            for (size_t i = 0; i < g->dField.selectable_cards.size(); ++i) {
                auto* c = g->dField.selectable_cards[i];
                if (c->code) g_mention_ctx.insert(c->code & 0x7fffffff);
                out << i << ". " << MCP_CardName(c) << MCP_ChoiceSuffix(c) << "\n";
            }
            return MCP_OptionHeader((int)g->dField.selectable_cards.size(),
                                    (int)g->dField.selectable_cards.size(),
                                    (int)g->dField.selectable_cards.size()) + out.str();
        }
        break;
    default:
        break;
    }
    return "";
}

static int MCP_ExplainRounds() {
    const char* e = std::getenv("YGOCLI_EXPLAIN_ROUNDS");
    if (!e) return 3;
    int n = atoi(e);
    return n >= 1 ? n : 1;
}

// STOC seq at which the agent last acted (ygo_choose). The text block shows
// a prompt only when seq != last_acted (i.e. a fresh message arrived).
static std::atomic<uint64_t> g_last_acted_seq{ 0 };
static int g_detail_round = 0;                              // ygo_log read counter
static std::unordered_map<uint32_t, int> g_last_detail;     // code -> round explained

static bool MCP_PromptPending() {
    if (!mainGame || !mainGame->dInfo.isStarted || mainGame->dInfo.isReplay) return false;
    if (mainGame->dInfo.isFinished) return false;
    if (!MCP_IsPrompt(mainGame->dInfo.curMsg)) return false;
    uint64_t seq = g_stoc_seq.load(std::memory_order_relaxed);
    return seq != g_last_acted_seq.load(std::memory_order_relaxed);
}

// Full duel board + prompt surface. Line 1 keeps the legacy
// "Turn N | LP X / Y" shape so older consumers can still regex it.
// The text must be STABLE for a given prompt instance (consumers key
// rejection/retry tracking on the whole text) -- so NO auto-explain lines
// here: cards mentioned (field/choices) are pushed to g_mentioned and the
// ---------- board format (design doc section 4) ----------

// Phase short names for the turn line: DP/SP/M1/BP/M2/EP.
static const char* MCP_PhaseShort(uint16_t ph) {
    switch (ph) {
    case PHASE_DRAW: return "DP";
    case PHASE_STANDBY: return "SP";
    case PHASE_MAIN1: return "M1";
    case PHASE_BATTLE_START:
    case PHASE_BATTLE_STEP:
    case PHASE_DAMAGE:
    case PHASE_DAMAGE_CAL:
    case PHASE_BATTLE: return "BP";
    case PHASE_MAIN2: return "M2";
    case PHASE_END: return "EP";
    default: return "?";
    }
}


// One pile entry: name (or ?) plus special-state parens, deduped as name*N.
// Dedup key = (code, marker); first-seen order preserved.
struct PileEntry { std::string text; uint32_t code; std::string marker; int count = 0; };

static std::string MCP_PileLine(const char* label, int loc, const std::vector<ClientCard*>& cards,
                                bool known_when_down /* face-down name known (self piles) */) {
    std::vector<PileEntry> entries;
    auto add = [&](ClientCard* c, const std::string& marker) {
        std::string name = (c && c->code) ? U8(dataManager.GetName(c->code)) : "?";
        for (auto& e : entries) {
            if (e.code == (c ? c->code : 0) && e.marker == marker) { e.count++; return; }
        }
        PileEntry e; e.code = c ? c->code : 0; e.marker = marker; e.count = 1;
        e.text = name + (marker.empty() ? "" : "(" + marker + ")");
        entries.push_back(e);
    };
    // Face-down is SPECIAL only for grave/remove (design doc §4.2): own keeps
    // its name `name(down)`, the opponent's is `?(down)`. For deck/extra/hand
    // face-down is NORMAL (no marker) and face-up is the revealed `(up)` state.
    bool down_special = (loc == LOCATION_GRAVE || loc == LOCATION_REMOVED);
    for (auto* c : cards) {
        bool up = c && (c->position & POS_FACEUP);
        bool down = c && (c->position & POS_FACEDOWN);
        if (up && !down_special) add(c, "up");               // deck/extra/hand revealed
        else if (down && down_special) {                     // grave/remove face-down
            if (known_when_down) add(c, "down");             // own: name(down)
            else add(nullptr, "down");                       // oppo: ?(down)
        } else {
            add(c, "");                                      // normal / hidden name
        }
    }
    std::ostringstream out;
    out << "  " << label << ": ";
    if (entries.empty()) { out << "\n"; return out.str(); }
    bool first = true;
    for (auto& e : entries) {
        if (!first) out << ", ";
        first = false;
        out << e.text << (e.count > 1 ? "*" + std::to_string(e.count) : "");
    }
    out << "\n";
    return out.str();
}

// Field cell: name + parens (position, stat changes, disabled, self/oppo tag).
static std::string MCP_FieldCell(ClientCard* c, bool tag_owner = false) {
    if (!c) return "-";
    std::string name = c->code ? U8(dataManager.GetName(c->code)) : "?";
    std::vector<std::string> marks;
    if (tag_owner) marks.push_back(c->controler == 0 ? "self" : "oppo");
    if (c->location == LOCATION_MZONE) {
        if (c->position == POS_FACEUP_DEFENSE) marks.push_back("def");
        else if (c->position == POS_FACEDOWN_DEFENSE || c->position == POS_FACEDOWN_ATTACK)
            marks.push_back("down");
        // stat changes vs the printed card text
        CardData base;
        if (c->code && dataManager.GetData(c->code, &base)) {
            if (c->level != (base.level & 0xff)) marks.push_back("\xe2\x98\x85" + std::to_string(c->level));
            if (c->attack != base.attack && c->attack >= 0) marks.push_back("atk" + std::to_string(c->attack));
            if (c->defense != base.defense && c->defense >= 0) marks.push_back("def" + std::to_string(c->defense));
        }
    } else if (c->position == POS_FACEDOWN) {
        marks.push_back("down");
    }
    if (c->status & STATUS_DISABLED) marks.push_back("dis");
    if (!marks.empty()) {
        name += "(";
        for (size_t i = 0; i < marks.size(); ++i) name += (i ? "/" : "") + marks[i];
        name += ")";
    }
    return name;
}

// The 5x5 matrix + fzone column (design doc section 4.1). Rows are fixed:
// row0 oppo szone, row1 oppo mzone, row2 shared EMZ, row3 self mzone,
// row4 self szone. EMZ seqs are mirrored: (row2,col1) = mzone[0][5]/mzone[1][6],
// (row2,col3) = mzone[0][6]/mzone[1][5].
static void MCP_FieldTable(std::ostringstream& out, ClientField& f) {
    auto cell = [&](ClientCard* c, bool tag) { return MCP_FieldCell(c, tag); };
    auto szone_row = [&](int p) {
        // col0..col4 = szone[p][0..4] (0/4 double as pendulum), fzone col = szone[p][5]
        std::vector<std::string> cells;
        for (int i = 0; i < 5; ++i) cells.push_back(cell(f.szone[p][i], false));
        cells.push_back(cell(f.szone[p][5], false));
        return cells;
    };
    auto mzone_row = [&](int p) {
        std::vector<std::string> cells;
        for (int i = 0; i < 5; ++i) cells.push_back(cell(f.mzone[p][i], false));
        cells.push_back("-");
        return cells;
    };
    auto emz_row = [&]() {
        // col1: mzone[0][5] (self) or mirrored mzone[1][6] (oppo); col3: mzone[0][6] / mzone[1][5]
        ClientCard* c1 = f.mzone[0][5] ? f.mzone[0][5] : f.mzone[1][6];
        ClientCard* c3 = f.mzone[0][6] ? f.mzone[0][6] : f.mzone[1][5];
        std::vector<std::string> cells = {"-", cell(c1, true), "-", cell(c3, true), "-", "-"};
        return cells;
    };
    auto emit = [&](const char* rowlabel, const std::vector<std::string>& cells) {
        out << "| " << rowlabel << " ";
        for (auto& c : cells) out << "| " << c << " ";
        out << "|\n";
    };
    out << "|      | col0 | col1 | col2 | col3 | col4 | fzone |\n";
    out << "| --- | --- | --- | --- | --- | --- | --- |\n";
    emit("row0", szone_row(1));
    emit("row1", mzone_row(1));
    emit("row2", emz_row());
    emit("row3", mzone_row(0));
    emit("row4", szone_row(0));
}

static std::string MCP_BoardText() {
    Game* g = mainGame;
    auto& f = g->dField;
    std::ostringstream out;

    // turn/phase line: T<n> <self|oppo> turn, <phase>
    int turn = g->dInfo.turn;
    out << "T" << turn << " " << (g_cur_turn_player.load() == 0 ? "self" : "oppo")
        << " turn, " << MCP_PhaseShort(g_phase.load()) << "\n\n";

    // oppo piles
    out << "oppo lp" << g->dInfo.lp[1] << "\n";
    out << MCP_PileLine("deck", LOCATION_DECK, f.deck[1], false);
    out << MCP_PileLine("extra", LOCATION_EXTRA, f.extra[1], false);
    out << MCP_PileLine("hand", LOCATION_HAND, f.hand[1], false);
    out << MCP_PileLine("grave", LOCATION_GRAVE, f.grave[1], false);
    out << MCP_PileLine("remove", LOCATION_REMOVED, f.remove[1], false);
    out << "\n";

    MCP_FieldTable(out, f);
    out << "\n";

    // self piles: deck/extra from the uploaded deck (the client never learns
    // its in-game deck order); hand from the client hand; grave/remove public.
    out << "self lp" << g->dInfo.lp[0] << "\n";
    {
        // deck: contents of the uploaded deck (dedup, first-seen order)
        std::vector<std::string> names;
        for (auto* c : deckManager.current_deck.main) {
            std::string n = U8(dataManager.GetName(c->code));
            names.push_back(n.empty() ? "?" : n);
        }
        std::ostringstream dl;
        dl << "  deck: ";
        std::vector<std::pair<std::string,int>> ded;
        for (auto& n : names) {
            bool found = false;
            for (auto& e : ded) if (e.first == n) { e.second++; found = true; break; }
            if (!found) ded.push_back({n, 1});
        }
        bool first = true;
        for (auto& e : ded) {
            if (!first) dl << ", ";
            first = false;
            dl << e.first << (e.second > 1 ? "*" + std::to_string(e.second) : "");
        }
        dl << "\n";
        out << dl.str();
    }
    out << MCP_PileLine("extra", LOCATION_EXTRA, f.extra[0], true);
    out << MCP_PileLine("hand", LOCATION_HAND, f.hand[0], true);
    out << MCP_PileLine("grave", LOCATION_GRAVE, f.grave[0], true);
    out << MCP_PileLine("remove", LOCATION_REMOVED, f.remove[0], true);

    if (g->dInfo.isFinished) out << "Game Over\n";
    return out.str();
}

static std::string MCP_ChoicesText() {
    g_actions.clear();
    g_mention_ctx.clear();
    if (!mainGame || !mainGame->device) return "";
    Game* g = mainGame;
    // RPS -- window state is the accurate "pending" signal here (the click
    // handler closes it on answer); two protocol contexts exist (pre-duel
    // STOC_SELECT_HAND and in-duel MSG_ROCK_PAPER_SCISSORS).
    if (g->wHand && g->wHand->isVisible()) {
        MCP_AddClick(BUTTON_HAND1); MCP_AddClick(BUTTON_HAND2); MCP_AddClick(BUTTON_HAND3);
        return MCP_OptionHeader(1, 1, 3) + "0. rock\n1. paper\n2. scissors\n";
    }
    // Turn order
    if (g->wFTSelect && g->wFTSelect->isVisible()) {
        MCP_AddClick(BUTTON_FIRST); MCP_AddClick(BUTTON_SECOND);
        return MCP_OptionHeader(1, 1, 2) + "0. go first\n1. go second\n";
    }
    // Mandatory-ack message box (1~1/1; normally auto-acked before it gets here)
    if (g->wMessage && g->wMessage->isVisible()) {
        MCP_AddClickEl(g->btnMsgOK);
        return MCP_OptionHeader(1, 1, 1) + "0. OK\n";
    }
    if (g->dInfo.isFinished) return "";
    std::string choices;
    if (g->wOptions && g->wOptions->isVisible() && !g->dField.select_options.empty()) {
        // Multi-activation options window (e.g. "banish or search"): opened
        // by the activate button handler WITHOUT a new STOC message, so the
        // seq gate does not apply -- window-driven, render unconditionally.
        choices = MCP_OptionsText();
    } else if (MCP_IsPrompt(g->dInfo.curMsg)) {
        // seq gate: curMsg stays at the last prompt type after answering
        // until the next message; show the prompt only for a fresh instance.
        uint64_t seq = g_stoc_seq.load(std::memory_order_relaxed);
        bool acted = (seq == g_last_acted_seq.load(std::memory_order_relaxed));
        if (!acted) choices = MCP_PromptText(g->dInfo.curMsg);
    }
    // Explain candidates: narration mentions since the last drain, plus every
    // card rendered in the choices -- collected for the card-explain section.
    if (!g_mention_ctx.empty()) {
        std::lock_guard<std::mutex> lk(g_log_mtx);
        for (auto c : g_mention_ctx) g_mentioned.insert(c);
        g_mention_ctx.clear();
    }
    return choices;
}

// Parse the first "Option a~b/c:" line of a choices block.
static bool MCP_ParseOptionHeader(const std::string& s, int& min, int& max, int& total) {
    size_t p = s.find("Option ");
    if (p == std::string::npos) return false;
    int a = 0, b = 0, c = 0;
    if (sscanf(s.c_str() + p, "Option %d~%d/%d:", &a, &b, &c) != 3) return false;
    min = a; max = b; total = c;
    return true;
}

// Auto-answer a prompt with exactly ONE way to answer (options total == 1 and
// min >= 1, e.g. 1~1/1 boxes, 1~99/1 single-card selects). Returns true when
// something was acted. NOT for 2~N/2 (order/count matters there).
static bool MCP_AutoAnswer() {
    if (!mainGame) return false;
    Game* g = mainGame;
    // Blocking boxes are 1~1/1: ack immediately (RETRY ack must not mark acted).
    if (g->wMessage && g->wMessage->isVisible()) {
        dbg("auto-answer-message");
        if (g->btnMsgOK) MCP_Click(g->btnMsgOK);
        if (g->dInfo.curMsg != MSG_RETRY)
            g_last_acted_seq.store(g_stoc_seq.load(std::memory_order_relaxed));
        return true;
    }
    bool options_w = g->wOptions && g->wOptions->isVisible() && !g->dField.select_options.empty();
    if (!options_w && !MCP_PromptPending()) return false;
    std::string choices = MCP_ChoicesText();
    int a = 0, b = 0, c = 0;
    if (!MCP_ParseOptionHeader(choices, a, b, c)) return false;
    if (c == 1 && a >= 1 && !g_actions.empty()) {
        MCP_RunAction(g_actions[0]);
        g_last_acted_seq.store(g_stoc_seq.load(std::memory_order_relaxed));
        return true;
    }
    return false;
}

// A blocking call completes (the agent is asked to decide) when a decision is
// pending that is NOT auto-answerable.
static bool MCP_DecisionPending() {
    if (!mainGame) return true;
    Game* g = mainGame;
    if (g_connect_err_set) return true;
    if (g->dInfo.isFinished) return true;
    if (g->wHand && g->wHand->isVisible()) return true;      // RPS 1~1/3
    if (g->wFTSelect && g->wFTSelect->isVisible()) return true;  // turn order 1~1/2
    std::string choices = MCP_ChoicesText();
    if (choices.empty()) return false;
    int a = 0, b = 0, c = 0;
    if (MCP_ParseOptionHeader(choices, a, b, c))
        return !(c == 1 && a >= 1);   // unique (auto-answered) is not a decision
    return true;
}

// ---------------------------------------------------------------------------
// text block (every tool response: log -> state -> card explain -> options)
// ---------------------------------------------------------------------------

// Drain narration + incoming chat. Returns "" when empty.
static std::string MCP_DrainLog() {
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lk(g_log_mtx);
        for (auto& s : g_log) lines.push_back(s);
        g_log.clear();
    }
    std::string out;
    for (auto& s : lines) out += s + "\n";
    return out;
}

// Card explains for mentioned-but-not-recently-explained cards (recency =
// YGOCLI_EXPLAIN_ROUNDS log reads, default 3). Drains with the block.
static std::string MCP_DrainExplains() {
    std::set<uint32_t> cands;
    std::set<uint32_t> force;
    {
        std::lock_guard<std::mutex> lk(g_log_mtx);
        cands.swap(g_mentioned);
        force.swap(g_force_explain);
    }
    int rounds = MCP_ExplainRounds();
    g_detail_round++;
    std::vector<uint32_t> toexplain;
    for (uint32_t code : force) toexplain.push_back(code);   // explicit ygo_card: always
    for (uint32_t code : cands) {
        if (!code) continue;
        auto it = g_last_detail.find(code);
        int last = (it == g_last_detail.end()) ? -1 : it->second;
        if (last == -1 || g_detail_round - last >= rounds) toexplain.push_back(code);
    }
    std::sort(toexplain.begin(), toexplain.end());
    toexplain.erase(std::unique(toexplain.begin(), toexplain.end()), toexplain.end());
    std::string out;
    for (uint32_t code : toexplain) {
        std::string line = MCP_CardLine(code);
        if (!line.empty()) {
            out += line + "\n";
            g_last_detail[code] = g_detail_round;
        }
    }
    return out;
}

static std::string MCP_BlockText() {
    std::ostringstream out;
    std::string log = MCP_DrainLog();
    // Connect-flow failure reasons (design doc §3) ride in the # log section
    // as "[!] <reason>" so the agent sees why ygo_connect failed.
    if (g_connect_err_set) {
        log += "[!] " + g_connect_err + "\n";
        g_connect_err_set = false;   // surfaced once
    }
    if (!log.empty()) out << "# log\n" << log << "\n";
    out << "# state\n" << MCP_BoardText() << "\n";
    std::string explains = MCP_DrainExplains();
    if (!explains.empty()) out << "# card explain\n" << explains << "\n";
    std::string choices = MCP_ChoicesText();
    if (!choices.empty()) {
        int a = 0, b = 0, c = 0;
        if (MCP_ParseOptionHeader(choices, a, b, c))
            out << "# options: " << a << "~" << b << "/" << c << "\n";
        else
            out << "# options\n";
        size_t nl = choices.find('\n');
        out << (nl == std::string::npos ? "" : choices.substr(nl + 1));
    }
    return out.str();
}

static void MCP_CompleteJob(MCPJob* job, const std::string& text) {
    job->ok = true;
    job->result = {{"content", json::array({json{{"type", "text"}, {"text", text}}})}};
    {
        std::lock_guard<std::mutex> lk(job->mtx);
        job->done = true;
    }
    job->cv.notify_one();
}

static void MCP_FinishJob(MCPJob* job) {   // for already-set result/error fields
    {
        std::lock_guard<std::mutex> lk(job->mtx);
        job->done = true;
    }
    job->cv.notify_one();
}

// ---------------------------------------------------------------------------
// ygo_connect: join room, upload deck, click ready, poll start (if host), then
// block until the first decision point (RPS / turn order / prompt) or a failure.
// ---------------------------------------------------------------------------

static std::string MCP_DoConnect(const json& args) {
    if (g_connect_phase != ConnectPhase::NONE) return "Error: already connecting";
    if (GetConnectState() != CONNECT_STATE_NONE) return "Error: already connected";
    if (!mainGame) return "Error: game not ready";
    // Port may arrive as a JSON string ("7911") or a number (7911); the
    // client's ygo_connect schema declares it a string but be tolerant.
    std::string port;
    if (args.contains("port")) {
        const json& p = args["port"];
        if (p.is_string()) port = p.get<std::string>();
        else if (p.is_number_integer()) port = std::to_string(p.get<int64_t>());
    }
    std::string host = args.value("host", "");
    std::string pass = args.value("password", "");
    std::string name = args.value("name", "");
    std::string deck = args.value("deck", "");
    if (host.empty()) return "Error: host required";
    if (deck.empty()) return "Error: deck required";
    // deck (UTF-8 path -> wide)
    wchar_t wdeck[512];
    BufferIO::DecodeUTF8(deck.c_str(), wdeck);
    if (!deckManager.LoadCurrentDeck(wdeck))
        return "Error: cannot load deck: " + deck;
    // join fields
    wchar_t wbuf[256];
    BufferIO::DecodeUTF8(host.c_str(), wbuf);
    mainGame->ebJoinHost->setText(wbuf);
    if (!port.empty()) {
        BufferIO::DecodeUTF8(port.c_str(), wbuf);
        mainGame->ebJoinPort->setText(wbuf);
    }
    if (!pass.empty()) {
        BufferIO::DecodeUTF8(pass.c_str(), wbuf);
        mainGame->ebJoinPass->setText(wbuf);
    }
    if (!name.empty()) {
        BufferIO::DecodeUTF8(name.c_str(), wbuf);
        mainGame->ebNickName->setText(wbuf);
    }
    mainGame->HideElement(mainGame->wMainMenu);
    ClickButton(mainGame->btnJoinHost);
    if (std::getenv("YGOCLI_MCP_DEBUG"))
        fprintf(stderr, "[mcp] connect: clicked join host\n");
    g_connect_phase = ConnectPhase::JOIN;
    g_connect_err_set = false;
    g_connect_err.clear();
    g_connect_deck_err = false;
    g_connect_t0 = std::chrono::steady_clock::now();
    return "connecting to " + host + ":" + (port.empty() ? "7911" : port);
}

static std::chrono::steady_clock::time_point g_connect_last_start_click;
static bool g_connect_readied = false;

static void MCP_ConnectTick() {
    if (!mainGame) return;
    Game* g = mainGame;
    auto fail = [&](const char* why) {
        g_connect_err = why;
        g_connect_err_set = true;
        g_connect_phase = ConnectPhase::NONE;
        g_connect_readied = false;
    };
    // CLI -j entry (or any path landing in the room-prep without ygo_connect):
    // drive ready + start the same way as the ygo_connect flow.
    if (g_connect_phase == ConnectPhase::NONE
        && g->wHostPrepare && g->wHostPrepare->isVisible() && !g->dInfo.isStarted) {
        if (std::getenv("YGOCLI_MCP_DEBUG"))
            fprintf(stderr, "[mcp] connect: entered from room-prep (deckmain=%zu)\n",
                deckManager.current_deck.main.size());
        g_connect_phase = ConnectPhase::READY;
        // ygo_connect preloads the deck; CLI entry loads it from the combos
        if (deckManager.current_deck.main.empty()) {
            deckManager.LoadCurrentDeck(g->cbCategorySelect->getSelected(),
                                        g->cbCategorySelect->getText(),
                                        g->cbDeckSelect->getText());
        }
    }
    if (g_connect_phase == ConnectPhase::NONE) return;
    switch (g_connect_phase) {
    case ConnectPhase::JOIN: {
        if (std::getenv("YGOCLI_MCP_DEBUG"))
            fprintf(stderr, "[mcp] connect: JOIN phase (prep=%d menu=%d deckerr=%d)\n",
                g->wHostPrepare && g->wHostPrepare->isVisible(),
                g->wMainMenu && g->wMainMenu->isVisible(), (int)g_connect_deck_err);
        if (g_connect_deck_err) { fail("deck rejected"); break; }
        if (g->wHostPrepare && g->wHostPrepare->isVisible()) {
            g_connect_phase = ConnectPhase::READY;
            break;
        }
        if (g->wMainMenu && g->wMainMenu->isVisible()) { fail("connect failed"); break; }
        if (std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - g_connect_t0).count() > 30) {
            fail("connect timeout");
        }
        break;
    }
    case ConnectPhase::READY: {
        if (std::getenv("YGOCLI_MCP_DEBUG"))
            fprintf(stderr, "[mcp] connect: READY (start=%d started=%d)\n",
                g->btnHostPrepStart && g->btnHostPrepStart->isVisible(), (int)g->dInfo.isStarted);
        if (g->dInfo.isStarted || (g->wHand && g->wHand->isVisible())) {
            g_connect_phase = ConnectPhase::DONE;
            break;
        }
        if (g->wMainMenu && g->wMainMenu->isVisible()) { fail("left the room"); break; }
        // send deck + ready exactly once
        if (!g_connect_readied) {
            if (std::getenv("YGOCLI_MCP_DEBUG"))
                fprintf(stderr, "[mcp] connect: sending deck+ready (deckmain=%zu)\n",
                    deckManager.current_deck.main.size());
            UpdateDeck();
            DuelClient::SendPacketToServer(CTOS_HS_READY);
            g_connect_readied = true;
        }
        // host: poll the start button (clickable when both players are ready)
        if (g->btnHostPrepStart && g->btnHostPrepStart->isVisible()) {
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - g_connect_last_start_click).count() >= 1000) {
                ClickButton(g->btnHostPrepStart);
                g_connect_last_start_click = now;
                g_connect_phase = ConnectPhase::START;
            }
        }
        break;
    }
    case ConnectPhase::START: {
        if (std::getenv("YGOCLI_MCP_DEBUG"))
            fprintf(stderr, "[mcp] connect: START (started=%d hand=%d)\n",
                (int)g->dInfo.isStarted, g->wHand && g->wHand->isVisible());
        if (g->dInfo.isStarted || (g->wHand && g->wHand->isVisible())) {
            g_connect_phase = ConnectPhase::DONE;
            break;
        }
        // keep polling the start button (clickable once both players are ready)
        if (g->btnHostPrepStart && g->btnHostPrepStart->isVisible()) {
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - g_connect_last_start_click).count() >= 1000) {
                ClickButton(g->btnHostPrepStart);
                g_connect_last_start_click = now;
            }
        }
        break;
    }
    default:
        break;
    }
}
// ---------------------------------------------------------------------------
// tools
// ---------------------------------------------------------------------------

static std::string MCP_DoChoose(const json& args) {
    // id: int (single option) or list[int] (subset). ids ALWAYS start at 0.
    std::vector<int> picks;
    if (args.contains("id")) {
        const json& id = args["id"];
        if (id.is_number_integer()) {
            picks.push_back(id.get<int>());
        } else if (id.is_array()) {
            for (auto& v : id) {
                if (!v.is_number_integer()) return "Error: id must be an int or a list of ints";
                picks.push_back(v.get<int>());
            }
        } else {
            return "Error: id must be an int or a list of ints";
        }
    } else {
        return "Error: id parameter required (int or list of ints)";
    }
    for (int p : picks)
        if (p < 0) return "Error: invalid choice (range 0..N) - ids start at 0";
    if (!mainGame) return "Error: game not ready";

    // RETRY box: the agent is acking a rejection; the SAME prompt is then
    // re-issued internally (no new STOC packet), so this ack must NOT mark
    // the prompt as acted -- otherwise the seq gate would hide the re-prompt.
    bool retry_box = mainGame->wMessage && mainGame->wMessage->isVisible()
        && mainGame->dInfo.curMsg == MSG_RETRY;
    auto mark_acted = [&]() {
        if (!retry_box)
            g_last_acted_seq.store(g_stoc_seq.load(std::memory_order_relaxed));
    };

    auto& f = mainGame->dField;
    int cur = mainGame->dInfo.curMsg;

    // Empty list = cancel, for the prompt families whose wire accepts an
    // empty answer (design doc §7: cancelable -> min=0 -> empty = cancel):
    //   select_card  (CARD/TRIBUTE/COUNTER/UNSELECT): cancel = ivalue -1
    //   select_place (PLACE/DISFIELD):                cancel = empty zone list
    // Everything else (OPTION, CHAIN, YESNO, ...) has no empty-cancel.
    if (picks.empty() && MCP_PromptPending()) {
        bool card_cancel = (cur == MSG_SELECT_CARD || cur == MSG_SELECT_TRIBUTE
            || cur == MSG_SELECT_COUNTER || cur == MSG_SELECT_UNSELECT_CARD)
            && f.select_cancelable;
        bool place_cancel = (cur == MSG_SELECT_PLACE || cur == MSG_SELECT_DISFIELD)
            && f.select_cancelable;
        if (card_cancel) {
            for (auto* c : f.selectable_cards) c->is_selected = false;
            f.selected_cards.clear();
            if (mainGame->wCardSelect && mainGame->wCardSelect->isVisible())
                mainGame->HideElement(mainGame->wCardSelect, false);
            f.ShowCancelOrFinishButton(0);
            DuelClient::SetResponseI(-1);
            DuelClient::SendResponse();
            mark_acted();
            return "Canceled.";
        }
        if (place_cancel) {
            // Match ClientField::CancelOrFinish's MSG_SELECT_PLACE cancel:
            // 3 bytes {player, 0, 0}. A 0-byte response would read garbage
            // in the core (set_responseb memcpy's SIZE_RETURN_VALUE).
            unsigned char respbuf[3];
            respbuf[0] = (unsigned char)mainGame->LocalPlayer(0);
            respbuf[1] = 0;
            respbuf[2] = 0;
            f.selectable_field = 0;
            f.selected_field = 0;
            DuelClient::SetResponseB(respbuf, 3);
            DuelClient::SendResponse();
            f.ShowCancelOrFinishButton(0);
            mark_acted();
            return "Canceled.";
        }
        return "Error: empty selection not allowed";
    }

    // Batch-capable prompts (card selects, sort, place, announce) take the
    // subset directly; a single pick on anything else (command, yes/no,
    // chain, position, option, boxes, RPS, turn-order) runs the option
    // action, which works for any visible window.
    bool sel_family = (cur == MSG_SELECT_CARD || cur == MSG_SELECT_TRIBUTE
        || cur == MSG_SELECT_COUNTER) && !f.selectable_cards.empty();
    // NOTE: MSG_SELECT_UNSELECT_CARD (single card, core hardcodes 1,1) and
    // MSG_SELECT_SUM (valid-combination options) are deliberately NOT batch
    // -- the uniform single-pick route handles them.
    bool batch = sel_family || cur == MSG_SORT_CARD
        || cur == MSG_SELECT_PLACE || cur == MSG_SELECT_DISFIELD
        || cur == MSG_ANNOUNCE_RACE || cur == MSG_ANNOUNCE_ATTRIB;
    if (batch && MCP_PromptPending()) {
        if (sel_family) {
            std::vector<ClientCard*> chosen;
            for (int idx : picks) {
                if (idx < 0 || (size_t)idx >= f.selectable_cards.size())
                    return "Error: invalid choice (range 0.." + std::to_string((int)f.selectable_cards.size() - 1) + ")";
                auto* c = f.selectable_cards[idx];
                if (!c->is_selectable) return "Error: invalid choice (range 0..N)";
                chosen.push_back(c);
            }
            // Pre-validate the count against [min, max] so the core never
            // needs to reject (avoids MSG_RETRY round-trips).
            int min = MCP_AdjMin(f.select_min, f.select_cancelable);
            if ((int)chosen.size() < min || (int)chosen.size() > f.select_max)
                return "Error: need " + std::to_string(min) + "-" + std::to_string(f.select_max)
                    + " options, got " + std::to_string(chosen.size());
            for (auto* c : f.selectable_cards) c->is_selected = false;
            f.selected_cards.clear();
            for (auto* c : chosen) { c->is_selected = true; f.selected_cards.push_back(c); }
            f.SetResponseSelectedCards();
            f.ShowCancelOrFinishButton(0);
            if (mainGame->wCardSelect && mainGame->wCardSelect->isVisible())
                mainGame->HideElement(mainGame->wCardSelect, false);
            DuelClient::SendResponse();
            mark_acted();
            return "ok";
        }
        // MSG_SORT_CARD: the picks order IS the desired card order.
        if (cur == MSG_SORT_CARD) {
            size_t n = f.selectable_cards.size();
            if (picks.size() != n) return "Error: sort needs exactly " + std::to_string(n) + " indices";
            std::vector<int> order;
            for (int idx : picks) {
                if (idx < 0 || (size_t)idx >= n) return "Error: invalid choice";
                order.push_back(idx);
            }
            f.RespondSortOrder(order);
            if (mainGame->wCardSelect && mainGame->wCardSelect->isVisible())
                mainGame->HideElement(mainGame->wCardSelect, false);
            DuelClient::SendResponse();
            mark_acted();
            return "ok";
        }
        // MSG_SELECT_PLACE / DISFIELD: picks = zone options to place on.
        if (cur == MSG_SELECT_PLACE || cur == MSG_SELECT_DISFIELD) {
            std::vector<int> opts;
            for (size_t i = 0; i < g_actions.size(); ++i)
                if (g_actions[i].kind == MCActionKind::PLACE) opts.push_back((int)i);
            int need = (int)f.select_min;
            if ((int)picks.size() != need) return "Error: need exactly " + std::to_string(need) + " zones";
            for (int idx : picks) {
                if (idx < 0 || (size_t)idx >= opts.size()) return "Error: invalid zone index";
                unsigned flag = (unsigned)g_actions[opts[idx]].pos;
                if (!(flag & f.selectable_field)) return "Error: invalid zone";
                f.selected_field |= flag;
                f.select_min--;
            }
            f.RespondZones();
            DuelClient::SendResponse();
            f.ShowCancelOrFinishButton(0);
            mark_acted();
            return "ok";
        }
        // ANNOUNCE_RACE / ANNOUNCE_ATTRIB: picks are sequential over VISIBLE
        // checkboxes; map id -> actual checkbox index, send bitmask.
        if (cur == MSG_ANNOUNCE_RACE || cur == MSG_ANNOUNCE_ATTRIB) {
            std::vector<int> visible;
            if (cur == MSG_ANNOUNCE_RACE)
                for (int i = 0; i < RACES_COUNT; ++i)
                    if (mainGame->chkRace[i]->isVisible()) visible.push_back(i);
            else
                for (int i = 0; i < 7; ++i)
                    if (mainGame->chkAttribute[i]->isVisible()) visible.push_back(i);
            if ((int)picks.size() != (int)f.announce_count)
                return "Error: need " + std::to_string((int)f.announce_count) + " announce indices";
            int val = 0;
            for (int idx : picks) {
                if (idx < 0 || (size_t)idx >= visible.size()) return "Error: invalid announce index";
                val |= (1 << visible[idx]);
            }
            int nbits = (cur == MSG_ANNOUNCE_RACE) ? RACES_COUNT : 7;
            for (int i = 0; i < nbits; ++i) {
                bool on = val & (1 << i);
                if (cur == MSG_ANNOUNCE_RACE) mainGame->chkRace[i]->setChecked(on);
                else mainGame->chkAttribute[i]->setChecked(on);
            }
            DuelClient::SetResponseI(val);
            DuelClient::SendResponse();
            if (cur == MSG_ANNOUNCE_RACE && mainGame->wANRace && mainGame->wANRace->isVisible())
                mainGame->HideElement(mainGame->wANRace, false);
            if (cur == MSG_ANNOUNCE_ATTRIB && mainGame->wANAttribute && mainGame->wANAttribute->isVisible())
                mainGame->HideElement(mainGame->wANAttribute, false);
            mark_acted();
            return "ok";
        }
    }
    // Single pick via the option action (any visible window: prompts,
    // message boxes, RPS, turn-order). Rebuild g_actions for the CURRENT
    // window before indexing.
    if (picks.size() == 1) {
        int oid = picks[0];
        MCP_ChoicesText();
        if (oid < 0 || (size_t)oid >= g_actions.size()) {
            if (g_actions.empty())
                return "Error: no pending prompt (options are empty)";
            return "Error: invalid choice (range 0.." + std::to_string((int)g_actions.size() - 1) + ")";
        }
        MCP_RunAction(g_actions[oid]);
        mark_acted();
        return "ok";
    }
    return "Error: id does not match the current prompt";
}

static std::string MCP_DoChat(const json& args) {
    std::string text = args.value("text", "");
    if (text.empty()) return "Error: text parameter required";
    uint16_t msgbuf[LEN_CHAT_MSG];
    size_t len = 0;
    size_t i = 0;
    // UTF-8 -> UTF-16, decoded codepoint-by-codepoint (design doc §8): 4-byte
    // codepoints become U+FFFD; truncated at 255 UTF-16 units + NUL. This is
    // NOT BufferIO::DecodeUTF8 (mbsrtowcs is locale-dependent and yields
    // wchar_t, not the U+FFFD-substituted 16-bit units the wire needs).
    while (i < text.size() && len < LEN_CHAT_MSG - 1) {
        // text is UTF-8; decode one codepoint
        unsigned char c0 = (unsigned char)text[i];
        unsigned int cp = c0;
        int extra = 0;
        if ((c0 & 0xe0) == 0xc0) { cp = c0 & 0x1f; extra = 1; }
        else if ((c0 & 0xf0) == 0xe0) { cp = c0 & 0x0f; extra = 2; }
        else if ((c0 & 0xf8) == 0xf0) { cp = c0 & 0x07; extra = 3; }
        if (i + extra >= text.size()) break;
        for (int k = 1; k <= extra; ++k) cp = (cp << 6) | ((unsigned char)text[i + k] & 0x3f);
        if (cp > 0xffff) cp = 0xfffd;
        msgbuf[len++] = (uint16_t)cp;
        i += extra + 1;
    }
    msgbuf[len] = 0;
    DuelClient::SendBufferToServer(CTOS_CHAT, msgbuf, (len + 1) * sizeof(uint16_t));
    // the ack rides in the # log section of the response block
    MCP_Log("Sent: " + text);
    return "";
}

// Card explain: code or name (substring match, up to 20 hits). Single line
// per card, ydkshow-style. Never acts. The hits are force-explained in the
// response block's # card explain section.
static std::string MCP_DoCard(const json& args) {
    std::string q = args.value("name", "");
    if (q.empty() && args.contains("code")) q = std::to_string(args["code"].get<int64_t>());
    if (q.empty()) return "Error: name or code parameter required";
    std::vector<uint32_t> hits;
    // exact code lookup
    bool all_digits = !q.empty() && std::all_of(q.begin(), q.end(), ::isdigit);
    if (all_digits) {
        uint32_t code = (uint32_t)strtoul(q.c_str(), nullptr, 10);
        if (!MCP_CardLine(code).empty()) hits.push_back(code);
    }
    // name substring match over the loaded card table
    if (hits.empty()) {
        auto& table = dataManager.GetDataTable();
        for (auto& kv : table) {
            std::string n = U8(dataManager.GetName(kv.first));
            if (n.empty() || n == "?") continue;
            if (n.find(q) != std::string::npos || q.find(n) != std::string::npos) {
                hits.push_back(kv.first);
                if (hits.size() >= 20) break;
            }
        }
        std::sort(hits.begin(), hits.end());
    }
    if (hits.empty()) {
        MCP_Log("No card found: " + q);
        return "";
    }
    // MCP_Poll returns the standard log/state/explain/options block for a
    // nonblocking tool. Queue these explicit hits for that block instead of
    // returning a string that MCP_Poll would discard.
    {
        std::lock_guard<std::mutex> lk(g_log_mtx);
        g_force_explain.insert(hits.begin(), hits.end());
    }
    return "";
}

static std::string MCP_DoSurrender() {
    // Protocol-direct: CTOS_SURRENDER is accepted by the server gate in any
    // state; no window needed (the old button-click path required wSurrender).
    // BLOCKS: the response comes at the next decision point (side exchange /
    // win-lose message); the ack rides in the # log section.
    DuelClient::SendPacketToServer(CTOS_SURRENDER);
    MCP_Log("Surrendered.");
    return "";
}

static void MCP_FinishDisconnect() {
    Game* g = mainGame;
    if (!g || !g->device) return;
    g->dInfo.Clear();
    g->dField.Clear();
    g->is_building = false;
    g->is_siding = false;
    g->CloseDuelWindow();
    g->HideElement(g->wHostPrepare);
    g->device->setEventReceiver(&g->menuHandler);
    g->btnCreateHost->setEnabled(true);
    g->btnJoinHost->setEnabled(true);
    g->btnJoinCancel->setEnabled(true);
    g->btnStartBot->setEnabled(true);
    g->btnBotCancel->setEnabled(true);
    if (g->bot_mode) g->ShowElement(g->wSinglePlay);
    else g->ShowElement(g->wLanWindow);
    // curMsg and modal visibility belonged to the old network session.
    g_last_acted_seq.store(g_stoc_seq.load(std::memory_order_relaxed),
                           std::memory_order_relaxed);
}

static std::string MCP_DoDisconnect() {
    Game* g = mainGame;
    if (!g || !g->device) return "Error: game not ready";
    if (GetConnectState() == CONNECT_STATE_NONE
        && g_connect_phase == ConnectPhase::NONE)
        return "Error: no room connection";
    // A duelist's Leave button opens the surrender confirmation, and the
    // room-prep Cancel button is ignored by the field receiver during RPS.
    // Stop the transport instead; respond only after it actually closes.
    DuelClient::CancelPendingEndgamePrompts();
    DuelClient::StopClient();
    {
        // Serialize modal cleanup with the network callback's PopupElement.
        // A replay packet already in flight must not reopen the dialog after
        // we hide it on the GUI thread.
        std::lock_guard<std::mutex> gui_lock(g->gMutex);
        g->CloseGameWindow();
        g->HideElement(g->wHostPrepare);
    }
    // Return to the initial (disconnected) state (design doc §1): a later
    // ygo_connect must be able to start a fresh join flow.
    g_connect_phase = ConnectPhase::NONE;
    g_connect_readied = false;
    g_connect_deck_err = false;
    g_connect_err_set = false;
    g_connect_err.clear();
    g_disconnect_pending = true;
    g_disconnect_t0 = std::chrono::steady_clock::now();
    return "";
}

// Deck tools operate on the live graphical editor on this same GUI thread.
// A selected combo-box item is the only source of a writable filename.
static bool MCP_DeckLeaf(const std::wstring& name) {
    if (name.empty() || name == L"." || name == L".." || name.size() > 180
        || name.back() == L'.' || name.back() == L' ') return false;
    for (wchar_t c : name)
        if (c < 32 || std::wcschr(L"<>:\"/\\|?*", c)) return false;
    return true;
}

static bool MCP_DeckOrdinary(const std::wstring& path, bool directory) {
#ifdef _WIN32
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_REPARSE_POINT)
        && !!(attr & FILE_ATTRIBUTE_DIRECTORY) == directory;
#else
    char utf8[1024]{};
    BufferIO::EncodeUTF8(path.c_str(), utf8);
    struct stat info{};
    return lstat(utf8, &info) == 0 && (directory ? S_ISDIR(info.st_mode) : S_ISREG(info.st_mode));
#endif
}

static bool MCP_DeckCategoryPath(int category, std::wstring& path) {
    auto* combo = mainGame->cbDBCategory;
    if (category != DECK_CATEGORY_NONE && category < DECK_CATEGORY_CUSTOM) return false;
    if (category < 0 || category >= static_cast<int>(combo->getItemCount())) return false;
    path = L"./deck";
    if (!MCP_DeckOrdinary(path, true)) return false;
    if (category >= DECK_CATEGORY_CUSTOM) {
        std::wstring name = combo->getItem(category);
        if (!MCP_DeckLeaf(name)) return false;
        path += L"/" + name;
    }
    return path.size() <= 220 && MCP_DeckOrdinary(path, true);
}

static bool MCP_DeckFilePath(int category, const std::wstring& deck, std::wstring& path) {
    if (!MCP_DeckLeaf(deck) || !MCP_DeckCategoryPath(category, path)) return false;
    path += L"/" + deck + L".ydk";
    return path.size() <= 210 && MCP_DeckOrdinary(path, false);
}

static std::string MCP_DeckEditor() {
    if (!mainGame || !mainGame->device) return "Error: game not ready";
    if (!mainGame->is_building) {
        if (!mainGame->wMainMenu || !mainGame->wMainMenu->isVisible()
            || mainGame->dInfo.isStarted || !mainGame->btnDeckEdit)
            return "Error: deck editor is unavailable outside the main menu";
        MCP_Click(mainGame->btnDeckEdit);
    }
    if (!mainGame->is_building || mainGame->is_siding || !mainGame->wDeckEdit->isVisible())
        return "Error: deck editor is unavailable";
    if (mainGame->deckBuilder.is_draging || mainGame->deckBuilder.is_starting_dragging)
        return "Error: finish the current card drag first";
    if (mainGame->wQuery->isVisible() || mainGame->wDeckManage->isVisible()
        || mainGame->wDMQuery->isVisible() || mainGame->wCategories->isVisible()
        || mainGame->wLinkMarks->isVisible())
        return "Error: close the deck editor dialog first";
    return "";
}

static bool MCP_DeckString(const json& args, const char* key, std::string& value) {
    if (!args.is_object() || !args.contains(key) || !args[key].is_string()) return false;
    value = args[key].get<std::string>();
    return !value.empty() && value.size() <= 180;
}

static bool MCP_DeckNumber(const json& args, const char* key, uint32_t& value) {
    if (!args.is_object() || !args.contains(key) || !args[key].is_number_integer()) return false;
    if (args[key].is_number_unsigned()) {
        auto n = args[key].get<uint64_t>();
        if (n > UINT32_MAX) return false;
        value = static_cast<uint32_t>(n);
    } else {
        auto n = args[key].get<int64_t>();
        if (n < 0 || n > UINT32_MAX) return false;
        value = static_cast<uint32_t>(n);
    }
    return true;
}

static std::string MCP_DeckState() {
    auto& builder = mainGame->deckBuilder;
    auto& deck = deckManager.current_deck;
    std::ostringstream out;
    int category = mainGame->cbDBCategory->getSelected();
    int selected = mainGame->cbDBDecks->getSelected();
    out << "# deck editor\ncategory: "
        << (category == DECK_CATEGORY_NONE ? "local" : U8(mainGame->cbDBCategory->getText()))
        << "\ndeck: " << (selected < 0 ? "(none)" : U8(mainGame->cbDBDecks->getText()))
        << "\nmodified: " << (builder.is_modified ? "yes" : "no") << "\n";
    auto section = [&out](const char* name, const std::vector<const CardDataC*>& cards) {
        out << name << " (" << cards.size() << "):\n";
        for (const auto* card : cards)
            out << "  " << card->code << " " << U8(dataManager.GetName(card->code)) << "\n";
    };
    section("main", deck.main);
    section("extra", deck.extra);
    section("side", deck.side);
    return out.str();
}

static std::string MCP_DoDeckList() {
    std::string error = MCP_DeckEditor();
    if (!error.empty()) return error;
    std::ostringstream out;
    auto* categories = mainGame->cbDBCategory;
    for (int i = DECK_CATEGORY_NONE; i < static_cast<int>(categories->getItemCount()); ++i) {
        std::wstring path;
        if (!MCP_DeckCategoryPath(i, path)) continue;
        std::string label = i == DECK_CATEGORY_NONE ? "local" : U8(categories->getItem(i));
        out << label << ":";
        mainGame->RefreshDeck(path.c_str(), [&](const wchar_t* item) {
            std::wstring file;
            if (MCP_DeckFilePath(i, item, file)) out << " " << U8(item);
        });
        out << "\n";
    }
    out << MCP_DeckState();
    return out.str();
}

static bool MCP_DeckRead(const std::wstring& path, Deck& deck) {
    FILE* file = DeckManager::OpenDeckFile(path.c_str(), "rb");
    if (!file) return false;
    char content[DeckManager::MAX_YDK_SIZE]{};
    size_t length = std::fread(content, 1, sizeof(content), file);
    bool okay = !std::ferror(file) && length < sizeof(content);
    if (std::fclose(file) != 0 || !okay) return false;
    std::istringstream input(std::string(content, length));
    return DeckManager::LoadDeckFromStream(deck, input) == 0;
}

static std::string MCP_DoDeckOpen(const json& args) {
    std::string error = MCP_DeckEditor();
    if (!error.empty()) return error;
    if (mainGame->deckBuilder.is_modified)
        return "Error: current deck has unsaved edits; save it in the editor first";
    std::string name;
    if (!MCP_DeckString(args, "deck", name)) return "Error: deck must be a listed local deck name";
    std::string category = "local";
    if (args.is_object() && args.contains("category")
        && !MCP_DeckString(args, "category", category))
        return "Error: category must be a listed local category";
    int selected_category = -1;
    auto* categories = mainGame->cbDBCategory;
    for (int i = DECK_CATEGORY_NONE; i < static_cast<int>(categories->getItemCount()); ++i) {
        if (i == DECK_CATEGORY_SEPARATOR) continue;
        if ((i == DECK_CATEGORY_NONE && category == "local")
            || U8(categories->getItem(i)) == category) {
            selected_category = i;
            break;
        }
    }
    if (selected_category < 0) return "Error: category is not listed locally";
    std::wstring category_path;
    if (!MCP_DeckCategoryPath(selected_category, category_path))
        return "Error: category is not an ordinary local directory";
    std::wstring selected_deck;
    mainGame->RefreshDeck(category_path.c_str(), [&](const wchar_t* item) {
        if (U8(item) == name) selected_deck = item;
    });
    std::wstring file;
    if (selected_deck.empty() || !MCP_DeckFilePath(selected_category, selected_deck, file))
        return "Error: deck is not a listed local file";
    Deck loaded;
    if (!MCP_DeckRead(file, loaded)) return "Error: cannot load selected deck";
    int previous_category = categories->getSelected();
    auto* decks = mainGame->cbDBDecks;
    std::wstring previous_deck = decks->getSelected() < 0 ? L"" : decks->getText();
    categories->setSelected(selected_category);
    mainGame->RefreshDeck(categories, decks);
    int selected_index = -1;
    for (int i = 0; i < static_cast<int>(decks->getItemCount()); ++i)
        if (selected_deck == decks->getItem(i)) { selected_index = i; break; }
    if (selected_index < 0) {
        categories->setSelected(previous_category);
        mainGame->RefreshDeck(categories, decks);
        for (int i = 0; i < static_cast<int>(decks->getItemCount()); ++i)
            if (previous_deck == decks->getItem(i)) { decks->setSelected(i); break; }
        return "Error: selected deck disappeared while opening";
    }
    decks->setSelected(selected_index);
    deckManager.current_deck = std::move(loaded);
    auto& builder = mainGame->deckBuilder;
    builder.RefreshReadonly(selected_category);
    builder.RefreshPackListScroll();
    builder.prev_category = selected_category;
    builder.prev_deck = decks->getSelected();
    builder.is_modified = false;
    mainGame->ebDeckname->setText(L"");
    builder.GetHoveredCard();
    return MCP_DeckState();
}

static std::string MCP_DoDeckSearch(const json& args) {
    std::string error = MCP_DeckEditor();
    if (!error.empty()) return error;
    std::string query;
    if (!MCP_DeckString(args, "query", query) || query.size() > 120)
        return "Error: query must be nonempty text of at most 120 bytes";
    wchar_t wide[256]{};
    BufferIO::DecodeUTF8(query.c_str(), wide);
    auto& builder = mainGame->deckBuilder;
    builder.ClearSearch();
    mainGame->ebCardName->setText(wide);
    builder.StartFilter();
    builder.GetHoveredCard();
    std::ostringstream out;
    out << "matches: " << builder.results.size() << "\n";
    for (size_t i = 0; i < builder.results.size() && i < 20; ++i) {
        auto* card = builder.results[i];
        out << card->code << " " << U8(dataManager.GetName(card->code))
            << " (available " << builder.available_copies(card) << ")\n";
    }
    if (builder.results.size() > 20) out << "showing first 20\n";
    return out.str();
}

static std::string MCP_DoDeckEdit(const json& args, bool add) {
    std::string error = MCP_DeckEditor();
    if (!error.empty()) return error;
    auto& builder = mainGame->deckBuilder;
    if (builder.readonly) return "Error: this deck category is read-only";
    int selected = mainGame->cbDBDecks->getSelected();
    std::wstring selected_path;
    if (selected < 0 || selected >= static_cast<int>(mainGame->cbDBDecks->getItemCount())
        || !MCP_DeckFilePath(mainGame->cbDBCategory->getSelected(),
                             mainGame->cbDBDecks->getItem(selected), selected_path))
        return "Error: select an existing local deck first";
    uint32_t code{}, count{1};
    std::string section;
    if (!MCP_DeckNumber(args, "code", code) || !MCP_DeckString(args, "section", section)
        || (args.contains("count") && !MCP_DeckNumber(args, "count", count))
        || count < 1 || count > 3)
        return "Error: provide a card code, main/extra/side section, and count 1-3";
    std::vector<const CardDataC*>* cards = nullptr;
    size_t capacity = 0;
    if (section == "main") { cards = &deckManager.current_deck.main; capacity = DECK_MAX_SIZE; }
    else if (section == "extra") { cards = &deckManager.current_deck.extra; capacity = EXTRA_MAX_SIZE; }
    else if (section == "side") { cards = &deckManager.current_deck.side; capacity = SIDE_MAX_SIZE; }
    else return "Error: section must be main, extra, or side";
    if (add) {
        auto& table = dataManager.GetDataTable();
        auto found = table.find(code);
        if (found == table.end() || found->second.type & TYPE_TOKEN)
            return "Error: card code is unknown or a token";
        const auto* card = &found->second;
        bool extra_type = card->type & (TYPE_FUSION | TYPE_SYNCHRO | TYPE_XYZ | TYPE_LINK);
        if ((section == "main" && extra_type) || (section == "extra" && !extra_type))
            return "Error: card type does not belong in that section";
        if (cards->size() + count > capacity) return "Error: section is full";
        if (builder.available_copies(card) < static_cast<int>(count))
            return "Error: alias or selected card limit would be exceeded";
        for (uint32_t i = 0; i < count; ++i) {
            bool pushed = section == "main" ? builder.push_main(card)
                : section == "extra" ? builder.push_extra(card) : builder.push_side(card);
            if (!pushed) return "Error: editor rejected card after validation";
        }
    } else {
        std::vector<int> positions;
        for (int i = 0; i < static_cast<int>(cards->size()); ++i)
            if ((*cards)[i]->code == code) positions.push_back(i);
        if (positions.size() < count) return "Error: selected section has too few copies";
        for (uint32_t i = 0; i < count; ++i) {
            int pos = positions[positions.size() - 1 - i];
            if (section == "main") builder.pop_main(pos);
            else if (section == "extra") builder.pop_extra(pos);
            else builder.pop_side(pos);
        }
    }
    return MCP_DeckState();
}

static bool MCP_DeckWriteSelected(const std::wstring& path) {
    std::stringstream output;
    DeckManager::SaveDeck(deckManager.current_deck, output);
    std::string bytes = output.str();
    // A private same-directory file prevents a failed write from truncating
    // the selected deck. Exclusive creation also avoids touching other files.
    std::wstring temp;
    FILE* file = nullptr;
    for (int attempt = 0; attempt < 8 && !file; ++attempt) {
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        temp = path + L".mcp-" + std::to_wstring(stamp) + L"-" + std::to_wstring(attempt);
        file = DeckManager::OpenDeckFile(temp.c_str(), "wbx");
    }
    if (!file) return false;
    bool written = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    if (std::fflush(file) != 0) written = false;
    if (std::fclose(file) != 0) written = false;
    bool replaced = false;
    if (written) {
#ifdef _WIN32
        replaced = MoveFileExW(temp.c_str(), path.c_str(),
                               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        replaced = FileSystem::Rename(temp.c_str(), path.c_str());
#endif
    }
    if (!replaced) FileSystem::RemoveFile(temp.c_str());
    return replaced;
}

static std::string MCP_DoDeckSave() {
    std::string error = MCP_DeckEditor();
    if (!error.empty()) return error;
    auto& builder = mainGame->deckBuilder;
    if (builder.readonly) return "Error: this deck category is read-only";
    int category = mainGame->cbDBCategory->getSelected();
    int selected = mainGame->cbDBDecks->getSelected();
    if (selected < 0 || selected >= static_cast<int>(mainGame->cbDBDecks->getItemCount()))
        return "Error: select an existing local deck first";
    std::wstring path;
    if (!MCP_DeckFilePath(category, mainGame->cbDBDecks->getItem(selected), path))
        return "Error: selected deck is not an ordinary local file";
    if (builder.is_modified && !MCP_DeckWriteSelected(path))
        return "Error: could not save selected deck; unsaved edits remain";
    builder.is_modified = false;
    return MCP_DeckState();
}

// ---------------------------------------------------------------------------
// dispatch
// ---------------------------------------------------------------------------

static json ToolSchema(const char* name, const char* desc, json props) {
    json s;
    s["type"] = "object";
    s["properties"] = props;
    return {{"name", name}, {"description", desc}, {"inputSchema", s}};
}

// Executes a request on the GUI thread. For tools/call returns the tool's
// text ("" when the response block carries it); for initialize/tools/list
// sets job->result and returns "".
static std::string MCP_Execute(MCPJob* job) {
    if (job->method == "initialize") {
        job->ok = true;
        // serverInfo is REQUIRED by the MCP SDK's InitializeResultSchema; the
        // tools capability must be advertised or listTools() returns empty.
        job->result = {{"protocolVersion", "2024-11-05"},
                       {"capabilities", {{"tools", json::object()}}},
                       {"serverInfo", {{"name", "ygopro-mcp"}, {"version", "1.0.0"}}}};
        return "";
    }
    if (job->method == "ping") {
        job->ok = true;
        job->result = json::object();
        return "";
    }
    if (job->method == "tools/list") {
        json props;
        props["id"] = {{"type", "integer"}, {"description", "an option id (int) or a list of option ids (subset); ids always start at 0"}};
        json tools = json::array();
        tools.push_back(ToolSchema("ygo_connect", "Connect to a room and block until the first decision point (RPS/turn order/prompt) or a failure. Joins, uploads the deck, readies, and polls the start button (if host) automatically.", {{"host", {{"type", "string"}}}, {"port", {{"type", "string"}}}, {"password", {{"type", "string"}}}, {"name", {{"type", "string"}, {"description", "nickname (optional)"}}}, {"deck", {{"type", "string"}, {"description", "deck file path"}}}}));
        tools.push_back(ToolSchema("ygo_choose", "Answer the pending prompt and block until the next decision point. id = int (single option) or list of ints (subset), per the '# options: min~max/total' section. Prompts with exactly one way to answer are auto-answered and never reach you.", props));
        tools.push_back(ToolSchema("ygo_card", "Explain a card on one line (ydkshow-style): name or code", {{"name", {{"type", "string"}, {"description", "card name (substring) or numeric code"}}}}));
        tools.push_back(ToolSchema("ygo_chat", "Send a chat message to the room", {{"text", {{"type", "string"}, {"description", "message text"}}}}));
        tools.push_back(ToolSchema("ygo_surrender", "Surrender the duel; blocks until the server responds (side exchange / win-lose)", json::object()));
        tools.push_back(ToolSchema("ygo_disconnect", "Leave the room; returns after the connection closes and the graphical client returns to its room browser", json::object()));
        tools.push_back(ToolSchema("ygo_deck_list", "Show listed local decks and the current graphical editor deck", json::object()));
        tools.push_back(ToolSchema("ygo_deck_open", "Load a listed local deck in the graphical editor; refuses to discard unsaved edits", {{"deck", {{"type", "string"}}}, {"category", {{"type", "string"}, {"description", "local or a listed category; defaults to local"}}}}));
        tools.push_back(ToolSchema("ygo_deck_search", "Search cards with the graphical editor's normal filter", {{"query", {{"type", "string"}}}}));
        tools.push_back(ToolSchema("ygo_deck_add", "Add copies to the selected local deck, subject to editor section and card limits", {{"code", {{"type", "integer"}}}, {"section", {{"type", "string"}, {"enum", {"main", "extra", "side"}}}}, {"count", {{"type", "integer"}, {"description", "1 to 3; defaults to 1"}}}}));
        tools.push_back(ToolSchema("ygo_deck_remove", "Remove exact card-code copies from the selected local deck", {{"code", {{"type", "integer"}}}, {"section", {{"type", "string"}, {"enum", {"main", "extra", "side"}}}}, {"count", {{"type", "integer"}, {"description", "1 to 3; defaults to 1"}}}}));
        tools.push_back(ToolSchema("ygo_deck_save", "Save only the current selected local deck; preserve unsaved edits if writing fails", json::object()));
        job->ok = true;
        job->result = {{"tools", tools}};
        return "";
    }
    if (job->method == "tools/call") {
        std::string name = job->params.value("name", "");
        json args = job->params.contains("arguments") ? job->params["arguments"] : json::object();
        std::string text;
        if (name == "ygo_connect") text = MCP_DoConnect(args);
        else if (name == "ygo_choose") text = MCP_DoChoose(args);
        else if (name == "ygo_card") text = MCP_DoCard(args);
        else if (name == "ygo_chat") text = MCP_DoChat(args);
        else if (name == "ygo_surrender") text = MCP_DoSurrender();
        else if (name == "ygo_disconnect") text = MCP_DoDisconnect();
        else if (name == "ygo_deck_list") text = MCP_DoDeckList();
        else if (name == "ygo_deck_open") text = MCP_DoDeckOpen(args);
        else if (name == "ygo_deck_search") text = MCP_DoDeckSearch(args);
        else if (name == "ygo_deck_add") text = MCP_DoDeckEdit(args, true);
        else if (name == "ygo_deck_remove") text = MCP_DoDeckEdit(args, false);
        else if (name == "ygo_deck_save") text = MCP_DoDeckSave();
        else {
            job->ok = false;
            job->error_msg = "unknown tool: " + name;
            return "";
        }
        job->ok = true;
        return text;
    }
    job->ok = false;
    job->error_msg = "method not found: " + job->method;
    return "";
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

void MCP_Init() {
    if (g_running) return;
    // The MCP channel owns the real stdout; the app's own output goes to stderr.
    if (!g_stdio.Start()) return;
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif
    g_running = true;
    dbg("init: starting reader thread");
#ifdef _WIN32
    std::promise<HANDLE> reader_ready;
    auto reader_handle = reader_ready.get_future();
    g_thread = std::thread([&reader_ready] {
        HANDLE owned = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                             &owned, 0, FALSE, DUPLICATE_SAME_ACCESS))
            std::fprintf(stderr, "MCP: could not retain reader thread handle: %lu\n", GetLastError());
        reader_ready.set_value(owned);
        if (owned) MCP_ReaderThread();
    });
    g_reader_handle = reader_handle.get();
    if (!g_reader_handle) {
        g_running = false;
        g_thread.join();
        g_stdio.Stop();
    }
#else
    g_thread = std::thread(MCP_ReaderThread);
#endif
}

void MCP_Shutdown() {
    g_running = false;
    if (g_thread.joinable()) {
#ifdef _WIN32
        // The reader duplicated its own real Win32 HANDLE at startup, so it
        // remains valid even if the std::thread's native identifier expires.
        if (g_reader_handle) {
            for (;;) {
                const DWORD wait = WaitForSingleObject(g_reader_handle, 25);
                if (wait == WAIT_OBJECT_0) break;
                if (wait == WAIT_TIMEOUT) {
                    if (!CancelSynchronousIo(g_reader_handle)) {
                        const DWORD error = GetLastError();
                        // No I/O may be pending between reads/writes. Keep
                        // watching: a racing operation may start afterward.
                        if (error != ERROR_NOT_FOUND)
                            std::fprintf(stderr, "MCP: CancelSynchronousIo failed: %lu\n", error);
                    }
                    continue;
                }
                std::fprintf(stderr, "MCP: WaitForSingleObject failed: %lu\n", GetLastError());
                break;
            }
        }
#endif
        g_thread.join();
#ifdef _WIN32
        if (g_reader_handle) CloseHandle(g_reader_handle);
        g_reader_handle = nullptr;
#endif
    }
    g_queue.clear();
    g_active_job = nullptr;
    g_disconnect_pending = false;
    g_stdio.Stop();
}

void MCP_Poll() {
    if (!g_running) return;
    dbg("poll called");
    // The GUI event receiver drops button events while a fade animation is
    // running (ClientField::OnEvent: if(fadingList.size()) break). Defer the
    // whole job until the fades clear so clicks are never silently lost.
    if (mainGame && mainGame->fadingList.size()) {
        return;
    }
    // Auto-answer prompts with exactly ONE way to answer (min~max/1, boxes
    // included); the agent stays blocked until a real decision.
    // An explicit leave request takes precedence over acknowledging the
    // endgame modal. Otherwise this tick can dismiss it immediately before
    // MCP_DoDisconnect gets to cancel its network callback.
    if (!MCP_DisconnectQueued()) MCP_AutoAnswer();
    // Drive the ygo_connect pre-duel flow (join -> deck -> ready -> start).
    MCP_ConnectTick();
    // A blocking call is in flight: wait for transport closure on disconnect,
    // otherwise complete it at the next decision point.
    if (g_active_job) {
        if (g_disconnect_pending) {
            if (GetConnectState() != CONNECT_STATE_NONE) {
                if (std::chrono::steady_clock::now() - g_disconnect_t0
                    > std::chrono::seconds(15)) {
                    g_disconnect_pending = false;
                    MCP_CompleteJob(g_active_job,
                                    "Error: disconnect timed out\n" + MCP_BlockText());
                    g_active_job = nullptr;
                }
                return;
            }
            g_disconnect_pending = false;
            MCP_FinishDisconnect();
            MCP_Log("Left the room.");
            MCP_CompleteJob(g_active_job, MCP_BlockText());
            g_active_job = nullptr;
            return;
        }
        // A different player can close the room while this client is waiting
        // for its next decision. The network worker has then finished, but
        // there may be no next prompt and the normal menu is still hidden by
        // the LAN window. Complete the pending call with a terminal snapshot.
        const std::string pending_name = g_active_job->method == "tools/call"
            ? g_active_job->params.value("name", "") : "";
        if ((pending_name == "ygo_choose" || pending_name == "ygo_surrender")
            && GetConnectState() == CONNECT_STATE_NONE) {
            MCP_FinishDisconnect();
            g_connect_phase = ConnectPhase::NONE;
            g_connect_readied = false;
            MCP_Log("Room connection closed.");
            MCP_CompleteJob(g_active_job, MCP_BlockText());
            g_active_job = nullptr;
            return;
        }
        bool back_to_menu = mainGame && mainGame->wMainMenu
            && mainGame->wMainMenu->isVisible() && !mainGame->dInfo.isStarted;
        if (back_to_menu) {
            // Natural return to the menu (game over / left the room / kicked):
            // reset the connect flow so a later ygo_connect starts fresh.
            g_connect_phase = ConnectPhase::NONE;
            g_connect_readied = false;
        }
        if (MCP_DecisionPending() || back_to_menu) {
            MCP_CompleteJob(g_active_job, MCP_BlockText());
            g_active_job = nullptr;
        }
        return;   // one outstanding request at a time (coroutine model)
    }
    while (auto* job = MCP_PopJob()) {
        std::string text;
        try {
            text = MCP_Execute(job);
        } catch (const std::exception& e) {
            // A malformed argument (e.g. a type mismatch in args) must never
            // crash the GUI thread: surface it as a JSON-RPC error instead.
            job->ok = false;
            job->error_msg = std::string("internal error: ") + e.what();
        }
        if (!job->ok) {          // JSON-RPC error (unknown tool/method/bad args)
            MCP_FinishJob(job);
            break;
        }
        if (!job->blocking) {
            // initialize / tools/list: result already set. tools/call:
            // immediate response = the same text block as every tool.
            if (job->method == "tools/call") {
                std::string name = job->params.value("name", "");
                MCP_CompleteJob(job, name.rfind("ygo_deck_", 0) == 0 ? text : MCP_BlockText());
            }
            else
                MCP_FinishJob(job);
            continue;
        }
        if (text.rfind("Error:", 0) == 0) {   // invalid input: respond now
            MCP_CompleteJob(job, text + "\n" + MCP_BlockText());
            break;
        }
        // ALWAYS hold after a successful blocking action. Completing in the
        // same tick is the classic re-delivery bug: the action's window is
        // hidden with a FADE (e.g. wFTSelect turn order), so isVisible() is
        // still true for one frame and MCP_DecisionPending() would re-render
        // the SAME prompt. The next tick (after the fade clears) sees the
        // true next decision.
        g_active_job = job;
        break;
    }
}

} // namespace ygo
