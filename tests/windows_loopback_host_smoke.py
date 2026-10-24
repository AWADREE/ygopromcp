"""Verify the normal graphical Host screen binds only to loopback when configured."""
import argparse
import ctypes
from ctypes import wintypes
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

from windows_mcp_stdio_smoke import make_runtime


USER32 = ctypes.WinDLL("user32", use_last_error=True) if sys.platform == "win32" else None


def window_for(pid):
    handles = []
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    @callback_type
    def collect(hwnd, _):
        owner = wintypes.DWORD()
        USER32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and USER32.IsWindowVisible(hwnd):
            handles.append(hwnd)
        return True

    USER32.EnumWindows(collect, 0)
    return handles[0] if handles else None


def click(hwnd, x, y):
    rect = wintypes.RECT()
    assert USER32.GetClientRect(hwnd, ctypes.byref(rect))
    point = (round(y * rect.bottom / 640) << 16) | round(x * rect.right / 1024)
    for message, buttons in ((0x0200, 0), (0x0201, 1), (0x0202, 0)):
        assert USER32.PostMessageW(hwnd, message, buttons, point)
    time.sleep(.3)


def bound_addresses(port, pid):
    output = subprocess.check_output(["netstat", "-ano", "-p", "tcp"], text=True)
    addresses = []
    for line in output.splitlines():
        fields = line.split()
        if len(fields) == 5 and fields[0] == "TCP" and fields[3] == "LISTENING" \
                and fields[4] == str(pid) and fields[1].endswith(f":{port}"):
            addresses.append(fields[1])
    return addresses


def run(exe, scratch_dir, dll_dir):
    with tempfile.TemporaryDirectory(prefix="loopback-host-", dir=scratch_dir) as scratch:
        runtime = Path(scratch)
        make_runtime(runtime)
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        (runtime / "system.conf").write_text(
            f"serverport = {port}\nlocal_host_only = 1\n", encoding="ascii")
        env = os.environ.copy()
        if dll_dir:
            env["PATH"] = str(dll_dir) + os.pathsep + env.get("PATH", "")
        proc = subprocess.Popen([str(exe)], cwd=runtime, env=env,
                                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 12
            hwnd = None
            while time.monotonic() < deadline and not hwnd:
                hwnd = window_for(proc.pid)
                time.sleep(.2)
            assert hwnd, f"No visible GUI; exit={proc.poll()}"
            click(hwnd, 500, 245)  # LAN
            click(hwnd, 680, 137)  # Create Host
            click(hwnd, 635, 467)  # Confirm
            deadline = time.monotonic() + 10
            addresses = []
            while time.monotonic() < deadline:
                addresses = bound_addresses(port, proc.pid)
                if addresses:
                    break
                time.sleep(.2)
            assert addresses == [f"127.0.0.1:{port}"], addresses
            print(f"PASS visible GUI host listened only on {addresses[0]}", flush=True)
        finally:
            if proc.poll() is None:
                hwnd = window_for(proc.pid)
                if hwnd:
                    USER32.PostMessageW(hwnd, 0x0010, 0, 0)
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.terminate()
                    proc.wait(timeout=5)
            stderr = proc.stderr.read().decode(errors="replace")
            proc.stderr.close()
            assert not bound_addresses(port, proc.pid), "Listener survived GUI exit"
            if proc.returncode != 0:
                raise AssertionError(f"GUI exit={proc.returncode}; stderr={stderr[-500:]}")
            print("PASS GUI shutdown released loopback listener", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--scratch-dir", type=Path, required=True)
    parser.add_argument("--dll-dir", type=Path)
    args = parser.parse_args()
    if sys.platform != "win32":
        parser.error("Windows desktop required")
    run(args.exe.resolve(), args.scratch_dir.resolve(),
        args.dll_dir.resolve() if args.dll_dir else None)
