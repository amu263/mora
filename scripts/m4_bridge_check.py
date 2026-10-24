#!/usr/bin/env python3
r"""MORA M4-A end-to-end bridge acceptance (real network, frozen binary, hidden console).

What this proves
----------------
The terminal player owns the one PlaybackSession and exposes it over the per-user named
pipe \\.\pipe\mora-player-<SID> (single instance). The MCP stdio mode (mora --mcp) is a
thin proxy that lazily connects to that pipe, while music_search / music_open_collection
stay read-only network operations inside the MCP process. This harness drives the
*frozen* build/mora.exe and observes real behavior:

  1. fingerprint   SHA256 of build/mora.exe recorded before and after; exit 2 when it
                   does not match --expected-sha256 or changed during the run
  2. preflight     no other MORA player may own the session pipe (repeatability)
  3. alone         MCP without a player: music_play -> isError "needs_player";
                   music_search still returns a real result from a local fallback
  4. tui           live TUI (no --demo) in a hidden console with --snapshot/--report
  5. second-tui    a second real TUI is rejected immediately with exit code 2
  6. mcp           initialize(2025-11-25) -> initialized -> tools/list (6 tools)
  7. status        music_get_status: playerSessionAttached=true, session non-empty
  7b pipe-busy     a second adapter cannot own the pipe: play -> needs_player in
                   well under 25 s, search -> prompt local fallback with a real result
  8. search        real music_search served by the TUI network stack through the
                   bridge (the MCP process must not grow its own HTTP stack)
  8b bridged-error a bridged backend error answers with network_* quickly instead
                   of hanging until the 25 s bridge timeout
  9. play          music_play(id) then poll music_now_playing until playing=true
 10. memory        TUI + MCP sampled at the same instant while audio is playing;
                   sum working set and sum private commit must each stay < 50 MiB
 11. shared        the TUI --snapshot shows the same track name and PLAYING
 12-14 pause/resume/volume, each by polling the target state (the audio thread applies
                   commands asynchronously, so the immediate control snapshot can lag)
 15. status-again  music_get_status agrees with music_now_playing
 16. eof           close stdin: MCP exits 0 and stdout was pure JSON-RPC (no ANSI)
 17. cleanup       /quit, report written, pipe released
 18. fingerprint   binary unchanged during the run (never rebuilt)
 19. classify      every failure classified as environment/product/harness

Exit codes
----------
  0  every step passed (or only environment failures with --allow-environment-failures)
  1  product defect or harness failure
  2  binary fingerprint mismatch / binary replaced during the run (nothing rebuilt)
  3  environment / permission / network / console failure only

Writes only build/m4-bridge/** (gitignored artifacts) and the --out JSON (default
docs/verification/m4-bridge.json). Never builds, never invokes build.ps1, never
modifies the frozen binary, the user's playlists or favorites.
"""

from __future__ import annotations

import argparse
import ctypes as c
from ctypes import wintypes as w
from datetime import datetime
import hashlib
import json
import os
import platform
import queue
import re
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = Path(__file__).resolve()
DEFAULT_EXE = ROOT / "build" / "mora.exe"
DEFAULT_OUT = ROOT / "docs" / "verification" / "m4-bridge.json"
DEFAULT_ARTIFACTS = ROOT / "build" / "m4-bridge"

MB = 1048576.0
BUDGET_MB = 50.0
SNAPSHOT_NAME = "tui-snapshot.txt"
REPORT_NAME = "tui-report.json"
TUI_SECONDS_DEFAULT = 180

TOOL_NAMES = {
    "music_search",
    "music_open_collection",
    "music_play",
    "music_control",
    "music_now_playing",
    "music_get_status",
}

# Public song id used only for the "no player attached" negative case: it must fail
# before any playback is attempted, so this value never becomes a hidden play path.
STANDALONE_PROBE_ID = 539928216

VK_RETURN = 0x0D
VK_ESCAPE = 0x1B

STEP_ORDER = [
    ("1-binary-fingerprint", "记录被测二进制指纹"),
    ("2-preflight", "预检：没有其他 MORA 播放器占用会话管道"),
    ("3-negative-alone", "负例 B：MCP 无 TUI 时 music_play -> needs_player"),
    ("4-tui-start", "启动真实 TUI 并等待首页快照（阿木）"),
    ("5-negative-second-tui", "负例 A：第二个真实 TUI 立即拒绝（exit 2）"),
    ("6-mcp-handshake", "MCP initialize / initialized / tools/list（6 个工具）"),
    ("7-mcp-status", "music_get_status: playerSessionAttached=true 且 session 非空"),
    ("7b-pipe-busy", "负例：第二个适配器遇管道忙 -> needs_player 且 search 本地回退"),
    ("8-mcp-search", "music_search 经桥接由 TUI 联网，取第一条 song id"),
    ("8b-bridged-error", "负例：桥接后端错误按 network_* 立即回帧（不超时）"),
    ("9-mcp-play", "music_play(id) 并轮询 music_now_playing 到 playing=true"),
    ("10-memory", "播放中同时刻采样 TUI 与 MCP 内存，合计 < 50 MB x2"),
    ("11-shared-state", "TUI 快照出现同一曲目名与 PLAYING"),
    ("12-mcp-pause", "music_control pause（轮询 paused=true）"),
    ("13-mcp-resume", "music_control resume（轮询 paused=false）"),
    ("14-mcp-volume", "music_control volume=40（轮询 volumePercent=40）"),
    ("15-mcp-status-again", "再次 music_get_status 与 now_playing 一致"),
    ("16-mcp-eof", "关闭 stdin：MCP 退出 0 且 stdout 只有 JSON-RPC"),
    ("17-tui-cleanup", "TUI /quit 退出、报告写出、管道释放"),
    ("18-fingerprint-stable", "运行期间二进制未被替换"),
]
CLASSIFICATION_STEP = ("19-classification", "失败分类与未验证项")

# Honest UI/error fragments that mean the host, account, network or audio device
# blocked the run: environment failures, not product defects.
ENV_MARKERS = (
    "音频输出设备不可用",
    "Media Foundation",
    "没有完整播放权限",
    "请登录有权限的账号",
    "无版权",
    "无权限",
    "会员",
    "VIP",
    "付费",
    "受限",
    "登录已失效",
    "试听",
    "风控",
    "频繁",
    "操作过",
    "验证码",
    "网络",
    "超时",
    "连接失败",
    "请求失败",
    "暂时不可用",
    "服务器",
    "HTTP",
    "TLS",
    "下架",
    "找不到",
    "不可用",
)


# --------------------------------------------------------------------- helpers
def now_iso() -> str:
    return datetime.now().astimezone().isoformat(timespec="seconds")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def fingerprint(path: Path) -> dict:
    try:
        stat = path.stat()
        relative_path = str(path.relative_to(ROOT))
    except (OSError, ValueError):
        return {"path": str(path), "exists": path.exists()}
    return {
        "path": str(path),
        "relativePath": relative_path,
        "exists": True,
        "sizeBytes": stat.st_size,
        "sha256": sha256_file(path),
        "lastWriteTime": datetime.fromtimestamp(stat.st_mtime).astimezone().isoformat(
            timespec="seconds"
        ),
        "sha256Tool": "hashlib.sha256 over the file bytes (same value as Get-FileHash -Algorithm SHA256)",
    }


def relative(path: Path) -> str:
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


def configure_stdout() -> None:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass


def classify_environment(text: str):
    """Return (is_environment_failure, matched_marker) for real UI/error text."""
    lowered = (text or "").lower()
    for marker in ENV_MARKERS:
        if marker.lower() in lowered:
            return True, marker
    return False, ""


def list_mora_pipes() -> list:
    try:
        names = os.listdir("\\\\.\\pipe\\")
    except OSError:
        return []
    return sorted(name for name in names if "mora-player" in name.lower())


# ------------------------------------------------------------- process memory
class PROCESS_MEMORY_COUNTERS_EX(c.Structure):
    _fields_ = [
        ("cb", w.DWORD),
        ("PageFaultCount", w.DWORD),
        ("PeakWorkingSetSize", c.c_size_t),
        ("WorkingSetSize", c.c_size_t),
        ("QuotaPeakPagedPoolUsage", c.c_size_t),
        ("QuotaPagedPoolUsage", c.c_size_t),
        ("QuotaPeakNonPagedPoolUsage", c.c_size_t),
        ("QuotaNonPagedPoolUsage", c.c_size_t),
        ("PagefileUsage", c.c_size_t),
        ("PeakPagefileUsage", c.c_size_t),
        ("PrivateUsage", c.c_size_t),
    ]


_PSAPI = c.WinDLL("psapi", use_last_error=True)
_PSAPI.GetProcessMemoryInfo.argtypes = [w.HANDLE, c.POINTER(PROCESS_MEMORY_COUNTERS_EX), w.DWORD]
_PSAPI.GetProcessMemoryInfo.restype = w.BOOL

_K32 = c.WinDLL("kernel32", use_last_error=True)
_K32.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
_K32.OpenProcess.restype = w.HANDLE
_K32.CloseHandle.argtypes = [w.HANDLE]
_K32.CloseHandle.restype = w.BOOL

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010


def process_memory(pid: int) -> dict:
    counters = PROCESS_MEMORY_COUNTERS_EX()
    counters.cb = c.sizeof(PROCESS_MEMORY_COUNTERS_EX)
    handle = _K32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, int(pid))
    if not handle:
        return {"pid": pid, "error": c.get_last_error(), "errorSource": "OpenProcess"}
    try:
        if not _PSAPI.GetProcessMemoryInfo(handle, c.byref(counters), counters.cb):
            return {"pid": pid, "error": c.get_last_error(), "errorSource": "GetProcessMemoryInfo"}
    finally:
        _K32.CloseHandle(handle)
    return {
        "pid": pid,
        "workingSetBytes": int(counters.WorkingSetSize),
        "privateBytes": int(counters.PrivateUsage),
        "peakWorkingSetBytes": int(counters.PeakWorkingSetSize),
        "peakPrivateBytes": int(counters.PeakPagefileUsage),
        "workingSetMB": round(counters.WorkingSetSize / MB, 3),
        "privateMB": round(counters.PrivateUsage / MB, 3),
    }


def sample_pair(tui_pid: int, mcp_pid: int) -> dict:
    tui = process_memory(tui_pid)
    mcp = process_memory(mcp_pid)
    record = {"tui": tui, "mcp": mcp}
    if "workingSetBytes" in tui and "workingSetBytes" in mcp:
        record["sumWorkingSetMB"] = round((tui["workingSetBytes"] + mcp["workingSetBytes"]) / MB, 3)
        record["sumPrivateMB"] = round((tui["privateBytes"] + mcp["privateBytes"]) / MB, 3)
    return record


# ---------------------------------------------------------------- test console
class _Coord(c.Structure):
    _fields_ = [("X", w.SHORT), ("Y", w.SHORT)]


class _Rect(c.Structure):
    _fields_ = [("Left", w.SHORT), ("Top", w.SHORT), ("Right", w.SHORT), ("Bottom", w.SHORT)]


class _ScreenBufferInfo(c.Structure):
    _fields_ = [
        ("dwSize", _Coord),
        ("dwCursorPosition", _Coord),
        ("wAttributes", w.WORD),
        ("srWindow", _Rect),
        ("dwMaximumWindowSize", _Coord),
    ]


class _Char(c.Union):
    _fields_ = [("UnicodeChar", w.WCHAR), ("AsciiChar", c.c_char)]


class _Key(c.Structure):
    _fields_ = [
        ("down", w.BOOL),
        ("repeat", w.WORD),
        ("vk", w.WORD),
        ("scan", w.WORD),
        ("char", _Char),
        ("control", w.DWORD),
    ]


class _Mouse(c.Structure):
    _fields_ = [("position", _Coord), ("buttons", w.DWORD), ("control", w.DWORD), ("flags", w.DWORD)]


class _Event(c.Union):
    _fields_ = [("key", _Key), ("mouse", _Mouse), ("raw", c.c_byte * 16)]


class _Input(c.Structure):
    _fields_ = [("type", w.WORD), ("event", _Event)]


class ConsoleUnavailable(OSError):
    pass


