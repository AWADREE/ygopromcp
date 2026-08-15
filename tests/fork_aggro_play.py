#!/usr/bin/env python3
"""Random-choice gameplay test through the MCP bridge (blocking model, v4).

Entry is via ygo_connect (no CLI join flags). Each client blocks on
ygo_connect (first decision) then loops ygo_choose (next decision). The
bridge returns a block ONLY at a decision point, so a non-terminal block
without '# options' is a bug and raises. Random subset per the
'# options: min~max/total' header. All input/output recorded to
/tmp/mcp_transcript.log. A client process exit stops the run.

Run: python3 fork_aggro_play.py [room-name] [deck-path]
"""
import json, subprocess, sys, time, select, re, random, threading

EXE = "/home/z/ygo/bin/release/YGOPro"
HOST = "127.0.0.1"; PORT = 7911
ROOM = sys.argv[1] if len(sys.argv) > 1 else "aggro-room"
DECK = sys.argv[2] if len(sys.argv) > 2 else "/home/z/ygo/deck/example.ydk"
TO = 60.0       # blocking calls wait for a decision (no timeout in-bridge)
RUN = 150       # seconds max per run

TRANS = open("/tmp/mcp_transcript.log", "w")
T0 = time.time()


def dump(tag, what, body):
    TRANS.write("[%.1f %s] %s\n%s\n" % (time.time() - T0, tag, what, body))
    TRANS.flush()


class MCP:
    def __init__(self, name):
        self.errpath = "/tmp/aggro_%s.err" % name
        self.proc = subprocess.Popen(
            [EXE], cwd="/home/z/ygo", stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=open(self.errpath, "w"),
            text=True, bufsize=1)
        self._id = 0; self.name = name; self.lock = threading.Lock()
        self.actions = 0
        time.sleep(3.5)   # let the client reach the main menu
        self.rpc("initialize", {"protocolVersion": "2024-11-05"})

    def dead(self):
        """Non-empty when the client process has exited."""
        if self.proc.poll() is None:
            return ""
        tail = ""
        try:
            tail = open(self.errpath).read()[-300:]
        except OSError:
            pass
        return "rc=%s stderr=%s" % (self.proc.returncode, tail[:250])

    def rpc(self, method, params=None, timeout=TO):
        with self.lock:
            self._id += 1
            req = {"jsonrpc": "2.0", "id": self._id, "method": method}
            if params is not None:
                req["params"] = params
            self.proc.stdin.write(json.dumps(req) + "\n"); self.proc.stdin.flush()
            dump("SEND", method, json.dumps(req))
        r, _, _ = select.select([self.proc.stdout], [], [], timeout)
        if not r:
            raise RuntimeError(self.name + " no response" + (" (" + self.dead() + ")" if self.dead() else ""))
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError(self.name + " closed" + (" (" + self.dead() + ")" if self.dead() else ""))
        try:
            resp = json.loads(line)
        except ValueError:
            raise RuntimeError(self.name + " bad json: " + line[:200])
        if resp.get("result"):
            text = resp["result"].get("content", [{}])[0].get("text", "")
        else:
            text = "ERROR " + str(resp.get("error"))
        dump("RECV", method, text[:4000])
        return text

    def call(self, tool, **args):
        return self.rpc("tools/call", {"name": tool, "arguments": args})

    def kill(self):
        self.proc.kill(); self.proc.wait()


def picks_of(text):
    m = re.search(r"# options: (\d+)~(\d+)/(\d+)", text or "")
    if not m:
        return None
    lo, hi, total = map(int, m.groups())
    hi = min(hi, total)
    if lo == 0 and hi >= 1 and random.random() > 0.10:
        # min=0 means "empty = cancel" is valid, but a dumb aggro player
        # should only decline an optional effect ~10% of the time.
        lo = 1
    k = random.randint(lo, hi)
    return random.sample(range(total), k) if k > 0 else []


def status_of(text):
    m = re.search(r"T(\d+) (self|oppo) turn", text or "")
    if not m:
        return None
    turn = int(m.group(1))
    ls = re.search(r"self lp(\d+)", text or "")
    lo = re.search(r"oppo lp(\d+)", text or "")
    return (turn, int(ls.group(1)) if ls else 0, int(lo.group(1)) if lo else 0)


def play(m, key, results, t0):
    try:
        blk = m.call("ygo_connect", host=HOST, port=str(PORT), password=ROOM,
                     name=m.name, deck=DECK)
        last = None; max_turn = 0
        while time.time() - t0 < RUN:
            if m.proc.poll() is not None:
                raise RuntimeError(m.name + " exited " + m.dead())
            if "Game Over" in blk or "Match Over" in blk:
                results[key] = ("game over", max_turn, m.actions)
                return
            s = status_of(blk)
            if s:
                max_turn = max(max_turn, s[0])
                if s != last:
                    last = s
            p = picks_of(blk)
            if p is None:
                # Blocking model: every non-terminal block is a decision.
                raise RuntimeError(m.name + " no options in block: " + (blk or "")[:200])
            m.actions += 1
            blk = m.call("ygo_choose", id=p)
        results[key] = ("timeout", max_turn, m.actions)
    except Exception as e:
        results[key] = (str(e), 0, m.actions)


def main():
    a = MCP("A"); b = MCP("B")
    t0 = time.time(); results = {}
    ta = threading.Thread(target=play, args=(a, "A", results, t0))
    tb = threading.Thread(target=play, args=(b, "B", results, t0))
    ta.start(); tb.start()
    ta.join(RUN + 30); tb.join(RUN + 30)
    a.kill(); b.kill()
    TRANS.close()
    print("results:", results)
    ra = results.get("A"); rb = results.get("B")
    ok = (ra and rb and ra[0] in ("game over", "timeout")
          and rb[0] in ("game over", "timeout")
          and max(ra[1], rb[1]) >= 3 and (ra[2] + rb[2]) > 0)
    print("AGGRO PLAY VERIFIED" if ok else "AGGRO PLAY NOT VERIFIED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
