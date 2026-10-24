"""Offline GUI regression: a peer's pending MCP choice ends when host leaves.

Pass a matching local cards.cdb, script/ and normal40.ydk. This uses only an
ephemeral 127.0.0.1 room and copies assets to a private temporary runtime.
"""

import argparse
import os
from pathlib import Path
import shutil
import socket
import sys
import tempfile
import threading
import time

from windows_mcp_disconnect_smoke import (
    Client, any_listeners, bound_addresses, established, host_room,
)


def run(exe, assets, scratch_dir, dll_dir):
    assert (assets / "cards.cdb").is_file()
    assert (assets / "deck/normal40.ydk").is_file()
    assert (assets / "script").is_dir()
    with tempfile.TemporaryDirectory(prefix="mcp-peer-disconnect-", dir=scratch_dir) as scratch:
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
            host = host_room("Host", exe, runtime, dll_dir, port)
            clients.append(host)
            guest = Client("Guest", exe, runtime, dll_dir)
            clients.append(guest)
            guest.rpc("initialize", {"protocolVersion": "2024-11-05"})
            rps = guest.tool("ygo_connect", {
                "host": "127.0.0.1", "port": str(port), "password": "",
                "name": "Guest", "deck": "deck/normal40.ydk"})
            assert "0. rock" in rps and established(port, guest.proc.pid)
            limit = time.monotonic() + 8
            host_rps = ""
            while time.monotonic() < limit and "0. rock" not in host_rps:
                host_rps = host.tool("ygo_card", {"code": 32864}, timeout=8)
            assert "0. rock" in host_rps
            pending = {}

            def peer_choice():
                try:
                    pending["response"] = guest.tool("ygo_choose", {"id": [0]}, timeout=28)
                except Exception as exc:
                    pending["error"] = repr(exc)

            thread = threading.Thread(target=peer_choice)
            thread.start()
            first = host.tool("ygo_choose", {"id": [1]}, timeout=28)  # paper wins
            assert "go first" in first
            live = host.tool("ygo_choose", {"id": [0]}, timeout=28)
            assert "T1 self turn" in live and "# options:" in live
            assert established(port, host.proc.pid)
            left = host.tool("ygo_disconnect", timeout=20)
            assert "Left the room" in left and "Error:" not in left
            thread.join(timeout=18)
            assert not thread.is_alive() and "error" not in pending, pending
            response = pending.get("response", "")
            assert "Room connection closed" in response, response
            assert "# options:" not in response, response
            assert not established(port, guest.proc.pid)
            assert not bound_addresses(port, host.proc.pid)
            print("PASS peer's pending MCP call returned terminal state without stale choices", flush=True)
        except BaseException as exc:
            failure = exc
        finally:
            exits = {client.name: client.close() for client in reversed(clients)}
            time.sleep(.5)
            remaining = any_listeners(port)
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
    args = parser.parse_args()
    if sys.platform != "win32":
        parser.error("Windows desktop required")
    assert args.exe.is_file() and args.assets.is_dir() and args.scratch_dir.is_dir()
    run(args.exe.resolve(), args.assets.resolve(), args.scratch_dir.resolve(),
        args.dll_dir.resolve() if args.dll_dir else None)