class TestConsole:
    """A hidden console this process owns; children inherit CONIN$/CONOUT$.

    When the harness runs python with redirected stdout while the process still owns a
    console, AllocConsole() alone returns False. FreeConsole() first (piped stdout and
    stderr survive the detach) and then allocate a private hidden console - the same
    recovery scripts/m1_network_check.py uses. A DETACHED_PROCESS self-relaunch is the
    fallback when even that fails.
    """

    GENERIC_READ_WRITE = 0xC0000000
    OPEN_EXISTING = 3
    SHARE = 3
    STD_OUTPUT_HANDLE = -11

    def __init__(self, columns: int, rows: int) -> None:
        k = c.WinDLL("kernel32", use_last_error=True)
        u = c.WinDLL("user32", use_last_error=True)
        self.kernel32 = k
        self.user32 = u
        k.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, c.c_void_p, w.DWORD, w.DWORD, w.HANDLE]
        k.CreateFileW.restype = w.HANDLE
        k.GetConsoleWindow.restype = w.HWND
        k.GetStdHandle.argtypes = [w.DWORD]
        k.GetStdHandle.restype = w.HANDLE
        k.GetConsoleMode.argtypes = [w.HANDLE, c.POINTER(w.DWORD)]
        k.GetConsoleMode.restype = w.BOOL
        k.GetConsoleScreenBufferInfo.argtypes = [w.HANDLE, c.POINTER(_ScreenBufferInfo)]
        k.GetConsoleScreenBufferInfo.restype = w.BOOL
        k.SetConsoleScreenBufferSize.argtypes = [w.HANDLE, _Coord]
        k.SetConsoleScreenBufferSize.restype = w.BOOL
        k.SetConsoleWindowInfo.argtypes = [w.HANDLE, w.BOOL, c.POINTER(_Rect)]
        k.SetConsoleWindowInfo.restype = w.BOOL
        k.WriteConsoleInputW.argtypes = [w.HANDLE, c.POINTER(_Input), w.DWORD, c.POINTER(w.DWORD)]
        k.WriteConsoleInputW.restype = w.BOOL
        k.FlushConsoleInputBuffer.argtypes = [w.HANDLE]
        k.FlushConsoleInputBuffer.restype = w.BOOL
        k.CloseHandle.argtypes = [w.HANDLE]
        u.ShowWindow.argtypes = [w.HWND, c.c_int]
        u.ShowWindow.restype = w.BOOL

        mode = w.DWORD()
        has_console = bool(k.GetConsoleWindow())
        stdout_is_console = bool(k.GetConsoleMode(k.GetStdHandle(self.STD_OUTPUT_HANDLE), c.byref(mode)))
        steps = []
        self.allocated = False
        if has_console and not stdout_is_console:
            steps.append(("FreeConsole", bool(k.FreeConsole())))
        if not has_console or not stdout_is_console:
            allocated = bool(k.AllocConsole())
            steps.append(("AllocConsole", allocated))
            if not allocated:
                raise ConsoleUnavailable(c.get_last_error(), "AllocConsole failed")
            self.allocated = True
        self.acquire_steps = steps
        self.window = k.GetConsoleWindow()
        if self.allocated and self.window:
            u.ShowWindow(self.window, 0)
        invalid = w.HANDLE(-1).value
        self.conin = k.CreateFileW("CONIN$", self.GENERIC_READ_WRITE, self.SHARE, None, self.OPEN_EXISTING, 0, None)
        self.conout = k.CreateFileW("CONOUT$", self.GENERIC_READ_WRITE, self.SHARE, None, self.OPEN_EXISTING, 0, None)
        if self.conin == invalid or self.conout == invalid:
            raise ConsoleUnavailable(c.get_last_error(), "CONIN$/CONOUT$ unavailable")
        self.mode = "allocated-hidden" if self.allocated else "reused"
        self.columns = columns
        self.rows = rows
        self.size = self.set_size(columns, rows)

    def geometry(self):
        info = _ScreenBufferInfo()
        if not self.kernel32.GetConsoleScreenBufferInfo(self.conout, c.byref(info)):
            return None
        return {
            "bufferWidth": int(info.dwSize.X),
            "bufferHeight": int(info.dwSize.Y),
            "windowLeft": int(info.srWindow.Left),
            "windowTop": int(info.srWindow.Top),
            "windowWidth": int(info.srWindow.Right - info.srWindow.Left + 1),
            "windowHeight": int(info.srWindow.Bottom - info.srWindow.Top + 1),
        }

    def set_size(self, columns: int, rows: int) -> dict:
        before = self.geometry() or {}
        buffer_width = max(int(before.get("bufferWidth", columns)), 1)
        buffer_height = max(int(before.get("bufferHeight", rows)), 1)
        shrink = _Rect(0, 0, min(columns, buffer_width) - 1, min(rows, buffer_height) - 1)
        first = bool(self.kernel32.SetConsoleWindowInfo(self.conout, True, c.byref(shrink)))
        second = bool(self.kernel32.SetConsoleScreenBufferSize(self.conout, _Coord(columns, max(rows, 120))))
        third = bool(self.kernel32.SetConsoleWindowInfo(self.conout, True, c.byref(_Rect(0, 0, columns - 1, rows - 1))))
        observed = self.geometry()
        return {
            "requested": {"columns": columns, "rows": rows},
            "observed": observed,
            "matched": bool(observed and observed["windowWidth"] == columns and observed["windowHeight"] == rows),
            "steps": {"shrinkWindow": first, "setBuffer": second, "setWindow": third},
        }

    def flush_input(self) -> None:
        if self.conin:
            self.kernel32.FlushConsoleInputBuffer(self.conin)

    def send_key(self, vk: int = 0, char: str = "\0") -> None:
        events = (_Input * 2)()
        for index, down in enumerate((True, False)):
            events[index].type = 1
            events[index].event.key = _Key(down, 1, vk, 0, _Char(UnicodeChar=char), 0)
        written = w.DWORD()
        if not self.kernel32.WriteConsoleInputW(self.conin, events, 2, c.byref(written)):
            raise OSError(c.get_last_error(), "WriteConsoleInputW failed")

    def type_text(self, text: str) -> None:
        for char in text:
            self.send_key(0, char)

    def close(self) -> None:
        try:
            if getattr(self, "conin", None):
                self.kernel32.CloseHandle(self.conin)
            if getattr(self, "conout", None):
                self.kernel32.CloseHandle(self.conout)
        finally:
            self.conin = self.conout = None
            if self.allocated:
                self.kernel32.FreeConsole()


# ------------------------------------------------------------------- TUI run
class TuiProcess:
    def __init__(self, exe: Path, console: TestConsole, artifacts: Path, seconds: int) -> None:
        self.exe = exe
        self.console = console
        self.artifacts = artifacts
        self.snapshot = artifacts / SNAPSHOT_NAME
        self.report = artifacts / REPORT_NAME
        self.stderr_path = artifacts / "tui-stderr.txt"
        self.argv = [
            str(exe),
            "--seconds",
            str(seconds),
            "--snapshot",
            str(self.snapshot),
            "--report",
            str(self.report),
        ]
        for stale in (self.snapshot, self.report, self.stderr_path):
            try:
                stale.unlink()
            except FileNotFoundError:
                pass
        self.proc = None
        self.stdin = None
        self.stdout = None
        self.stderr = None

    def start(self) -> None:
        startup = subprocess.STARTUPINFO()
        startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        self.stdin = open("CONIN$", "rb", buffering=0)
        self.stdout = open("CONOUT$", "wb", buffering=0)
        self.stderr = open(self.stderr_path, "wb")
        self.proc = subprocess.Popen(
            self.argv,
            cwd=str(ROOT),
            stdin=self.stdin,
            stdout=self.stdout,
            stderr=self.stderr,
            startupinfo=startup,
        )

    def frame(self) -> str:
        try:
            return self.snapshot.read_bytes().decode("utf-8", "replace")
        except OSError:
            return ""

    def wait(self, predicate, timeout: float, interval: float = 0.05):
        started = time.monotonic()
        frame = ""
        while time.monotonic() - started < timeout:
            frame = self.frame()
            if frame and predicate(frame):
                return frame, round(time.monotonic() - started, 3)
            if self.proc is not None and self.proc.poll() is not None:
                frame = self.frame()
                if frame and predicate(frame):
                    return frame, round(time.monotonic() - started, 3)
                return None, round(time.monotonic() - started, 3)
            time.sleep(interval)
        return None, round(time.monotonic() - started, 3)

    def command(self, text: str) -> None:
        self.console.type_text(text)
        self.console.send_key(VK_RETURN)

    def quit(self, timeout: float) -> int:
        if self.proc is None:
            return -1
        if self.proc.poll() is not None:
            return self.proc.returncode
        try:
            self.console.flush_input()
        except OSError:
            pass
        self.command("/quit")
        try:
            return self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            pass
        try:
            self.console.flush_input()
        except OSError:
            pass
        self.console.send_key(VK_ESCAPE)
        time.sleep(0.4)
        try:
            self.command("/quit")
        except OSError:
            pass
        try:
            return self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.terminate()
            try:
                return self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                return self.proc.wait()

    def terminate(self) -> int:
        if self.proc is None:
            return -1
        if self.proc.poll() is not None:
            return self.proc.returncode
        self.proc.terminate()
        try:
            return self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            return self.proc.wait()

    def stderr_tail(self, limit: int = 4000) -> str:
        try:
            return self.stderr_path.read_text(encoding="utf-8", errors="replace")[-limit:]
        except OSError:
            return ""

    def stop_handles(self) -> None:
        for handle in (self.stdin, self.stdout, self.stderr):
            try:
                if handle:
                    handle.close()
            except OSError:
                pass


def run_second_instance(exe: Path, console: TestConsole, artifacts: Path, timeout: float) -> dict:
    """Start a second real TUI (live mode, --seconds 5) on the same console.

    It must be refused before it ever renders: bridge.start() fails because the first
    player owns the single-instance session pipe.
    """
    stderr_path = artifacts / "second-tui-stderr.txt"
    try:
        stderr_path.unlink()
    except FileNotFoundError:
        pass
    startup = subprocess.STARTUPINFO()
    startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    stdin_handle = open("CONIN$", "rb", buffering=0)
    stdout_handle = open("CONOUT$", "wb", buffering=0)
    stderr_handle = open(stderr_path, "wb")
    proc = None
    started = time.monotonic()
    returncode = None
    timed_out = False
    try:
        proc = subprocess.Popen(
            [str(exe), "--seconds", "5"],
            cwd=str(ROOT),
            stdin=stdin_handle,
            stdout=stdout_handle,
            stderr=stderr_handle,
            startupinfo=startup,
        )
        try:
            returncode = proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            proc.kill()
            returncode = proc.wait()
    finally:
        for handle in (stdin_handle, stdout_handle, stderr_handle):
            try:
                handle.close()
            except OSError:
                pass
    text = ""
    try:
        text = stderr_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        pass
    return {
        "argv": [str(exe), "--seconds", "5"],
        "returncode": returncode,
        "timedOut": timed_out,
        "elapsedSeconds": round(time.monotonic() - started, 3),
        "stderr": text[-4000:],
    }


# --------------------------------------------------------------------- MCP
class McpTimeout(TimeoutError):
    pass


class McpTransportError(RuntimeError):
    pass


class McpClient:
    def __init__(self, exe: Path, artifacts: Path, label: str) -> None:
        self.exe = exe
        self.label = label
        self.stderr_path = artifacts / ("mcp-%s-stderr.txt" % label)
        self.proc = None
        self.stderr = None
        self.lines = []
        self.requests = []
        self.returncode = None
        self._queue = queue.Queue()
        self._reader = None
        self._next_id = 0
        self._lock = threading.Lock()

    def start(self) -> None:
        self.stderr = open(self.stderr_path, "wb")
        self.proc = subprocess.Popen(
            [str(self.exe), "--mcp"],
            cwd=str(ROOT),
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self.stderr,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
            creationflags=subprocess.CREATE_NO_WINDOW,
        )
        self._reader = threading.Thread(target=self._read, name="mcp-reader-%s" % self.label, daemon=True)
        self._reader.start()

    def _read(self) -> None:
        try:
            for line in self.proc.stdout:
                with self._lock:
                    self.lines.append(line)
                self._queue.put(line)
        except Exception as exc:  # noqa: BLE001 - surfaced through the queue
            self._queue.put(("\x00reader-error\x00", str(exc)))
        finally:
            self._queue.put(None)

    def _send(self, message: dict) -> None:
        if self.proc is None or self.proc.stdin is None:
            raise McpTransportError("MCP process is not running")
        try:
            self.proc.stdin.write(json.dumps(message, ensure_ascii=False) + "\n")
            self.proc.stdin.flush()
        except (BrokenPipeError, OSError, ValueError) as exc:
            raise McpTransportError("MCP stdin write failed: %s" % exc) from exc

    def notify(self, method: str, params=None) -> None:
        message = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            message["params"] = params
        self.requests.append({"id": None, "method": method})
        self._send(message)

    def request(self, method: str, params=None, timeout: float = 30.0) -> dict:
        self._next_id += 1
        identity = self._next_id
        message = {"jsonrpc": "2.0", "id": identity, "method": method}
        if params is not None:
            message["params"] = params
        started = time.monotonic()
        self.requests.append({"id": identity, "method": method, "params": params})
        self._send(message)
        try:
            line = self._queue.get(timeout=timeout)
        except queue.Empty:
            raise McpTimeout("%s did not answer within %.1fs" % (method, timeout))
        elapsed = round(time.monotonic() - started, 3)
        if isinstance(line, tuple) and line and line[0] == "\x00reader-error\x00":
            raise McpTransportError("MCP stdout reader failed: %s" % line[1])
        if line is None:
            raise McpTransportError("MCP exited before answering %s (id=%d)" % (method, identity))
        raw = line.rstrip("\r\n")
        try:
            parsed = json.loads(raw)
        except json.JSONDecodeError as exc:
            raise McpTransportError("MCP stdout carried a non-JSON line: %r" % raw[:200]) from exc
        if not isinstance(parsed, dict) or parsed.get("jsonrpc") != "2.0":
            raise McpTransportError("MCP stdout line was not a JSON-RPC 2.0 object: %r" % raw[:200])
        if parsed.get("id") != identity:
            raise McpTransportError(
                "MCP response id %r does not match request id %d" % (parsed.get("id"), identity)
            )
        return {"id": identity, "method": method, "elapsedSeconds": elapsed, "raw": raw, "message": parsed}

    def call_tool(self, name: str, arguments=None, timeout: float = 30.0) -> dict:
        arguments = arguments if arguments is not None else {}
        response = self.request("tools/call", {"name": name, "arguments": arguments}, timeout=timeout)
        message = response["message"]
        record = {
            "tool": name,
            "arguments": arguments,
            "elapsedSeconds": response["elapsedSeconds"],
            "requestId": response["id"],
            "jsonRpcError": message.get("error"),
            "transportError": None,
            "isError": False,
            "structured": None,
            "contentText": "",
        }
        if message.get("error") is not None:
            record["transportError"] = message["error"]
            return record
        result = message.get("result")
        if not isinstance(result, dict):
            record["transportError"] = {"code": "invalid_result", "message": "tools/call result is not an object"}
            return record
        record["isError"] = bool(result.get("isError"))
        structured = result.get("structuredContent")
        record["structured"] = structured if isinstance(structured, (dict, list)) else None
        texts = []
        content = result.get("content")
        if isinstance(content, list):
            for item in content:
                if isinstance(item, dict) and isinstance(item.get("text"), str):
                    texts.append(item["text"])
        record["contentText"] = " ".join(texts)
        return record

    def tool_error_text(self, record: dict) -> str:
        parts = []
        if record.get("transportError"):
            parts.append(json.dumps(record["transportError"], ensure_ascii=False))
        if record.get("structured"):
            parts.append(json.dumps(record["structured"], ensure_ascii=False))
        if record.get("contentText"):
            parts.append(record["contentText"])
        return " ".join(parts)

    def close(self, timeout: float = 20.0) -> int:
        returncode = None
        if self.proc is not None:
            try:
                if self.proc.stdin:
                    self.proc.stdin.close()
            except (OSError, ValueError):
                pass
            try:
                returncode = self.proc.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                returncode = self.proc.wait()
        if self.stderr:
            try:
                self.stderr.close()
            except OSError:
                pass
        if self._reader is not None:
            self._reader.join(timeout=2.0)
        self.returncode = returncode
        return returncode

    def kill(self) -> None:
        if self.proc is not None and self.proc.poll() is None:
            self.proc.kill()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pass
        if self.stderr:
            try:
                self.stderr.close()
            except OSError:
                pass
        self.returncode = self.proc.returncode if self.proc is not None else None

    def stderr_text(self, limit: int = 4000) -> str:
        try:
            return self.stderr_path.read_text(encoding="utf-8", errors="replace")[-limit:]
        except OSError:
            return ""


# ------------------------------------------------------------ steps & polling
class Abort(Exception):
    def __init__(self, reason: str, classification: str = "harness") -> None:
        self.reason = reason
        self.classification = classification
        super().__init__(reason)


def assertion(name: str, expected, passed: bool, observed) -> dict:
    return {"name": name, "expected": expected, "passed": bool(passed), "observed": observed}


def make_step(
    step_id: str,
    title: str,
    input_info,
    observations: dict,
    assertions: list,
    failure_status: str = "product-defect",
    classification: dict = None,
    note: str = "",
) -> dict:
    ok = all(item["passed"] for item in assertions)
    entry = {
        "id": step_id,
        "title": title,
        "status": "passed" if ok else failure_status,
        "input": input_info,
        "observations": observations,
        "assertions": assertions,
        "passed": ok,
    }
    if not ok:
        entry["classification"] = classification or {"kind": "product", "reason": "assertion failed"}
    if note:
        entry["note"] = note
    return entry


