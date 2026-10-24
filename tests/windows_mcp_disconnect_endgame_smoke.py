"""Offline GUI regression: a replay-save prompt must not block MCP disconnect.

Requires a matching local cards.cdb, script/ tree and normal40.ydk. The test
copies those assets to an isolated runtime, hosts on 127.0.0.1 and reaches a
real engine win at 100 starting LP before disconnecting the replay recipient.
It never joins public rooms or sends chat.
"""

import argparse
import ctypes
from ctypes import wintypes
import os
from pathlib import Path
import queue
import re
import shutil
import socket
import sys
import tempfile
import threading
import time

from windows_loopback_host_smoke import bound_addresses, click, window_for
from windows_mcp_disconnect_smoke import Client, any_listeners


USER32 = ctypes.WinDLL("user32", use_last_error=True) if sys.platform == "win32" else None
KERNEL32 = ctypes.WinDLL("kernel32", use_last_error=True) if sys.platform == "win32" else None
if KERNEL32:
    KERNEL32.OpenThread.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    KERNEL32.OpenThread.restype = wintypes.HANDLE
    KERNEL32.SuspendThread.argtypes = [wintypes.HANDLE]
    KERNEL32.SuspendThread.restype = wintypes.DWORD
    KERNEL32.ResumeThread.argtypes = [wintypes.HANDLE]
    KERNEL32.ResumeThread.restype = wintypes.DWORD
    KERNEL32.CloseHandle.argtypes = [wintypes.HANDLE]
    KERNEL32.CloseHandle.restype = wintypes.BOOL


def key(hwnd, vk, char=None):
    assert USER32.PostMessageW(hwnd, 0x0100, vk, 0)
    if char is not None:
        assert USER32.PostMessageW(hwnd, 0x0102, ord(char), 0)
    assert USER32.PostMessageW(hwnd, 0x0101, vk, 0)


def set_start_lp(hwnd):
    click(hwnd, 531, 347)
    for _ in range(12):
        key(hwnd, 0x08, "\b")
    for char in "100":
        key(hwnd, ord(char), char)


def choose(block, role):
    match = re.search(r"# options: (\d+)~(\d+)/(\d+)", block)
    if not match:
        return None
    section = block[match.end():].split("\n# ", 1)[0]
    choices = [(int(i), label.lower()) for i, label in
               re.findall(r"(?m)^(\d+)\.\s+([^\r\n]+)", section)]
    assert len(choices) == int(match.group(3)), choices
    if not choices:
        return []
    required = int(match.group(1))
    if required > 1:
        return [i for i, _ in choices[:required]]
    if any("rock" in label for _, label in choices):
        return [0 if role == "Host" else 1]  # Guest wins RPS
    if any("go first" in label for _, label in choices):
        assert role == "Guest"
        return [next(i for i, label in choices if "go first" in label)]
    if role == "Guest":
        for i, label in choices:
            if label == "pass":
                return [i]
        for i, label in choices:
            if "to main phase 2" in label:
                return [i]
        return [choices[0][0]]
    for i, label in choices:
        if "@hand" in label and label.endswith(" summon"):
            return [i]
    for i, label in choices:
        if "self mzone" in label:
            return [i]
    for i, label in choices:
        if "to battle phase" in label:
            return [i]
    for i, label in choices:
        if label.endswith(" attack"):
            return [i]
    for i, label in choices:
        if label == "pass":
            return [i]
    return [choices[0][0]]


def drive(client, role, port, out, deadline):
    state = {"actions": 0, "saw_lp100": False, "game_over": False}
    try:
        block = client.tool("ygo_connect", {
            "host": "127.0.0.1", "port": str(port), "password": "",
            "name": "Guest", "deck": "deck/normal40.ydk"}) if role == "Guest" else ""
        while time.monotonic() < deadline:
            if not block:
                block = client.tool("ygo_card", {"code": 32864}, timeout=12)
            state["saw_lp100"] |= "lp100" in block
            if "Game Over" in block:
                state["game_over"] = " wins" in block or " wins" in client.tool(
                    "ygo_card", {"code": 32864}, timeout=12)
                state["last_call_returned"] = True
                out[role] = state
                return
            selected = choose(block, role)
            if selected is None:
                block = ""
                time.sleep(.1)
                continue
            state["actions"] += 1
            block = client.tool("ygo_choose", {"id": selected}, timeout=45)
        raise AssertionError("No real engine win before fixture deadline")
    except Exception as exc:
        out[role] = {**state, "error": repr(exc)}


