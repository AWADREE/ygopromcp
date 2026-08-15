// MCP bridge implementation. See mcp.h.
//
// Protocol: newline-delimited JSON-RPC 2.0. Tools: ygo_choose, ygo_state,
// ygo_log, ygo_card, ygo_chat, ygo_chatlog, ygo_surrender, ygo_disconnect.
// Only the network-play path (join room, lobby, RPS, turn-order, duel
// prompts) is supported; other game modes are out of scope for the MCP mode.
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
#include "bufferio.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <cstring>
#include <csignal>
#include <string>
#include <deque>
#include <vector>
#include <set>
#include <atomic>
#include <sstream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <unistd.h>
#include <IEventReceiver.h>

namespace ygo {

using json = nlohmann::json;

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
    std::mutex mtx;
    std::condition_variable cv;
};

static int g_json_fd = -1;
static bool g_running = false;
static std::thread g_thread;
static std::mutex g_qmtx;
static std::deque<MCPJob*> g_queue;

static void MCP_Execute(MCPJob* job);
static void dbg(const char* s) { if (std::getenv("YGOCLI_MCP_DEBUG")) { fprintf(stderr, "[mcp] %s\n", s); fflush(stderr); } }

static MCPJob* MCP_PopJob() {
    std::lock_guard<std::mutex> lk(g_qmtx);
    if (g_queue.empty()) return nullptr;
    MCPJob* job = g_queue.front();
    g_queue.pop_front();
    return job;
}

static void MCP_ReaderThread() {
    std::string line;
    while (g_running && std::getline(std::cin, line)) {
        if (line.empty()) continue;
        json req;
        try {
            req = json::parse(line);
        } catch (...) {
            continue;
        }
        if (!req.is_object() || !req.contains("id")) { dbg("reader: bad req"); continue; }
        dbg("reader: got request");
        auto* job = new MCPJob;
        job->id = req.value("id", 0);
        job->method = req.value("method", "");
        if (req.contains("params")) job->params = req["params"];
        {
            std::lock_guard<std::mutex> lk(g_qmtx);
            g_queue.push_back(job);
        }
        {
            // 30 s per-request cap: the GUI thread must poll within it or the
            // request times out (agents should poll far more often).
            std::unique_lock<std::mutex> lk(job->mtx);
            job->cv.wait_for(lk, std::chrono::seconds(30), [&] { return job->done; });
        }
        json resp;
        resp["jsonrpc"] = "2.0";
        resp["id"] = job->id;
        if (job->ok) {
            resp["result"] = job->result;
        } else {
            resp["error"] = {{"code", -32603}, {"message", job->error_msg.empty() ? "internal error" : job->error_msg}};
        }
        std::string out = resp.dump() + "\n";
        if (g_json_fd >= 0) {
            ssize_t off = 0;
            while (off < (ssize_t)out.size()) {
                ssize_t n = ::write(g_json_fd, out.data() + off, out.size() - off);
                if (n <= 0) break;
                off += n;
            }
        }
        delete job;
    }
}

// ---------------------------------------------------------------------------
// inbound taps (network thread, publish-only; see header comment)
// ---------------------------------------------------------------------------

// forward decls (helpers are defined further down)
static std::string U8(const wchar_t* w);
static const char* MCP_LogLoc(int loc);
static const char* MCP_PhaseName(uint16_t ph);

static std::atomic<uint64_t> g_stoc_seq{ 0 };    // monotonic STOC counter
static std::atomic<uint16_t> g_phase{ 0 };       // current phase (from MSG_NEW_PHASE)