def not_run_step(step_id: str, title: str, reason: str) -> dict:
    return {
        "id": step_id,
        "title": title,
        "status": "not-run",
        "input": None,
        "observations": {"reason": reason},
        "assertions": [],
        "passed": False,
        "classification": {"kind": "not-run", "reason": reason},
    }


def poll_now_playing(mcp: McpClient, predicate, timeout: float, interval: float, label: str) -> dict:
    """Poll music_now_playing until predicate(state) is true.

    pause/resume are applied by the audio thread asynchronously, so the immediate
    music_control snapshot may still hold the old value. Polling the authoritative
    session view (not the command's echo) is what proves the state actually changed.
    """
    started = time.monotonic()
    polls = []
    matched = None
    error = None
    matched_at = None
    while True:
        try:
            response = mcp.call_tool("music_now_playing", {}, timeout=max(20.0, timeout + 5.0))
        except (McpTimeout, McpTransportError) as exc:
            error = "%s: %s" % (type(exc).__name__, exc)
            break
        elapsed = round(time.monotonic() - started, 3)
        structured = response.get("structured") if isinstance(response.get("structured"), dict) else {}
        track = structured.get("track") if isinstance(structured.get("track"), dict) else None
        polls.append(
            {
                "elapsedSeconds": elapsed,
                "isError": response.get("isError"),
                "playing": structured.get("playing"),
                "paused": structured.get("paused"),
                "volumePercent": structured.get("volumePercent"),
                "trackId": track.get("id") if track else None,
                "positionSeconds": structured.get("positionSeconds"),
            }
        )
        if response.get("transportError"):
            error = "transport: " + json.dumps(response["transportError"], ensure_ascii=False)
            break
        if response.get("isError"):
            error = mcp.tool_error_text(response)
            break
        if predicate(structured):
            matched = structured
            matched_at = elapsed
            break
        if time.monotonic() - started >= timeout:
            break
        time.sleep(interval)
    return {
        "label": label,
        "matched": matched is not None,
        "matchedAtSeconds": matched_at,
        "elapsedSeconds": round(time.monotonic() - started, 3),
        "pollCount": len(polls),
        "polls": polls[:80],
        "error": error,
        "last": polls[-1] if polls else None,
        "matchedState": matched,
    }


def poll_snapshot(tui: TuiProcess, predicate, timeout: float, interval: float = 0.1) -> dict:
    started = time.monotonic()
    frame = ""
    matched = False
    while time.monotonic() - started < timeout:
        frame = tui.frame()
        if frame and predicate(frame):
            matched = True
            break
        time.sleep(interval)
    return {
        "matched": matched,
        "elapsedSeconds": round(time.monotonic() - started, 3),
        "frameLength": len(frame),
    }


def cleanup_processes(resources: dict) -> dict:
    report = {"actions": [], "killed": []}
    for role in ("mcp", "alone", "busy"):
        client = resources.get(role)
        if client is None or client.proc is None:
            continue
        if client.proc.poll() is None:
            client.kill()
            report["actions"].append({"role": role, "pid": client.proc.pid, "action": "killed-after-failure"})
            report["killed"].append(role)
        else:
            report["actions"].append(
                {"role": role, "pid": client.proc.pid, "action": "already-exited", "returncode": client.proc.returncode}
            )
    tui = resources.get("tui")
    if tui is not None and tui.proc is not None:
        if tui.proc.poll() is None:
            tui.terminate()
            report["actions"].append({"role": "tui", "pid": tui.proc.pid, "action": "terminated-after-failure"})
            report["killed"].append("tui")
        else:
            report["actions"].append(
                {"role": "tui", "pid": tui.proc.pid, "action": "already-exited", "returncode": tui.proc.returncode}
            )
        tui.stop_handles()
    return report


def audit_stdout(client: McpClient) -> dict:
    lines = [line.rstrip("\r\n") for line in list(client.lines)]
    problems = []
    parsed = []
    for index, raw in enumerate(lines):
        if not raw.strip():
            problems.append({"line": index, "kind": "blank"})
            continue
        if "\x1b" in raw:
            problems.append({"line": index, "kind": "ansi-escape", "raw": raw[:160]})
            continue
        try:
            obj = json.loads(raw)
        except json.JSONDecodeError:
            problems.append({"line": index, "kind": "non-json", "raw": raw[:160]})
            continue
        if not isinstance(obj, dict) or obj.get("jsonrpc") != "2.0":
            problems.append({"line": index, "kind": "not-jsonrpc-2.0", "raw": raw[:160]})
            continue
        if "result" not in obj and "error" not in obj:
            problems.append({"line": index, "kind": "missing-result-or-error", "raw": raw[:160]})
            continue
        parsed.append(obj)
    expected_ids = [item["id"] for item in client.requests if item.get("id") is not None]
    observed_ids = [obj.get("id") for obj in parsed]
    return {
        "lineCount": len(lines),
        "parsedResponseCount": len(parsed),
        "problems": problems,
        "expectedRequestIds": expected_ids,
        "observedResponseIds": observed_ids,
        "idsMatchInOrder": expected_ids == observed_ids,
        "ansiFound": any("ansi-escape" == item["kind"] for item in problems),
        "nonJsonFound": any(item["kind"] != "ansi-escape" for item in problems),
    }


