"""Private end-to-end MCP batch-launcher check with owned GUI shutdown.

The caller supplies a *copy* of the package with compatible game data.
This test never joins a room. It verifies the cmd.exe -> native launcher ->
YGOPro.exe process chain by absolute executable path, and posts WM_CLOSE to
the game's own window after stdin EOF. It does not
capture the desktop or send game input.
"""

import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading
import time


TH32CS_SNAPPROCESS = 0x00000002
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
PROCESS_TERMINATE = 0x0001
WM_CLOSE = 0x0010
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
user32 = ctypes.WinDLL("user32", use_last_error=True)


class ProcessEntry(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD), ("th32DefaultHeapID", ctypes.c_void_p),
        ("th32ModuleID", wintypes.DWORD), ("cntThreads", wintypes.DWORD),
        ("th32ParentProcessID", wintypes.DWORD), ("pcPriClassBase", wintypes.LONG),
        ("dwFlags", wintypes.DWORD), ("szExeFile", wintypes.WCHAR * 260),
    ]


kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
kernel32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
kernel32.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
kernel32.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
kernel32.OpenProcess.restype = wintypes.HANDLE
kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel32.QueryFullProcessImageNameW.restype = wintypes.BOOL
kernel32.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
kernel32.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]


def owned_child(parent_pid: int, expected_exe: Path) -> int | None:
    snapshot = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snapshot == ctypes.c_void_p(-1).value:
        raise OSError(ctypes.get_last_error(), "Process snapshot failed")
    try:
        entry = ProcessEntry()
        entry.dwSize = ctypes.sizeof(entry)
        more = kernel32.Process32FirstW(snapshot, ctypes.byref(entry))
        while more:
            if entry.th32ParentProcessID == parent_pid and entry.szExeFile.lower() == expected_exe.name.lower():
                handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, entry.th32ProcessID)
                if not handle:
                    raise OSError(ctypes.get_last_error(), "Cannot verify owned child path")
                try:
                    path_buffer = ctypes.create_unicode_buffer(32768)
                    length = wintypes.DWORD(len(path_buffer))
                    if not kernel32.QueryFullProcessImageNameW(handle, 0, path_buffer, ctypes.byref(length)):
                        raise OSError(ctypes.get_last_error(), "Cannot read owned child path")
                    if Path(path_buffer.value).resolve() == expected_exe:
                        return entry.th32ProcessID
                finally:
                    kernel32.CloseHandle(handle)
            more = kernel32.Process32NextW(snapshot, ctypes.byref(entry))
    finally:
        kernel32.CloseHandle(snapshot)
    return None


def close_owned_window(child_pid: int) -> None:
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    found: list[int] = []

    @callback_type
    def visit(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == child_pid:
            found.append(hwnd)
        return True

    user32.EnumWindows(visit, 0)
    if not found:
        raise AssertionError(f"No window owned by verified YGOPro PID {child_pid}")
    if not user32.PostMessageW(found[0], WM_CLOSE, 0, 0):
        raise OSError(ctypes.get_last_error(), "Could not close owned YGOPro window")


def terminate_verified_child(child_pid: int, expected_exe: Path, parent_pid: int) -> None:
    if owned_child(parent_pid, expected_exe) != child_pid:
        return
    handle = kernel32.OpenProcess(PROCESS_TERMINATE, False, child_pid)
    if handle:
        try:
            kernel32.TerminateProcess(handle, 1)
        finally:
            kernel32.CloseHandle(handle)


def main() -> None:
    if sys.platform != "win32":
        raise RuntimeError("Windows only")
    runtime = Path(sys.argv[1]).resolve()
    exe = (runtime / "YGOPro.exe").resolve()
    native_launcher = (runtime / "YGOProUpdateLauncher.exe").resolve()
    launcher = runtime / "MCP YGOPro.cmd"
    assert exe.is_file() and native_launcher.is_file() and launcher.is_file()
    env = os.environ.copy()
    env["PATH"] = os.pathsep.join([os.environ["SystemRoot"] + r"\System32", os.environ["SystemRoot"]])
    proc = subprocess.Popen(
        [os.environ["ComSpec"], "/d", "/c", str(launcher)],
        cwd=runtime, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, creationflags=subprocess.CREATE_NO_WINDOW, bufsize=0,
    )
    lines: queue.Queue[bytes] = queue.Queue()
    threading.Thread(target=lambda: [lines.put(line) for line in proc.stdout], daemon=True).start()
    launcher_pid = None
    child_pid = None
    try:
        for number, method in ((1, "initialize"), (2, "tools/list")):
            proc.stdin.write((json.dumps({"jsonrpc": "2.0", "id": number,
                                          "method": method, "params": {}}) + "\n").encode())
            proc.stdin.flush()
            try:
                response = json.loads(lines.get(timeout=20))
            except queue.Empty as exc:
                raise AssertionError(f"No {method} response; launcher exit={proc.poll()}") from exc
            assert response.get("id") == number and "result" in response, response
            if method == "tools/list":
                names = {tool["name"] for tool in response["result"]["tools"]}
                assert {"ygo_connect", "ygo_choose", "ygo_disconnect"} <= names
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and child_pid is None:
            launcher_pid = owned_child(proc.pid, native_launcher)
            if launcher_pid is not None:
                child_pid = owned_child(launcher_pid, exe)
            if child_pid is None:
                time.sleep(0.05)
        assert launcher_pid is not None and child_pid is not None, "No verified native-launcher/game process chain"
        proc.stdin.close()
        time.sleep(0.2)
        assert proc.poll() is None, "GUI exited on MCP stdin EOF"
        close_owned_window(child_pid)
        assert proc.wait(timeout=10) == 0, "Launcher or game did not shut down cleanly"
        assert lines.empty(), "Unexpected nonprotocol stdout"
        print(f"PASS: actual MCP wrapper and native launcher, initialize, tools/list ({len(names)} tools), EOF survival, owned GUI close, exit 0")
    finally:
        if proc.poll() is None:
            if child_pid is not None and launcher_pid is not None:
                terminate_verified_child(child_pid, exe, launcher_pid)
            proc.kill()
            proc.wait(timeout=5)
        if not proc.stdin.closed:
            proc.stdin.close()
        proc.stdout.close()
        proc.stderr.close()


if __name__ == "__main__":
    main()
