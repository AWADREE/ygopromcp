"""Private packaged-executable stdio smoke test; never joins a game room.

Pass a separate runtime folder with compatible game data already supplied.
This test starts and terminates only its own YGOPro.exe process. It does not
use desktop capture, global input, or native window control.
"""

import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading


def main() -> None:
    runtime = Path(sys.argv[1]).resolve()
    exe = runtime / "YGOPro.exe"
    assert exe.is_file(), exe
    for name in ("cards.cdb", "strings.conf", "script/constant.lua", "textures/field.png"):
        assert (runtime / name).is_file(), name
    env = os.environ.copy()
    # The executable must resolve its bundled DLLs without the MSYS2 bin path.
    env["PATH"] = os.pathsep.join([os.environ["SystemRoot"] + r"\System32", os.environ["SystemRoot"]])
    creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    proc = subprocess.Popen(
        [str(exe)], cwd=runtime, env=env,
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        creationflags=creationflags, bufsize=0,
    )
    lines: queue.Queue[bytes] = queue.Queue()
    reader = threading.Thread(target=lambda: [lines.put(line) for line in proc.stdout], daemon=True)
    reader.start()
    try:
        for number, method in ((1, "initialize"), (2, "tools/list")):
            message = {"jsonrpc": "2.0", "id": number, "method": method, "params": {}}
            proc.stdin.write((json.dumps(message) + "\n").encode("utf-8"))
            proc.stdin.flush()
            try:
                line = lines.get(timeout=20)
            except queue.Empty as exc:
                raise AssertionError(f"No {method} result; exit={proc.poll()}") from exc
            response = json.loads(line)
            assert response.get("id") == number and "result" in response, response
            if method == "initialize":
                assert response["result"]["serverInfo"]["name"] == "ygopro-mcp"
            else:
                names = {tool["name"] for tool in response["result"]["tools"]}
                assert {"ygo_connect", "ygo_choose", "ygo_disconnect"} <= names
                print(f"PASS: packaged executable initialize and tools/list ({len(names)} tools)")
    finally:
        if proc.poll() is None:
            proc.terminate()  # Exact child started above; no process-name enumeration.
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
        proc.stdin.close()
        proc.stdout.close()
        proc.stderr.close()


if __name__ == "__main__":
    main()