def read_json_file(path: Path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None


def tui_failure_status(text: str) -> tuple:
    env, marker = classify_environment(text)
    if env:
        return "environment-failure", {"kind": "environment", "reason": "界面/错误提示命中环境标记: %s" % marker}
    return "product-defect", {"kind": "product", "reason": "界面没有给出可解释的环境原因"}


# ------------------------------------------------------------------- execute
def execute(args, console, harness_extra) -> dict:
    started_at = time.monotonic()
    exe = Path(args.exe).resolve()
    artifacts = Path(args.artifacts).resolve()
    artifacts.mkdir(parents=True, exist_ok=True)
    steps = []
    state = {
        "findings": [],
        "memory": None,
        "sharedState": None,
        "mcpStdout": None,
        "tuiReport": None,
        "cleanup": None,
        "environment": {
            "pipeNamePattern": "\\\\.\\pipe\\mora-player-<current user SID>",
            "mcpTransport": "stdio JSON-RPC, one UTF-8 JSON object per line",
            "tuiMode": "live (no --demo) in the hidden test console",
            "accountMode": "user's real NetEase session if present; search/play only, never a write",
        },
        "exitOverride": None,
        "abort": None,
        "binaryBefore": None,
        "snapshotInfo": None,
    }
    resources = {"tui": None, "mcp": None, "alone": None}
    state["environment"]["pipesBefore"] = list_mora_pipes()
    binary_before = fingerprint(exe)
    state["binaryBefore"] = binary_before

    try:
        try:
            # ---------------------------------------------------------- step 1
            expected = args.expected_sha256.upper() if args.expected_sha256 else None
            if not binary_before.get("exists"):
                steps.append(
                    make_step(
                        "1-binary-fingerprint",
                        "记录被测二进制指纹",
                        {"path": str(exe)},
                        {"exists": False},
                        [assertion("binary-exists", True, False, str(exe))],
                        failure_status="environment-failure",
                        classification={
                            "kind": "environment",
                            "reason": "build/mora.exe 不存在，无法进行端到端验收；脚本不重建",
                        },
                        note="不做任何构建或替换。",
                    )
                )
                state["exitOverride"] = ("environment-failed", 3)
                raise Abort("binary missing", "environment")
            matches = (binary_before["sha256"] == expected) if expected else True
            steps.append(
                make_step(
                    "1-binary-fingerprint",
                    "记录被测二进制指纹",
                    {
                        "path": str(exe),
                        "method": "Get-FileHash -Algorithm SHA256 equivalent (hashlib over the file bytes)",
                        "expectedSha256": expected,
                    },
                    {
                        "sizeBytes": binary_before["sizeBytes"],
                        "sha256": binary_before["sha256"],
                        "lastWriteTime": binary_before["lastWriteTime"],
                        "matchesExpected": matches,
                        "rebuiltByHarness": False,
                    },
                    [
                        assertion("binary-exists", True, True, binary_before["relativePath"]),
                        assertion("sha256-matches-expected", expected or "(record only, no --expected-sha256)", matches, binary_before["sha256"]),
                    ],
                    failure_status="harness-failure",
                    classification={"kind": "fingerprint", "reason": "SHA256 与 --expected-sha256 不一致，未执行任何被测行为"},
                    note="二进制冻结：脚本只读该文件，不构建、不替换。",
                )
            )
            if not matches:
                state["exitOverride"] = ("binary-mismatch", 2)
                raise Abort("binary fingerprint mismatch", "fingerprint")

            # ---------------------------------------------------------- step 2
            pipes_before = state["environment"]["pipesBefore"]
            steps.append(
                make_step(
                    "2-preflight",
                    "预检：没有其他 MORA 播放器占用会话管道",
                    {"listedPipeRoot": "\\\\.\\pipe\\", "observedMoraPipes": pipes_before},
                    {
                        "observedMoraPipes": pipes_before,
                        "interpretation": "空列表 = 当前用户会话没有其他 MORA 播放器占用单实例会话管道",
                    },
                    [assertion("no-foreign-mora-player", "[]", len(pipes_before) == 0, pipes_before)],
                    failure_status="environment-failure",
                    classification={
                        "kind": "environment",
                        "reason": "已有 MORA 播放器占用会话管道 %s；不能在同一用户会话里再起一个权威播放器" % (pipes_before,),
                    },
                    note="为保证负例 B 与单实例判断有效，脚本拒绝在别的播放器存在时继续；不会结束用户的播放器。",
                )
            )
            if pipes_before:
                state["exitOverride"] = ("environment-failed", 3)
                raise Abort("another MORA player owns the session pipe", "environment")

            # ---------------------------------------------------------- step 3
            alone = McpClient(exe, artifacts, "alone")
            resources["alone"] = alone
            alone_obs = {}
            alone_assertions = []
            alone_kind = "product"
            alone_reason = "无 TUI 时 MCP 仍声称可播放，或返回结构不符合 needs_player 契约"
            alone_status = "product-defect"
            try:
                alone.start()
                init = alone.request(
                    "initialize",
                    {
                        "protocolVersion": "2025-11-25",
                        "capabilities": {},
                        "clientInfo": {"name": "mora-m4-bridge-check", "version": "1"},
                    },
                    timeout=20.0,
                )
                init_result = init["message"].get("result") or {}
                alone.notify("notifications/initialized")
                tools_resp = alone.request("tools/list", {}, timeout=20.0)
                tool_names = sorted(
                    item.get("name") for item in (tools_resp["message"].get("result") or {}).get("tools", [])
                )
                status_resp = alone.call_tool("music_get_status", {}, timeout=20.0)
                status_structured = status_resp.get("structured") or {}
                attached = status_structured.get("playerSessionAttached")
                fallback = alone.call_tool(
                    "music_search",
                    {"query": args.search_query, "kind": "song", "limit": 10},
                    timeout=args.search_timeout,
                )
                fallback_structured = fallback.get("structured") or {}
                fallback_items = (
                    fallback_structured.get("items")
                    if isinstance(fallback_structured.get("items"), list)
                    else []
                )
                fallback_first = fallback_items[0] if fallback_items else None
                fallback_first_id = fallback_first.get("id") if isinstance(fallback_first, dict) else None
                fallback_ws = process_memory(alone.proc.pid)
                play_resp = alone.call_tool("music_play", {"id": STANDALONE_PROBE_ID}, timeout=30.0)
                play_structured = play_resp.get("structured") or {}
                alone_obs = {
                    "initialize": {
                        "protocolVersion": init_result.get("protocolVersion"),
                        "serverInfo": init_result.get("serverInfo"),
                    },
                    "tools": tool_names,
                    "getStatus": {"playerSessionAttached": attached, "stage": status_structured.get("stage")},
                    "musicSearchFallback": {
                        "isError": fallback.get("isError"),
                        "total": fallback_structured.get("total"),
                        "itemCount": len(fallback_items),
                        "firstItem": fallback_first,
                        "elapsedSeconds": fallback.get("elapsedSeconds"),
                        "mcpWorkingSetMB": fallback_ws.get("workingSetMB"),
                        "note": "无 TUI 时 MCP 必须本地回退联网；这会临时创建本进程 Api（工作集升到约 14 MB），是设计允许的回退，不计入双进程合计。",
                    },
                    "musicPlay": {
                        "isError": play_resp.get("isError"),
                        "structured": play_structured,
                        "content": play_resp.get("contentText")[:300],
                        "elapsedSeconds": play_resp.get("elapsedSeconds"),
                    },
                    "note": "只发一次 music_play，不重试；该 id 仅用于触发 needs_player 失败路径。search 回退必须返回真实结果。",
                }
                alone_assertions = [
                    assertion("mcp-initializes-without-tui", "serverInfo mora-music", (init_result.get("serverInfo") or {}).get("name") == "mora-music", init_result.get("serverInfo")),
                    assertion("tools-list-6", sorted(TOOL_NAMES), tool_names, tool_names),
                    assertion("playerSessionAttached", False, attached is False, attached),
                    assertion("music_search-fallback-isError", False, fallback.get("isError") is False, fallback.get("isError")),
                    assertion("music_search-fallback-real-result", ">=1 real item", len(fallback_items) >= 1 and isinstance(fallback_first_id, int) and fallback_first_id > 0, {"firstId": fallback_first_id, "total": fallback_structured.get("total"), "itemCount": len(fallback_items)}),
                    assertion("music_play-isError", True, play_resp.get("isError") is True, play_resp.get("isError")),
                    assertion("structuredContent.error", "needs_player", play_structured.get("error") == "needs_player", play_structured.get("error")),
                ]
                if attached is True or play_resp.get("isError") is False:
                    alone_kind = "environment"
                    alone_reason = "预检之后仍有播放器占用会话管道（例如并行运行的真实 TUI），负例无法隔离"
                    alone_status = "environment-failure"
            except (McpTimeout, McpTransportError, OSError) as exc:
                alone_obs = {"error": "%s: %s" % (type(exc).__name__, exc)}
                alone_assertions = [
                    assertion("mcp-alone-probe", "initialize + music_play(needs_player)", False, alone_obs["error"])
                ]
                alone_kind = "harness"
                alone_reason = "独立 MCP 进程未能完成协议往返: %s" % exc
                alone_status = "harness-failure"
            alone_rc = alone.close(timeout=20.0)
            alone_obs["exitAfterEof"] = {"returncode": alone_rc, "stderrTail": alone.stderr_text(1200)}
            steps.append(
                make_step(
                    "3-negative-alone",
                    "负例 B：MCP 无 TUI 时 music_play -> needs_player，music_search 本地回退",
                    {"command": [str(exe), "--mcp"], "tools": ["music_play", "music_search"], "arguments": {"music_play": {"id": STANDALONE_PROBE_ID}, "music_search": {"query": args.search_query}}},
                    alone_obs,
                    alone_assertions,
                    failure_status=alone_status,
                    classification={"kind": alone_kind, "reason": alone_reason},
                    note="该负例在任何 TUI 启动之前执行，确保确实没有播放器。music_search 的本地回退是设计允许路径：只有 play/control 必须 needs_player。",
                )
            )
            if not all(item["passed"] for item in alone_assertions):
                raise Abort("standalone MCP needs_player case failed", alone_kind)

            # ---------------------------------------------------------- step 4
            tui = TuiProcess(exe, console, artifacts, args.tui_seconds)
            resources["tui"] = tui
            snapshot_before_mtime = tui.snapshot.stat().st_mtime if tui.snapshot.exists() else None
            try:
                tui.start()
            except OSError as exc:
                steps.append(
                    make_step(
                        "4-tui-start",
                        "启动真实 TUI 并等待首页快照（阿木）",
                        {"argv": tui.argv},
                        {"error": str(exc)},
                        [assertion("tui-process-started", True, False, str(exc))],
                        failure_status="environment-failure",
                        classification={"kind": "environment", "reason": "无法用隐藏测试控制台启动 TUI: %s" % exc},
                    )
                )
                state["exitOverride"] = ("environment-failed", 3)
                raise Abort("cannot start TUI", "environment")
            home_frame, home_elapsed = tui.wait(lambda frame: "阿木" in frame, args.tui_timeout)
            alive = tui.proc.poll() is None
            snapshot_exists = tui.snapshot.exists()
            snapshot_mtime = tui.snapshot.stat().st_mtime if snapshot_exists else None
            snapshot_fresh = bool(snapshot_mtime and (time.time() - snapshot_mtime) < 10)
            pipes_during = list_mora_pipes()
            state["environment"]["pipesDuring"] = pipes_during
            state["snapshotInfo"] = {
                "path": relative(tui.snapshot),
                "exists": snapshot_exists,
                "mtime": snapshot_mtime,
                "mtimeBefore": snapshot_before_mtime,
                "fresh": snapshot_fresh,
                "firstHomeFrameSeconds": home_elapsed,
                "frameLength": len(home_frame or ""),
                "sha256AtHome": hashlib.sha256((home_frame or "").encode("utf-8")).hexdigest().upper(),
            }
            real_mode = ("DEMO" not in (home_frame or "")) and ("离线演示" not in (home_frame or ""))
            pipe_observed = any(name.lower().startswith("mora-player-") for name in pipes_during)
            tui_assertions = [
                assertion("home-frame-with-amu", "阿木 出现在 --snapshot 首页", home_frame is not None, home_elapsed),
                assertion("real-mode", "没有 DEMO / 离线演示 标记", real_mode, real_mode),
                assertion("snapshot-fresh", "快照文件在 10s 内被重写", snapshot_fresh, snapshot_mtime),
                assertion("tui-alive", "进程仍在运行", alive, tui.proc.returncode if not alive else "running"),
                assertion("session-pipe-observed", "出现 mora-player-* 命名管道", pipe_observed, pipes_during),
            ]
            if home_frame is not None:
                steps.append(
                    make_step(
                        "4-tui-start",
                        "启动真实 TUI 并等待首页快照（阿木）",
                        {"argv": tui.argv, "console": {"mode": console.mode, "columns": console.columns, "rows": console.rows}},
                        {
                            "firstHomeFrameSeconds": home_elapsed,
                            "pipesObserved": pipes_during,
                            "snapshot": state["snapshotInfo"],
                            "homeExcerpt": [line for line in (home_frame or "").splitlines() if "阿木" in line][:3],
                        },
                        tui_assertions,
                        failure_status="product-defect",
                        classification={"kind": "product", "reason": "TUI 启动后快照/管道/进程状态不符合预期"},
                    )
                )
            else:
                stderr_tail = tui.stderr_tail()
                rc = tui.proc.returncode if not alive else None
                env_hit, env_marker = classify_environment(stderr_tail)
                if not alive and rc == 2:
                    failure_status = "environment-failure"
                    classification = {"kind": "environment", "reason": "TUI 因另一个播放器占用会话管道而退出 (exit 2)"}
                elif not alive and env_hit:
                    failure_status = "environment-failure"
                    classification = {"kind": "environment", "reason": "TUI 启动失败并给出环境原因: %s" % env_marker}
                elif not alive:
                    failure_status = "product-defect"
                    classification = {"kind": "product", "reason": "TUI 在写出首页快照前退出（exit=%s）" % rc}
                else:
                    failure_status = "product-defect"
                    classification = {"kind": "product", "reason": "TUI 存活但 %ss 内没有写出含阿木的首页快照" % args.tui_timeout}
                steps.append(
                    make_step(
                        "4-tui-start",
                        "启动真实 TUI 并等待首页快照（阿木）",
                        {"argv": tui.argv},
                        {
                            "alive": alive,
                            "returncode": rc,
                            "stderrTail": stderr_tail,
                            "snapshot": state["snapshotInfo"],
                            "pipesObserved": pipes_during,
                        },
                        tui_assertions,
                        failure_status=failure_status,
                        classification=classification,
                    )
                )
                raise Abort("TUI did not render the home frame", classification["kind"])

            # ---------------------------------------------------------- step 5
            second = run_second_instance(exe, console, artifacts, args.second_tui_timeout)
            text = second["stderr"]
            mentions = any(
                token in text
                for token in ("another MORA player", "Only one MORA player", "already owns the session", "已有")
            )
            first_alive = tui.proc.poll() is None
            second_assertions = [
                assertion("second-tui-exit-code", 2, second["returncode"] == 2, second["returncode"]),
                assertion("second-tui-exits-promptly", "not timed out", not second["timedOut"], second["elapsedSeconds"]),
                assertion("second-tui-explains-running-player", "stderr names the running player", mentions, text[-600:]),
                assertion("first-tui-unaffected", "first TUI still running", first_alive, tui.proc.returncode if not first_alive else "running"),
            ]
            steps.append(
                make_step(
                    "5-negative-second-tui",
                    "负例 A：第二个真实 TUI 立即拒绝（exit 2）",
                    {
                        "argv": second["argv"],
                        "mode": "real TUI on the shared hidden console, no --demo",
                        "elapsedSeconds": second["elapsedSeconds"],
                    },
                    {"returncode": second["returncode"], "stderr": second["stderr"], "timedOut": second["timedOut"]},
                    second_assertions,
                    failure_status="product-defect",
                    classification={"kind": "product", "reason": "第二个实例没有被单实例会话管道拒绝，或拒绝时没有说明原因"},
                    note="期望：单实例命名管道拒绝，exit 2，并提示已有播放器在运行。",
                )
            )
            if not all(item["passed"] for item in second_assertions):
                raise Abort("second TUI was not refused cleanly", "product")

            # ---------------------------------------------------------- step 6
            mcp = McpClient(exe, artifacts, "main")
            resources["mcp"] = mcp
            mcp.start()
            init_req = {
                "protocolVersion": "2025-11-25",
                "capabilities": {},
                "clientInfo": {"name": "mora-m4-bridge-check", "version": "1"},
            }
            init = mcp.request("initialize", init_req, timeout=20.0)
            init_result = init["message"].get("result") or {}
            mcp.notify("notifications/initialized")
            tools_resp = mcp.request("tools/list", {}, timeout=20.0)
            tool_names = sorted(
                item.get("name") for item in (tools_resp["message"].get("result") or {}).get("tools", [])
            )
            handshake_audit = audit_stdout(mcp)
            instructions = str(init_result.get("instructions") or "")
            if instructions and (
                "Framework preview" in instructions
                or "read-only" in instructions
                or "Do not claim" in instructions
            ):
                state["findings"].append(
                    {
                        "id": "initialize-instructions-stale",
                        "severity": "low",
                        "kind": "product-doc",
                        "detail": "initialize 的 instructions 仍是 M0 只读骨架文案（Framework preview / read-only / Do not claim ... control），与 M4 已接通共享会话控制不一致；宿主模型会读到这段过期说明。",
                        "observed": instructions,
                    }
                )
            handshake_assertions = [
                assertion("negotiated-version", "2025-11-25", init_result.get("protocolVersion") == "2025-11-25", init_result.get("protocolVersion")),
                assertion("server-info", "mora-music", (init_result.get("serverInfo") or {}).get("name") == "mora-music", init_result.get("serverInfo")),
                assertion("tools-list-6", sorted(TOOL_NAMES), tool_names, tool_names),
                assertion("stdout-pure-json-rpc", "no ANSI / no non-JSON text", not handshake_audit["problems"], handshake_audit["problems"]),
            ]
            steps.append(
                make_step(
                    "6-mcp-handshake",
                    "MCP initialize / initialized / tools/list（6 个工具）",
                    {"initialize": init_req, "tools/list": {}},
                    {
                        "protocolVersion": init_result.get("protocolVersion"),
                        "serverInfo": init_result.get("serverInfo"),
                        "capabilities": init_result.get("capabilities"),
                        "tools": tool_names,
                        "stdoutAudit": handshake_audit,
                    },
                    handshake_assertions,
                    failure_status="product-defect",
                    classification={"kind": "product", "reason": "MCP 握手/工具清单不符合契约"},
                )
            )
            if not all(item["passed"] for item in handshake_assertions):
                raise Abort("MCP handshake failed", "product")

            # ---------------------------------------------------------- step 7
            status_resp = mcp.call_tool("music_get_status", {}, timeout=20.0)
            status_structured = status_resp.get("structured") or {}
            attached = status_structured.get("playerSessionAttached")
            session_view = status_structured.get("session")
            status_assertions = [
                assertion("isError", False, status_resp.get("isError") is False, status_resp.get("isError")),
                assertion("playerSessionAttached", True, attached is True, attached),
                assertion("session-non-empty", "dict with track/queue fields", isinstance(session_view, dict) and len(session_view) > 0, session_view),
                assertion("stage", "m4-session-bridge", status_structured.get("stage") == "m4-session-bridge", status_structured.get("stage")),
            ]
            status_audit = audit_stdout(mcp)
            steps.append(
                make_step(
                    "7-mcp-status",
                    "music_get_status: playerSessionAttached=true 且 session 非空",
                    {"tool": "music_get_status", "arguments": {}},
                    {
                        "structuredContent": {
                            "stage": status_structured.get("stage"),
                            "playerSessionAttached": attached,
                            "sessionKeys": sorted(session_view.keys()) if isinstance(session_view, dict) else None,
                            "session": session_view,
                            "implemented": status_structured.get("implemented"),
                        },
                        "elapsedSeconds": status_resp.get("elapsedSeconds"),
                        "stdoutAudit": status_audit,
                    },
                    status_assertions,
                    failure_status="product-defect",
                    classification={"kind": "product", "reason": "MCP 没有连上 TUI 的权威播放会话"},
                    note="这是 M4-A 的核心断言：MCP 与 TUI 共享同一个 PlaybackSession。",
                )
            )
            if not all(item["passed"] for item in status_assertions):
                raise Abort("MCP did not attach to the TUI session", "product")

            # ---------------------------------------------------------- step 7b
            busy = McpClient(exe, artifacts, "second-adapter")
            resources["busy"] = busy
            busy_obs = {}
            busy_assertions = []
            busy_kind = "product"
            busy_reason = "第二个适配器在会话管道被占用时没有快速返回 needs_player / 本地回退"
            busy_status = "product-defect"
            try:
                busy.start()
                busy.request(
                    "initialize",
                    {
                        "protocolVersion": "2025-11-25",
                        "capabilities": {},
                        "clientInfo": {"name": "mora-m4-bridge-check-second", "version": "1"},
                    },
                    timeout=20.0,
                )
                busy.notify("notifications/initialized")
                busy_play = busy.call_tool("music_play", {"id": 149229}, timeout=35.0)
                busy_play_sc = busy_play.get("structured") or {}
                busy_search = busy.call_tool(
                    "music_search",
                    {"query": args.search_query, "kind": "song", "limit": 10},
                    timeout=args.search_timeout,
                )
                busy_search_sc = busy_search.get("structured") or {}
                busy_items = (
                    busy_search_sc.get("items")
                    if isinstance(busy_search_sc.get("items"), list)
                    else []
                )
                busy_first = busy_items[0] if busy_items else None
                busy_first_id = busy_first.get("id") if isinstance(busy_first, dict) else None
                busy_ws = process_memory(busy.proc.pid)
                busy_obs = {
                    "note": "第一个 MCP 已独占会话管道；第二个适配器应快速 needs_player（play/control），search 回退到本进程本地联网。",
                    "musicPlay": {
                        "isError": busy_play.get("isError"),
                        "structured": busy_play_sc,
                        "elapsedSeconds": busy_play.get("elapsedSeconds"),
                        "content": busy_play.get("contentText")[:300],
                    },
                    "musicSearch": {
                        "isError": busy_search.get("isError"),
                        "total": busy_search_sc.get("total"),
                        "itemCount": len(busy_items),
                        "firstItem": busy_first,
                        "elapsedSeconds": busy_search.get("elapsedSeconds"),
                        "mcpWorkingSetMB": busy_ws.get("workingSetMB"),
                    },
                }
                busy_assertions = [
                    assertion("second-adapter-play-isError", True, busy_play.get("isError") is True, busy_play.get("isError")),
                    assertion("second-adapter-play-error", "needs_player", busy_play_sc.get("error") == "needs_player", busy_play_sc.get("error")),
                    assertion("play-prompt", "< 5s (not the 25s bridge timeout)", float(busy_play.get("elapsedSeconds") or 99) < 5.0, busy_play.get("elapsedSeconds")),
                    assertion("search-local-fallback-isError", False, busy_search.get("isError") is False, busy_search.get("isError")),
                    assertion("search-local-fallback-real", ">=1 real item", len(busy_items) >= 1 and isinstance(busy_first_id, int) and busy_first_id > 0, {"firstId": busy_first_id, "total": busy_search_sc.get("total")}),
                    assertion("search-prompt", "< 25s", float(busy_search.get("elapsedSeconds") or 99) < 25.0, busy_search.get("elapsedSeconds")),
                ]
                if busy_play.get("isError") is False:
                    busy_kind = "environment"
                    busy_reason = "第二个适配器仍拿到管道（第一个 MCP 可能未保持连接），管道忙场景未成立"
                    busy_status = "environment-failure"
            except (McpTimeout, McpTransportError, OSError) as exc:
                busy_obs = {"error": "%s: %s" % (type(exc).__name__, exc)}
                busy_assertions = [
                    assertion("second-adapter-probe", "prompt needs_player + local search fallback", False, busy_obs["error"])
                ]
                busy_kind = "harness"
                busy_reason = "第二个适配器未能完成协议往返: %s" % exc
                busy_status = "harness-failure"
            busy_rc = busy.close(timeout=20.0)
            busy_obs["exitAfterEof"] = {"returncode": busy_rc, "stderrTail": busy.stderr_text(800)}
            steps.append(
                make_step(
                    "7b-pipe-busy",
                    "负例：第二个适配器遇管道忙 -> needs_player 且 search 本地回退",
                    {"command": [str(exe), "--mcp"], "withPlayerAttached": True, "tools": ["music_play", "music_search"]},
                    busy_obs,
                    busy_assertions,
                    failure_status=busy_status,
                    classification={"kind": busy_kind, "reason": busy_reason},
                    note="管道忙（单实例、第一个适配器已连接）时不得白等 25–30s；search 的本地回退是设计允许路径，运行时按需临时建网络栈。",
                )
            )
            if not all(item["passed"] for item in busy_assertions):
                raise Abort("pipe-busy adapter did not fail/fallback promptly", busy_kind)

            # ---------------------------------------------------------- step 8
            search = mcp.call_tool(
                "music_search",
                {"query": args.search_query, "kind": "song", "limit": 10},
                timeout=args.search_timeout,
            )
            search_structured = search.get("structured") or {}
            items = search_structured.get("items") if isinstance(search_structured.get("items"), list) else []
            first_item = items[0] if items else None
            song_id = first_item.get("id") if isinstance(first_item, dict) else None
            song_name = first_item.get("name") if isinstance(first_item, dict) else None
            search_text = mcp.tool_error_text(search)
            search_ws = process_memory(mcp.proc.pid)
            search_assertions = [
                assertion("isError", False, search.get("isError") is False, search.get("isError")),
                assertion("items-non-empty", ">=1 real song", len(items) >= 1, len(items)),
                assertion("first-song-id", "positive integer", isinstance(song_id, int) and song_id > 0, song_id),
                assertion("first-song-id-149229", "149229 (data may drift)", song_id == 149229, song_id),
                assertion("first-song-name", "non-empty string", bool(song_name), song_name),
                assertion("total-reported", "167 observed", isinstance(search_structured.get("total"), int), search_structured.get("total")),
                assertion("mcp-no-local-backend", "MCP working set < 10 MB after bridged search", bool(search_ws.get("workingSetMB") is not None and search_ws["workingSetMB"] < 10.0), search_ws.get("workingSetMB")),
            ]
            if all(item["passed"] for item in search_assertions):
                search_status, search_classification = "product-defect", None
            else:
                env_hit, env_marker = classify_environment(search_text)
                if env_hit:
                    search_status = "environment-failure"
                    search_classification = {"kind": "environment", "reason": "真实联网检索被环境阻断: %s" % env_marker}
                else:
                    search_status = "product-defect"
                    search_classification = {"kind": "product", "reason": "music_search 没有返回真实结果，也没有可解释的错误"}
            steps.append(
                make_step(
                    "8-mcp-search",
                    "music_search 经桥接由 TUI 联网，取第一条 song id",
                    {"tool": "music_search", "arguments": {"query": args.search_query, "kind": "song", "limit": 10}},
                    {
                        "isError": search.get("isError"),
                        "query": search_structured.get("query"),
                        "total": search_structured.get("total"),
                        "itemCount": len(items),
                        "firstItem": first_item,
                        "firstThree": items[:3],
                        "elapsedSeconds": search.get("elapsedSeconds"),
                        "mcpWorkingSetMBAfterSearch": search_ws.get("workingSetMB"),
                        "errorText": search_text[:500] if search.get("isError") else "",
                        "note": "有 TUI 时 music_search 经桥接交给 TUI 的网络工作线程执行，MCP 不建自己的 Api；工作集保持约 9 MB 即为证据。music_play 只把 id 提交给 TUI，播放地址由 TUI 获取。",
                    },
                    search_assertions,
                    failure_status=search_status,
                    classification=search_classification or {"kind": "product", "reason": "检索结果不符合契约"},
                )
            )
            if not all(item["passed"] for item in search_assertions):
                raise Abort("real music_search failed", (search_classification or {}).get("kind", "product"))

            # ---------------------------------------------------------- step 8b
            bridged_error = mcp.call_tool(
                "music_open_collection",
                {"kind": "album", "id": 1, "limit": 5},
                timeout=args.search_timeout,
            )
            bridged_error_sc = bridged_error.get("structured") or {}
            bridged_error_code = bridged_error_sc.get("error")
            bridged_error_elapsed = float(bridged_error.get("elapsedSeconds") or 99.0)
            after_error = mcp.call_tool("music_get_status", {}, timeout=20.0)
            after_error_sc = after_error.get("structured") or {}
            error_is_network = isinstance(bridged_error_code, str) and bridged_error_code.startswith("network_")
            bridged_error_obs = {
                "tool": "music_open_collection",
                "arguments": {"kind": "album", "id": 1, "limit": 5},
                "isError": bridged_error.get("isError"),
                "errorCode": bridged_error_code,
                "elapsedSeconds": bridged_error.get("elapsedSeconds"),
                "content": bridged_error.get("contentText")[:300],
                "adapterAliveAfterError": {
                    "isError": after_error.get("isError"),
                    "playerSessionAttached": after_error_sc.get("playerSessionAttached"),
                },
                "note": "album id=1 是稳定的无效集合样本；桥接把后端错误映射为 network_*，并立即唤醒等待中的适配器帧。25s 客户端超时不应被触发。",
            }
            bridged_error_assertions = [
                assertion("bridged-error-isError", True, bridged_error.get("isError") is True, bridged_error.get("isError")),
                assertion("bridged-error-code", "network_* (e.g. network_unavailable)", error_is_network, bridged_error_code),
                assertion("bridged-error-prompt", "< 5s (not the 25s bridge timeout)", bridged_error_elapsed < 5.0, bridged_error.get("elapsedSeconds")),
                assertion("adapter-alive-after-error", "music_get_status still attached", after_error.get("isError") is False and after_error_sc.get("playerSessionAttached") is True, after_error_sc.get("playerSessionAttached")),
            ]
            if not bridged_error.get("isError"):
                bridged_error_status = "environment-failure"
                bridged_error_classification = {
                    "kind": "environment",
                    "reason": "无效集合样本 album id=1 未返回错误（外部数据可能变化），无法验证错误回帧路径",
                }
            elif not error_is_network:
                bridged_error_status = "product-defect"
                bridged_error_classification = {"kind": "product", "reason": "桥接后端错误没有映射为 network_* 代码"}
            elif bridged_error_elapsed >= 5.0:
                bridged_error_status = "product-defect"
                bridged_error_classification = {
                    "kind": "product",
                    "reason": "错误回帧耗时接近 25s 超时（%.2fs），等待中的帧没有被及时唤醒" % bridged_error_elapsed,
                }
            else:
                bridged_error_status = "product-defect"
                bridged_error_classification = {"kind": "product", "reason": "桥接错误回帧断言失败"}
            steps.append(
                make_step(
                    "8b-bridged-error",
                    "负例：桥接后端错误按 network_* 立即回帧（不超时）",
                    {"tool": "music_open_collection", "arguments": {"kind": "album", "id": 1, "limit": 5}, "clientTimeoutSeconds": args.search_timeout},
                    bridged_error_obs,
                    bridged_error_assertions,
                    failure_status=bridged_error_status,
                    classification=bridged_error_classification,
                    note="验证适配器独立 ticket 的错误回复也能唤醒等待帧，而不是白等 25–30s。",
                )
            )
            if not all(item["passed"] for item in bridged_error_assertions):
                raise Abort("bridged backend error did not answer promptly", bridged_error_status.replace("-failure", ""))

            # ---------------------------------------------------------- step 9
            play_attempts = []
            play_resp = None
            for attempt in range(1, 7):
                play_resp = mcp.call_tool("music_play", {"id": song_id}, timeout=30.0)
                play_structured = play_resp.get("structured") or {}
                play_attempts.append(
                    {
                        "attempt": attempt,
                        "elapsedSeconds": play_resp.get("elapsedSeconds"),
                        "isError": play_resp.get("isError"),
                        "structured": play_structured,
                        "content": play_resp.get("contentText")[:200],
                    }
                )
                if not play_resp.get("isError"):
                    break
                play_text = mcp.tool_error_text(play_resp)
                if "busy" in play_text:
                    time.sleep(0.8)
                    continue
                break
            play_structured = (play_resp or {}).get("structured") or {}
            accepted = (
                play_resp is not None
                and play_resp.get("isError") is False
                and play_structured.get("status") == "accepted"
            )
            play_poll = poll_now_playing(
                mcp,
                lambda current: (
                    current.get("playing") is True
                    and isinstance(current.get("track"), dict)
                    and current["track"].get("id") == song_id
                ),
                args.play_timeout,
                0.5,
                "music_play waited for playing=true",
            )
            playing_state = play_poll.get("matchedState") or {}
            track_state = playing_state.get("track") if isinstance(playing_state.get("track"), dict) else {}
            play_assertions = [
                assertion("music_play-accepted", "isError=false, status=accepted", accepted, {"isError": (play_resp or {}).get("isError"), "structured": play_structured}),
                assertion("playing-true", "playing=true within %ss" % args.play_timeout, play_poll["matched"], {"pollCount": play_poll["pollCount"], "matchedAt": play_poll["matchedAtSeconds"], "last": play_poll["last"], "error": play_poll["error"]}),
                assertion("playing-track-id", song_id, play_poll["matched"] and track_state.get("id") == song_id, track_state.get("id")),
                assertion("playing-track-name", "non-empty", bool(track_state.get("name")), track_state.get("name")),
            ]
            play_text = mcp.tool_error_text(play_resp or {}) + " " + str(play_poll.get("error") or "")
            if all(item["passed"] for item in play_assertions):
                play_status, play_classification = "product-defect", None
            else:
                env_hit, env_marker = classify_environment(play_text)
                tui_gone = tui.proc.poll() is not None
                if env_hit:
                    play_status = "environment-failure"
                    play_classification = {"kind": "environment", "reason": "播放被环境/权限/音频设备阻断: %s" % env_marker}
                elif tui_gone:
                    play_status = "harness-failure"
                    play_classification = {
                        "kind": "harness",
                        "reason": "TUI 在播放确认前退出（--seconds=%s 或外部结束），播放断言不成立，不能判产品缺陷" % args.tui_seconds,
                    }
                else:
                    play_status = "product-defect"
                    play_classification = {
                        "kind": "product",
                        "reason": "music_play 未能在 %ss 内让权威会话进入 playing（轮询错误: %s）"
                        % (args.play_timeout, (play_poll.get("error") or "无")[:160]),
                    }
            steps.append(
                make_step(
                    "9-mcp-play",
                    "music_play(id) 并轮询 music_now_playing 到 playing=true",
                    {"tool": "music_play", "arguments": {"id": song_id}, "retryOnBusy": 6},
                    {
                        "attempts": play_attempts,
                        "accepted": accepted,
                        "poll": {
                            "matched": play_poll["matched"],
                            "matchedAtSeconds": play_poll["matchedAtSeconds"],
                            "elapsedSeconds": play_poll["elapsedSeconds"],
                            "pollCount": play_poll["pollCount"],
                            "error": play_poll["error"],
                            "first": play_poll["polls"][0] if play_poll["polls"] else None,
                            "last": play_poll["last"],
                        },
                        "lastTrack": track_state or None,
                        "note": "music_play 返回 accepted 只表示已提交；真正播放以 music_now_playing.playing 为准。",
                    },
                    play_assertions,
                    failure_status=play_status,
                    classification=play_classification or {"kind": "product", "reason": "播放状态轮询未达标"},
                )
            )
            if not all(item["passed"] for item in play_assertions):
                raise Abort("playback did not start", (play_classification or {}).get("kind", "product"))

            # ---------------------------------------------------------- step 10
            tui_pid = tui.proc.pid
            mcp_pid = mcp.proc.pid
            samples = []
            sampling_error = None
            sampling_started = time.monotonic()
            while True:
                tick = time.monotonic()
                record = sample_pair(tui_pid, mcp_pid)
                record["atSeconds"] = round(tick - sampling_started, 3)
                samples.append(record)
                for role in ("tui", "mcp"):
                    if "error" in record[role]:
                        sampling_error = "%s memory sample failed: %s (%s)" % (role, record[role].get("error"), record[role].get("errorSource"))
                if sampling_error:
                    break
                if time.monotonic() - sampling_started >= args.memory_seconds:
                    break
                time.sleep(max(0.0, args.memory_interval - (time.monotonic() - tick)))
            valid = [item for item in samples if "sumWorkingSetMB" in item]
            tui_peak_ws = max((item["tui"]["workingSetMB"] for item in valid), default=None)
            tui_peak_private = max((item["tui"]["privateMB"] for item in valid), default=None)
            mcp_peak_ws = max((item["mcp"]["workingSetMB"] for item in valid), default=None)
            mcp_peak_private = max((item["mcp"]["privateMB"] for item in valid), default=None)
            sum_peak_ws = max((item["sumWorkingSetMB"] for item in valid), default=None)
            sum_peak_private = max((item["sumPrivateMB"] for item in valid), default=None)
            memory = {
                "unit": "MiB (bytes / 1048576, same as GetProcessMemoryInfo and MORA --report)",
                "scope": "TUI (live build/mora.exe) + MCP proxy (build/mora.exe --mcp) sampled at the same instant while audio is playing; the external host/terminal is not counted",
                "method": "GetProcessMemoryInfo(PROCESS_MEMORY_COUNTERS_EX): WorkingSetSize and PrivateUsage per process, summed per sample",
                "budgetMB": BUDGET_MB,
                "durationSeconds": round(time.monotonic() - sampling_started, 3),
                "intervalSeconds": args.memory_interval,
                "sampleCount": len(samples),
                "validSampleCount": len(valid),
                "processes": {
                    "tui": {"pid": tui_pid, "peakWorkingSetMB": tui_peak_ws, "peakPrivateMB": tui_peak_private},
                    "mcp": {"pid": mcp_pid, "peakWorkingSetMB": mcp_peak_ws, "peakPrivateMB": mcp_peak_private},
                },
                "sum": {
                    "peakWorkingSetMB": sum_peak_ws,
                    "peakPrivateMB": sum_peak_private,
                    "workingSetPassed": bool(sum_peak_ws is not None and sum_peak_ws < BUDGET_MB),
                    "privatePassed": bool(sum_peak_private is not None and sum_peak_private < BUDGET_MB),
                },
                "samples": samples,
                "note": "合计=两个进程同时刻求和；外部宿主/终端不计入。TUI --report 的整段峰值另见 tuiReport。",
            }
            headroom = None
            if sum_peak_ws is not None and sum_peak_private is not None:
                headroom = round(BUDGET_MB - max(sum_peak_ws, sum_peak_private), 3)
                memory["sum"]["headroomMB"] = headroom
            if headroom is not None and 0 < headroom < 5.0:
                state["findings"].append(
                    {
                        "id": "memory-headroom-narrow",
                        "severity": "medium",
                        "kind": "product-risk",
                        "detail": (
                            "TUI+MCP 同时刻合计工作集峰值距 50 MB 预算只剩 %.2f MB（本次合计 %.2f MB）；"
                            "正常的网络/TLS 缓冲波动就可能让下一次验收越线，建议在 M6 前压缩 MCP 侧网络占用或明确多进程预算口径。"
                            % (headroom, max(sum_peak_ws, sum_peak_private))
                        ),
                        "observed": {
                            "budgetMB": BUDGET_MB,
                            "sumPeakWorkingSetMB": sum_peak_ws,
                            "sumPeakPrivateMB": sum_peak_private,
                            "headroomMB": headroom,
                        },
                    }
                )
            state["memory"] = memory
            memory_assertions = [
                assertion("samples-collected", ">=5 samples", len(valid) >= 5, len(valid)),
                assertion("sum-working-set", "< %.0f MB" % BUDGET_MB, bool(sum_peak_ws is not None and sum_peak_ws < BUDGET_MB), sum_peak_ws),
                assertion("sum-private-commit", "< %.0f MB" % BUDGET_MB, bool(sum_peak_private is not None and sum_peak_private < BUDGET_MB), sum_peak_private),
            ]
            if sampling_error:
                memory_status = "harness-failure"
                memory_classification = {"kind": "harness", "reason": sampling_error}
            elif all(item["passed"] for item in memory_assertions):
                memory_status, memory_classification = "product-defect", None
            else:
                memory_status = "product-defect"
                memory_classification = {"kind": "product", "reason": "TUI+MCP 合计内存超出 50 MB 预算（工作集或私有提交）"}
            steps.append(
                make_step(
                    "10-memory",
                    "播放中同时刻采样 TUI 与 MCP 内存，合计 < 50 MB x2",
                    {"tuiPid": tui_pid, "mcpPid": mcp_pid, "seconds": args.memory_seconds, "interval": args.memory_interval},
                    {
                        "table": {
                            "process": ["tui", "mcp", "sum"],
                            "peakWorkingSetMB": [tui_peak_ws, mcp_peak_ws, sum_peak_ws],
                            "peakPrivateMB": [tui_peak_private, mcp_peak_private, sum_peak_private],
                        },
                        "budgetMB": BUDGET_MB,
                        "headroomMB": headroom,
                        "sampleCount": len(valid),
                        "口径": memory["scope"],
                        "note": memory["note"],
                        "samplingError": sampling_error,
                    },
                    memory_assertions,
                    failure_status=memory_status,
                    classification=memory_classification or {"kind": "product", "reason": "内存断言失败"},
                )
            )
            if not all(item["passed"] for item in memory_assertions):
                raise Abort("memory budget check failed", (memory_classification or {}).get("kind", "product"))

            # ---------------------------------------------------------- step 11
            track_name = str(track_state.get("name") or "")
            name_core = re.split(r"[\(（]", track_name)[0].strip() or track_name[:6]
            snapshot_poll = poll_snapshot(
                tui,
                lambda frame: bool(name_core) and (name_core in frame) and ("PLAYING" in frame),
                10.0,
            )
            shared_frame = tui.frame()
            excerpt = []
            for index, line in enumerate(shared_frame.splitlines()):
                if (name_core and name_core in line) or "PLAYING" in line or "PAUSED" in line:
                    excerpt.append({"line": index, "text": line.strip()[:200]})
                if len(excerpt) >= 8:
                    break
            has_name = bool(name_core) and (name_core in shared_frame)
            has_playing = "PLAYING" in shared_frame
            tui_alive_shared = tui.proc.poll() is None
            shared_state = {
                "snapshotPath": relative(tui.snapshot),
                "snapshotSha256": hashlib.sha256(shared_frame.encode("utf-8")).hexdigest().upper(),
                "frameLength": len(shared_frame),
                "trackNameFromMcp": track_name,
                "matchedNameCore": name_core,
                "hasTrackName": has_name,
                "hasPlaying": has_playing,
                "excerpt": excerpt,
                "matched": snapshot_poll["matched"],
                "elapsedSeconds": snapshot_poll["elapsedSeconds"],
            }
            state["sharedState"] = shared_state
            shared_assertions = [
                assertion("snapshot-shows-same-track", name_core or "(track name)", has_name, {"excerpt": excerpt[:4]}),
                assertion("snapshot-shows-PLAYING", "PLAYING", has_playing, {"excerpt": excerpt[:4]}),
                assertion("snapshot-is-tui-snapshot", relative(tui.snapshot), tui.snapshot.exists(), shared_state["snapshotPath"]),
                assertion("tui-alive", "running", tui_alive_shared, tui.proc.returncode if not tui_alive_shared else "running"),
            ]
            steps.append(
                make_step(
                    "11-shared-state",
                    "TUI 快照出现同一曲目名与 PLAYING",
                    {"snapshot": relative(tui.snapshot), "expectedNameCore": name_core, "source": "music_now_playing.track.name"},
                    shared_state,
                    shared_assertions,
                    failure_status="product-defect",
                    classification={"kind": "product", "reason": "TUI 的 --snapshot 没有反映 MCP 操作后的权威播放状态"},
                    note="同一曲目名来自 music_now_playing；快照由 TUI 自己每 tick 写出。",
                )
            )
            if not all(item["passed"] for item in shared_assertions):
                raise Abort("TUI snapshot does not mirror the MCP-driven playback", "product")

            # ---------------------------------------------------------- step 12
            pause_control = mcp.call_tool("music_control", {"action": "pause"}, timeout=20.0)
            pause_echo = pause_control.get("structured") or {}
            pause_poll = poll_now_playing(
                mcp,
                lambda current: current.get("paused") is True,
                args.state_timeout,
                args.state_interval,
                "pause -> paused=true",
            )
            pause_text = mcp.tool_error_text(pause_control) + " " + str(pause_poll.get("error") or "")
            pause_assertions = [
                assertion("music_control-isError", False, pause_control.get("isError") is False, pause_control.get("isError")),
                assertion("paused-true", "paused=true within %.1fs (polling)" % args.state_timeout, pause_poll["matched"], {"pollCount": pause_poll["pollCount"], "matchedAt": pause_poll["matchedAtSeconds"], "last": pause_poll["last"], "error": pause_poll["error"]}),
                assertion("poll-timing-recorded", "matchedAt <= %.1fs" % args.state_timeout, bool(pause_poll["matched"] and (pause_poll["matchedAtSeconds"] or 0) <= args.state_timeout), pause_poll["matchedAtSeconds"]),
            ]
            env_hit, env_marker = classify_environment(pause_text)
            pause_status = "product-defect"
            pause_classification = {"kind": "product", "reason": "pause 后 %.1fs 内没有观测到 paused=true" % args.state_timeout}
            if env_hit:
                pause_status = "environment-failure"
                pause_classification = {"kind": "environment", "reason": "暂停被环境阻断: %s" % env_marker}
            steps.append(
                make_step(
                    "12-mcp-pause",
                    "music_control pause（轮询 paused=true）",
                    {"tool": "music_control", "arguments": {"action": "pause"}, "pollIntervalSeconds": args.state_interval, "pollTimeoutSeconds": args.state_timeout},
                    {
                        "controlEcho": pause_echo,
                        "controlEchoPaused": pause_echo.get("paused"),
                        "poll": {
                            "matched": pause_poll["matched"],
                            "matchedAtSeconds": pause_poll["matchedAtSeconds"],
                            "elapsedSeconds": pause_poll["elapsedSeconds"],
                            "pollCount": pause_poll["pollCount"],
                            "error": pause_poll["error"],
                            "first": pause_poll["polls"][0] if pause_poll["polls"] else None,
                            "last": pause_poll["last"],
                        },
                        "note": "音频线程异步应用命令，music_control 立即读到的快照可能仍是旧值；以轮询到 paused=true 为准。",
                    },
                    pause_assertions,
                    failure_status=pause_status,
                    classification=pause_classification,
                )
            )
            if not all(item["passed"] for item in pause_assertions):
                raise Abort("pause did not reach the paused state", (pause_classification or {}).get("kind", "product"))

            # ---------------------------------------------------------- step 13
            resume_control = mcp.call_tool("music_control", {"action": "resume"}, timeout=20.0)
            resume_echo = resume_control.get("structured") or {}
            resume_poll = poll_now_playing(
                mcp,
                lambda current: current.get("paused") is False,
                args.state_timeout,
                args.state_interval,
                "resume -> paused=false",
            )
            resume_text = mcp.tool_error_text(resume_control) + " " + str(resume_poll.get("error") or "")
            resume_assertions = [
                assertion("music_control-isError", False, resume_control.get("isError") is False, resume_control.get("isError")),
                assertion("paused-false", "paused=false within %.1fs (polling)" % args.state_timeout, resume_poll["matched"], {"pollCount": resume_poll["pollCount"], "matchedAt": resume_poll["matchedAtSeconds"], "last": resume_poll["last"], "error": resume_poll["error"]}),
                assertion("poll-timing-recorded", "matchedAt <= %.1fs" % args.state_timeout, bool(resume_poll["matched"] and (resume_poll["matchedAtSeconds"] or 0) <= args.state_timeout), resume_poll["matchedAtSeconds"]),
            ]
            env_hit, env_marker = classify_environment(resume_text)
            resume_status = "product-defect"
            resume_classification = {"kind": "product", "reason": "resume 后 %.1fs 内没有观测到 paused=false" % args.state_timeout}
            if env_hit:
                resume_status = "environment-failure"
                resume_classification = {"kind": "environment", "reason": "恢复播放被环境阻断: %s" % env_marker}
            steps.append(
                make_step(
                    "13-mcp-resume",
                    "music_control resume（轮询 paused=false）",
                    {"tool": "music_control", "arguments": {"action": "resume"}, "pollIntervalSeconds": args.state_interval, "pollTimeoutSeconds": args.state_timeout},
                    {
                        "controlEcho": resume_echo,
                        "controlEchoPaused": resume_echo.get("paused"),
                        "poll": {
                            "matched": resume_poll["matched"],
                            "matchedAtSeconds": resume_poll["matchedAtSeconds"],
                            "elapsedSeconds": resume_poll["elapsedSeconds"],
                            "pollCount": resume_poll["pollCount"],
                            "error": resume_poll["error"],
                            "first": resume_poll["polls"][0] if resume_poll["polls"] else None,
                            "last": resume_poll["last"],
                        },
                    },
                    resume_assertions,
                    failure_status=resume_status,
                    classification=resume_classification,
                )
            )
            if not all(item["passed"] for item in resume_assertions):
                raise Abort("resume did not clear the paused state", (resume_classification or {}).get("kind", "product"))

            # ---------------------------------------------------------- step 14
            volume_control = mcp.call_tool(
                "music_control", {"action": "volume", "value": 40}, timeout=20.0
            )
            volume_echo = volume_control.get("structured") or {}
            volume_poll = poll_now_playing(
                mcp,
                lambda current: current.get("volumePercent") == 40,
                args.state_timeout,
                args.state_interval,
                "volume -> volumePercent=40",
            )
            volume_text = mcp.tool_error_text(volume_control) + " " + str(volume_poll.get("error") or "")
            volume_assertions = [
                assertion("music_control-isError", False, volume_control.get("isError") is False, volume_control.get("isError")),
                assertion("volumePercent-40", "40 within %.1fs (polling)" % args.state_timeout, volume_poll["matched"], {"pollCount": volume_poll["pollCount"], "matchedAt": volume_poll["matchedAtSeconds"], "last": volume_poll["last"], "error": volume_poll["error"]}),
                assertion("volume-echo-reasonable", "control echo is a snapshot; session value must converge", bool(volume_poll["matched"]), {"echo": volume_echo.get("volumePercent"), "matchedAtSeconds": volume_poll["matchedAtSeconds"]}),
            ]
            env_hit, env_marker = classify_environment(volume_text)
            volume_status = "product-defect"
            volume_classification = {"kind": "product", "reason": "volume=40 后 %.1fs 内没有观测到 volumePercent=40" % args.state_timeout}
            if env_hit:
                volume_status = "environment-failure"
                volume_classification = {"kind": "environment", "reason": "音量调整被环境阻断: %s" % env_marker}
            steps.append(
                make_step(
                    "14-mcp-volume",
                    "music_control volume=40（轮询 volumePercent=40）",
                    {"tool": "music_control", "arguments": {"action": "volume", "value": 40}, "pollIntervalSeconds": args.state_interval, "pollTimeoutSeconds": args.state_timeout},
                    {
                        "controlEcho": volume_echo,
                        "controlEchoVolumePercent": volume_echo.get("volumePercent"),
                        "poll": {
                            "matched": volume_poll["matched"],
                            "matchedAtSeconds": volume_poll["matchedAtSeconds"],
                            "elapsedSeconds": volume_poll["elapsedSeconds"],
                            "pollCount": volume_poll["pollCount"],
                            "error": volume_poll["error"],
                            "last": volume_poll["last"],
                        },
                    },
                    volume_assertions,
                    failure_status=volume_status,
                    classification=volume_classification,
                )
            )
            if not all(item["passed"] for item in volume_assertions):
                raise Abort("volume did not reach 40%", (volume_classification or {}).get("kind", "product"))

            # ---------------------------------------------------------- step 15
            status2_resp = mcp.call_tool("music_get_status", {}, timeout=20.0)
            status2 = status2_resp.get("structured") or {}
            session2 = status2.get("session") if isinstance(status2.get("session"), dict) else {}
            now2_resp = mcp.call_tool("music_now_playing", {}, timeout=20.0)
            now2 = now2_resp.get("structured") if isinstance(now2_resp.get("structured"), dict) else {}
            track2 = session2.get("track") if isinstance(session2.get("track"), dict) else {}
            track2_now = now2.get("track") if isinstance(now2.get("track"), dict) else {}
            consistent = (
                isinstance(session2, dict)
                and len(session2) > 0
                and track2.get("id") == song_id
                and track2_now.get("id") == song_id
                and session2.get("paused") is False
                and now2.get("paused") is False
                and session2.get("volumePercent") == 40
                and now2.get("volumePercent") == 40
                and session2.get("playing") is True
                and (session2.get("queueSize") or 0) >= 1
            )
            status2_assertions = [
                assertion("attached-again", True, status2.get("playerSessionAttached") is True, status2.get("playerSessionAttached")),
                assertion("session-non-empty", "dict", bool(session2), sorted(session2.keys())),
                assertion("track-id", song_id, track2.get("id") == song_id, track2.get("id")),
                assertion("paused-false", False, session2.get("paused") is False, session2.get("paused")),
                assertion("volumePercent", 40, session2.get("volumePercent") == 40, session2.get("volumePercent")),
                assertion("playing-true", True, session2.get("playing") is True, session2.get("playing")),
                assertion("queueSize", ">=1", (session2.get("queueSize") or 0) >= 1, session2.get("queueSize")),
                assertion("status-matches-now-playing", "track id, paused, volume, playing agree", consistent, {"status": {"trackId": track2.get("id"), "paused": session2.get("paused"), "volumePercent": session2.get("volumePercent"), "playing": session2.get("playing"), "queueSize": session2.get("queueSize")}, "nowPlaying": {"trackId": track2_now.get("id"), "paused": now2.get("paused"), "volumePercent": now2.get("volumePercent"), "playing": now2.get("playing")}}),
            ]
            steps.append(
                make_step(
                    "15-mcp-status-again",
                    "再次 music_get_status 与 now_playing 一致",
                    {"tools": ["music_get_status", "music_now_playing"], "expected": {"trackId": song_id, "paused": False, "volumePercent": 40}},
                    {
                        "status": {"stage": status2.get("stage"), "playerSessionAttached": status2.get("playerSessionAttached"), "session": session2},
                        "nowPlaying": now2,
                        "elapsedSeconds": {"status": status2_resp.get("elapsedSeconds"), "nowPlaying": now2_resp.get("elapsedSeconds")},
                    },
                    status2_assertions,
                    failure_status="product-defect",
                    classification={"kind": "product", "reason": "get_status 与 now_playing 对同一权威会话给出不一致状态"},
                    note="两个只读视图都来自 TUI 的 PlaybackSession，必须一致。",
                )
            )
            if not all(item["passed"] for item in status2_assertions):
                raise Abort("get_status disagrees with now_playing", "product")

            # ---------------------------------------------------------- step 16
            eof_rc = mcp.close(timeout=args.eof_timeout)
            stdout_audit = audit_stdout(mcp)
            state["mcpStdout"] = stdout_audit
            mcp_stderr = mcp.stderr_text()
            expected_line_count = len([item for item in mcp.requests if item.get("id") is not None])
            eof_assertions = [
                assertion("exit-code", 0, eof_rc == 0, eof_rc),
                assertion("stdout-line-count", expected_line_count, stdout_audit["lineCount"] == expected_line_count, {"lines": stdout_audit["lineCount"], "expected": expected_line_count}),
                assertion("stdout-only-json-rpc", "no ANSI, no free text", len(stdout_audit["problems"]) == 0, stdout_audit["problems"]),
                assertion("response-ids", "one response per request, in order", stdout_audit["idsMatchInOrder"], {"expected": stdout_audit["expectedRequestIds"], "observed": stdout_audit["observedResponseIds"]}),
                assertion("stderr-no-ansi", "no terminal escapes on stderr", "\x1b" not in mcp_stderr, mcp_stderr[-400:]),
            ]
            steps.append(
                make_step(
                    "16-mcp-eof",
                    "关闭 stdin：MCP 退出 0 且 stdout 只有 JSON-RPC",
                    {"action": "close stdin (EOF)", "timeoutSeconds": args.eof_timeout},
                    {
                        "returncode": eof_rc,
                        "stdout": stdout_audit,
                        "stderrTail": mcp_stderr,
                        "note": "stdout 只允许一行一个 JSON-RPC 2.0 对象；带有 ANSI 或杂文本即视为破坏宿主解析。",
                    },
                    eof_assertions,
                    failure_status="product-defect",
                    classification={"kind": "product", "reason": "MCP 在 EOF 后没有干净退出，或 stdout 不纯"},
                )
            )
            if not all(item["passed"] for item in eof_assertions):
                raise Abort("MCP did not exit cleanly on EOF", "product")

            # ---------------------------------------------------------- step 17
            quit_rc = tui.quit(args.quit_timeout)
            tui_release_started = time.monotonic()
            released = False
            while time.monotonic() - tui_release_started < 5.0:
                if not list_mora_pipes():
                    released = True
                    break
                time.sleep(0.2)
            tui.stop_handles()
            report = read_json_file(tui.report)
            state["tuiReport"] = {
                "path": relative(tui.report),
                "written": bool(report),
                "raw": report,
                "fieldsOfInterest": {
                    "mode": (report or {}).get("mode"),
                    "peakWorkingSetMB": (report or {}).get("peakWorkingSetMB"),
                    "peakPrivateMB": (report or {}).get("peakPrivateMB"),
                    "budgetMB": (report or {}).get("budgetMB"),
                    "passed": (report or {}).get("passed"),
                    "audio": (report or {}).get("audio"),
                    "audioPlayed": (report or {}).get("audioPlayed"),
                    "maxPositionSeconds": (report or {}).get("maxPositionSeconds"),
                    "audioError": (report or {}).get("audioError"),
                    "actualQuality": (report or {}).get("actualQuality"),
                    "sampleRate": (report or {}).get("sampleRate"),
                    "sampleBits": (report or {}).get("sampleBits"),
                    "seconds": (report or {}).get("seconds"),
                },
            }
            peaks_ok = bool(
                report
                and isinstance(report.get("peakWorkingSetMB"), (int, float))
                and isinstance(report.get("peakPrivateMB"), (int, float))
            )
            audio_played = bool((report or {}).get("audioPlayed"))
            max_position = (report or {}).get("maxPositionSeconds") or 0
            audio_error = str((report or {}).get("audioError") or "")
            cleanup_assertions = [
                assertion("tui-exit-code", 0, quit_rc == 0, quit_rc),
                assertion("report-written", relative(tui.report), bool(report), state["tuiReport"]["path"]),
                assertion("report-mode", "live", (report or {}).get("mode") == "live", (report or {}).get("mode")),
                assertion("report-peaks", "peakWorkingSetMB + peakPrivateMB", peaks_ok, {"peakWorkingSetMB": (report or {}).get("peakWorkingSetMB"), "peakPrivateMB": (report or {}).get("peakPrivateMB")}),
                assertion("audio-evidence", "audioPlayed=true, maxPositionSeconds>0.5", audio_played and max_position > 0.5, {"audioPlayed": audio_played, "maxPositionSeconds": max_position, "audioError": audio_error}),
                assertion("pipe-released", "no mora-player-* pipe", released, list_mora_pipes()),
            ]
            cleanup_status = "product-defect"
            cleanup_classification = {"kind": "product", "reason": "TUI 没有干净退出、报告缺失或音频证据不足"}
            if quit_rc != 0 or not report:
                cleanup_status = "harness-failure"
                cleanup_classification = {"kind": "harness", "reason": "/quit 注入或报告写出失败（隐藏控制台限制）"}
            elif not audio_played and audio_error:
                cleanup_status = "environment-failure"
                cleanup_classification = {"kind": "environment", "reason": "TUI 报告音频设备错误: %s" % audio_error[:160]}
            steps.append(
                make_step(
                    "17-tui-cleanup",
                    "TUI /quit 退出、报告写出、管道释放",
                    {"command": "/quit via WriteConsoleInputW", "report": relative(tui.report)},
                    {
                        "returncode": quit_rc,
                        "pipesAfter": list_mora_pipes(),
                        "report": state["tuiReport"],
                        "stderrTail": tui.stderr_tail(1200),
                    },
                    cleanup_assertions,
                    failure_status=cleanup_status,
                    classification=cleanup_classification,
                    note="TUI 的 --report 只在本进程内记录 peakWorkingSetMB/peakPrivateMB；多进程合计口径见 memory 步骤。",
                )
            )
            if not all(item["passed"] for item in cleanup_assertions):
                raise Abort("TUI cleanup/report failed", (cleanup_classification or {}).get("kind", "product"))

        finally:
            state["cleanup"] = cleanup_processes(resources)
    except Abort as exc:
        state["abort"] = {"reason": exc.reason, "classification": exc.classification}

    # ---------------------------------------------------------- step 18
    binary_after = fingerprint(exe)
    state["binaryAfter"] = binary_after
    fingerprint_changed = bool(
        binary_before.get("exists")
        and binary_after.get("exists")
        and (binary_after.get("sha256") != binary_before.get("sha256"))
    )
    if fingerprint_changed:
        state["exitOverride"] = ("binary-replaced", 2)
    steps.append(
        make_step(
            "18-fingerprint-stable",
            "运行期间二进制未被替换",
            {
                "path": str(exe),
                "sha256Before": binary_before.get("sha256"),
                "sha256After": binary_after.get("sha256"),
            },
            {
                "before": binary_before,
                "after": binary_after,
                "replacedDuringRun": fingerprint_changed,
                "rebuiltByHarness": False,
            },
            [
                assertion("binary-still-exists", True, bool(binary_after.get("exists")), binary_after.get("path")),
                assertion("sha256-unchanged", binary_before.get("sha256"), not fingerprint_changed, binary_after.get("sha256")),
            ],
            failure_status="harness-failure",
            classification={
                "kind": "fingerprint",
                "reason": "被测二进制在运行期间被替换；脚本没有重建，按指纹不符退出 2，结果不可采信",
            },
        )
    )

    # Fill every step that never ran (after an abort) so the report is complete.
    missing_reason = state["abort"]["reason"] if state.get("abort") else "前置步骤未通过，未运行"
    by_id = {item["id"]: item for item in steps}
    ordered = []
    for step_id, title in STEP_ORDER:
        ordered.append(by_id.get(step_id) or not_run_step(step_id, title, missing_reason))
    steps = ordered

    # ---------------------------------------------------------- step 19
    failed_steps = [
        item for item in steps if item["status"] in ("environment-failure", "product-defect", "harness-failure")
    ]
    failures = [
        {"step": item["id"], "status": item["status"], "classification": item.get("classification")}
        for item in failed_steps
    ]
    environment_failures = [item["id"] for item in failed_steps if item["status"] == "environment-failure"]
    product_defects = [item["id"] for item in failed_steps if item["status"] == "product-defect"]
    harness_failures = [item["id"] for item in failed_steps if item["status"] == "harness-failure"]
    not_run = [item["id"] for item in steps if item["status"] == "not-run"]
    unverified = [
        "真实 MCP 宿主（DST 等）的生命周期：本次由脚本直接管理 MCP 的 stdin/stdout；宿主关闭/重启后的重连与“关闭宿主是否停止播放”未验证。",
        "命名管道安全边界：只验证了同用户第二个实例被拒绝（exit 2）；DACL 仅当前用户、nMaxInstances=1 未用第二个 Windows 用户令牌或外部工具验证。",
        "M4-B 账号写工具（红心/加入歌单）本版未实现，未验证；本次按约束没有执行任何账号写操作。",
        "内置模型与 LLM 工具循环（M3）未实现，未验证。",
        "一小时双进程并发播放的内存漂移未验证；本次只在播放中采样约 %s 秒。" % args.memory_seconds,
        "music_open_collection 的真实联网分页未在本次逐项验证（M0/M1 与离线边界测试已覆盖）。",
        "真实 VIP/Hi-Res 音质下的双进程合计内存未单独验证；本次为当前账号实际返回的音质。",
        "第二个实例的提示只在共享隐藏控制台的 stderr 中核对，未在用户可见终端窗口里做人工可读性确认。",
        "合计口径只覆盖 MORA 自身的 TUI 与 MCP 两个进程；Windows 音频/网络系统进程与宿主终端不计入，也没有把 MCP 的瞬时峰值并入合计（只按同一时刻求和）。",
        "本次未做解灰、换音源、试听绕过、随机 IP 或风控规避；所有播放 id 都来自 music_search 的真实返回。",
    ]
    memory_sum = (state.get("memory") or {}).get("sum")
    classification_observations = {
        "failedSteps": failures,
        "environmentFailures": environment_failures,
        "productDefects": product_defects,
        "harnessFailures": harness_failures,
        "notRun": not_run,
        "abort": state.get("abort"),
        "findings": state["findings"],
        "unverified": unverified,
        "memorySummary": memory_sum,
        "memoryMeasurementPolicy": (
            "合计=两个 MORA 进程（TUI + MCP 代理）在同一时刻的 WorkingSetSize 与 PrivateUsage 分别求和；"
            "外部宿主/终端不计入；TUI 自己的 --report 只给出单进程整段峰值。"
        ),
        "policy": (
            "不修改用户真实歌单、不红心/收藏、不解灰、不换音源、不使用随机 IP；"
            "播放只使用 music_search 真实返回的 id；不对失败操作自动重试（仅对明确的 busy 重提一次播放意图）"
        ),
    }
    classified = all(item.get("classification") for item in failed_steps)
    classification_assertions = [
        assertion("every-failure-classified", "environment/product/harness", classified, failures),
        assertion("unverified-recorded", "non-empty", len(unverified) > 0, len(unverified)),
        assertion("findings-recorded", "list", isinstance(state["findings"], list), len(state["findings"])),
    ]
    steps.append(
        make_step(
            CLASSIFICATION_STEP[0],
            CLASSIFICATION_STEP[1],
            {"kind": "derived", "dependsOn": [item["id"] for item in steps]},
            classification_observations,
            classification_assertions,
            failure_status="harness-failure",
            classification={"kind": "harness", "reason": "失败步骤缺少分类或未验证项为空"},
            note="环境失败（网络/权限/音频设备/并发播放器）与产品缺陷严格分开；harness 失败不能当作产品结论。",
        )
    )
    state["unverified"] = unverified
    state["failures"] = failures
    return build_payload(args, exe, steps, state, started_at, harness_extra, console)


# -------------------------------------------------------------------- payload
def verdict_summary(verdict: str) -> str:
    return {
        "passed": "通过：MCP 与 TUI 共享同一 PlaybackSession；播放、暂停、恢复、音量与两次状态查询一致；双进程合计内存低于 50 MB。",
        "product-defect": "产品缺陷：存在没有达到预期、且界面/接口也没有给出环境原因的行为。",
        "harness-failed": "测试装置失败：隐藏控制台、进程驱动或内存采样失败，不能当作产品结论。",
        "environment-failed": "环境失败：被网络/权限/音频设备/并发播放器阻断，MCP 与界面如实返回错误；未发现产品缺陷，也未绕过权限。",
        "binary-mismatch": "被测二进制 SHA256 与期望值不一致，未采信测试结果；未重建。",
        "binary-replaced": "运行期间 build/mora.exe 被替换（脚本没有重建），指纹不符，结果不可采信。",
        "incomplete": "未完成：部分步骤因前置失败未运行。",
    }.get(verdict, verdict)


def compute_verdict(steps, state):
    override = state.get("exitOverride")
    if override:
        verdict, code = override
        return verdict, code, verdict_summary(verdict)
    product = [item["id"] for item in steps if item["status"] == "product-defect"]
    harness = [item["id"] for item in steps if item["status"] == "harness-failure"]
    environment = [item["id"] for item in steps if item["status"] == "environment-failure"]
    pending = [item["id"] for item in steps if item["status"] == "not-run"]
    if product:
        return "product-defect", 1, verdict_summary("product-defect")
    if harness:
        return "harness-failed", 1, verdict_summary("harness-failed")
    if environment:
        return "environment-failed", 3, verdict_summary("environment-failed")
    if pending:
        return "incomplete", 1, verdict_summary("incomplete")
    return "passed", 0, verdict_summary("passed")


def build_payload(args, exe: Path, steps, state, started_at, harness_extra, console) -> dict:
    binary_before = state.get("binaryBefore") or fingerprint(exe)
    binary_after = state.get("binaryAfter")
    expected = args.expected_sha256.upper() if args.expected_sha256 else None
    replaced = bool(
        binary_before.get("exists")
        and isinstance(binary_after, dict)
        and binary_after.get("exists")
        and binary_after.get("sha256") != binary_before.get("sha256")
    )
    matches = (binary_before.get("sha256") == expected) if expected else True
    verdict, code, summary = compute_verdict(steps, state)
    memory = state.get("memory") or {}
    memory_sum = memory.get("sum") or {}
    artifacts_root = Path(args.artifacts).resolve()
    artifact_paths = []
    for name in (
        SNAPSHOT_NAME,
        REPORT_NAME,
        "tui-stderr.txt",
        "mcp-main-stderr.txt",
        "mcp-alone-stderr.txt",
        "second-tui-stderr.txt",
        "detached-worker.log",
    ):
        candidate = artifacts_root / name
        if candidate.exists():
            artifact_paths.append(relative(candidate))
    console_info = (
        {
            "mode": console.mode,
            "columns": console.columns,
            "rows": console.rows,
            "size": console.size,
            "acquireSteps": console.acquire_steps,
        }
        if console is not None
        else {"mode": "unavailable", **harness_extra}
    )
    environment = dict(state.get("environment") or {})
    environment["pipesAfter"] = list_mora_pipes()
    environment["audioDevice"] = "real system audio endpoint of the current Windows user session"
    environment["network"] = "real NetEase endpoints through the frozen binary; no proxy, no random IP headers"
    processes = {}
    if memory.get("processes"):
        processes = {
            "tuiPid": memory["processes"]["tui"].get("pid"),
            "mcpPid": memory["processes"]["mcp"].get("pid"),
        }
    payload = {
        "schema": "mora-m4-bridge/1",
        "generatedAt": now_iso(),
        "harness": {
            "script": "scripts/m4_bridge_check.py",
            "command": " ".join(sys.argv),
            "pythonVersion": sys.version.split()[0],
            "platform": platform.platform(),
            "host": platform.node(),
            "console": console_info,
            "elapsedSeconds": round(time.monotonic() - started_at, 3),
            "networkRequired": True,
            "binaryFrozen": True,
            "rebuiltByHarness": False,
            "repeatable": True,
            "artifacts": relative(artifacts_root),
            "note": (
                "被测二进制为 build/mora.exe；脚本只读该文件，不构建、不替换。"
                "临时快照/报告/日志写入 build/m4-bridge/（.gitignore 已忽略 /build/）。"
                "记录 sha256 的方式与 Get-FileHash -Algorithm SHA256 等价。"
            ),
            **harness_extra,
        },
        "binary": {
            **(binary_before if binary_before.get("exists") else {"path": str(exe), "exists": False}),
            "expectedSha256": expected,
            "matchesExpected": matches,
            "fingerprintBefore": binary_before,
            "fingerprintAfter": binary_after,
            "replacedDuringRun": replaced,
            "rebuiltByHarness": False,
        },
        "environment": environment,
        "processes": processes,
        "steps": steps,
        "sharedState": state.get("sharedState"),
        "memory": memory,
        "tuiReport": state.get("tuiReport"),
        "mcpStdout": state.get("mcpStdout"),
        "cleanup": state.get("cleanup"),
        "failures": state.get("failures", []),
        "findings": state.get("findings", []),
        "artifacts": artifact_paths,
        "conclusion": {
            "verdict": verdict,
            "suggestedExitCode": code,
            "summary": summary,
            "productDefects": [item["id"] for item in steps if item["status"] == "product-defect"],
            "environmentFailures": [item["id"] for item in steps if item["status"] == "environment-failure"],
            "harnessFailures": [item["id"] for item in steps if item["status"] == "harness-failure"],
            "notRun": [item["id"] for item in steps if item["status"] == "not-run"],
            "failedSteps": state.get("failures", []),
            "unverified": state.get("unverified", []),
            "findings": state.get("findings", []),
            "abort": state.get("abort"),
            "memory": {
                "budgetMB": BUDGET_MB,
                "sumPeakWorkingSetMB": memory_sum.get("peakWorkingSetMB"),
                "sumPeakPrivateMB": memory_sum.get("peakPrivateMB"),
                "headroomMB": memory_sum.get("headroomMB"),
                "workingSetPassed": memory_sum.get("workingSetPassed"),
                "privatePassed": memory_sum.get("privatePassed"),
            },
        },
    }
    return payload


def exit_code_for(payload: dict, allow_environment_failures: bool) -> int:
    conclusion = payload.get("conclusion") or {}
    verdict = conclusion.get("verdict")
    code = conclusion.get("suggestedExitCode", 1)
    if verdict == "environment-failed" and allow_environment_failures:
        return 0
    return code


def write_payload(payload: dict, out: Path, exit_code: int) -> None:
    payload["exitCode"] = exit_code
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def print_summary(payload: dict) -> None:
    binary = payload.get("binary") or {}
    print("")
    print(
        "binary           : %s sha256 %s"
        % (binary.get("relativePath") or binary.get("path"), (binary.get("sha256") or "n/a")[:16])
    )
    for item in payload.get("steps", []):
        suffix = ""
        if item["status"] != "passed":
            classification = item.get("classification") or {}
            suffix = "  [%s] %s" % (classification.get("kind"), classification.get("reason"))
        print("  %-24s %-18s%s" % (item["id"], item["status"], suffix))
    memory = payload.get("memory") or {}
    memory_sum = memory.get("sum") or {}
    if memory_sum:
        print(
            "memory sum       : working set %.2f MB / private %.2f MB (budget %.0f MB)"
            % (
                memory_sum.get("peakWorkingSetMB") or 0,
                memory_sum.get("peakPrivateMB") or 0,
                BUDGET_MB,
            )
        )
    conclusion = payload.get("conclusion") or {}
    print("verdict          : %s" % conclusion.get("verdict"))
    print("summary          : %s" % conclusion.get("summary"))
    if conclusion.get("productDefects"):
        print("productDefects   : %s" % ", ".join(conclusion["productDefects"]))
    if conclusion.get("environmentFailures"):
        print("environmentFails : %s" % ", ".join(conclusion["environmentFailures"]))
    if conclusion.get("harnessFailures"):
        print("harnessFailures  : %s" % ", ".join(conclusion["harnessFailures"]))
    if conclusion.get("notRun"):
        print("notRun           : %s" % ", ".join(conclusion["notRun"]))
    print("exitCode         : %s" % payload.get("exitCode"))
    print("report           : %s" % payload.get("_out", "docs/verification/m4-bridge.json"))


# ----------------------------------------------------------------------- main
def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="m4_bridge_check.py",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description=(
            "M4-A end-to-end acceptance for the frozen build/mora.exe: real TUI owns the\n"
            "PlaybackSession, MCP (--mcp) proxies it over the per-user named pipe, and this\n"
            "harness drives both processes, polls the authoritative state, and measures\n"
            "their combined memory while audio is playing."
        ),
        epilog=(
            "examples:\n"
            "  python scripts/m4_bridge_check.py\n"
            "  python scripts/m4_bridge_check.py --expected-sha256 <frozen exe sha256>\n"
            "  python scripts/m4_bridge_check.py --allow-environment-failures\n"
        ),
    )
    parser.add_argument("--exe", default=str(DEFAULT_EXE), help="frozen binary (default: build/mora.exe)")
    parser.add_argument("--out", default=str(DEFAULT_OUT), help="verification JSON (default: docs/verification/m4-bridge.json)")
    parser.add_argument("--artifacts", default=str(DEFAULT_ARTIFACTS), help="snapshot/report artifacts dir (default: build/m4-bridge)")
    parser.add_argument("--expected-sha256", default="", help="expected frozen SHA256 (optional; recorded only when omitted)")
    parser.add_argument("--columns", type=int, default=116, help="test console columns (default: 116)")
    parser.add_argument("--rows", type=int, default=36, help="test console rows (default: 36)")
    parser.add_argument("--tui-seconds", type=int, default=TUI_SECONDS_DEFAULT, help="TUI --seconds budget (default: 180)")
    parser.add_argument("--tui-timeout", type=float, default=25.0, help="seconds to wait for the home frame (default: 25)")
    parser.add_argument("--second-tui-timeout", type=float, default=20.0, help="seconds to wait for the rejected second TUI (default: 20)")
    parser.add_argument("--search-query", default="夜的钢琴曲 石进", help="song search used by the MCP playback step")
    parser.add_argument("--search-timeout", type=float, default=45.0, help="seconds for the real music_search (default: 45)")
    parser.add_argument("--play-timeout", type=float, default=20.0, help="seconds to poll playing=true (default: 20)")
    parser.add_argument("--state-timeout", type=float, default=2.0, help="seconds to poll pause/resume/volume target state (default: 2)")
    parser.add_argument("--state-interval", type=float, default=0.2, help="poll interval for state changes (default: 0.2)")
    parser.add_argument("--memory-seconds", type=float, default=6.0, help="same-instant memory sampling window while playing (default: 6)")
    parser.add_argument("--memory-interval", type=float, default=0.2, help="memory sampling interval (default: 0.2)")
    parser.add_argument("--eof-timeout", type=float, default=20.0, help="seconds to wait for MCP exit after stdin EOF (default: 20)")
    parser.add_argument("--quit-timeout", type=float, default=25.0, help="seconds to wait for TUI /quit (default: 25)")
    parser.add_argument("--allow-environment-failures", action="store_true", help="exit 0 when only environment failures occurred")
    parser.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--result", default="", help=argparse.SUPPRESS)
    return parser


