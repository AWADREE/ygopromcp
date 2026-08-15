#!/usr/bin/env python3
"""Random-choice gameplay test through the MCP bridge.

The agent is deliberately stupid: it reads ygo_state, counts the numbered
choices the bridge printed (the bridge only prints VALID choices from the
STOC prompt), and picks one at random. No strategy. If the game progresses
(turns advance, LP moves, actions happen) the bridge works; deadlocks and
invalid-operation rejections surface as failures to fix.

Run: python3 fork_aggro_play.py [room-name] [deck]
"""
import json, subprocess, sys, time, select, re, random

EXE = "/home/z/ygo/bin/release/YGOPro"
HOST = "127.0.0.1"; PORT = 7911
PASS = sys.argv[1] if len(sys.argv) > 1 else "aggro-room"
DECK = sys.argv[2] if len(sys.argv) > 2 else "example.ydk"
DECKNAME = DECK[:-4] if DECK.lower().endswith(".ydk") else DECK
TO = 10.0
WATCHDOG = 45   # seconds of no turn/LP progress before forcing a pass
RUN = 150       # seconds max per run

TRANS = open("/tmp/mcp_transcript.log", "w")
def dump(tag, what, body):
    ts = "%.1f" % (time.time() - T0)
    TRANS.write("[%s %s] %s\n%s\n" % (ts, tag, what, body))
    TRANS.flush()
T0 = time.time()


class MCP:
    def __init__(self, name, errpath):
        self.proc = subprocess.Popen(
            [EXE, "--mcp", "-k", "-n", name, "-h", HOST, "-p", str(PORT),
             "-w", PASS, "-d", DECKNAME, "-j"],
            cwd="/home/z/ygo", stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=open(errpath, "w"), text=True, bufsize=1)
        self._id = 0; self.name = name
        time.sleep(3.5)
        self.rpc("initialize", {"protocolVersion": "2024-11-05"})
        self.actions = 0
        self.prompts = {}

    def rpc(self, method, params=None):
        self._id += 1
        req = {"jsonrpc": "2.0", "id": self._id, "method": method}
        if params is not None: req["params"] = params
        self.proc.stdin.write(json.dumps(req) + "\n"); self.proc.stdin.flush()
        r, _, _ = select.select([self.proc.stdout], [], [], TO)
        if not r: raise TimeoutError(self.name + " timeout")
        line = self.proc.stdout.readline()
        if not line: raise RuntimeError(self.name + " closed")
        return json.loads(line)

    def choose(self, **args):
        r = self.rpc("tools/call", {"name": "ygo_choose", "arguments": args})
        return r["result"]["content"][0]["text"]

    def state(self):
        r = self.rpc("tools/call", {"name": "ygo_state", "arguments": {}})
        return r["result"]["content"][0]["text"]

    def log(self):
        r = self.rpc("tools/call", {"name": "ygo_log", "arguments": {}})
        return r["result"]["content"][0]["text"]

    def kill(self):
        self.proc.kill(); self.proc.wait()


def strip_prefix(t):
    for pre in ("Chose option", "Selected ", "Sorted.", "Placed.", "Announced."):
        if t.startswith(pre):
            idx = t.find("\n")
            return t[idx + 1:] if idx >= 0 else t
    return t


def numbered(text):
    # ALL printed options, including printed -1/-2 lines ("-1. pass").
    # Lines may be indented ("  1. rock"). Only send what is printed.
    return [(int(m.group(1)), m.group(2).strip())
            for m in re.finditer(r"^\s*(-?\d+)\. (.*)$", (text or ""), re.M)]


def status(t):
    m = re.search(r"Turn (\d+) \| LP (\d+) / (\d+)", t or "")
    if not m: return None
    turn, lp0, lp1 = int(m.group(1)), int(m.group(2)), int(m.group(3))
    if turn == 0 and lp0 == 0 and lp1 == 0:
        return None
    return (turn, lp0, lp1)


def act(m, text):
    """PURE random: read the Option header (how many choices), pick a random
    subset, send. No option-text parsing, no strategy."""
    t = strip_prefix(text)
    if "Game Over" in t or "Match Over" in t:
        return (None, True)
    if "0. ready" in t:
        return ({"id": 0}, True)          # lobby
    if "start duel" in t:
        return ({"id": 1}, True)
    if "0. cancel ready" in t:
        return ({"id": -1}, True)         # printed lobby option
    mh = re.search(r"Option (\d+)~(\d+)/(\d+):", t)
    if mh:
        lo, hi, total = map(int, mh.groups())
        k = random.randint(lo, min(hi, total))
        ids = random.sample(range(total), k) if k > 0 else []
        m.actions += 1
        return ({"id": 0, "indices": ids}, True)
    return (None, True)                    # nothing printable: wait


def main():
    a = MCP("A", "/tmp/aggro_A.err")
    b = MCP("B", "/tmp/aggro_B.err")
    samples = []
    game_over = False
    t0 = time.time()
    last_progress = time.time()
    last_seen = None
    watchdog_passes = 0
    try:
        while time.time() - t0 < RUN:
            for m in (a, b):
                if game_over: break
                t = m.state()
                dump(m.name, "STATE", t)
                lg = m.log()
                if lg != "(no new events)\n":
                    dump(m.name, "LOG", lg)
                s = status(t)
                if s:
                    samples.append(s)
                    if s != last_seen:
                        last_seen = s
                        last_progress = time.time()
                if "Game Over" in t or "Match Over" in t:
                    print("GAME OVER:", (t or "").strip()[:120])
                    game_over = True
                    break
                kw, acted = act(m, t)
                if kw is None:
                    continue
                r = m.choose(**kw)
                dump(m.name, "CHOOSE %s" % json.dumps(kw), r)
            if time.time() - last_progress > WATCHDOG:
                st = m.state()
                if "0. ready" in st:
                    print("  watchdog re-ready on", m.name)
                    m.choose(id=0)
                elif "start duel" in st:
                    print("  watchdog re-start on", m.name)
                    m.choose(id=1)
                elif "message:" in st:
                    print("  watchdog ack box on", m.name)
                    m.choose(id=0)
                else:
                    print("  watchdog pass on", m.name)
                    m.choose(id=-1)
                watchdog_passes += 1
                last_progress = time.time()
            time.sleep(0.05)
    finally:
        a.kill(); b.kill()

    max_turn = max((s[0] for s in samples), default=0)
    lp0s = {s[1] for s in samples}; lp1s = {s[2] for s in samples}
    lp_moved = len(lp0s) > 1 or len(lp1s) > 1
    actions = a.actions + b.actions
    prompts = set()
    for m in (a, b):
        prompts.update(m.prompts.keys())
    print("turns: %d | actions sent: A %d B %d" % (max_turn, a.actions, b.actions))
    print("LP0 seen:", sorted(lp0s)[:8], "LP1 seen:", sorted(lp1s)[:8])
    print("prompt kinds seen:", sorted(prompts), "watchdog passes:", watchdog_passes)
    ok = (max_turn >= 3 or game_over) and actions > 0 and watchdog_passes <= 3
    print("AGGRO PLAY VERIFIED" if ok else "AGGRO PLAY NOT VERIFIED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