def run(exe, assets, scratch_dir, dll_dir, ack_replay, hold_gui, trace_file):
    assert (assets / "cards.cdb").is_file()
    assert (assets / "deck/normal40.ydk").is_file()
    assert (assets / "script").is_dir()
    with tempfile.TemporaryDirectory(prefix="mcp-replay-disconnect-", dir=scratch_dir) as scratch:
        runtime = Path(scratch).resolve()
        assert runtime.is_relative_to(scratch_dir.resolve())
        shutil.copytree(assets, runtime, dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns("pics", "replay", "*.log"))
        (runtime / "replay").mkdir(exist_ok=True)
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        (runtime / "system.conf").write_text(
            f"serverport = {port}\nlocal_host_only = 1\nauto_save_replay = 0\n",
            encoding="ascii")
        clients = []
        barrier = {}
        failure = None
        try:
            host = Client("Host", exe, runtime, dll_dir, capture_stderr=True, debug=True)
            clients.append(host)
            host.rpc("initialize", {"protocolVersion": "2024-11-05"})
            limit = time.monotonic() + 12
            hwnd = None
            while time.monotonic() < limit and not hwnd:
                hwnd = window_for(host.proc.pid)
                time.sleep(.2)
            assert hwnd, "Host GUI missing"
            click(hwnd, 500, 245)
            click(hwnd, 680, 137)
            set_start_lp(hwnd)
            click(hwnd, 635, 467)
            limit = time.monotonic() + 8
            while time.monotonic() < limit and not bound_addresses(port, host.proc.pid):
                time.sleep(.2)
            assert bound_addresses(port, host.proc.pid) == [f"127.0.0.1:{port}"]
            guest = Client("Guest", exe, runtime, dll_dir)
            clients.append(guest)
            guest.rpc("initialize", {"protocolVersion": "2024-11-05"})
            assert window_for(guest.proc.pid)
            out = {}
            deadline = time.monotonic() + 95
            threads = [threading.Thread(target=drive, args=(host, "Host", port, out, deadline)),
                       threading.Thread(target=drive, args=(guest, "Guest", port, out, deadline))]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join(timeout=115)
            assert all(not thread.is_alive() for thread in threads), "MCP call stayed blocked"
            assert set(out) == {"Host", "Guest"}, out
            assert all(v.get("saw_lp100") and v.get("game_over") and
                       v.get("last_call_returned") and "error" not in v
                       for v in out.values()), out
            # The host is the replay recipient. With auto-save disabled,
            # receiving this packet exposes the save dialog and parks the
            # network callback before the following STOC_DUEL_END is handled.
            replay_packet = False
            replay_wait = False
            until = time.monotonic() + 12
            while time.monotonic() < until:
                try:
                    line = host.stderr_lines.get(timeout=min(.5, max(0, until - time.monotonic())))
                except queue.Empty:
                    continue
                if "[stoc] type=23 " in line:
                    replay_packet = True
                if "[stoc] replay-save-wait" in line:
                    replay_wait = True
                    break
            assert replay_packet, "host never received STOC_REPLAY"
            assert replay_wait, "host never entered the replay-save wait"
            if ack_replay:
                if hold_gui:
                    gui_thread = USER32.GetWindowThreadProcessId(hwnd, None)
                    assert gui_thread, "host GUI thread missing"
                    def pause_at_duel_end():
                        until = time.monotonic() + 12
                        while time.monotonic() < until:
                            try:
                                line = host.stderr_lines.get(timeout=min(.5, max(0, until - time.monotonic())))
                            except queue.Empty:
                                continue
                            if "[stoc] type=22 " in line:
                                barrier["packet"] = True
                            if "[stoc] duel-end-wait" in line:
                                handle = KERNEL32.OpenThread(0x0002, False, gui_thread)
                                assert handle, f"OpenThread failed: {ctypes.get_last_error()}"
                                previous = KERNEL32.SuspendThread(handle)
                                if previous == 0xFFFFFFFF:
                                    KERNEL32.CloseHandle(handle)
                                    raise AssertionError(f"SuspendThread failed: {ctypes.get_last_error()}")
                                barrier["handle"] = handle
                                return
                        raise AssertionError("duel-end wait never reached barrier")
                    def capture_barrier_error():
                        try:
                            pause_at_duel_end()
                        except BaseException as exc:
                            barrier["error"] = exc
                    watcher = threading.Thread(target=capture_barrier_error)
                    watcher.start()
                time.sleep(.35)  # PopupElement animates the window for 10 frames
                click(hwnd, 715, 292)  # replay dialog: No
                duel_end_packet = False
                duel_end_wait = False
                if hold_gui:
                    watcher.join(timeout=13)
                    assert not watcher.is_alive(), "duel-end barrier watcher hung"
                    if "error" in barrier:
                        raise barrier["error"]
                    assert barrier.get("packet") and barrier.get("handle"), barrier
                    duel_end_packet = duel_end_wait = True
                else:
                    until = time.monotonic() + 12
                    while time.monotonic() < until:
                        try:
                            line = host.stderr_lines.get(timeout=min(.5, max(0, until - time.monotonic())))
                        except queue.Empty:
                            continue
                        if "[stoc] type=22 " in line:
                            duel_end_packet = True
                        if "[stoc] duel-end-wait" in line:
                            duel_end_wait = True
                            break
                assert duel_end_packet and duel_end_wait, (
                    "host did not reach duel-end acknowledgement", host.stderr_tail[-12:])
            if hold_gui:
                disconnect = {}
                def request_disconnect():
                    try:
                        disconnect["result"] = host.tool("ygo_disconnect", timeout=23)
                    except BaseException as exc:
                        disconnect["error"] = exc
                caller = threading.Thread(target=request_disconnect)
                try:
                    caller.start()
                    until = time.monotonic() + 5
                    queued = False
                    while time.monotonic() < until:
                        try:
                            line = host.stderr_lines.get(timeout=min(.5, max(0, until - time.monotonic())))
                        except queue.Empty:
                            continue
                        if "[mcp] reader: queued request" in line:
                            queued = True
                            break
                    assert queued, "disconnect was not queued while GUI was suspended"
                finally:
                    assert KERNEL32.ResumeThread(barrier["handle"]) != 0xFFFFFFFF
                    KERNEL32.CloseHandle(barrier["handle"])
                    barrier["resumed"] = True
                caller.join(timeout=24)
                assert not caller.is_alive(), "disconnect RPC did not return"
                if "error" in disconnect:
                    raise disconnect["error"]
                left = disconnect["result"]
            else:
                left = host.tool("ygo_disconnect", timeout=23)
            assert "Left the room" in left and "Error:" not in left, left
            assert not bound_addresses(port, host.proc.pid)
            if ack_replay and hold_gui:
                marker_index = next(i for i, line in enumerate(host.stderr_tail)
                                    if "[stoc] duel-end-wait" in line)
                assert not any("[mcp] auto-answer-message" in line
                               for line in host.stderr_tail[marker_index + 1:]), (
                    "normal auto-answer released duel-end before disconnect", host.stderr_tail[-12:])
                assert any("[stoc] duel-end-wait-return canceled=1" in line
                           for line in host.stderr_tail[marker_index + 1:]), (
                    "duel-end wait did not return through cancellation", host.stderr_tail[-12:])
                print("PASS host entered duel-end acknowledgement wait; MCP disconnect released it", flush=True)
            elif ack_replay:
                print("PASS replay No reached duel-end acknowledgement; subsequent disconnect succeeded", flush=True)
            else:
                assert not any("[stoc] type=22 " in line for line in host.stderr_tail), (
                    "duel-end packet dispatched after replay cancellation", host.stderr_tail[-12:])
                print("PASS host entered replay-save wait; following duel-end was not dispatched", flush=True)
            print("PASS replay-save recipient disconnected after real engine win", out, flush=True)
        except BaseException as exc:
            failure = exc
        finally:
            if barrier.get("handle") and not barrier.get("resumed"):
                KERNEL32.ResumeThread(barrier["handle"])
                KERNEL32.CloseHandle(barrier["handle"])
            exits = {client.name: client.close() for client in reversed(clients)}
            time.sleep(.5)
            remaining = any_listeners(port)
            if trace_file and clients:
                trace_file.write_text("".join(host.stderr_tail), encoding="utf-8")
        assert all(code == 0 and not forced for code, forced, _ in exits.values()), exits
        assert not remaining, remaining
        if failure:
            raise failure
        print("PASS both GUI exits 0 and listener released", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--scratch-dir", type=Path, required=True)
    parser.add_argument("--dll-dir", type=Path)
    parser.add_argument("--ack-replay", action="store_true",
                        help="click replay No, then disconnect while duel-end acknowledgement waits")
    parser.add_argument("--hold-gui-at-duel-end", action="store_true",
                        help="test-only GUI dispatch barrier to isolate cancellation from auto-answer")
    parser.add_argument("--trace-file", type=Path, help="new raw host stderr receipt in scratch-dir")
    args = parser.parse_args()
    if sys.platform != "win32":
        parser.error("Windows desktop required")
    assert args.exe.is_file() and args.assets.is_dir() and args.scratch_dir.is_dir()
    assert not args.hold_gui_at_duel_end or args.ack_replay
    if args.trace_file:
        assert args.trace_file.resolve().parent == args.scratch_dir.resolve()
        assert not args.trace_file.exists(), "preserve prior raw trace"
    run(args.exe.resolve(), args.assets.resolve(), args.scratch_dir.resolve(),
        args.dll_dir.resolve() if args.dll_dir else None, args.ack_replay,
        args.hold_gui_at_duel_end,
        args.trace_file.resolve() if args.trace_file else None)