def minimal_payload(exe: Path, args, verdict: str, summary: str, code: int, extra=None) -> dict:
    binary = fingerprint(exe) if exe.exists() else {"path": str(exe), "exists": False}
    return {
        "schema": "mora-m4-bridge/1",
        "generatedAt": now_iso(),
        "harness": {
            "script": "scripts/m4_bridge_check.py",
            "command": " ".join(sys.argv),
            "pythonVersion": sys.version.split()[0],
            "platform": platform.platform(),
            "host": platform.node(),
            "binaryFrozen": True,
            "rebuiltByHarness": False,
            "note": "未运行被测行为；脚本不构建、不替换 build/mora.exe。",
        },
        "binary": {**binary, "expectedSha256": (args.expected_sha256.upper() if args.expected_sha256 else None)},
        "environment": {"pipesBefore": list_mora_pipes()},
        "steps": [],
        "failures": [],
        "findings": [],
        "conclusion": {
            "verdict": verdict,
            "suggestedExitCode": code,
            "summary": summary,
            "unverified": [],
            **(extra or {}),
        },
    }


def main(argv=None) -> int:
    configure_stdout()
    args = build_parser().parse_args(argv)
    exe = Path(args.exe).resolve()
    out = Path(args.out).resolve()
    artifacts = Path(args.artifacts).resolve()
    if not exe.exists():
        payload = minimal_payload(
            exe,
            args,
            "environment-failed",
            "build/mora.exe 不存在，无法进行端到端验收；脚本不重建。",
            3,
            {"environmentFailures": ["binary"]},
        )
        payload["_out"] = relative(out)
        write_payload(payload, out, 3)
        print_summary(payload)
        return 3

    if args.expected_sha256:
        actual = sha256_file(exe)
        if actual != args.expected_sha256.upper():
            payload = minimal_payload(
                exe,
                args,
                "binary-mismatch",
                "被测二进制 SHA256 与 --expected-sha256 不一致，未执行测试；未重建。",
                2,
                {"actualSha256": actual, "expectedSha256": args.expected_sha256.upper()},
            )
            payload["binary"]["matchesExpected"] = False
            payload["_out"] = relative(out)
            write_payload(payload, out, 2)
            print_summary(payload)
            print("error: SHA256 mismatch\n  actual  : %s\n  expected: %s" % (actual, args.expected_sha256.upper()), file=sys.stderr)
            return 2

    if args.worker:
        result_path = Path(args.result).resolve()
        console = None
        console_error = ""
        try:
            console = TestConsole(args.columns, args.rows)
        except ConsoleUnavailable as exc:
            console_error = str(exc)
        if console is None:
            payload = minimal_payload(
                exe,
                args,
                "environment-failed",
                "DETACHED_PROCESS 后备进程仍无法创建隐藏测试控制台: %s" % console_error,
                3,
                {"environmentFailures": ["console"]},
            )
        else:
            try:
                payload = execute(args, console, {"consoleAttempt": "detached-worker", "consoleError": console_error})
            finally:
                console.close()
        payload["_out"] = relative(out)
        code = exit_code_for(payload, args.allow_environment_failures)
        write_payload(payload, out, code)
        result_path.parent.mkdir(parents=True, exist_ok=True)
        result_path.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print_summary(payload)
        return code

    console = None
    console_error = ""
    try:
        console = TestConsole(args.columns, args.rows)
    except ConsoleUnavailable as exc:
        console_error = str(exc)

    if console is None:
        print("console unavailable (%s); retrying as DETACHED_PROCESS worker ..." % console_error, flush=True)
        artifacts.mkdir(parents=True, exist_ok=True)
        result_path = artifacts / "detached-result.json"
        try:
            result_path.unlink()
        except FileNotFoundError:
            pass
        worker_cmd = [
            sys.executable,
            str(SCRIPT),
            "--worker",
            "--result",
            str(result_path),
            "--exe",
            str(exe),
            "--out",
            str(out),
            "--artifacts",
            str(artifacts),
            "--expected-sha256",
            args.expected_sha256,
            "--columns",
            str(args.columns),
            "--rows",
            str(args.rows),
            "--tui-seconds",
            str(args.tui_seconds),
            "--tui-timeout",
            str(args.tui_timeout),
            "--second-tui-timeout",
            str(args.second_tui_timeout),
            "--search-query",
            args.search_query,
            "--search-timeout",
            str(args.search_timeout),
            "--play-timeout",
            str(args.play_timeout),
            "--state-timeout",
            str(args.state_timeout),
            "--state-interval",
            str(args.state_interval),
            "--memory-seconds",
            str(args.memory_seconds),
            "--memory-interval",
            str(args.memory_interval),
            "--eof-timeout",
            str(args.eof_timeout),
            "--quit-timeout",
            str(args.quit_timeout),
        ]
        if args.allow_environment_failures:
            worker_cmd.append("--allow-environment-failures")
        log_path = artifacts / "detached-worker.log"
        with open(log_path, "wb") as log:
            completed = subprocess.run(
                worker_cmd,
                cwd=str(ROOT),
                stdout=log,
                stderr=log,
                creationflags=subprocess.DETACHED_PROCESS | subprocess.CREATE_NO_WINDOW,
                timeout=1200,
            )
        payload = read_json_file(result_path)
        if not payload:
            payload = minimal_payload(
                exe,
                args,
                "harness-failed",
                "无法创建测试控制台，DETACHED_PROCESS 后备也没有产出结果。",
                1,
                {"harnessFailures": ["console"]},
            )
            payload["harness"]["console"] = {"mode": "detached-worker", "error": console_error}
            code = 1
        else:
            payload.setdefault("harness", {})["workerReturncode"] = completed.returncode
            code = exit_code_for(payload, args.allow_environment_failures)
        payload["_out"] = relative(out)
        write_payload(payload, out, code)
        print_summary(payload)
        return code

    try:
        payload = execute(args, console, {"consoleAttempt": "direct"})
    finally:
        console.close()
    payload["_out"] = relative(out)
    code = exit_code_for(payload, args.allow_environment_failures)
    write_payload(payload, out, code)
    print_summary(payload)
    return code


if __name__ == "__main__":
    sys.exit(main())
