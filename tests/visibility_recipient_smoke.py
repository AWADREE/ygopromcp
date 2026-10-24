"""Check the MCP view of visibility-filtered network recipient packets.

Run against a built graphical client with --exe, --assets and --scratch-dir.
The only listener is bound to 127.0.0.1; no public room or game chat is used.
Packets model SingleDuel::Analyze's recipient output and are consumed by the
real DuelClient parser. This does not exercise the server's filtering itself.
"""

import argparse
from contextlib import closing
import json
import os
from pathlib import Path
import queue
import shutil
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import threading
import time

def make_runtime(path, assets):
    shutil.copytree(assets / "textures", path / "textures")
    shutil.copy2(assets / "strings.conf", path / "strings.conf")
    with closing(sqlite3.connect(path / "cards.cdb")) as db:
        db.execute("CREATE TABLE datas (id INTEGER, ot INTEGER, alias INTEGER, "
                   "setcode INTEGER, type INTEGER, atk INTEGER, def INTEGER, "
                   "level INTEGER, race INTEGER, attribute INTEGER, category INTEGER)")
        fields = ", ".join(f"str{i} TEXT" for i in range(1, 17))
        db.execute(f"CREATE TABLE texts (id INTEGER, name TEXT, desc TEXT, {fields})")


def packet(conn, kind, body=b""):
    payload = bytes([kind]) + body
    conn.sendall(struct.pack("<H", len(payload)) + payload)


def message(conn, kind, body=b""):
    packet(conn, 1, bytes([kind]) + body)


def state(proc, lines, request_id):
    req = {"jsonrpc": "2.0", "id": request_id, "method": "tools/call",
           "params": {"name": "ygo_card", "arguments": {"name": "RevealedCard"}}}
    proc.stdin.write((json.dumps(req) + "\n").encode())
    proc.stdin.flush()
    try:
        response = json.loads(lines.get(timeout=15))
    except queue.Empty as exc:
        raise AssertionError(f"No MCP reply; client exit={proc.poll()}") from exc
    assert "result" in response, response
    return response["result"]["content"][0]["text"]


def opponent_line(block, pile):
    opponent = block.split("oppo lp", 1)[1].split("self lp", 1)[0]
    return opponent.split(f"  {pile}: ", 1)[1].splitlines()[0]


def add_card(path):
    with closing(sqlite3.connect(path / "cards.cdb")) as db:
        db.execute("INSERT INTO datas VALUES (?,?,?,?,?,?,?,?,?,?,?)",
                   (123, 3, 0, 0, 1, 1000, 1000, 4, 1, 1, 0))
        fields = ",".join("?" for _ in range(19))
        db.execute(f"INSERT INTO texts VALUES ({fields})",
                   (123, "RevealedCard", "Local fixture", *("" for _ in range(16))))
        db.commit()


def drain(conn):
    try:
        while conn.recv(4096):
            pass
    except OSError:
        pass


