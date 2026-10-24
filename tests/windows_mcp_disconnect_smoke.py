"""Offline GUI regression for MCP disconnect and fresh RPS reconnect.

Pass a complete local runtime with cards.cdb, script/, and deck/normal40.ydk.
The test copies it to a private scratch directory, omits artwork, and hosts
only on 127.0.0.1. It never joins a public room or sends chat.
"""

import argparse
import ctypes
import json
import os
from pathlib import Path
import queue
import shutil
import socket
import subprocess
import sys
import tempfile
import time

from windows_loopback_host_smoke import bound_addresses, click, window_for


USER32 = ctypes.WinDLL("user32", use_last_error=True) if sys.platform == "win32" else None


def established(port, pid):
    rows = subprocess.check_output(["netstat", "-ano", "-p", "tcp"], text=True)
    return [row.strip() for row in rows.splitlines()
            if "ESTABLISHED" in row and f":{port} " in row
            and row.strip().endswith(f" {pid}")]


def any_listeners(port):
    rows = subprocess.check_output(["netstat", "-ano", "-p", "tcp"], text=True)
    return [row.strip() for row in rows.splitlines()
            if "LISTENING" in row and f":{port} " in row]


class Client:
    def __init__(self, name, exe, runtime, dll_dir, capture_stderr=False, debug=False):
        env = os.environ.copy()
        if dll_dir:
            env["PATH"] = str(dll_dir) + os.pathsep + env.get("PATH", "")
        if debug:
            env["YGOCLI_MCP_DEBUG"] = "1"
        self.proc = subprocess.Popen([str(exe)], cwd=runtime, env=env,
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, bufsize=0)
        self.name = name
        self.next_id = 0
        self.lines = queue.Queue()
        self.stderr_lines = queue.Queue() if capture_stderr else None
        self.stderr_tail = []

        def read():
            for raw in self.proc.stdout:
                self.lines.put(raw)

        import threading
        threading.Thread(target=read, daemon=True).start()
        if capture_stderr:
            def read_stderr():
                for raw in self.proc.stderr:
                    line = raw.decode(errors="replace")
                    self.stderr_tail.append(line)
                    self.stderr_lines.put(line)
            self.stderr_thread = threading.Thread(target=read_stderr, daemon=True)
            self.stderr_thread.start()

    def rpc(self, method, params=None, timeout=55):
        self.next_id += 1
        payload = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        if params is not None:
            payload["params"] = params
        self.proc.stdin.write((json.dumps(payload) + "\n").encode())
        self.proc.stdin.flush()
        try:
            response = json.loads(self.lines.get(timeout=timeout))
        except queue.Empty as exc:
            raise AssertionError(f"{self.name} {method} timed out") from exc
        assert response.get("id") == self.next_id and "error" not in response, response
        return response["result"]

    def tool(self, name, args=None, timeout=55):
        result = self.rpc("tools/call", {"name": name, "arguments": args or {}}, timeout)
        return result["content"][0]["text"]

    def close(self):
        forced = False
        if self.proc.poll() is None:
            hwnd = window_for(self.proc.pid)
            if hwnd and USER32.PostMessageW(hwnd, 0x0010, 0, 0):
                try:
                    self.proc.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    forced = True
            else:
                forced = True
        if forced and self.proc.poll() is None:
            self.proc.terminate()  # cleanup after failure; never passes below
            self.proc.wait(timeout=5)
        code = self.proc.returncode
        if self.stderr_lines is None:
            stderr = self.proc.stderr.read().decode(errors="replace")
        else:
            self.stderr_thread.join(timeout=2)
            assert not self.stderr_thread.is_alive(), "stderr reader did not finish"
            stderr = "".join(self.stderr_tail)
        self.proc.stdin.close()
        self.proc.stdout.close()
        self.proc.stderr.close()
        return code, forced, stderr[-400:]


def host_room(name, exe, runtime, dll_dir, port):
    host = Client(name, exe, runtime, dll_dir)
    try:
        host.rpc("initialize", {"protocolVersion": "2024-11-05"})
        limit = time.monotonic() + 12
        hwnd = None
        while time.monotonic() < limit and not hwnd:
            hwnd = window_for(host.proc.pid)
            time.sleep(.2)
        assert hwnd, "Host GUI did not open"
        click(hwnd, 500, 245)
        click(hwnd, 680, 137)
        click(hwnd, 635, 467)
        limit = time.monotonic() + 8
        while time.monotonic() < limit and not bound_addresses(port, host.proc.pid):
            time.sleep(.2)
        assert bound_addresses(port, host.proc.pid) == [f"127.0.0.1:{port}"]
        return host
    except BaseException:
        host.close()
        raise


def run(exe, assets, scratch_dir, dll_dir):
    assert (assets / "cards.cdb").is_file()
    assert (assets / "deck/normal40.ydk").is_file()
    assert (assets / "script").is_dir()
    with tempfile.TemporaryDirectory(prefix="mcp-disconnect-", dir=scratch_dir) as scratch:
        runtime = Path(scratch).resolve()
        assert runtime.is_relative_to(scratch_dir.resolve())
        shutil.copytree(assets, runtime, dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns("pics", "replay", "*.log"))
        (runtime / "replay").mkdir(exist_ok=True)
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        (runtime / "system.conf").write_text(
            f"serverport = {port}\nlocal_host_only = 1\n", encoding="ascii")
        clients = []
        failure = None
        try:
            host = host_room("HostOne", exe, runtime, dll_dir, port)
            clients.append(host)
            guest = Client("Guest", exe, runtime, dll_dir)
            clients.append(guest)
            guest.rpc("initialize", {"protocolVersion": "2024-11-05"})
            args = {"host": "127.0.0.1", "port": str(port), "password": "",
                    "name": "Guest", "deck": "deck/normal40.ydk"}
            first = guest.tool("ygo_connect", args)
            assert "0. rock" in first and established(port, guest.proc.pid)
            left = guest.tool("ygo_disconnect", timeout=20)
            assert "Left the room" in left and not established(port, guest.proc.pid), left
            time.sleep(1)
            settled = guest.tool("ygo_card", {"code": 32864}, timeout=12)
            assert "# options:" not in settled, settled
            old = clients.pop(0)
            code, forced, stderr = old.close()
            assert code == 0 and not forced, (code, forced, stderr)
            assert not bound_addresses(port, old.proc.pid)
            host = host_room("HostTwo", exe, runtime, dll_dir, port)
            clients.insert(0, host)
            second = guest.tool("ygo_connect", args)
            assert "0. rock" in second and established(port, guest.proc.pid), second
            guest.tool("ygo_disconnect", timeout=20)
            print("PASS settled disconnect has no old RPS choice; fresh room gives new RPS", flush=True)
        except BaseException as exc:
            failure = exc
        finally:
            exits = {client.name: client.close() for client in reversed(clients)}
            time.sleep(.5)
            remaining = any_listeners(port)
        assert all(code == 0 and not forced for code, forced, _ in exits.values()), exits
        assert not remaining, remaining
        if failure is not None:
            raise failure
        print("PASS both GUIs exited 0 and loopback listener closed", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--scratch-dir", type=Path, required=True)
    parser.add_argument("--dll-dir", type=Path)
    args = parser.parse_args()
    if sys.platform != "win32":
        parser.error("Windows desktop required")
    assert args.exe.is_file() and args.assets.is_dir() and args.scratch_dir.is_dir()
    run(args.exe.resolve(), args.assets.resolve(), args.scratch_dir.resolve(),
        args.dll_dir.resolve() if args.dll_dir else None)
