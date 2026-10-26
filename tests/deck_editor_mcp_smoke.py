"""Offline graphical deck-editor MCP regression test.

Run with --exe, --assets, --scratch-dir and optional --dll-dir. All decks and
database records are temporary; no room, chat or public listener is used.
"""

import argparse
from contextlib import closing
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import queue
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time


def make_runtime(path, assets):
    shutil.copytree(assets / "textures", path / "textures")
    shutil.copy2(assets / "strings.conf", path / "strings.conf")
    (path / "deck").mkdir()
    (path / "deck" / "alpha.ydk").write_text(
        "#main\n100\n#extra\n!side\n", encoding="ascii")
    (path / "deck" / "beta.ydk").write_text(
        "#main\n102\n#extra\n!side\n", encoding="ascii")
    (path / "deck" / "invalid.ydk").write_text(
        "#main\n999\n#extra\n!side\n", encoding="ascii")
    (path / "deck" / "custom").mkdir()
    (path / "deck" / "custom" / "gamma.ydk").write_text(
        "#main\n102\n#extra\n!side\n", encoding="ascii")
    (path / "deck" / "unrelated.txt").write_text("keep me", encoding="ascii")
    (path / "lflist.conf").write_text("!Fixture\n100 2\n", encoding="ascii")
    (path / "system.conf").write_text(
        "use_lflist = 1\ndefault_lflist = 0\n", encoding="ascii")
    with closing(sqlite3.connect(path / "cards.cdb")) as db:
        db.execute("CREATE TABLE datas (id INTEGER, ot INTEGER, alias INTEGER, "
                   "setcode INTEGER, type INTEGER, atk INTEGER, def INTEGER, "
                   "level INTEGER, race INTEGER, attribute INTEGER, category INTEGER)")
        fields = ", ".join(f"str{i} TEXT" for i in range(1, 17))
        db.execute(f"CREATE TABLE texts (id INTEGER, name TEXT, desc TEXT, {fields})")

        def card(code, name, type_, alias=0):
            db.execute("INSERT INTO datas VALUES (?,?,?,?,?,?,?,?,?,?,?)",
                       (code, 3, alias, 0, type_, 1000, 1000, 4, 1, 1, 0))
            db.execute("INSERT INTO texts VALUES (" + ",".join("?" for _ in range(19)) + ")",
                       (code, name, "Local fixture", *("" for _ in range(16))))

        card(100, "MainCard", 1)
        card(101, "AliasCard", 1, alias=100)
        card(102, "OtherCard", 1)
        card(103, "UnsavedCard", 1)
        card(300, "TokenCard", 0x4001)
        for code in range(200, 206):
            card(code, f"FusionCard{code}", 0x41)
        db.commit()


def start_client(exe, runtime, dll_dir):
    env = os.environ.copy()
    if dll_dir:
        env["PATH"] = str(dll_dir) + os.pathsep + env.get("PATH", "")
    proc = subprocess.Popen([str(exe)], cwd=runtime, stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            bufsize=0, env=env)
    lines = queue.Queue()

    def read_stdout():
        for line in proc.stdout:
            lines.put(line)

    threading.Thread(target=read_stdout, daemon=True).start()
    return proc, lines


