"""Offline Windows GUI/MCP stdio smoke test. Run after a release build.

Uses an empty local card database and the checkout's own assets. Never joins
a room or sends game chat. Requires an interactive Windows desktop session.
"""

import ctypes
from ctypes import wintypes
from contextlib import closing
import json
from pathlib import Path
import queue
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time


ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / "bin" / "release" / "YGOPro.exe"
USER32 = ctypes.WinDLL("user32", use_last_error=True) if sys.platform == "win32" else None


def make_runtime(path):
    shutil.copytree(ROOT / "textures", path / "textures")
    shutil.copy2(ROOT / "strings.conf", path / "strings.conf")
    with closing(sqlite3.connect(path / "cards.cdb")) as db:
        db.execute("CREATE TABLE datas (id INTEGER, ot INTEGER, alias INTEGER, setcode INTEGER, "
                   "type INTEGER, atk INTEGER, def INTEGER, level INTEGER, race INTEGER, "
                   "attribute INTEGER, category INTEGER)")
        fields = ", ".join(f"str{i} TEXT" for i in range(1, 17))
        db.execute(f"CREATE TABLE texts (id INTEGER, name TEXT, desc TEXT, {fields})")


def find_window(pid):
    found = []
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    @callback_type
    def visit(hwnd, _):
        owner = wintypes.DWORD()
        USER32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and USER32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True

    USER32.EnumWindows(visit, 0)
    return found[0] if found else None


def start_client(runtime):
    proc = subprocess.Popen([str(EXE)], cwd=runtime, stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
    lines = queue.Queue()

    def reader():
        for line in proc.stdout:
            lines.put(line)

    threading.Thread(target=reader, daemon=True).start()
    return proc, lines


def request(proc, lines, method, request_id, split=False):
    payload = (json.dumps({"jsonrpc": "2.0", "id": request_id,
                           "method": method, "params": {}})
               + ("\r\n" if split else "\n")).encode()
    if split:
        midpoint = len(payload) // 2
        proc.stdin.write(payload[:midpoint])
        proc.stdin.flush()
        time.sleep(0.1)
        assert lines.empty(), "Response before request newline"
        payload = payload[midpoint:]
    proc.stdin.write(payload)
    proc.stdin.flush()
    try:
        raw = lines.get(timeout=15)
    except queue.Empty as exc:
        raise AssertionError(f"No {method} response; process exit={proc.poll()}") from exc
    response = json.loads(raw)
    assert response["id"] == request_id, response
    assert "result" in response, response
    return response["result"]


def close_gui(proc):
    deadline = time.monotonic() + 10
    hwnd = None
    while time.monotonic() < deadline and proc.poll() is None:
        hwnd = find_window(proc.pid)
        if hwnd:
            break
        time.sleep(0.05)
    assert hwnd, f"No graphical window for PID {proc.pid}; process exit={proc.poll()}"
    assert USER32.PostMessageW(hwnd, 0x0010, 0, 0), "WM_CLOSE failed"
    proc.wait(timeout=10)
    assert proc.returncode == 0, f"GUI exit code {proc.returncode}"


def run_case(runtime, close_stdin):
    proc, lines = start_client(runtime)
    try:
        info = request(proc, lines, "initialize", 1)
        assert info["serverInfo"]["name"] == "ygopro-mcp", info
        tools = request(proc, lines, "tools/list", 2)["tools"]
        assert {"ygo_connect", "ygo_choose", "ygo_disconnect"}.issubset(
            {tool["name"] for tool in tools})
        assert request(proc, lines, "ping", 3, split=True) == {}
        if close_stdin:
            proc.stdin.close()
            time.sleep(0.2)  # Allow the reader to observe EOF before GUI shutdown.
            assert proc.poll() is None, "GUI quit on MCP EOF"
        close_gui(proc)
        assert lines.empty(), "Unexpected extra stdout content after MCP responses"
        return proc.stderr.read().decode(errors="replace")
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=5)
        proc.stdout.close()
        proc.stderr.close()
        if not proc.stdin.closed:
            proc.stdin.close()


def run_blocked_pipe_case(runtime, kind):
    # Deliberately leave stdout unread. The reader thread must not hold GUI
    # shutdown hostage when the JSON output pipe fills.
    proc = subprocess.Popen([str(EXE)], cwd=runtime, stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
    written = [0]
    try:
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and not find_window(proc.pid):
            time.sleep(0.05)
        assert find_window(proc.pid), "No graphical window for pressure case"
        if kind == "backpressure":
            payload = (json.dumps({"jsonrpc": "2.0", "id": 1,
                                   "method": "tools/list"}) + "\n").encode()

            def send_many():
                try:
                    for index in range(2000):
                        proc.stdin.write(payload.replace(b'"id": 1',
                                                         f'"id": {index}'.encode()))
                        proc.stdin.flush()
                        written[0] += 1
                except OSError:
                    pass

            sender = threading.Thread(target=send_many, daemon=True)
            sender.start()
            time.sleep(1)
            assert sender.is_alive() and 20 <= written[0] < 2000, (
                "Expected a full unread output pipe", written[0])
        else:
            # A literal trailing CR triggered UCRT text-mode lookahead in the
            # original build. No newline follows before window closure.
            proc.stdin.write(b'{"jsonrpc":"2.0","id":1,"method":"initialize"}\r')
            proc.stdin.flush()
        close_gui(proc)
        stderr = proc.stderr.read().decode(errors="replace")
        assert "MCP: " not in stderr, stderr
        print(f"PASS: {kind} shutdown; requests queued={written[0]}")
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=5)
        proc.stdin.close()
        proc.stdout.close()
        proc.stderr.close()


def run_gui_without_mcp(runtime):
    with open(runtime / "gui-no-mcp-stderr.log", "wb") as stderr:
        proc = subprocess.Popen([str(EXE)], cwd=runtime, stdin=subprocess.DEVNULL,
                                stdout=subprocess.DEVNULL, stderr=stderr)
        try:
            close_gui(proc)
            print("PASS: graphical client opens and closes with null stdio")
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait(timeout=5)


def main():
    assert sys.platform == "win32", "Windows desktop required"
    assert EXE.is_file(), f"Build the client first: {EXE}"
    with tempfile.TemporaryDirectory(prefix="mcp-stdio-", dir=ROOT / "build") as scratch:
        runtime = Path(scratch)
        make_runtime(runtime)
        for eof in (True, False):
            stderr = run_case(runtime, close_stdin=eof)
            print(f"PASS: initialize, tools/list, split ping, GUI shutdown; stdin EOF={eof}; "
                  f"stderr bytes={len(stderr.encode())}")
        for kind in ("backpressure", "trailing CR"):
            run_blocked_pipe_case(runtime, kind)
        run_gui_without_mcp(runtime)
        (runtime / "cards.cdb").rename(runtime / "cards.fixture.cdb")
        proc, lines = start_client(runtime)
        try:
            proc.wait(timeout=10)
            assert proc.returncode != 0, "Missing card database did not fail startup"
            assert lines.empty(), "Startup failure emitted nonprotocol stdout"
            print(f"PASS: failed GUI initialization exits with stdin open; code={proc.returncode}")
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait(timeout=5)
            proc.stdin.close()
            proc.stdout.close()
            proc.stderr.close()


if __name__ == "__main__":
    main()
