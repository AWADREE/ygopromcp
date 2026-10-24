"""Offline regression for ygo_card detail delivery in the normal MCP block."""
import argparse
from pathlib import Path
import sys
import tempfile

from deck_editor_mcp_smoke import make_runtime, request, start_client


def run(exe, assets, scratch_dir, dll_dir):
    with tempfile.TemporaryDirectory(prefix="card-explain-", dir=scratch_dir) as scratch:
        runtime = Path(scratch)
        make_runtime(runtime, assets)
        proc, lines = start_client(exe, runtime, dll_dir)
        try:
            for ident in (1, 2):
                detail = request(proc, lines, "ygo_card", {"code": 100}, ident)
                assert "# card explain\n" in detail, detail
                assert "[Card] 100 MainCard" in detail, detail
                assert "Local fixture" in detail, detail
            print("PASS explicit card explain repeats in the normal MCP block", flush=True)
            missing = request(proc, lines, "ygo_card", {"name": "absent-card"}, 3)
            assert "No card found: absent-card" in missing, missing
            assert "[Card] 100 MainCard" not in missing, missing
            print("PASS missing card reports an answer without stale detail", flush=True)
        finally:
            if proc.poll() is None:
                proc.terminate()
                proc.wait(timeout=5)
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
        parser.error("Windows GUI test only")
    run(args.exe.resolve(), args.assets.resolve(), args.scratch_dir.resolve(),
        args.dll_dir.resolve() if args.dll_dir else None)