def remove_first_main_with_gui(proc):
    """Right-click the first visible main-deck card in the real Windows GUI."""
    if sys.platform != "win32":
        return False
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    windows = []
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    @callback_type
    def collect(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == proc.pid and user32.IsWindowVisible(hwnd):
            windows.append(hwnd)
        return True

    user32.EnumWindows(collect, 0)
    assert windows, "No visible graphical client window"
    rect = wintypes.RECT()
    assert user32.GetClientRect(windows[0], ctypes.byref(rect))
    x = round(330 * (rect.right - rect.left) / 1024)
    y = round(190 * (rect.bottom - rect.top) / 640)
    position = (y << 16) | x
    assert user32.PostMessageW(windows[0], 0x0200, 0, position)  # WM_MOUSEMOVE
    time.sleep(0.1)
    assert user32.PostMessageW(windows[0], 0x0205, 0, position)  # WM_RBUTTONUP
    time.sleep(0.3)
    return True


def request(proc, lines, name, args, request_id):
    payload = {"jsonrpc": "2.0", "id": request_id, "method": "tools/call",
               "params": {"name": name, "arguments": args}}
    proc.stdin.write((json.dumps(payload) + "\n").encode())
    proc.stdin.flush()
    try:
        reply = json.loads(lines.get(timeout=15))
    except queue.Empty as exc:
        raise AssertionError(f"No {name} response; exit={proc.poll()}") from exc
    assert reply["id"] == request_id and "result" in reply, reply
    return reply["result"]["content"][0]["text"]


def run(exe, assets, scratch_dir, dll_dir):
    with tempfile.TemporaryDirectory(prefix="deck-editor-", dir=scratch_dir) as scratch:
        runtime = Path(scratch)
        make_runtime(runtime, assets)
        alpha = runtime / "deck" / "alpha.ydk"
        beta = runtime / "deck" / "beta.ydk"
        gamma = runtime / "deck" / "custom" / "gamma.ydk"
        unrelated = runtime / "deck" / "unrelated.txt"
        beta_original = beta.read_bytes()
        alpha_original = alpha.read_bytes()
        gamma_original = gamma.read_bytes()
        proc, lines = start_client(exe, runtime, dll_dir)
        counter = 0

        def call(name, args=None):
            nonlocal counter
            counter += 1
            return request(proc, lines, name, args or {}, counter)

        def rejected(name, args, reason):
            result = call(name, args)
            assert result.startswith("Error:") and reason in result, result
            print(f"PASS rejected {name}: {reason}", flush=True)

        try:
            proc.stdin.write((json.dumps({"jsonrpc": "2.0", "id": "init",
                "method": "initialize", "params": {"protocolVersion": "2024-11-05",
                "capabilities": {}, "clientInfo": {"name": "deck-smoke", "version": "1"}}}) + "\n").encode())
            proc.stdin.flush()
            initialized = json.loads(lines.get(timeout=15))
            assert initialized["id"] == "init"
            assert "ygo_guide" in initialized["result"]["instructions"]
            proc.stdin.write((json.dumps({"jsonrpc": "2.0", "id": "list",
                "method": "tools/list", "params": {}}) + "\n").encode())
            proc.stdin.flush()
            tool_list = json.loads(lines.get(timeout=15))
            names = {item["name"] for item in tool_list["result"]["tools"]}
            assert {"ygo_guide", "ygo_deck_create", "ygo_deck_save",
                    "ygo_deck_close"}.issubset(names)
            assert "cannot launch" in call("ygo_guide", {"topic": "limits"})
            rejected("ygo_guide", {"topic": 17}, "topic")
            print("PASS bundled initialize guidance, guide topics and tool discovery", flush=True)

            listed = call("ygo_deck_list")
            local = listed.split("local:", 1)[1].splitlines()[0]
            assert {"alpha", "beta", "invalid"}.issubset(set(local.split())), listed
            assert "custom: gamma" in listed, listed
            assert "# deck editor" in listed, listed
            print("PASS graphical editor entered and local decks listed", flush=True)

            created = call("ygo_deck_create", {"name": "agent"})
            agent = runtime / "deck" / "agent.ydk"
            assert "deck: agent" in created and "main (0)" in created and agent.is_file(), created
            empty_bytes = agent.read_bytes()
            rejected("ygo_deck_create", {"name": "agent"}, "already exists")
            rejected("ygo_deck_create", {"name": "../escape"}, "single deck")
            rejected("ygo_deck_create", {"name": "custom/escape"}, "single deck")
            rejected("ygo_deck_create", {"name": "alpha"}, "already exists")
            assert agent.read_bytes() == empty_bytes and alpha.read_bytes() == alpha_original
            changed = call("ygo_deck_add", {"code": 102, "section": "main"})
            assert "modified: yes" in changed and agent.read_bytes() == empty_bytes
            rejected("ygo_deck_close", {}, "unsaved")
            rejected("ygo_deck_create", {"name": "another"}, "unsaved")
            assert not (runtime / "deck" / "another.ydk").exists()
            saved = call("ygo_deck_save")
            assert "modified: no" in saved and b"102" in agent.read_bytes()
            assert "deck: agent" in call("ygo_deck_open", {"deck": "agent"})
            assert "main menu is ready" in call("ygo_deck_close")
            print("PASS MCP-only create, edit, save, close and no-overwrite bounds", flush=True)

            state = call("ygo_deck_open", {"category": "custom", "deck": "gamma"})
            assert "category: custom" in state and "deck: gamma" in state, state
            state = call("ygo_deck_add", {"code": 103, "section": "main"})
            assert "modified: yes" in state, state
            state = call("ygo_deck_save")
            assert "modified: no" in state and gamma.read_bytes() != gamma_original, state
            assert alpha.read_bytes() == alpha_original and beta.read_bytes() == beta_original
            print("PASS custom category opens and saves only its selected local file", flush=True)

            state = call("ygo_deck_open", {"deck": "alpha"})
            assert "deck: alpha" in state and "100 MainCard" in state, state
            assert "modified: no" in state
            rejected("ygo_deck_open", {"deck": "invalid"}, "cannot load")
            assert "deck: alpha" in call("ygo_deck_list"), "Failed load changed GUI selection"
            assert "modified: no" in call("ygo_deck_save")
            assert alpha.read_bytes() == alpha_original, "Clean save rewrote the file"
            search = call("ygo_deck_search", {"query": "MainCard"})
            assert "100 MainCard" in search, search
            rejected("ygo_deck_search", {"query": 17}, "query")
            rejected("ygo_deck_open", {"deck": "../beta"}, "listed local")
            rejected("ygo_deck_open", {"deck": "beta", "category": "bot"}, "category")

            rejected("ygo_deck_add", {"code": "100", "section": "main"}, "card code")
            rejected("ygo_deck_add", {"code": 100, "section": "main", "count": -1}, "count")
            rejected("ygo_deck_add", {"code": 999, "section": "main"}, "unknown")
            rejected("ygo_deck_add", {"code": 300, "section": "main"}, "token")
            rejected("ygo_deck_add", {"code": 200, "section": "main"}, "section")
            rejected("ygo_deck_add", {"code": 102, "section": "extra"}, "section")
            rejected("ygo_deck_add", {"code": 100, "section": "main", "count": 2}, "limit")
            assert alpha.read_bytes() == alpha_original, "Rejected edits changed the file"

            state = call("ygo_deck_add", {"code": 100, "section": "main"})
            assert state.count("100 MainCard") == 2 and "modified: yes" in state, state
            rejected("ygo_deck_add", {"code": 101, "section": "side"}, "limit")
            rejected("ygo_deck_remove", {"code": 100, "section": "main", "count": 3}, "few copies")
            rejected("ygo_deck_open", {"deck": "beta"}, "unsaved")
            assert "deck: alpha" in call("ygo_deck_list")
            assert alpha.read_bytes() == alpha_original, "Unsaved GUI edit touched the file"
            state = call("ygo_deck_remove", {"code": 100, "section": "main"})
            assert state.count("100 MainCard") == 1, state
            state = call("ygo_deck_add", {"code": 101, "section": "side"})
            assert "101 AliasCard" in state, state

            for code in range(200, 205):
                state = call("ygo_deck_add", {"code": code, "section": "extra", "count": 3})
            assert "extra (15)" in state, state
            rejected("ygo_deck_add", {"code": 205, "section": "extra"}, "full")
            state = call("ygo_deck_remove", {"code": 200, "section": "extra", "count": 3})
            assert "extra (12)" in state, state
            state = call("ygo_deck_add", {"code": 205, "section": "extra"})
            assert "extra (13)" in state, state

            saved = call("ygo_deck_save")
            assert "modified: no" in saved, saved
            assert alpha.read_bytes() != alpha_original
            assert alpha.read_text(encoding="ascii").count("101") == 1
            assert beta.read_bytes() == beta_original
            assert unrelated.read_text(encoding="ascii") == "keep me"
            assert not list((runtime / "deck").glob("*.mcp-*")), "Temporary save file remains"
            print("PASS selected-file save, alias limit, section capacity and unrelated-file preservation", flush=True)

            opened = call("ygo_deck_open", {"deck": "beta"})
            assert "deck: beta" in opened and "102 OtherCard" in opened, opened
            opened = call("ygo_deck_open", {"deck": "alpha"})
            assert "101 AliasCard" in opened and "205 FusionCard205" in opened, opened
            if remove_first_main_with_gui(proc):
                state = call("ygo_deck_list")
                assert "main (0)" in state and "modified: yes" in state, state
                rejected("ygo_deck_open", {"deck": "beta"}, "unsaved")
                saved = call("ygo_deck_save")
                assert "main (0)" in saved and "modified: no" in saved, saved
                print("PASS manual GUI removal shares MCP dirty state and selected-file save", flush=True)
            state = call("ygo_deck_add", {"code": 103, "section": "main"})
            assert "modified: yes" in state, state
            alpha.unlink()  # fixture file only: a vanished selected file must not be recreated
            rejected("ygo_deck_save", {}, "ordinary local file")
            rejected("ygo_deck_open", {"deck": "beta"}, "unsaved")
            assert not alpha.exists() and beta.read_bytes() == beta_original
            print("PASS missing selected file leaves live edits dirty and other files intact", flush=True)
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=5)
            stderr = proc.stderr.read().decode(errors="replace")
            if stderr:
                print(stderr, file=sys.stderr)
            proc.stdin.close()
            proc.stdout.close()
            proc.stderr.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--assets", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--scratch-dir", type=Path, required=True)
    parser.add_argument("--dll-dir", type=Path)
    args = parser.parse_args()
    if not args.exe.is_file() or not (args.assets / "textures").is_dir() \
            or not (args.assets / "strings.conf").is_file() or not args.scratch_dir.is_dir():
        parser.error("provide an executable, assets with textures/ and strings.conf, and an existing scratch directory")
    if args.dll_dir and not args.dll_dir.is_dir():
        parser.error("--dll-dir must be an existing directory")
    run(args.exe.resolve(), args.assets.resolve(), args.scratch_dir.resolve(),
        args.dll_dir.resolve() if args.dll_dir else None)