def run(exe, assets, scratch_dir, dll_dir, seat, observer=False):
    name = f"{'observer' if observer else 'player'}-seat-{seat}"
    with tempfile.TemporaryDirectory(prefix=name + "-", dir=scratch_dir) as scratch:
        path = Path(scratch)
        make_runtime(path, assets)
        add_card(path)
        with closing(socket.socket(socket.AF_INET, socket.SOCK_STREAM)) as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            listener.settimeout(10)
            port = listener.getsockname()[1]
            env = os.environ.copy()
            if dll_dir:
                env["PATH"] = str(dll_dir) + os.pathsep + env.get("PATH", "")
            proc = subprocess.Popen([str(exe), "-h", "127.0.0.1", "-p", str(port), "-j"],
                                    cwd=path, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, bufsize=0, env=env)
            lines = queue.Queue()
            threading.Thread(target=lambda: [lines.put(line) for line in proc.stdout], daemon=True).start()
            try:
                try:
                    accepted = listener.accept()[0]
                except socket.timeout as exc:
                    raise AssertionError(
                        f"Graphical client did not connect to loopback; exit={proc.poll()}"
                    ) from exc
                with closing(accepted) as conn:
                    threading.Thread(target=drain, args=(conn,), daemon=True).start()
                    host_info = struct.pack("<IBBBBB3xiBBH", 0, 4, 0, 4, 1, 0,
                                            8000, 5, 1, 0)
                    assert len(host_info) == 20
                    packet(conn, 0x12, host_info)  # STOC_JOIN_GAME cancels join timeout
                    packet(conn, 0x13, bytes([2 if observer else seat]))
                    packet(conn, 0x15)  # STOC_DUEL_START
                    time.sleep(0.5)
                    playertype = seat | (0x10 if observer else 0)
                    message(conn, 4, struct.pack("<BBiiHHHH", playertype, 4, 8000, 8000,
                                                 4, 0, 4, 0))  # MSG_START
                    time.sleep(1.8)
                    raw_opponent = 1 - seat
                    raw_self = seat

                    def update(label, request_id, kind, body):
                        message(conn, kind, body)
                        time.sleep(2)
                        block = state(proc, lines, request_id)
                        hand = opponent_line(block, "hand")
                        print(f"{name} {label}: opponent hand = {hand}", flush=True)
                        return block

                    hidden = update("ordinary hidden draw", 1, 90,
                                    struct.pack("<BBII", raw_opponent, 2, 0, 0))
                    assert opponent_line(hidden, "hand") == "?*2"
                    # Negative control: ask again without a confirm packet.
                    # An ignored MSG_CONFIRM_CARDS must not be hidden by an
                    # earlier public draw of the same card name.
                    control = state(proc, lines, 2)
                    print(f"{name} before confirm: opponent hand = "
                          f"{opponent_line(control, 'hand')}", flush=True)
                    assert opponent_line(control, "hand") == "?*2"
                    revealed = update("public confirm", 3, 31,
                                      struct.pack("<BBBIBBB", raw_self, 0, 1, 123,
                                                  raw_opponent, 2, 0))
                    assert opponent_line(revealed, "hand") == "RevealedCard, ?"
                    public_draw = update("public draw", 4, 90,
                                         struct.pack("<BBI", raw_opponent, 1, 0x8000007B))
                    assert "RevealedCard*2" in opponent_line(public_draw, "hand")
                    shuffled = update("hand shuffle", 5, 33,
                                      struct.pack("<BBIII", raw_opponent, 3, 0, 0, 0))
                    assert opponent_line(shuffled, "hand") == "?*3"
                    if seat == 0 and not observer:
                        def move(code, src, dst, old_position, new_position):
                            return struct.pack("<IBBBBBBBBI", code,
                                               raw_opponent, src, 0, old_position,
                                               raw_opponent, dst, 0, new_position, 0)

                        def move_and_refresh(code, src, dst, old_position,
                                             new_position, visible_code):
                            # SingleDuel::Analyze sends RefreshSingle after
                            # cross-zone MSG_MOVE. The opponent's face-down
                            # QUERY_CODE is zero; UpdateInfo clears prior code.
                            message(conn, 50, move(code, src, dst,
                                                   old_position, new_position))
                            message(conn, 7, struct.pack("<BBBIIII", raw_opponent,
                                                         dst, 0, 16, 3,
                                                         visible_code,
                                                         new_position << 24))
                            time.sleep(2)

                        update("second public confirm", 6, 31,
                               struct.pack("<BBBIBBB", raw_self, 0, 1, 123,
                                           raw_opponent, 2, 0))
                        move_and_refresh(123, 2, 16, 8, 1, 123)
                        grave = state(proc, lines, 7)
                        print("public move: opponent grave =", opponent_line(grave, "grave"), flush=True)
                        assert "RevealedCard" in opponent_line(grave, "grave")
                        move_and_refresh(0, 16, 32, 1, 8, 0)
                        banished = state(proc, lines, 8)
                        print("face-down banish: opponent removed =",
                              opponent_line(banished, "remove"), flush=True)
                        assert "RevealedCard" not in opponent_line(banished, "remove")
                        move_and_refresh(123, 2, 16, 8, 1, 123)
                        move_and_refresh(0, 16, 1, 1, 8, 0)
                        refreshed_deck = state(proc, lines, 9)
                        print("move plus refresh: opponent deck =",
                              opponent_line(refreshed_deck, "deck"), flush=True)
                        assert opponent_line(refreshed_deck, "deck") == "?*2"
                        message(conn, 32, bytes([raw_opponent]))
                        time.sleep(2)
                        unknown_deck = state(proc, lines, 10)
                        print("deck shuffle: opponent deck =", opponent_line(unknown_deck, "deck"), flush=True)
                        assert "RevealedCard" not in opponent_line(unknown_deck, "deck")
                    print(f"PASS {name}", flush=True)
            finally:
                if proc.poll() is None:
                    # Process lifetime is outside this visibility test. End the
                    # local fixture without coupling it to GUI shutdown behavior.
                    proc.terminate()
                    try:
                        proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait(timeout=5)
                stderr = proc.stderr.read().decode(errors="replace")
                if stderr:
                    print(f"{name} stderr: {stderr}", file=sys.stderr, flush=True)
                proc.stdin.close()
                proc.stdout.close()
                proc.stderr.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--assets", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--scratch-dir", type=Path, required=True)
    parser.add_argument("--dll-dir", type=Path,
                        help="directory containing the built client's runtime DLLs")
    args = parser.parse_args()
    if not args.exe.is_file():
        parser.error("--exe must be a built graphical client")
    if not (args.assets / "textures").is_dir() or not (args.assets / "strings.conf").is_file():
        parser.error("--assets must contain textures/ and strings.conf")
    if not args.scratch_dir.is_dir():
        parser.error("--scratch-dir must already exist")
    if args.dll_dir and not args.dll_dir.is_dir():
        parser.error("--dll-dir must be a directory")
    for seat_number, is_observer in ((0, False), (1, False), (0, True)):
        run(args.exe.resolve(), args.assets.resolve(), args.scratch_dir.resolve(),
            args.dll_dir.resolve() if args.dll_dir else None,
            seat_number, is_observer)
