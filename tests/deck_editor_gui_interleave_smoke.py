"""Offline GUI mouse/MCP interleave regression for the deck editor.

Runs a real Windows client with temporary local decks. No room is joined.
"""

import argparse
import ctypes
from ctypes import wintypes
from pathlib import Path
import sys
import tempfile
import time

from deck_editor_mcp_smoke import make_runtime, request, start_client


def window_for(proc):
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
    return user32, windows[0], rect


def mouse(user32, hwnd, rect, event, x, y, buttons=0):
    x = round(x * (rect.right - rect.left) / 1024)
    y = round(y * (rect.bottom - rect.top) / 640)
    assert user32.PostMessageW(hwnd, event, buttons, (y << 16) | x)
    time.sleep(0.2)


def run(exe, assets, scratch_dir, dll_dir):
    with tempfile.TemporaryDirectory(prefix="deck-interleave-", dir=scratch_dir) as scratch:
        runtime = Path(scratch)
        make_runtime(runtime, assets)
        alpha = runtime / "deck" / "alpha.ydk"
        beta = runtime / "deck" / "beta.ydk"
        beta_original = beta.read_bytes()
        proc, lines = start_client(exe, runtime, dll_dir)
        counter = 0

        def call(name, args=None):
            nonlocal counter
            counter += 1
            return request(proc, lines, name, args or {}, counter)

        def blocked(name, args=None):
            result = call(name, args)
            assert result == "Error: finish the current card drag first", result

        try:
            assert "deck: alpha" in call("ygo_deck_open", {"deck": "alpha"})
            user32, hwnd, rect = window_for(proc)

            # A stale hover at index 1 used to survive opening a one-card deck.
            assert "main (2)" in call("ygo_deck_add", {"code": 100, "section": "main"})
            assert "modified: no" in call("ygo_deck_save")
            mouse(user32, hwnd, rect, 0x0200, 380, 190)
            assert "main (1)" in call("ygo_deck_open", {"deck": "beta"})
            mouse(user32, hwnd, rect, 0x0205, 380, 190)
            state = call("ygo_deck_list")
            assert "deck: beta" in state and "main (1)" in state, state
            assert beta.read_bytes() == beta_original
            print("PASS opening shorter deck invalidates stationary GUI hover", flush=True)

            assert "main (2)" in call("ygo_deck_open", {"deck": "alpha"})
            alpha_saved = alpha.read_bytes()
            mouse(user32, hwnd, rect, 0x0200, 330, 190)
            mouse(user32, hwnd, rect, 0x0201, 330, 190, 0x0001)
            blocked("ygo_deck_save")  # mouse-down before the first move
            blocked("ygo_deck_open", {"deck": "beta"})
            mouse(user32, hwnd, rect, 0x0200, 500, 190, 0x0001)
            for name, args in (
                ("ygo_deck_save", {}),
                ("ygo_deck_add", {"code": 101, "section": "side"}),
                ("ygo_deck_remove", {"code": 100, "section": "main"}),
                ("ygo_deck_open", {"deck": "beta"}),
                ("ygo_deck_search", {"query": "OtherCard"}),
                ("ygo_deck_list", {}),
            ):
                blocked(name, args)
            assert alpha.read_bytes() == alpha_saved and beta.read_bytes() == beta_original
            mouse(user32, hwnd, rect, 0x0202, 500, 190)
            state = call("ygo_deck_list")
            assert "deck: alpha" in state and "main (2)" in state, state
            assert "modified: no" in call("ygo_deck_save")
            print("PASS active deck drag blocks all MCP deck calls and safe retry", flush=True)

            assert "102 OtherCard" in call("ygo_deck_search", {"query": "OtherCard"})
            mouse(user32, hwnd, rect, 0x0200, 820, 190)
            mouse(user32, hwnd, rect, 0x0201, 820, 190, 0x0001)
            mouse(user32, hwnd, rect, 0x0200, 330, 190, 0x0001)
            blocked("ygo_deck_open", {"deck": "beta"})
            assert alpha.read_bytes() == alpha_saved
            mouse(user32, hwnd, rect, 0x0202, 330, 190)
            state = call("ygo_deck_list")
            assert "deck: alpha" in state and "102 OtherCard" in state
            assert "modified: yes" in state, state
            assert "modified: no" in call("ygo_deck_save")
            assert "deck: beta" in call("ygo_deck_open", {"deck": "beta"})
            print("PASS search-result drag blocks open; drop, save and retry work", flush=True)
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except Exception:
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
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--scratch-dir", type=Path, required=True)
    parser.add_argument("--dll-dir", type=Path)
    args = parser.parse_args()
    if sys.platform != "win32":
        parser.error("Windows GUI message test only")
    run(args.exe.resolve(), args.assets.resolve(), args.scratch_dir.resolve(),
        args.dll_dir.resolve() if args.dll_dir else None)
