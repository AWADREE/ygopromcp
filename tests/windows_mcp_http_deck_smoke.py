"""Exercise the ordinary GUI's localhost MCP and deck tools in a throwaway runtime.

No stdio pipes, public room, user deck, or saved MCP URL is involved.
"""

import argparse
import ctypes
from ctypes import wintypes
import http.client
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
from urllib.parse import urlsplit

from deck_editor_mcp_smoke import make_runtime


user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
user32.GetClipboardData.restype = ctypes.c_void_p
kernel32.GlobalLock.argtypes = [ctypes.c_void_p]
kernel32.GlobalUnlock.argtypes = [ctypes.c_void_p]
CF_UNICODETEXT = 13


def game_window(pid):
    found = []
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    @callback_type
    def collect(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True

    user32.EnumWindows(collect, 0)
    return found[0] if found else None


def click(hwnd, x, y):
    rect = wintypes.RECT()
    assert user32.GetClientRect(hwnd, ctypes.byref(rect))
    x = round(x * rect.right / 1024)
    y = round(y * rect.bottom / 640)
    point = (y << 16) | x
    assert user32.PostMessageW(hwnd, 0x0200, 0, point)
    assert user32.PostMessageW(hwnd, 0x0201, 1, point)
    assert user32.PostMessageW(hwnd, 0x0202, 0, point)
    time.sleep(0.25)


def clipboard_text():
    if not user32.OpenClipboard(None):
        return ""
    try:
        handle = user32.GetClipboardData(CF_UNICODETEXT)
        if not handle:
            return ""
        kernel32.GlobalLock.restype = ctypes.c_void_p
        pointer = kernel32.GlobalLock(handle)
        try:
            return ctypes.wstring_at(pointer) if pointer else ""
        finally:
            if pointer:
                kernel32.GlobalUnlock(handle)
    finally:
        user32.CloseClipboard()


def call(url, session, method, params, request_id):
    parts = urlsplit(url)
    body = json.dumps({"jsonrpc": "2.0", "id": request_id,
                       "method": method, "params": params})
    headers = {"Host": parts.netloc, "Content-Type": "application/json",
               "Accept": "application/json, text/event-stream"}
    if session:
        headers.update({"MCP-Protocol-Version": "2025-11-25",
                        "Mcp-Session-Id": session})
    connection = http.client.HTTPConnection(parts.hostname, parts.port, timeout=10)
    try:
        connection.request("POST", parts.path, body=body, headers=headers)
        response = connection.getresponse()
        data = response.read()
        assert response.status == 200, (method, response.status)
        result = json.loads(data)
        assert result["id"] == request_id and "result" in result, (method, result)
        return result["result"], response.getheader("Mcp-Session-Id")
    finally:
        connection.close()


def run(exe, assets, scratch, dll_dir):
    with tempfile.TemporaryDirectory(prefix="http-deck-", dir=scratch) as folder:
        runtime = Path(folder)
        make_runtime(runtime, assets)
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        (runtime / "system.conf").write_text(f"mcp_port = {port}\n", encoding="ascii")
        environment = os.environ.copy()
        environment["PATH"] = str(dll_dir) + os.pathsep + environment.get("PATH", "")
        proc = subprocess.Popen([str(exe)], cwd=runtime, stdin=subprocess.DEVNULL,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                env=environment)
        try:
            hwnd = None
            for _ in range(100):
                hwnd = game_window(proc.pid)
                if hwnd:
                    break
                assert proc.poll() is None, "GUI exited before opening"
                time.sleep(0.1)
            assert hwnd, "GUI did not open"
            time.sleep(0.8)
            click(hwnd, 510, 385)  # Classic home: Local MCP
            click(hwnd, 575, 248)  # Enable MCP
            click(hwnd, 420, 390)  # Copy connection URL
            endpoint = clipboard_text()
            parts = urlsplit(endpoint)
            assert parts.hostname == "127.0.0.1" and parts.port == port
            assert parts.path.startswith("/mcp/")

            result, session = call(endpoint, None, "initialize", {
                "protocolVersion": "2025-11-25", "capabilities": {},
                "clientInfo": {"name": "http-deck-smoke", "version": "1"}}, 1)
            assert "ygo_guide" in result["instructions"] and session
            result, _ = call(endpoint, session, "tools/list", {}, 2)
            names = {tool["name"] for tool in result["tools"]}
            assert {"ygo_guide", "ygo_deck_create", "ygo_deck_save"}.issubset(names)

            def tool(name, arguments, request_id):
                output, _ = call(endpoint, session, "tools/call",
                                 {"name": name, "arguments": arguments}, request_id)
                return output["content"][0]["text"]

            assert "cannot launch" in tool("ygo_guide", {"topic": "limits"}, 3)
            click(hwnd, 670, 390)  # Back to home; listener stays enabled.
            created = tool("ygo_deck_create", {"name": "http-agent"}, 4)
            assert "deck: http-agent" in created and "main (0)" in created, created
            deck = runtime / "deck" / "http-agent.ydk"
            assert deck.is_file()
            assert "already exists" in tool("ygo_deck_create", {"name": "http-agent"}, 5)
            assert "modified: yes" in tool("ygo_deck_add", {
                "code": 102, "section": "main"}, 6)
            assert "modified: no" in tool("ygo_deck_save", {}, 7)
            assert b"102" in deck.read_bytes()
            click(hwnd, 250, 42)  # Leave the saved graphical deck editor.
            click(hwnd, 510, 385)  # Reopen Local MCP from home.
            click(hwnd, 575, 248)  # Disable MCP
            with socket.socket() as check:
                assert check.connect_ex(("127.0.0.1", port)) != 0
            print("PASS normal GUI HTTP initialize, guidance, create/save and disable")
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=5)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--scratch-dir", type=Path, required=True)
    parser.add_argument("--dll-dir", type=Path, required=True)
    options = parser.parse_args()
    run(options.exe.resolve(), options.assets.resolve(),
        options.scratch_dir.resolve(), options.dll_dir.resolve())