static std::mutex g_log_mtx;                     // guards g_log + g_mentioned
static std::deque<std::string> g_log;            // narration ring (drained by ygo_log)
static std::set<uint32_t> g_mentioned;           // card codes mentioned since last ygo_state

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
    switch (msg[0]) {
    case MSG_START: {
        MCP_Log("Duel start.");
        break;
    }
    case MSG_NEW_TURN: {
        uint8_t pl = rd8();
        // dInfo.turn is incremented by the client's own case AFTER this tap.
        MCP_Log("=== T" + std::to_string(mainGame->dInfo.turn + 1) + " P" + std::to_string((int)pl) + " ===");
        break;
    }
    case MSG_NEW_PHASE: {
        uint16_t ph = rd16();
        g_phase.store(ph);
        MCP_Log(std::string("Ph ") + MCP_PhaseName(ph));
        break;
    }
    case MSG_LPUPDATE: {
        uint8_t pl = rd8();
        int32_t v = rd32();
        MCP_Log("P" + std::to_string((int)pl) + " LP " + std::to_string(v));
        break;
    }
    case MSG_DAMAGE: {
        uint8_t pl = rd8();
        int32_t v = rd32();
        MCP_Log("P" + std::to_string((int)pl) + " takes " + std::to_string(v) + " damage");
        break;
    }
    case MSG_RECOVER: {
        uint8_t pl = rd8();
        int32_t v = rd32();
        MCP_Log("P" + std::to_string((int)pl) + " recovers " + std::to_string(v) + " LP");
        break;
    }
    case MSG_PAY_LPCOST: {
        uint8_t pl = rd8();
        int32_t v = rd32();
        MCP_Log("P" + std::to_string((int)pl) + " pays " + std::to_string(v) + " LP");
        break;
    }
    case MSG_DRAW: {
        uint8_t pl = rd8();
        uint8_t cnt = rd8();
        // Draw names: the core sends the OPPONENT's drawn codes (the classic
        // ygopro v1 leak) but the LOCAL player's draws carry code 0 (the
        // client learns its hand via a later UPDATE_DATA). We never echo the
        // opponent's codes (privacy) and the local codes aren't in the
        // payload -- so log the count only, both sides. The drawn cards
        // appear with names in the ygo_state Hand listing instead.
        if (cnt == 0) break;
        for (int i = 0; i < cnt; ++i) rd32();
        MCP_Log("P" + std::to_string((int)pl) + " draws " + std::to_string((int)cnt)
                + (cnt > 1 ? " cards" : " card") + (pl ? " (face-down)" : ""));
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
        std::string verb;
        if (reason & REASON_DESTROY) verb = "destroyed";
        else if (cl == LOCATION_REMOVED && pl != cl) verb = "banished";
        else if (pl == LOCATION_HAND && cl == LOCATION_GRAVE) verb = "discarded";
        else verb = "moved";
        MCP_Log(name(code) + " " + verb + " (" + MCP_LogLoc(pl) + "->" + MCP_LogLoc(cl) + ")");
        break;
    }
    case MSG_SUMMONING:
    case MSG_SPSUMMONING:
    case MSG_FLIPSUMMONING: {
        uint32_t code = (uint32_t)rd32() & 0x7fffffff;
        MCP_Mention(code);
        std::string verb = msg[0] == MSG_SUMMONING ? "summons "
            : (msg[0] == MSG_SPSUMMONING ? "special summons " : "flip summons ");
        MCP_Log(verb + name(code));
        break;
    }
    case MSG_SUMMONED: MCP_Log("summon successful."); break;
    case MSG_SPSUMMONED: MCP_Log("special summon successful."); break;
    case MSG_FLIPSUMMONED: MCP_Log("flip summon successful."); break;
    case MSG_CHAINING: {
        uint32_t code = (uint32_t)rd32() & 0x7fffffff;
        for (int i = 0; i < 4; ++i) rd8(); // pcc,pcl,pcs,subs
        for (int i = 0; i < 3; ++i) rd8(); // cc,cl,cs
        rd32(); // desc
        rd8();  // ct
        MCP_Mention(code);
        // chains holds confirmed links; the new link is chains.size()+1.
        size_t n = mainGame->dField.chains.size() + 1;
        MCP_Log("Chain " + std::to_string(n) + ": " + name(code));
        break;
    }
    case MSG_CHAINED: {
        rd8(); // ct
        uint32_t c = mainGame->dField.current_chain.code;
        MCP_Mention(c);
        MCP_Log("Chained: " + name(c));
        break;
    }
    case MSG_CHAIN_SOLVING: {
        uint8_t ct = rd8();
        MCP_Log("Solving chain " + std::to_string((int)ct));
        break;
    }
    case MSG_CHAIN_SOLVED: {
        uint8_t ct = rd8();
        MCP_Log("Chain " + std::to_string((int)ct) + " solved.");
        break;
    }
    case MSG_CHAIN_END: {
        MCP_Log("Chain ended.");
        break;
    }
    case MSG_CHAIN_NEGATED:
    case MSG_CHAIN_DISABLED: {
        uint8_t ct = rd8();
        std::string n;
        if (ct >= 1 && (size_t)ct <= mainGame->dField.chains.size())
            n = name(mainGame->dField.chains[ct - 1].code);
        MCP_Log("Chain " + std::to_string((int)ct) + (msg[0] == MSG_CHAIN_NEGATED ? " negated" : " disabled") + (n.empty() ? "" : ": " + n));
        break;
    }
    case MSG_ATTACK: {
        uint8_t ca = rd8(); uint8_t la = rd8(); uint8_t sa = rd8(); rd8();
        uint8_t cd = rd8(); uint8_t ld = rd8(); uint8_t sd = rd8(); rd8();
        std::string an = "", dn = "";
        auto* a = mainGame->dField.GetCard(ca, la, sa);
        if (a && a->code) { an = name(a->code); MCP_Mention(a->code); }
        if (ld) {
            auto* d = mainGame->dField.GetCard(cd, ld, sd);
            if (d && d->code) { dn = name(d->code); MCP_Mention(d->code); }
        }
        MCP_Log("attack: P" + std::to_string((int)ca) + " " + an + (ld ? " vs P" + std::to_string((int)cd) + " " + dn : " (direct)"));
        break;
    }
    case MSG_BATTLE: {
        uint8_t ca = rd8(); uint8_t la = rd8(); uint8_t sa = rd8(); rd8();
        int32_t aatk = rd32(); rd32(); // adef
        rd8(); // da
        uint8_t cd = rd8(); uint8_t ld = rd8(); uint8_t sd = rd8(); rd8();
        int32_t datk = rd32(); rd32(); // ddef
        rd8(); // dd
        std::string an = "", dn = "";
        auto* a = mainGame->dField.GetCard(ca, la, sa);
        if (a && a->code) { an = name(a->code); MCP_Mention(a->code); }
        if (ld) {
            auto* d = mainGame->dField.GetCard(cd, ld, sd);
            if (d && d->code) { dn = name(d->code); MCP_Mention(d->code); }
        }
        MCP_Log("battle: P" + std::to_string((int)ca) + " " + an + " " + std::to_string(aatk)
                + (ld ? " vs P" + std::to_string((int)cd) + " " + dn + " " + std::to_string(datk) : " (direct)"));
        break;
    }
    case MSG_HINT: {
        uint8_t type = rd8();
        rd8(); // player
        int32_t data = rd32();
        switch (type) {
        case HINT_MESSAGE:
            // Blocking message box: the network thread parks on actionSignal
            // until OK -- the agent must ack (see header caveats).
            MCP_Log("[!] " + U8(dataManager.GetDesc(data)));
            break;
        case HINT_OPSELECTED:
            MCP_Log(U8(dataManager.GetDesc(data)));
            break;
        case HINT_RACE:
            MCP_Log("announced race: " + U8(dataManager.FormatRace(data).c_str()));
            break;
        case HINT_ATTRIB:
            MCP_Log("announced attribute: " + U8(dataManager.FormatAttribute(data).c_str()));
            break;
        case HINT_CODE: {
            std::string n = name((uint32_t)data);
            if (!n.empty() && n != "?") { MCP_Log("announced: " + n); MCP_Mention((uint32_t)data); }
            break;
        }
        case HINT_NUMBER:
            MCP_Log("announced number: " + std::to_string(data));
            break;
        case HINT_CARD: {
            std::string n = name((uint32_t)data);
            if (!n.empty() && n != "?") { MCP_Log("shows: " + n); MCP_Mention((uint32_t)data); }
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
        if (pl == 2) MCP_Log("Draw!");
        else MCP_Log("P" + std::to_string((int)mainGame->LocalPlayer(pl)) + " wins!");
        break;
    }
    case MSG_SHUFFLE_DECK: {
        uint8_t pl = rd8();
        MCP_Log("P" + std::to_string((int)pl) + " shuffles deck");
        break;
    }
    case MSG_SHUFFLE_HAND: {
        uint8_t pl = rd8();
        MCP_Log("P" + std::to_string((int)pl) + " shuffles hand");
        break;
    }
    case MSG_SHUFFLE_EXTRA: {
        uint8_t pl = rd8();
        MCP_Log("P" + std::to_string((int)pl) + " shuffles extra deck");
        break;
    }
    case MSG_SWAP_GRAVE_DECK: {
        uint8_t pl = rd8();
        MCP_Log("P" + std::to_string((int)pl) + " swaps grave and deck");
        break;
    }
    case MSG_REVERSE_DECK: {
        uint8_t pl = rd8();
        MCP_Log("P" + std::to_string((int)pl) + " reverses deck");
        break;
    }
    case MSG_FIELD_DISABLED: {
        uint32_t disabled = (uint32_t)rd32();
        char buf[16];
        snprintf(buf, sizeof buf, "%x", disabled);
        MCP_Log(std::string("field disabled: 0x") + buf);
        break;
    }
    case MSG_TOSS_COIN:
    case MSG_TOSS_DICE: {
        uint8_t pl = rd8();
        uint8_t cnt = rd8();
        std::ostringstream out;
        out << "P" << (int)pl << " tosses " << (msg[0] == MSG_TOSS_COIN ? "coin" : "dice") << ":";
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
        MCP_Log("P" + std::to_string((int)pl) + " hand result: " + std::to_string((int)res));
        break;
    }
    case MSG_RETRY: {
        // Response rejected; the box must be acked, then ClientAnalyze
        // re-runs the last PROMPT (which is not narrated -> no duplicates).
        MCP_Log("[!] response rejected — re-answering prompt");
        break;
    }
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
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

// Short zone label for the narration log (H->GY etc.).
static const char* MCP_LogLoc(int loc) {
    switch (loc) {
    case LOCATION_DECK: return "D";
    case LOCATION_HAND: return "H";
    case LOCATION_MZONE: return "MZ";
    case LOCATION_SZONE: return "SZ";
    case LOCATION_GRAVE: return "GY";
    case LOCATION_REMOVED: return "RF";
    case LOCATION_EXTRA: return "EX";
    case LOCATION_OVERLAY: return "OV";
    case LOCATION_FZONE: return "FZ";
    case LOCATION_PZONE: return "PZ";
    default: return "?";
    }
}

static const char* MCP_PhaseName(uint16_t ph) {
    switch (ph) {
    case PHASE_DRAW: return "Draw";
    case PHASE_STANDBY: return "Standby";
    case PHASE_MAIN1: return "Main1";
    case PHASE_BATTLE_START: return "BatStart";
    case PHASE_BATTLE_STEP: return "Step";
    case PHASE_DAMAGE: return "Dmg";
    case PHASE_DAMAGE_CAL: return "DmgCal";
    case PHASE_BATTLE: return "Battle";
    case PHASE_MAIN2: return "Main2";
    case PHASE_END: return "End";
    default: return "?";
    }
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

static std::string MCP_LvLabel(ClientCard* c) {
    if (!c) return "";
    if (c->type & TYPE_XYZ) return "R" + std::to_string(c->rank & 0xff);
    if (c->type & TYPE_LINK) return "L" + std::to_string(c->link & 0xff);
    return "Lv" + std::to_string(c->level & 0xff);
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

enum class MCActionKind { CLICK, MENU_CARD_CLICK, POS, TOGGLE_CARD, PLACE, CHAIN_CARD, CHAIN_DECLINE, OPTION, SUM, ANNOUNCE_RACE, ANNOUNCE_ATTRIB, ANNOUNCE_CARD, ANNOUNCE_NUMBER };

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
static void MCP_AddCardCmd(ClientCard* card, irr::gui::IGUIElement* btn) { g_actions.push_back({MCActionKind::MENU_CARD_CLICK, btn, card}); }
static void MCP_AddToggleCard(ClientCard* card) { g_actions.push_back({MCActionKind::TOGGLE_CARD, nullptr, card}); }

static void MCP_RunAction(const MCAction& a) {
    if (a.kind == MCActionKind::MENU_CARD_CLICK) {
        if (a.card) mainGame->dField.menu_card = a.card;
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
        // SetResponseSelectedOption).
        auto& f = mainGame->dField;
        int idx = a.pos;
        if (idx < 0 || (size_t)idx >= f.select_options.size()) return;
        if (mainGame->dInfo.curMsg == MSG_SELECT_OPTION) {
            DuelClient::SetResponseI(idx);
        } else {
            int index = (size_t)idx < f.select_options_index.size() ? f.select_options_index[idx] : idx;
            if (mainGame->dInfo.curMsg == MSG_SELECT_IDLECMD)
                DuelClient::SetResponseI((index << 16) + 5);
            else if (mainGame->dInfo.curMsg == MSG_SELECT_BATTLECMD)
                DuelClient::SetResponseI(index << 16);
            else
                DuelClient::SetResponseI(index);
        }
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
// Optional min/max override: MSG_SELECT_UNSELECT_CARD must always respond
// with exactly ONE card (core check_response hardcodes 1,1).
static std::string MCP_CardsText(int min_override = -1, int max_override = -1) {
    auto& f = mainGame->dField;
    std::ostringstream out;
    int min = MCP_AdjMin(f.select_min, f.select_cancelable);
    int max = f.select_max;
    if (min_override >= 0) {
        // UNSELECT: cancel/finish flag permits the -1 (cancel) response
        min = (f.select_cancelable || min_override == 0) ? 0 : 1;
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
    auto addz = [&](int ctrl, unsigned loc, int seq, unsigned flag) {
        if (!(f.selectable_field & flag)) return;
        out << n << ". " << (ctrl ? "opp" : "my") << " " << MCP_LocName(loc) << "[" << seq << "]\n";
        g_actions.push_back({MCActionKind::PLACE, nullptr, nullptr, (int)flag});
        ++n;
    };
    for (int i = 0; i < 7; ++i) addz(0, LOCATION_MZONE, i, 1u << i);
    for (int i = 0; i < 8; ++i) addz(0, LOCATION_SZONE, i, 0x100u << i);
    for (int i = 0; i < 7; ++i) addz(1, LOCATION_MZONE, i, 0x10000u << i);
    for (int i = 0; i < 8; ++i) addz(1, LOCATION_SZONE, i, 0x1000000u << i);
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

static std::string MCP_LobbyText() {
    std::ostringstream out;
    bool is_host = mainGame->btnHostPrepStart->isVisible();   // host-only start button
    bool ready = mainGame->btnHostPrepNotReady->isVisible();   // not-ready button shown => we ARE ready
    const wchar_t* deck = mainGame->cbDeckSelect->getSelected() >= 0
        ? mainGame->cbDeckSelect->getItem(mainGame->cbDeckSelect->getSelected()) : L"(none)";
    out << "deck: " << U8(deck) << "\n";
    if (ready) {
        out << "0. cancel ready\n";
        MCP_AddClickEl(mainGame->btnHostPrepNotReady);
    } else {
        out << "0. ready\n";
        MCP_AddClickEl(mainGame->btnHostPrepReady);
    }
    if (is_host) {
        out << "1. start duel\n";
        MCP_AddClickEl(mainGame->btnHostPrepStart);
    }
    out << "2. leave room\n";
    MCP_AddClickEl(mainGame->btnHostPrepCancel);
    out << "-1. wait\n";
    return out.str();
}

// ---------------------------------------------------------------------------
// game state serialization (text format)
// ---------------------------------------------------------------------------

static std::string MCP_MeOpp() {
    if (!mainGame) return "";
    uint8_t st = DuelClient::selftype;
    if (st < 2) {
        const wchar_t* me = (st == 0) ? mainGame->dInfo.hostname : mainGame->dInfo.clientname;
        const wchar_t* opp = (st == 0) ? mainGame->dInfo.clientname : mainGame->dInfo.hostname;
        return "me=" + U8(me) + " opp=" + U8(opp);
    }
    return "";
}

// One zone line of the board: names only, except MZone (pos/level/atk/def +
// chain marker) and SZone (UP/FD) -- field stats change with effects.
// MZone/SZone are fixed-size positional vectors (null = empty slot).
static void MCP_ZoneLine(std::ostringstream& out, const char* label,
                         const std::vector<ClientCard*>& v, bool stats) {
    out << "  " << label << ": ";
    if (v.empty()) { out << "-\n"; return; }
    bool any = false;
    if (stats) {
        for (size_t i = 0; i < v.size(); ++i) {
            auto* c = v[i];
            if (!c) continue;
            if (c->code) g_mention_ctx.insert(c->code & 0x7fffffff);
            out << (any ? ", " : "") << "[" << i << "] ";
            // Face-down cards have code 0 (the client learns them on flip).
            out << (c->code ? U8(dataManager.GetName(c->code)) : "?");
            out << " " << (c->location == LOCATION_SZONE ? MCP_PosST(c->position) : MCP_PosMonster(c->position));
            if (c->location == LOCATION_MZONE) {
                out << " " << MCP_LvLabel(c) << " " << c->attack << "/" << c->defense;
                int n = 1;
                for (auto& ch : mainGame->dField.chains) {
                    if (ch.controler == c->controler && ch.location == c->location && ch.sequence == c->sequence) {
                        out << " (ch" << n << ")";
                        break;
                    }
                    ++n;
                }
            }
            any = true;
        }
    } else {
        bool first = true;
        for (auto* c : v) {
            if (!c) continue;
            if (c->code) g_mention_ctx.insert(c->code & 0x7fffffff);
            out << (first ? "" : ", ") << (c->code ? U8(dataManager.GetName(c->code)) : "?");
            first = false;
            any = true;
        }
    }
    out << (any ? "\n" : "-\n");
}

// A duel prompt is PENDING only when a fresh STOC message arrived since the
// agent's last action. curMsg stays at the old prompt type after answering
// until the next message, so acting on (curMsg, state) alone re-sends
// responses for prompts that are already answered -- the core rejects those
// (MSG_RETRY / server "invalid operation") and the UI (e.g. the EP button)
// keeps flashing waiting for a valid response. Exception: after a RETRY box
// is acked, the same prompt is re-issued WITHOUT a new STOC packet (internal
// recursion), so the seq gate must stay open for the re-prompt -- see
// MCP_DoChoose's retry_box handling.
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
        if (!g->dField.selectable_cards.empty()) return MCP_CardsText(1, 1);
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

// STOC seq at which the agent last acted (ygo_choose). The duel state shows
// a prompt only when seq != last_acted (i.e. a fresh message arrived).
static std::atomic<uint64_t> g_last_acted_seq{ 0 };
static int g_detail_round = 0;                              // ygo_log read counter
static std::unordered_map<uint32_t, int> g_last_detail;     // code -> round explained

// A duel prompt is PENDING only when a fresh STOC message arrived since the
// agent's last action. curMsg stays at the old prompt type after answering
// until the next message, so acting on (curMsg, state) alone re-sends
// responses for prompts that are already answered -- the core rejects those
// (MSG_RETRY / server "invalid operation") and the UI (e.g. the EP button)
// keeps flashing waiting for a valid response. Exception: after a RETRY box
// is acked, the same prompt is re-issued WITHOUT a new STOC packet (internal
// recursion), so the seq gate must stay open for the re-prompt -- see
// MCP_DoChoose's retry_box handling.
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
// ydkshow-style explains are emitted by ygo_log instead.
static std::string MCP_DuelState() {
    Game* g = mainGame;
    auto& f = g->dField;
    std::ostringstream out;
    uint64_t seq = g_stoc_seq.load(std::memory_order_relaxed);

    // Explain candidates: narration mentions since the last drain, plus every
    // card rendered below (field + choices) -- collected for ygo_log.
    auto absorb = [&]() {
        if (!g_mention_ctx.empty()) {
            std::lock_guard<std::mutex> lk(g_log_mtx);
            for (auto c : g_mention_ctx) g_mentioned.insert(c);
            g_mention_ctx.clear();
        }
    };

    // header. NOTE: no changing fields here (seq etc.) -- consumers key
    // retry/rejection tracking on the whole state text staying stable for a
    // given prompt instance. The seq gate is internal: the prompt surface is
    // rendered only for a fresh message (see below).
    out << "Turn " << g->dInfo.turn << " | LP " << g->dInfo.lp[0]
        << " / " << g->dInfo.lp[1] << " | Ph " << MCP_PhaseName(g_phase.load()) << " | " << MCP_MeOpp() << "\n";

    // board
    for (int p = 0; p < 2; ++p) {
        out << "P" << p << (p == 0 ? "(me)" : "(opp)")
            << " Deck" << f.deck[p].size() << " Extra" << f.extra[p].size()
            << " Grave" << f.grave[p].size() << " Removed" << f.remove[p].size() << "\n";
        if (p == 0) {
            MCP_ZoneLine(out, "Hand", f.hand[0], false);
        } else {
            // Privacy: opponent hand is face-down count only (the client does
            // hold the codes -- ygopro v1 leak -- but the GUI never shows them).
            out << "  Hand: [" << f.hand[1].size() << " face-down]\n";
        }
        MCP_ZoneLine(out, "MZone", f.mzone[p], true);
        MCP_ZoneLine(out, "SZone", f.szone[p], true);
        MCP_ZoneLine(out, "Grave", f.grave[p], false);
        MCP_ZoneLine(out, "Removed", f.remove[p], false);
    }
    // chain line (parsed ChainInfo state; mirrors GUI chain arrows)
    if (!f.chains.empty() || f.current_chain.code) {
        out << "Chain:";
        int n = 1;
        for (auto& ch : f.chains) {
            ClientCard* c = ch.chain_card;
            std::string cn = c ? U8(dataManager.GetName(c->code)) : U8(dataManager.GetName(ch.code));
            out << " " << n << ". P" << (int)ch.controler << " " << cn << (ch.solved ? "" : " (solving)");
            ++n;
        }
        bool cur_pending = f.current_chain.code
            && (f.chains.empty()
                || f.chains.back().code != f.current_chain.code
                || f.chains.back().sequence != f.current_chain.sequence);
        if (cur_pending) {
            ClientCard* c = f.current_chain.chain_card;
            std::string cn = c ? U8(dataManager.GetName(c->code)) : U8(dataManager.GetName(f.current_chain.code));
            out << " " << n << ". P" << (int)f.current_chain.controler << " " << cn << " (adding)";
        }
        out << "\n";
    }

    // prompt surface or wait state
    std::string choices;
    if (g->dInfo.isFinished) {
        out << "Game Over\n";
    } else if (g->wOptions && g->wOptions->isVisible() && !f.select_options.empty()) {
        // Multi-activation options window (e.g. "banish or search"): opened
        // by the activate button handler WITHOUT a new STOC message, so the
        // seq gate does not apply -- window-driven, render unconditionally.
        choices = MCP_OptionsText();
    } else if (MCP_IsPrompt(g->dInfo.curMsg)) {
        // seq gate: curMsg stays at the last prompt type after answering
        // until the next message; show the prompt only for a fresh instance.
        bool acted = (seq == g_last_acted_seq.load(std::memory_order_relaxed));
        if (!acted) choices = MCP_PromptText(g->dInfo.curMsg);
    }
    absorb();

    if (!choices.empty()) out << choices;
    else if (!g->dInfo.isFinished) out << "(waiting for opponent or disconnected)\n";
    if (std::getenv("YGOCLI_MCP_DEBUG")) {
        out << "[dbg] seq=" << seq << " acted=" << g_last_acted_seq.load(std::memory_order_relaxed)
            << " cur=" << (int)g->dInfo.curMsg
            << " wQuery=" << (g->wQuery && g->wQuery->isVisible())
            << " wMsg=" << (g->wMessage && g->wMessage->isVisible())
            << " wCard=" << (g->wCardSelect && g->wCardSelect->isVisible())
            << " selc=" << f.selectable_cards.size()
            << " chains=" << f.chains.size() << "\n";
    }
    return out.str();
}

static std::string MCP_CurrentState() {
    g_actions.clear();
    g_mention_ctx.clear();
    if (!mainGame || !mainGame->device) return "Error: game not ready\n";
    Game* g = mainGame;

    // Mandatory-ack message box: the network thread is parked here; there is
    // no protocol ack (see header caveats) -- the agent must click OK.
    if (g->wMessage && g->wMessage->isVisible()) {
        MCP_AddClickEl(g->btnMsgOK);
        std::wstring msg = g->stMessage ? g->stMessage->getText() : L"";
        return "message: " + U8(msg.c_str()) + "\n" + MCP_OptionHeader(1, 1, 1) + "0. OK\n";
    }
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
    if (g->dInfo.isStarted && !g->dInfo.isReplay) return MCP_DuelState();
    if (g->wHostPrepare && g->wHostPrepare->isVisible()) return MCP_LobbyText();
    if (g->wMainMenu && g->wMainMenu->isVisible()) return "Main menu (connect via launch flags -j -n -h -p -w -d)\n";
    return "(waiting)\n";
}

// ---------------------------------------------------------------------------
// tools
// ---------------------------------------------------------------------------

static std::string MCP_DoChoose(const json& args) {
    int id = args.value("id", -1);
    std::string state = MCP_CurrentState();
    // RETRY box: the agent is acking a rejection; the SAME prompt is then
    // re-issued internally (no new STOC packet), so this ack must NOT mark
    // the prompt as acted -- otherwise the seq gate would hide the re-prompt.
    bool retry_box = mainGame && mainGame->wMessage && mainGame->wMessage->isVisible()
        && mainGame->dInfo.curMsg == MSG_RETRY;

    // -1: in a duel prompt this means PASS (end turn / decline); otherwise wait.
    // It is an ACTION, not a read. It only acts when a prompt is actually
    // pending (a stale prompt must never be re-answered -- that is the source
    // of "invalid operation" rejections).
    if (id == -1 || id == -2) {
        // Defensive box ack: any visible blocking box is acked regardless of
        // the requested id (see header caveats; a parked network thread
        // freezes the client).
        if (mainGame && mainGame->wMessage && mainGame->wMessage->isVisible()) {
            if (mainGame->btnMsgOK) MCP_Click(mainGame->btnMsgOK);
            if (!retry_box)
                g_last_acted_seq.store(g_stoc_seq.load(std::memory_order_relaxed));
            return state;
        }
        if (MCP_PromptPending()) {
            if (id == -2) {
                // list-window mode: OK button; field mode: finish (CancelOrFinish).
                if (mainGame && mainGame->wCardSelect && mainGame->wCardSelect->isVisible()) {
                    if (mainGame->btnSelectOK) MCP_Click(mainGame->btnSelectOK);
                } else if (mainGame && mainGame->btnCancelOrFinish && mainGame->btnCancelOrFinish->isVisible()) {
                    MCP_Click(mainGame->btnCancelOrFinish);
                }
            }
            switch (mainGame->dInfo.curMsg) {
            case MSG_SELECT_IDLECMD:
            case MSG_SELECT_BATTLECMD:
                if (mainGame->btnEP) MCP_Click(mainGame->btnEP);   // end phase
                break;
            default:
                if (mainGame->btnCancelOrFinish && mainGame->btnCancelOrFinish->isVisible())
                    MCP_Click(mainGame->btnCancelOrFinish);
                break;
            }
        }
        g_last_acted_seq.store(g_stoc_seq.load(std::memory_order_relaxed));
        return state;
    }
    // "indices": the agent's uniform rule. Read "Option N~M/TOTAL:", pick
    // k = randint(N, M) distinct option ids from 0..TOTAL-1, send via
    // indices. Batch-capable prompts (card selects, sort, place, announce)
    // take the subset directly; a single pick on anything else (command,
    // yes/no, chain, position, option, boxes, RPS, turn-order) is routed to
    // the regular option action, which works for any visible window.
    if (args.contains("indices") && mainGame) {
        auto& f = mainGame->dField;
        int cur = mainGame->dInfo.curMsg;
        bool sel_family = (cur == MSG_SELECT_CARD || cur == MSG_SELECT_TRIBUTE
            || cur == MSG_SELECT_COUNTER) && !f.selectable_cards.empty();
        // NOTE: MSG_SELECT_UNSELECT_CARD (single card, core hardcodes 1,1) and
        // MSG_SELECT_SUM (valid-combination options) are deliberately NOT
        // batch -- the uniform single-pick route handles them.
        bool batch = sel_family || cur == MSG_SORT_CARD
            || cur == MSG_SELECT_PLACE || cur == MSG_SELECT_DISFIELD
            || cur == MSG_ANNOUNCE_RACE || cur == MSG_ANNOUNCE_ATTRIB;
        if (batch && MCP_PromptPending()) {
            if (sel_family) {
                std::vector<ClientCard*> chosen;
                bool ok = true;
                for (auto& v : args["indices"]) {
                    int idx = v.get<int>();
                    if (idx < 0 || (size_t)idx >= f.selectable_cards.size()) { ok = false; break; }
                    auto* c = f.selectable_cards[idx];
                    if (!c->is_selectable) { ok = false; break; }
                    chosen.push_back(c);
                }
                if (!ok) {
                    return "Error: invalid indices\n" + MCP_CurrentState();
                }
                // Empty selection = cancel, valid only for cancelable prompts
                // with min<=1 (presented as min=0). Never a separate option.
                int min = MCP_AdjMin(f.select_min, f.select_cancelable);
                if (chosen.empty()) {
                    if (min > 0)
                        return "Error: empty selection not allowed (min=" + std::to_string(min) + ")\n" + MCP_CurrentState();
                    // cancel: -1 is accepted by the core iff the cancelable
                    // flag is set (playerop.cpp select_card)
                    for (auto* c : f.selectable_cards) c->is_selected = false;
                    f.selected_cards.clear();
                    if (mainGame->wCardSelect && mainGame->wCardSelect->isVisible())
                        mainGame->HideElement(mainGame->wCardSelect, false);
                    f.ShowCancelOrFinishButton(0);
                    DuelClient::SetResponseI(-1);
                    DuelClient::SendResponse();
                    g_last_acted_seq.store(g_stoc_seq.load(std::memory_order_relaxed));
                    return "Canceled.\n" + MCP_CurrentState();
                }
                // Pre-validate the count against [min, max] so the core never
                // needs to reject (avoids MSG_RETRY round-trips).
                if ((int)chosen.size() < min || (int)chosen.size() > f.select_max) {
                    return "Error: need " + std::to_string(min) + "-" + std::to_string(f.select_max)
                        + " options, got " + std::to_string(chosen.size()) + "\n" + MCP_CurrentState();
                }
                for (auto* c : f.selectable_cards) c->is_selected = false;
                f.selected_cards.clear();
                for (auto* c : chosen) {
                    c->is_selected = true;
                    f.selected_cards.push_back(c);
                }
                f.SetResponseSelectedCards();
                f.ShowCancelOrFinishButton(0);
                if (mainGame->wCardSelect && mainGame->wCardSelect->isVisible())
                    mainGame->HideElement(mainGame->wCardSelect, false);
                DuelClient::SendResponse();
                g_last_acted_seq.store(g_stoc_seq.load(std::memory_order_relaxed));
                return "Selected " + std::to_string(chosen.size()) + " cards.\n" + MCP_CurrentState();
            }
            // MSG_SORT_CARD: indices = the desired card order.
            if (cur == MSG_SORT_CARD) {
                size_t n = f.selectable_cards.size();
                std::vector<int> order;
                bool ok = true;
                for (auto& v : args["indices"]) {
                    int idx = v.get<int>();
                    if (idx < 0 || (size_t)idx >= n) { ok = false; break; }
                    order.push_back(idx);
                }
                if (!ok || order.size() != n) {
                    return "Error: sort needs exactly " + std::to_string(n) + " indices\n" + MCP_CurrentState();
                }
                f.RespondSortOrder(order);
                if (mainGame->wCardSelect && mainGame->wCardSelect->isVisible())
                    mainGame->HideElement(mainGame->wCardSelect, false);
                DuelClient::SendResponse();
                g_last_acted_seq.store(g_stoc_seq.load(std::memory_order_relaxed));
                return "Sorted.\n" + MCP_CurrentState();
            }
            // MSG_SELECT_PLACE / DISFIELD: indices = zone options to place on.
            if (cur == MSG_SELECT_PLACE || cur == MSG_SELECT_DISFIELD) {
                std::vector<int> opts;
                for (size_t i = 0; i < g_actions.size(); ++i)
                    if (g_actions[i].kind == MCActionKind::PLACE) opts.push_back((int)i);
                std::vector<unsigned> flags;
                bool ok = true;
                for (auto& v : args["indices"]) {
                    int idx = v.get<int>();
                    if (idx < 0 || (size_t)idx >= opts.size()) { ok = false; break; }
                    flags.push_back((unsigned)g_actions[opts[idx]].pos);
                }
                if (!ok || flags.empty()) {
                    return "Error: invalid zone indices\n" + MCP_CurrentState();
                }
                int need = (int)f.select_min;
                for (unsigned flag : flags) {
                    if (!(flag & f.selectable_field)) { ok = false; break; }
                    f.selected_field |= flag;
                    f.select_min--;
                }
                if (!ok || (int)flags.size() != need) {
                    f.selected_field = 0;
                    f.select_min = need;
                    return "Error: need exactly " + std::to_string(need) + " zones\n" + MCP_CurrentState();
                }
                f.RespondZones();
                DuelClient::SendResponse();
                f.ShowCancelOrFinishButton(0);
                g_last_acted_seq.store(g_stoc_seq.load(std::memory_order_relaxed));
                return "Placed.\n" + MCP_CurrentState();
            }
            // ANNOUNCE_RACE / ANNOUNCE_ATTRIB: option ids are sequential over
            // VISIBLE checkboxes; map id -> actual checkbox index, send bitmask.
            if (cur == MSG_ANNOUNCE_RACE || cur == MSG_ANNOUNCE_ATTRIB) {
                std::vector<int> visible;
                if (cur == MSG_ANNOUNCE_RACE)
                    for (int i = 0; i < RACES_COUNT; ++i)
                        if (mainGame->chkRace[i]->isVisible()) visible.push_back(i);
                else
                    for (int i = 0; i < 7; ++i)
                        if (mainGame->chkAttribute[i]->isVisible()) visible.push_back(i);
                std::vector<int> bits;
                bool ok = true;
                for (auto& v : args["indices"]) {
                    int idx = v.get<int>();
                    if (idx < 0 || (size_t)idx >= visible.size()) { ok = false; break; }
                    bits.push_back(visible[idx]);
                }
                if (!ok || (int)bits.size() != (int)f.announce_count) {
                    return "Error: need " + std::to_string((int)f.announce_count)
                        + " announce indices\n" + MCP_CurrentState();
                }
                int val = 0;
                for (int b : bits) val |= (1 << b);
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
                g_last_acted_seq.store(g_stoc_seq.load(std::memory_order_relaxed));
                return "Announced.\n" + MCP_CurrentState();
            }
        }
        // Single pick via the option action (any visible window: prompts,
        // message boxes, RPS, turn-order).
        if (args["indices"].is_array() && args["indices"].size() == 1) {
            int oid = args["indices"][0].get<int>();
            if (oid < 0 || (size_t)oid >= g_actions.size())
                return "Error: invalid choice (range 0.." + std::to_string(g_actions.size() - 1) + ")\n" + MCP_CurrentState();
            MCP_RunAction(g_actions[oid]);
            if (!retry_box)
                g_last_acted_seq.store(g_stoc_seq.load(std::memory_order_relaxed));
            return "Chose option " + std::to_string(oid) + ".\n" + MCP_CurrentState();
        }
        // Empty indices = cancel for single-card prompts that permit it
        // (MSG_SELECT_UNSELECT_CARD: core accepts -1 iff cancelable||finishable).
        if (args["indices"].is_array() && args["indices"].empty()
            && cur == MSG_SELECT_UNSELECT_CARD && f.select_cancelable
            && MCP_PromptPending()) {
            DuelClient::SetResponseI(-1);
            DuelClient::SendResponse();
            g_last_acted_seq.store(g_stoc_seq.load(std::memory_order_relaxed));
            return "Canceled.\n" + MCP_CurrentState();
        }
        return "Error: indices do not match the current prompt\n" + MCP_CurrentState();
    }
    if (std::getenv("YGOCLI_MCP_DEBUG")) {
        std::string kinds;
        for (auto& a : g_actions) kinds += std::to_string(a.elem ? a.elem->getID() : -1) + " ";
        fprintf(stderr, "[mcp] choose id=%d actions=[%s]\n", id, kinds.c_str());
    }
    if (id < 0 || (size_t)id >= g_actions.size()) {
        return "Error: invalid choice (range 0.." + std::to_string(g_actions.size() - 1) + ")\n" + state;
    }
    MCP_RunAction(g_actions[id]);
    // Toggle-based card picks auto-send via the game's own logic at
    // select_max (see ClientField::ToggleSelectCard); finish below max with
    // an explicit -2. No extra OK-click auto-confirmation here.
    if (!retry_box)
        g_last_acted_seq.store(g_stoc_seq.load(std::memory_order_relaxed));
    return "Chose option " + std::to_string(id) + ".\n" + MCP_CurrentState();
}

static std::string MCP_DoChat(const json& args) {
    std::string text = args.value("text", "");
    if (text.empty()) return "Error: text parameter required";
    uint16_t msgbuf[LEN_CHAT_MSG];
    size_t len = 0;
    size_t i = 0;
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
    return "Sent: " + text;
}

// Recent incoming chat (server announcements, countdowns, room chat).
// May be empty in srvpro password rooms (server drops player chat there).
static std::string MCP_DoChatlog(const json& args) {
    int n = args.value("count", 20);
    std::lock_guard<std::mutex> lk(mainGame->gMutex);
    std::string out;
    size_t start = g_mcp_chat.size() > (size_t)n ? g_mcp_chat.size() - (size_t)n : 0;
    for (size_t i = start; i < g_mcp_chat.size(); ++i)
        out += g_mcp_chat[i] + "\n";
    return out;
}

// Narration log (drains the buffer): draws, summons, attacks, activations,
// chain, phase changes, discards/banishes/destructions, LP events, hints.
// Also emits the ydkshow-style card explains for mentioned-but-not-yet-
// explained cards (recency = YGOCLI_EXPLAIN_ROUNDS log reads, default 3).
static std::string MCP_DoLog(const json& args) {
    std::vector<std::string> lines;
    std::set<uint32_t> cands;
    {
        std::lock_guard<std::mutex> lk(g_log_mtx);
        for (auto& s : g_log) lines.push_back(s);
        g_log.clear();
        cands.swap(g_mentioned);
    }
    // card explains for candidates not recently explained (drained with the
    // log, so the ygo_state text stays stable for retry tracking)
    int rounds = MCP_ExplainRounds();
    g_detail_round++;
    std::vector<uint32_t> toexplain;
    for (uint32_t code : cands) {
        if (!code) continue;
        auto it = g_last_detail.find(code);
        int last = (it == g_last_detail.end()) ? -1 : it->second;
        if (last == -1 || g_detail_round - last >= rounds) toexplain.push_back(code);
    }
    std::sort(toexplain.begin(), toexplain.end());
    std::vector<std::string> explains;
    for (uint32_t code : toexplain) {
        std::string line = MCP_CardLine(code);
        if (!line.empty()) {
            explains.push_back(line);
            g_last_detail[code] = g_detail_round;
        }
    }
    if (lines.empty() && explains.empty()) return "(no new events)\n";
    std::string out;
    for (auto& s : lines) out += s + "\n";
    for (auto& s : explains) out += s + "\n";
    return out;
}

// Card explain: code or name (substring match, up to 20 hits). Single line
// per card, ydkshow-style. Never acts.
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
    std::string out;
    for (uint32_t c : hits) {
        std::string line = MCP_CardLine(c);
        if (!line.empty()) out += line + "\n";
    }
    if (out.empty()) return "No card found: " + q + "\n";
    return out;
}

static std::string MCP_DoSurrender() {
    // Protocol-direct: CTOS_SURRENDER is accepted by the server gate in any
    // state; no window needed (the old button-click path required wSurrender).
    DuelClient::SendPacketToServer(CTOS_SURRENDER);
    return "Surrendered.";
}

static std::string MCP_DoDisconnect() {
    Game* g = mainGame;
    if (g->dInfo.isStarted && g->btnLeaveGame) {
        MCP_Click(g->btnLeaveGame);
    } else if (g->btnHostPrepCancel) {
        MCP_Click(g->btnHostPrepCancel);
    } else {
        DuelClient::StopClient();
    }
    return "Left the room.";
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

static void MCP_Execute(MCPJob* job) {
    if (job->method == "initialize") {
        job->ok = true;
        job->result = {{"protocolVersion", "2024-11-05"}, {"capabilities", json::object()}};
        return;
    }
    if (job->method == "tools/list") {
        json props;
        props["id"] = {{"type", "integer"}, {"description", "one option id, or (with indices) the batch selector"}};
        props["indices"] = {{"type", "array"}, {"items", {{"type", "integer"}}}, {"description", "option ids to pick (see the Option N~M/TOTAL: header in ygo_state); empty = cancel when allowed"}};
        json tools = json::array();
        tools.push_back(ToolSchema("ygo_choose", "Answer the pending prompt: pick k = randint(N, M) ids from 0..TOTAL-1 (per the Option N~M/TOTAL: header) and send via indices; or send id for a single option", props));
        tools.push_back(ToolSchema("ygo_state", "Read-only board + prompt view (never acts)", json::object()));
        tools.push_back(ToolSchema("ygo_log", "Recent game events (draws, summons, attacks, activations, chain, phase changes, discards...). Drains the buffer.", json::object()));
        tools.push_back(ToolSchema("ygo_card", "Explain a card on one line (ydkshow-style): name or code", {{"name", {{"type", "string"}, {"description", "card name (substring) or numeric code"}}}}));
        tools.push_back(ToolSchema("ygo_chat", "Send a chat message to the room", {{"text", {{"type", "string"}, {"description", "message text"}}}}));
        tools.push_back(ToolSchema("ygo_chatlog", "Recent incoming chat (server announcements, countdowns, room chat)", {{"count", {{"type", "integer"}, {"description", "max lines (default 20)"}}}}));
        tools.push_back(ToolSchema("ygo_surrender", "Surrender the current duel", json::object()));
        tools.push_back(ToolSchema("ygo_disconnect", "Leave the current room", json::object()));
        job->ok = true;
        job->result = {{"tools", tools}};
        return;
    }
    if (job->method == "tools/call") {
        std::string name = job->params.value("name", "");
        json args = job->params.contains("arguments") ? job->params["arguments"] : json::object();
        std::string text;
        if (name == "ygo_choose") text = MCP_DoChoose(args);
        else if (name == "ygo_state") text = MCP_CurrentState();
        else if (name == "ygo_log") text = MCP_DoLog(args);
        else if (name == "ygo_card") text = MCP_DoCard(args);
        else if (name == "ygo_chat") text = MCP_DoChat(args);
        else if (name == "ygo_chatlog") text = MCP_DoChatlog(args);
        else if (name == "ygo_surrender") text = MCP_DoSurrender();
        else if (name == "ygo_disconnect") text = MCP_DoDisconnect();
        else {
            job->ok = false;
            job->error_msg = "unknown tool: " + name;
            return;
        }
        job->ok = true;
        job->result = {{"content", json::array({json{{"type", "text"}, {"text", text}}})}};
        return;
    }
    job->ok = false;
    job->error_msg = "method not found: " + job->method;
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

void MCP_Init() {
    if (g_running) return;
    // The MCP channel owns the real stdout; the app's own output goes to stderr.
    g_json_fd = ::dup(STDOUT_FILENO);
    if (g_json_fd < 0) return;
    if (::freopen("/dev/stderr", "w", stdout) == nullptr) {
        // fall back: keep stdout for the app (MCP will be unusable)
        ::close(g_json_fd);
        g_json_fd = -1;
        return;
    }
    std::signal(SIGPIPE, SIG_IGN);
    g_running = true;
    dbg("init: starting reader thread");
    g_thread = std::thread(MCP_ReaderThread);
}

void MCP_Shutdown() {
    g_running = false;
    if (g_thread.joinable()) g_thread.join();
    if (g_json_fd >= 0) {
        ::close(g_json_fd);
        g_json_fd = -1;
    }
}

void MCP_Poll() {
    dbg("poll called");
    // The GUI event receiver drops button events while a fade animation is
    // running (ClientField::OnEvent: if(fadingList.size()) break). Defer the
    // whole job until the fades clear so clicks are never silently lost.
    if (mainGame && mainGame->fadingList.size()) {
        return;
    }
    while (auto* job = MCP_PopJob()) {
        MCP_Execute(job);
        {
            std::lock_guard<std::mutex> lk(job->mtx);
            job->done = true;
        }
        job->cv.notify_one();
    }
}

} // namespace ygo
