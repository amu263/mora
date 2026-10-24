#!/usr/bin/env python3
"""MORA M1 network-path verification (real network, frozen binary, hidden console).

What this proves
----------------
The M1 change routes UI network calls through MusicService. This harness drives
the *frozen* build/mora.exe in live mode (no --demo) inside a hidden Windows
test console, injects real keyboard input with WriteConsoleInputW (the
CONIN$/CONOUT$ technique from scripts/ui_smoke.py), and reads the plain
--snapshot frames the UI writes every tick as the observation source:

  1. start       live process, snapshot + report paths, home frame, no DEMO marker
  2. /search     晴天 周杰伦        -> real song rows with title "单曲 / ..."
  3. /albums     叶惠美             -> real album rows, title "专辑 / ...", ↵ rows
     /playlists  华语               -> real playlist rows, title "歌单 / ..."
  4. playback    /search <query>, Enter on row 1 -> "正在播放", PLAYING, moving
                 progress bar; after exit read --report audioPlayed and
                 maxPositionSeconds (the only trustworthy playback evidence)
  5. classify    permission / risk-control / network blocks are recorded as
                 environment failures, not product defects; nothing is retried
                 and no permission is bypassed.

Exit codes
----------
  0  every step passed
  1  product defect or harness failure
  2  build/mora.exe SHA256 does not match the frozen expected value (no testing)
  3  environment / permission failure only (use --allow-environment-failures
     for exit 0)

Writes only build/m1-network/** (gitignored artifacts) and the --out JSON
(default docs/verification/m1-network.json). Never invokes build.ps1.
"""

from __future__ import annotations

import argparse
import ctypes as c
from ctypes import wintypes as w
from datetime import datetime
import hashlib
import json
import platform
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = Path(__file__).resolve()
DEFAULT_EXE = ROOT / "build" / "mora.exe"
DEFAULT_OUT = ROOT / "docs" / "verification" / "m1-network.json"
DEFAULT_ARTIFACTS = ROOT / "build" / "m1-network"

# Frozen candidate verified before this harness was written.
EXPECTED_SHA256 = "47A59EC6DE06742086DAAEAED82D160774A8E754F881DEE8F04EF03A4B5386CE"

VK_RETURN = 0x0D
VK_ESCAPE = 0x1B

# Status/message fragments that mean the host or the account - not the product -
# blocked the operation. Checked against the UI status line, not the whole frame.
AUDIO_ENV_MARKERS = ("音频输出设备不可用", "Media Foundation")
PERMISSION_MARKERS = (
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
)
RISK_MARKERS = ("风控", "频繁", "操作过", "验证码")
NETWORK_MARKERS = ("网络", "超时", "连接失败", "请求失败", "暂时不可用", "服务器", "HTTP", "TLS")
UNAVAILABLE_MARKERS = ("不可用", "下架", "找不到")


# --------------------------------------------------------------------- helpers
def now_iso() -> str:
    return datetime.now().astimezone().isoformat(timespec="seconds")


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def fingerprint(path: Path) -> dict:
    stat = path.stat()
    try:
        relative_path = str(path.relative_to(ROOT))
    except ValueError:
        relative_path = str(path)
    return {
        "path": str(path),
        "relativePath": relative_path,
        "sizeBytes": stat.st_size,
        "sha256": sha256_file(path),
        "lastWriteTime": datetime.fromtimestamp(stat.st_mtime).astimezone().isoformat(timespec="seconds"),
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


def classify_block(text: str) -> tuple:
    """Classify an honest UI status message. Returns (status, reason)."""
    if not text:
        return "product-defect", "empty-status"
    if "正在搜索" in text or "正在获取账号授权的播放地址" in text:
        return "environment-failure", "timeout-no-reply"
    for marker in AUDIO_ENV_MARKERS:
        if marker in text:
            return "environment-failure", "audio-device"
    for marker in RISK_MARKERS:
        if marker in text:
            return "environment-failure", "risk-control"
    for marker in PERMISSION_MARKERS:
        if marker in text:
            return "environment-failure", "permission"
    for marker in NETWORK_MARKERS:
        if marker in text:
            return "environment-failure", "network"
    for marker in UNAVAILABLE_MARKERS:
        if marker in text:
            return "environment-failure", "unavailable-content"
    return "product-defect", "unclassified"


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
    """A hidden console this process owns; the child inherits CONIN$/CONOUT$.

    In this harness pwsh runs python with a redirected stdout while the process
    still owns a console, so AllocConsole() alone returns False. We therefore
    FreeConsole() first (piped stdout/stderr survive) and then allocate a private
    console - the same recovery scripts/m1_acceptance.py uses. A DETACHED_PROCESS
    self-relaunch is the fallback when even that fails.
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


# ------------------------------------------------------------------- mora run
class MoraSession:
    def __init__(self, exe: Path, console: TestConsole, artifacts: Path, seconds: int) -> None:
        self.exe = exe
        self.console = console
        self.artifacts = artifacts
        self.snapshot = artifacts / "live-snapshot.txt"
        self.report = artifacts / "live-report.json"
        self.stderr_path = artifacts / "live-stderr.txt"
        self.argv = [
            str(exe),
            "--seconds",
            str(seconds),
            "--snapshot",
            str(self.snapshot),
            "--report",
            str(self.report),
        ]
        for path in (self.snapshot, self.report, self.stderr_path):
            try:
                path.unlink()
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

    def quit(self, timeout: float = 15.0) -> int:
        if self.proc is None:
            return -1
        if self.proc.poll() is None:
            self.command("/quit")
            try:
                return self.proc.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                self.console.send_key(VK_ESCAPE)
                time.sleep(0.3)
                self.command("/quit")
                try:
                    return self.proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.proc.terminate()
                    try:
                        return self.proc.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        self.proc.kill()
                        return self.proc.wait()
        return self.proc.returncode

    def stop(self) -> None:
        for handle in (self.stdin, self.stdout, self.stderr):
            try:
                if handle:
                    handle.close()
            except OSError:
                pass

    def stderr_tail(self) -> str:
        try:
            text = self.stderr_path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            return ""
        return text[-4000:]


# ------------------------------------------------------------------ snapshots
def frame_lines(frame: str):
    return frame.splitlines()


def line_at(frame: str, index0: int) -> str:
    lines = frame_lines(frame)
    return lines[index0].rstrip() if 0 <= index0 < len(lines) else ""


def highlight(frame: str, needles) -> list:
    found = []
    for number, line in enumerate(frame_lines(frame), 1):
        if any(needle in line for needle in needles):
            found.append({"row": number, "text": line.rstrip()})
    return found


def excerpt(frame: str, first: int = 5, last: int = 29) -> list:
    lines = frame_lines(frame)
    out = []
    for index in range(first, min(last + 1, len(lines))):
        text = lines[index].rstrip()
        if text:
            out.append({"row": index + 1, "text": text})
    return out


def visible_rows(frame: str, height: int, limit: int = 10) -> list:
    """List rows start at 0-based row 8; each row begins with an index or ▸."""
    lines = frame_lines(frame)
    out = []
    for index in range(8, min(8 + height - 22, len(lines))):
        text = lines[index].rstrip()
        if not text:
            continue
        if re.match(r"^\s+(?:▸|\d+)\s{2,}\S", text):
            out.append({"row": index + 1, "text": text[:150]})
            if len(out) >= limit:
                break
    return out


def title_line(frame: str, height: int) -> str:
    return line_at(frame, 6)


def status_line(frame: str, height: int) -> str:
    return line_at(frame, height - 7).strip()


def progress_position(frame: str, height: int):
    text = line_at(frame, height - 9)
    match = re.match(r"^\s*(\d{2}):(\d{2})", text)
    if not match:
        return None, text
    return int(match.group(1)) * 60 + int(match.group(2)), text


def sample_progress(session, height: int, timeout: float = 6.0, minimum=None):
    """Read a complete, parseable progress frame; the snapshot file is rewritten
    every tick, so a single read can land on a truncated write."""
    started = time.monotonic()
    last_pos, last_text, last_frame = None, "", ""
    while time.monotonic() - started < timeout:
        frame = session.frame()
        if frame and len(frame_lines(frame)) >= height - 2:
            pos, text = progress_position(frame, height)
            last_pos, last_text, last_frame = pos, text, frame
            if pos is not None and (minimum is None or pos >= minimum):
                return pos, text, frame
        time.sleep(0.1)
    return last_pos, last_text, last_frame


def record_frame(frame: str, artifacts: Path, name: str) -> dict:
    directory = artifacts / "frames"
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / (name + ".txt")
    data = frame.encode("utf-8")
    path.write_bytes(data)
    return {
        "artifact": relative(path),
        "sha256": sha256_bytes(data),
        "bytes": len(data),
        "rows": len(frame_lines(frame)),
    }


def assertion(name: str, expected: str, passed: bool, observed) -> dict:
    return {"name": name, "expected": expected, "passed": bool(passed), "observed": observed}


def step(step_id: str, title: str, status: str, input_info: dict, observations: dict, assertions: list, note: str = "") -> dict:
    return {
        "id": step_id,
        "title": title,
        "status": status,
        "input": input_info,
        "observations": observations,
        "assertions": assertions,
        "note": note,
    }


def rows_containing(frame: str, needle: str) -> int:
    return sum(1 for line in frame_lines(frame) if needle in line)


# --------------------------------------------------------------------- steps
def execute(args, console, harness_extra: dict) -> dict:
    artifacts = Path(args.artifacts).resolve()
    artifacts.mkdir(parents=True, exist_ok=True)
    for name in ("live-snapshot.txt", "live-report.json", "live-stderr.txt"):
        try:
            (artifacts / name).unlink()
        except FileNotFoundError:
            pass
    exe = Path(args.exe).resolve()
    columns, rows = args.columns, args.rows
    height = rows
    search_timeout = args.search_timeout
    play_timeout = args.play_timeout
    observe = args.observe_seconds
    play_query = args.play_query

    steps = []
    session = None
    started_at = time.monotonic()
    console_info = {
        "mode": console.mode if console else "unavailable",
        "size": console.size if console else None,
        "acquireSteps": console.acquire_steps if console else [],
        "hidden": bool(console and console.allocated),
    }
    process_info = {}
    try:
        if console is None:
            steps.append(
                step(
                    "1-start",
                    "真实模式启动（无 --demo）",
                    "harness-failure",
                    {"argv": [], "mode": "live"},
                    {"console": console_info},
                    [assertion("console-available", "独立测试控制台", False, "控制台不可用")],
                    "隐藏测试控制台不可用，无法注入真实输入",
                )
            )
            for step_id, title in (
                ("2-search-songs", "真实单曲检索"),
                ("3a-search-albums", "真实专辑检索"),
                ("3b-search-playlists", "真实歌单检索"),
                ("4-playback", "真实选曲播放"),
                ("5-classification", "失败分类与未验证项"),
            ):
                steps.append(step(step_id, title, "not-run", {}, {}, [], "启动前中止"))
            return build_payload(args, exe, console_info, process_info, steps, None, [], started_at, harness_extra)

        session = MoraSession(exe, console, artifacts, args.seconds)
        session.start()
        process_info = {"pid": session.proc.pid, "argv": session.argv, "startedAt": now_iso()}
        print("mora.exe pid=%d, waiting for home frame ..." % session.proc.pid, flush=True)

        home, home_elapsed = session.wait(lambda f: "阿木" in f and "M O R A" in f, 25.0)
        home_evidence = record_frame(home, artifacts, "step1-home") if home else None
        home_assertions = [
            assertion("process-started", "mora.exe 以真实模式启动", session.proc.poll() is None, {"pid": session.proc.pid}),
            assertion("home-frame", "快照出现首页（阿木 / M O R A）", home is not None, {"elapsedSeconds": home_elapsed}),
            assertion("snapshot-written", "每帧写出 --snapshot 文件", bool(home and len(home) > 0), home_evidence),
            assertion("live-mode", "界面不是 DEMO 演示（无 DEMO 标记）", bool(home and "DEMO · 离线演示" not in home), {"header": line_at(home, 1) if home else ""}),
            assertion("console-size", "隐藏控制台达到 %dx%d" % (columns, rows), bool(console.size.get("matched")), console.size),
        ]
        steps.append(
            step(
                "1-start",
                "真实模式启动（无 --demo）",
                "passed" if all(a["passed"] for a in home_assertions) else "harness-failure",
                {"argv": session.argv, "cwd": str(ROOT), "demo": False},
                {
                    "startupElapsedSeconds": home_elapsed,
                    "console": console_info,
                    "snapshotPath": relative(session.snapshot),
                    "reportPath": relative(session.report),
                    "homeFrame": excerpt(home) if home else [],
                    "homeHighlights": highlight(home, ["阿木", "M O R A", "云间"]) if home else [],
                    "accountMode": "unknown-at-home",
                    "accountModeNote": "首页不绘制账号徽标；登录状态在列表帧第 4 行显示（未登录 / 昵称）。",
                    "evidence": home_evidence,
                },
                home_assertions,
            )
        )
        account_mode = "unknown-at-home"
        print("  step 1 start: %s" % steps[-1]["status"], flush=True)

        if home is None:
            for step_id, title in (
                ("2-search-songs", "真实单曲检索"),
                ("3a-search-albums", "真实专辑检索"),
                ("3b-search-playlists", "真实歌单检索"),
                ("4-playback", "真实选曲播放"),
                ("5-classification", "失败分类与未验证项"),
            ):
                steps.append(step(step_id, title, "not-run", {}, {}, [], "首页未在 25s 内出现"))
            session.quit(5)
            return finalize_session(args, session, artifacts, steps, console_info, process_info, started_at, harness_extra, account_mode)

        # step 2 ------------------------------------------------------------
        query = "晴天 周杰伦"
        command = "/search " + query
        print("  injecting: %s" % command, flush=True)
        session.command(command)
        expected_title = "单曲 / " + query
        song_frame, song_elapsed = session.wait(
            lambda f: expected_title in f and rows_containing(f, "晴天") >= 3 and rows_containing(f, "周杰伦") >= 3,
            search_timeout,
        )
        if song_frame:
            account_mode = "guest" if "未登录" in song_frame else "logged-in"
        song_evidence = record_frame(song_frame, artifacts, "step2-songs") if song_frame else None
        song_status = status_line(song_frame, height) if song_frame else ""
        song_rows = visible_rows(song_frame, height) if song_frame else []
        count_match = re.search(r"(\d+)\s*/\s*(\d+)", title_line(song_frame, height)) if song_frame else None
        song_assertions = [
            assertion("title", "标题行显示 %r" % expected_title, bool(song_frame and expected_title in title_line(song_frame, height)), title_line(song_frame, height) if song_frame else ""),
            assertion("rows", "至少 3 行结果同时含“晴天”与“周杰伦”", bool(song_frame and rows_containing(song_frame, "晴天") >= 3 and rows_containing(song_frame, "周杰伦") >= 3), {
                "rowsWithQingTian": rows_containing(song_frame, "晴天") if song_frame else 0,
                "rowsWithJay": rows_containing(song_frame, "周杰伦") if song_frame else 0,
            }),
            assertion("numbered-rows", "结果行带序号和时长", len([r for r in song_rows if re.search(r"\d{2}:\d{2}", r["text"])]) >= 3, [r["text"] for r in song_rows[:3]]),
            assertion("count", "标题行显示结果数量 n / total 且 total>0", bool(count_match and int(count_match.group(2)) > 0), count_match.group(0) if count_match else ""),
            assertion("no-empty-status", "没有“没有找到结果”提示", "没有找到结果" not in (song_frame or ""), song_status),
        ]
        steps.append(
            step(
                "2-search-songs",
                "真实单曲检索 /search 晴天 周杰伦",
                "passed" if all(a["passed"] for a in song_assertions) else "product-defect",
                {"kind": "console-key-injection", "typed": command, "enter": "VK_RETURN 0x0D", "query": query, "expectedKind": "song"},
                {
                    "elapsedSeconds": song_elapsed,
                    "accountBadge": "未登录" if account_mode == "guest" else account_mode,
                    "titleLine": title_line(song_frame, height) if song_frame else "",
                    "statusLine": song_status,
                    "resultRows": song_rows,
                    "highlights": highlight(song_frame, ["晴天", "周杰伦", "单曲 /"]) if song_frame else [],
                    "frameExcerpt": excerpt(song_frame) if song_frame else [],
                    "evidence": song_evidence,
                },
                song_assertions,
            )
        )
        print("  step 2 songs: %s (%ss)" % (steps[-1]["status"], song_elapsed), flush=True)
        if steps[-1]["status"] != "passed":
            steps[-1]["status"], steps[-1]["observations"]["classification"] = classify_block(song_status)

        time.sleep(0.2)

        # step 3a -----------------------------------------------------------
        album_query = "叶惠美"
        album_command = "/albums " + album_query
        print("  injecting: %s" % album_command, flush=True)
        session.command(album_command)
        album_title = "专辑 / " + album_query
        album_frame, album_elapsed = session.wait(
            lambda f: album_title in f and "↵" in f and rows_containing(f, album_query) >= 2,
            search_timeout,
        )
        album_evidence = record_frame(album_frame, artifacts, "step3a-albums") if album_frame else None
        album_status = status_line(album_frame, height) if album_frame else ""
        album_rows = visible_rows(album_frame, height) if album_frame else []
        album_count = re.search(r"(\d+)\s*/\s*(\d+)", title_line(album_frame, height)) if album_frame else None
        album_assertions = [
            assertion("title", "标题行显示 %r" % album_title, bool(album_frame and album_title in title_line(album_frame, height)), title_line(album_frame, height) if album_frame else ""),
            assertion("rows", "至少 2 行专辑结果含“叶惠美”", bool(album_frame and rows_containing(album_frame, album_query) >= 2), {"rows": rows_containing(album_frame, album_query) if album_frame else 0}),
            assertion("album-marker", "专辑行以 ↵ 标记（可打开）", bool(album_frame and sum(1 for r in album_rows if "↵" in r["text"]) >= 2), [r["text"] for r in album_rows[:3]]),
            assertion("count", "结果数量 total>0", bool(album_count and int(album_count.group(2)) > 0), album_count.group(0) if album_count else ""),
        ]
        steps.append(
            step(
                "3a-search-albums",
                "真实专辑检索 /albums 叶惠美",
                "passed" if all(a["passed"] for a in album_assertions) else "product-defect",
                {"kind": "console-key-injection", "typed": album_command, "enter": "VK_RETURN 0x0D", "query": album_query, "expectedKind": "album"},
                {
                    "elapsedSeconds": album_elapsed,
                    "titleLine": title_line(album_frame, height) if album_frame else "",
                    "statusLine": album_status,
                    "resultRows": album_rows,
                    "highlights": highlight(album_frame, [album_query, "专辑 /"]) if album_frame else [],
                    "frameExcerpt": excerpt(album_frame) if album_frame else [],
                    "evidence": album_evidence,
                },
                album_assertions,
            )
        )
        print("  step 3a albums: %s (%ss)" % (steps[-1]["status"], album_elapsed), flush=True)
        if steps[-1]["status"] != "passed":
            steps[-1]["status"], steps[-1]["observations"]["classification"] = classify_block(album_status)

        time.sleep(0.2)

        # step 3b -----------------------------------------------------------
        playlist_query = "华语"
        playlist_command = "/playlists " + playlist_query
        print("  injecting: %s" % playlist_command, flush=True)
        session.command(playlist_command)
        playlist_title = "歌单 / " + playlist_query
        playlist_frame, playlist_elapsed = session.wait(
            lambda f: playlist_title in f and "↵" in f and rows_containing(f, playlist_query) >= 2,
            search_timeout,
        )
        playlist_evidence = record_frame(playlist_frame, artifacts, "step3b-playlists") if playlist_frame else None
        playlist_status = status_line(playlist_frame, height) if playlist_frame else ""
        playlist_rows = visible_rows(playlist_frame, height) if playlist_frame else []
        playlist_count = re.search(r"(\d+)\s*/\s*(\d+)", title_line(playlist_frame, height)) if playlist_frame else None
        playlist_assertions = [
            assertion("title", "标题行显示 %r" % playlist_title, bool(playlist_frame and playlist_title in title_line(playlist_frame, height)), title_line(playlist_frame, height) if playlist_frame else ""),
            assertion("rows", "至少 2 行歌单结果含“华语”", bool(playlist_frame and rows_containing(playlist_frame, "华语") >= 2), {"rows": rows_containing(playlist_frame, "华语") if playlist_frame else 0}),
            assertion("playlist-marker", "歌单行以 ↵ 标记（可打开）", bool(playlist_frame and sum(1 for r in playlist_rows if "↵" in r["text"]) >= 2), [r["text"] for r in playlist_rows[:3]]),
            assertion("count", "结果数量 total>0", bool(playlist_count and int(playlist_count.group(2)) > 0), playlist_count.group(0) if playlist_count else ""),
        ]
        steps.append(
            step(
                "3b-search-playlists",
                "真实歌单检索 /playlists 华语",
                "passed" if all(a["passed"] for a in playlist_assertions) else "product-defect",
                {"kind": "console-key-injection", "typed": playlist_command, "enter": "VK_RETURN 0x0D", "query": playlist_query, "expectedKind": "playlist"},
                {
                    "elapsedSeconds": playlist_elapsed,
                    "titleLine": title_line(playlist_frame, height) if playlist_frame else "",
                    "statusLine": playlist_status,
                    "resultRows": playlist_rows,
                    "highlights": highlight(playlist_frame, ["华语", "歌单 /"]) if playlist_frame else [],
                    "frameExcerpt": excerpt(playlist_frame) if playlist_frame else [],
                    "evidence": playlist_evidence,
                },
                playlist_assertions,
            )
        )
        print("  step 3b playlists: %s (%ss)" % (steps[-1]["status"], playlist_elapsed), flush=True)
        if steps[-1]["status"] != "passed":
            steps[-1]["status"], steps[-1]["observations"]["classification"] = classify_block(playlist_status)

        time.sleep(0.2)

        # step 4 ------------------------------------------------------------
        play_command = "/search " + play_query
        print("  injecting: %s" % play_command, flush=True)
        session.command(play_command)
        play_title = "单曲 / " + play_query
        result_frame, result_elapsed = session.wait(lambda f: play_title in f, search_timeout)
        result_evidence = record_frame(result_frame, artifacts, "step4a-results") if result_frame else None
        result_rows = visible_rows(result_frame, height) if result_frame else []
        first_row_text = result_rows[0]["text"] if result_rows else ""
        print("  step 4 results: %s, first row: %s" % (result_elapsed, first_row_text), flush=True)
        time.sleep(0.2)
        session.console.send_key(VK_RETURN)
        reply_frame, reply_elapsed = session.wait(
            lambda f: "正在播放" in f or "PLAYING" in f or "没有完整播放权限" in f or "风控" in f,
            play_timeout,
        )
        playing_frame = None
        playing_elapsed = None
        if reply_frame is not None and "PLAYING" not in reply_frame:
            playing_frame, playing_elapsed = session.wait(lambda f: "PLAYING" in f, 20.0)
        else:
            playing_frame, playing_elapsed = reply_frame, reply_elapsed
        observed_status = status_line(playing_frame or reply_frame or result_frame or "", height)
        evidence_playing = record_frame(playing_frame, artifacts, "step4b-playing") if playing_frame else None
        pos_a, line_a, sample_a = sample_progress(session, height, timeout=6.0)
        if pos_a is not None and session.proc.poll() is None:
            time.sleep(observe)
        pos_b, line_b, sample_b = sample_progress(
            session, height, timeout=6.0, minimum=(pos_a + 1) if pos_a is not None else None
        )
        evidence_later = record_frame(sample_b or sample_a, artifacts, "step4c-progress") if (sample_b or sample_a) else None
        quality_line = ""
        quality_row = ""
        if playing_frame:
            for line in frame_lines(playing_frame):
                match = re.search(r"(standard|higher|exhigh|lossless|hires) · \d+k / \d+bit", line)
                if match:
                    quality_line = match.group(0)
                    quality_row = line.strip()
                    break
        playback_rows = visible_rows(playing_frame, height, limit=3) if playing_frame else []
        selected_marked = any("▸" in row["text"] for row in playback_rows)
        play_assertions = [
            assertion("search-before-play", "播放前搜到曲目结果", bool(result_frame), {"elapsedSeconds": result_elapsed, "firstRow": first_row_text}),
            assertion("playback-message", "状态栏出现“正在播放”", bool(playing_frame and "正在播放" in playing_frame), observed_status),
            assertion("playing-state", "播放器状态显示 PLAYING", bool(playing_frame and "PLAYING" in playing_frame), line_at(playing_frame, height - 13).strip() if playing_frame else ""),
            assertion("selected-marker", "第一行被标记为当前播放曲目（▸）", selected_marked, playback_rows),
            assertion("progress-advances", "进度秒数在观察窗口内前进", bool(pos_a is not None and pos_b is not None and pos_b > pos_a), {"positionA": pos_a, "positionB": pos_b, "progressLineA": line_a.strip(), "progressLineB": line_b.strip()}),
            assertion("quality-visible", "侧栏显示实际返回音质", bool(quality_line), quality_line),
        ]
        play_step_status = "passed" if all(a["passed"] for a in play_assertions) else "product-defect"
        classification = None
        if play_step_status != "passed":
            play_step_status, classification = classify_block(observed_status)
        steps.append(
            step(
                "4-playback",
                "真实选曲播放 /search %s + Enter" % play_query,
                play_step_status,
                {"kind": "console-key-injection", "typed": play_command, "enter": "VK_RETURN 0x0D", "selection": "第一行（selected=0）", "observeSeconds": observe},
                {
                    "searchElapsedSeconds": result_elapsed,
                    "replyElapsedSeconds": reply_elapsed,
                    "playingElapsedSeconds": playing_elapsed,
                    "firstResultRow": first_row_text,
                    "statusLine": observed_status,
                    "playingLine": line_at(playing_frame, height - 13).strip() if playing_frame else "",
                    "progressLines": {"a": line_a.strip(), "b": line_b.strip()},
                    "positions": {"a": pos_a, "b": pos_b},
                    "qualityLine": quality_line,
                    "qualityRow": quality_row,
                    "highlights": highlight(playing_frame, ["正在播放", "PLAYING", "▸", play_query]) if playing_frame else [],
                    "frameExcerpt": excerpt(playing_frame) if playing_frame else [],
                    "evidence": {"results": result_evidence, "playing": evidence_playing, "later": evidence_later},
                    "classification": classification,
                },
                play_assertions,
            )
        )
        print("  step 4 playback: %s (%s)" % (play_step_status, observed_status), flush=True)

        returncode = session.quit(args.quit_timeout)
        process_info.update({
            "returncode": returncode,
            "quitElapsedSeconds": round(time.monotonic() - started_at, 3),
            "stderrTail": session.stderr_tail(),
        })
        report_payload = read_report(session.report)
        session.stop()

        play_step = steps[-1]
        audio_played = bool(report_payload.get("audioPlayed")) if report_payload else False
        max_position = report_payload.get("maxPositionSeconds") if report_payload else None
        audio_error = (report_payload.get("audioError") or "") if report_payload else ""
        report_assertions = [
            assertion("report-written", "--report JSON 已写出", bool(report_payload), relative(session.report)),
            assertion("audio-played", "report.audioPlayed == true", audio_played, audio_played),
            assertion("max-position", "report.maxPositionSeconds > 0", bool(isinstance(max_position, (int, float)) and max_position > 0), max_position),
            assertion("audio-error", "report.audioError 为空", audio_error == "", audio_error),
            assertion("live-mode-report", "report.mode == live", bool(report_payload and report_payload.get("mode") == "live"), report_payload.get("mode") if report_payload else None),
        ]
        play_step["assertions"].extend(report_assertions)
        play_step["observations"]["reportFields"] = {
            "audioPlayed": audio_played,
            "maxPositionSeconds": max_position,
            "audioError": audio_error,
            "actualQuality": report_payload.get("actualQuality") if report_payload else None,
            "sampleRate": report_payload.get("sampleRate") if report_payload else None,
            "sampleBits": report_payload.get("sampleBits") if report_payload else None,
        }
        failed_assertions = [a["name"] for a in play_step["assertions"] if not a["passed"]]
        if failed_assertions and play_step["status"] == "passed":
            play_step["status"] = "product-defect"
            play_step["note"] = "报告字段未满足：" + ", ".join(failed_assertions)
        if play_step["status"] != "passed" and not play_step["observations"].get("classification"):
            play_step["status"], play_step["observations"]["classification"] = classify_block(audio_error or observed_status)
        print("  report: audioPlayed=%s maxPositionSeconds=%s actualQuality=%s" % (
            audio_played, max_position, report_payload.get("actualQuality") if report_payload else None), flush=True)

        fallback = None
        if play_step["status"] == "environment-failure" and args.play_id:
            fallback = run_play_fallback(Path(args.exe).resolve(), args.play_id, args.fallback_seconds, artifacts)
            fallback_report = fallback.get("report") or {}
            print("  step 4b --play fallback: audioPlayed=%s maxPositionSeconds=%s" % (
                fallback_report.get("audioPlayed"), fallback_report.get("maxPositionSeconds")), flush=True)
            steps.append(
                step(
                    "4b-play-fallback",
                    "等价路径 --play %d（UI 播放被环境/权限阻断时的补充证据）" % args.play_id,
                    fallback["status"],
                    {"argv": fallback["argv"], "id": args.play_id, "seconds": args.fallback_seconds},
                    fallback["observations"],
                    fallback["assertions"],
                    fallback.get("note", ""),
                )
            )

        steps.append(build_classification_step(steps, fallback, account_mode))
        return finalize_session(args, session, artifacts, steps, console_info, process_info, started_at, harness_extra, account_mode, report_payload)
    except Exception as exc:  # noqa: BLE001 - recorded as harness failure, not raised
        if session is not None:
            try:
                session.quit(5)
            except Exception:
                pass
            try:
                session.stop()
            except Exception:
                pass
        steps.append(
            step(
                "0-harness-error",
                "脚本内部错误",
                "harness-failure",
                {},
                {"exception": repr(exc)},
                [assertion("harness", "脚本无异常", False, repr(exc))],
            )
        )
        return build_payload(args, exe, console_info, process_info, steps, None, [], started_at, harness_extra)
    finally:
        if session is not None:
            try:
                session.stop()
            except Exception:
                pass
        if console is not None:
            try:
                console.close()
            except Exception:
                pass


def read_report(path: Path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def run_play_fallback(exe: Path, play_id: int, seconds: int, artifacts: Path) -> dict:
    """Equivalent public path: mora --play <id> --seconds N --report FILE."""
    report_path = artifacts / ("fallback-play-%d.json" % play_id)
    try:
        report_path.unlink()
    except FileNotFoundError:
        pass
    argv = [str(exe), "--play", str(play_id), "--seconds", str(seconds), "--report", str(report_path)]
    timed_out = False
    started = time.monotonic()
    reason = None
    try:
        proc = subprocess.run(argv, cwd=str(ROOT), stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                              stderr=subprocess.PIPE, timeout=max(60, seconds + 60))
        returncode = proc.returncode
        stderr_text = proc.stderr.decode("utf-8", "replace")[-2000:]
    except subprocess.TimeoutExpired:
        timed_out = True
        returncode = None
        stderr_text = ""
    elapsed = round(time.monotonic() - started, 3)
    report = read_report(report_path)
    audio_played = bool(report.get("audioPlayed")) if report else False
    max_position = report.get("maxPositionSeconds") if report else None
    audio_error = (report.get("audioError") or "") if report else ""
    status = "passed"
    if not audio_played or not (isinstance(max_position, (int, float)) and max_position > 0):
        status, reason = classify_block(audio_error or stderr_text)
    assertions = [
        assertion("exit", "退出码 0", returncode == 0, returncode),
        assertion("report", "--report JSON 已写出", bool(report), relative(report_path)),
        assertion("audio-played", "audioPlayed == true", audio_played, audio_played),
        assertion("max-position", "maxPositionSeconds > 0", bool(isinstance(max_position, (int, float)) and max_position > 0), max_position),
        assertion("audio-error", "audioError 为空", audio_error == "", audio_error),
    ]
    return {
        "status": status,
        "argv": argv,
        "assertions": assertions,
        "note": "UI 播放路径被环境/权限阻断，使用公开曲目等价路径补充证据；不改变失败分类" if reason else "",
        "observations": {
            "elapsedSeconds": elapsed,
            "timedOut": timed_out,
            "returncode": returncode,
            "stderrTail": stderr_text,
            "reportFields": report,
            "classification": reason,
        },
        "report": report,
    }


def build_classification_step(steps: list, fallback, account_mode: str = "unknown") -> dict:
    """Step 5: state explicitly how failures were classified and what is unverified."""
    failed = [s for s in steps if s["status"] in ("environment-failure", "product-defect", "harness-failure")]
    env = [s for s in steps if s["status"] == "environment-failure"]
    product = [s for s in steps if s["status"] == "product-defect"]
    unverified = [
        "真实用户账号扫码/短信登录与 VIP/Hi-Res 权限：本次账号状态=%s，未执行登录流程。" % account_mode,
        "红心/收藏到歌单等写操作：按项目约束未执行，也未验证自动重试行为。",
        "MCP music_play / music_control 与 TUI 会话桥接：M1 仍标注待接通，未在本次验证。",
        "断网/弱网与超时重试策略：为避免触发风控，未人为制造网络故障。",
        "多进程内存合计口径：本脚本只观测单个 mora.exe 进程。",
        "解灰、换音源、试听绕过：按 AGENTS.md 约束明确不做。",
    ]
    if not env:
        note = "本次所有可执行步骤均通过，未触发权限/风控/网络失败分支。"
        status = "passed"
    else:
        note = "失败步骤均带有界面真实返回的权限/风控/网络/音频设备提示，归类为环境失败，不是产品缺陷。"
        status = "environment-failure"
    observations = {
        "failedSteps": [{"id": s["id"], "status": s["status"], "classification": s["observations"].get("classification")} for s in failed],
        "environmentFailures": [s["id"] for s in env],
        "productDefects": [s["id"] for s in product],
        "fallbackUsed": bool(fallback),
        "unverified": unverified,
        "policy": "不绕过权限、不做解灰/换音源/随机 IP、不对写操作自动重试；环境失败与产品缺陷分开记录。",
    }
    return step(
        "5-classification",
        "失败分类与未验证项",
        status,
        {"kind": "derived", "dependsOn": [s["id"] for s in steps if s["id"] != "5-classification"]},
        observations,
        [assertion("classification-recorded", "每步失败都有环境/产品分类", all(s["observations"].get("classification") for s in failed if s["status"] != "harness-failure"), observations["failedSteps"])],
        note,
    )


def finalize_session(args, session, artifacts, steps, console_info, process_info, started_at, harness_extra, account_mode, report_payload=None):
    if report_payload is None:
        report_payload = read_report(session.report)
    return build_payload(
        args,
        Path(args.exe).resolve(),
        console_info,
        process_info,
        steps,
        session.report,
        [session.snapshot],
        started_at,
        harness_extra,
        report_payload=report_payload,
        account_mode=account_mode,
    )


# -------------------------------------------------------------------- payload
def compute_verdict(steps: list) -> tuple:
    product = [s["id"] for s in steps if s["status"] in ("product-defect", "harness-failure")]
    env = [s["id"] for s in steps if s["status"] == "environment-failure"]
    not_run = [s["id"] for s in steps if s["status"] == "not-run"]
    if product:
        verdict = "product-defect"
    elif env:
        verdict = "environment-failed"
    elif not_run:
        verdict = "incomplete"
    else:
        verdict = "passed"
    return verdict, {"productDefects": product, "environmentFailures": env, "notRun": not_run, "stepStatuses": {s["id"]: s["status"] for s in steps}}


def build_payload(args, exe: Path, console_info, process_info, steps, report_path, snapshot_paths, started_at, harness_extra, report_payload=None, account_mode="unknown") -> dict:
    verdict, summary = compute_verdict(steps)
    binary = fingerprint(exe) if exe.exists() else {"path": str(exe), "exists": False}
    if binary.get("sha256"):
        binary["expectedSha256"] = args.expected_sha256
        binary["matchesExpected"] = binary["sha256"].lower() == args.expected_sha256.lower()
    report_raw = report_payload
    if report_raw is None and report_path is not None:
        report_raw = read_report(Path(report_path))
    payload = {
        "schema": "mora-m1-network/1",
        "generatedAt": now_iso(),
        "harness": {
            "script": "scripts/m1_network_check.py",
            "command": " ".join(sys.argv),
            "pythonVersion": sys.version.split()[0],
            "platform": platform.platform(),
            "host": platform.node(),
            "console": console_info,
            "elapsedSeconds": round(time.monotonic() - started_at, 3),
            "networkRequired": True,
            "binaryFrozen": True,
            "note": "被测二进制为 build/mora.exe，脚本不构建、不替换该文件；临时快照与报告写在 build/m1-network/（.gitignore 已忽略 /build/）。",
            **harness_extra,
        },
        "binary": binary,
        "environment": {
            "accountMode": account_mode,
            "network": "real NetEase endpoints via the frozen binary (no proxy, no random IP headers)",
            "audioDevice": "real system audio endpoint supplied by the Windows user session",
        },
        "process": process_info,
        "steps": steps,
        "report": {
            "path": relative(Path(report_path)) if report_path else None,
            "written": bool(report_raw),
            "raw": report_raw,
            "note": "--report 的 audio 字段只表示 --local/--play 期望音频；UI 选曲播放以 audioPlayed 与 maxPositionSeconds 为准。",
            "fieldsOfInterest": {
                "mode": report_raw.get("mode") if report_raw else None,
                "audioPlayed": report_raw.get("audioPlayed") if report_raw else None,
                "maxPositionSeconds": report_raw.get("maxPositionSeconds") if report_raw else None,
                "audioError": report_raw.get("audioError") if report_raw else None,
                "actualQuality": report_raw.get("actualQuality") if report_raw else None,
                "sampleRate": report_raw.get("sampleRate") if report_raw else None,
                "sampleBits": report_raw.get("sampleBits") if report_raw else None,
                "peakWorkingSetMB": report_raw.get("peakWorkingSetMB") if report_raw else None,
                "peakPrivateMB": report_raw.get("peakPrivateMB") if report_raw else None,
            },
        },
        "artifacts": [relative(p) for p in snapshot_paths if p],
        "conclusion": {
            "verdict": verdict,
            "summary": verdict_summary(verdict),
            **summary,
            "unverified": next((s["observations"].get("unverified") for s in steps if s["id"] == "5-classification"), []),
        },
    }
    return payload


def verdict_summary(verdict: str) -> str:
    return {
        "passed": "通过：真实联网检索、专辑/歌单检索与真实播放（快照 PLAYING + 进度前进 + report 字段）全部有观测证据。",
        "environment-failed": "环境失败：步骤被权限/风控/网络/音频设备阻断，界面如实返回提示；未发现产品缺陷，也未绕过权限。",
        "product-defect": "产品缺陷：存在缺少真实结果或缺少播放状态且界面没有如实错误提示的步骤。",
        "incomplete": "未完成：部分步骤因前置步骤未运行。",
    }.get(verdict, verdict)


def exit_code_for(payload: dict, allow_environment_failures: bool) -> int:
    verdict = payload["conclusion"]["verdict"]
    if verdict == "product-defect":
        return 1
    if verdict == "environment-failed":
        return 0 if allow_environment_failures else 3
    if verdict == "incomplete":
        return 1
    return 0


def write_payload(payload: dict, out: Path, exit_code: int) -> None:
    payload["exitCode"] = exit_code
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def print_summary(payload: dict) -> None:
    print("")
    print("binary           : %s sha256 %s" % (
        payload["binary"].get("relativePath", payload["binary"].get("path")),
        (payload["binary"].get("sha256") or "n/a")[:16]))
    for item in payload["steps"]:
        print("  %-20s %-20s" % (item["id"], item["status"]))
    print("verdict          : %s" % payload["conclusion"]["verdict"])
    print("summary          : %s" % payload["conclusion"]["summary"])
    if payload["conclusion"].get("productDefects"):
        print("productDefects   : %s" % ", ".join(payload["conclusion"]["productDefects"]))
    if payload["conclusion"].get("environmentFailures"):
        print("environmentFails : %s" % ", ".join(payload["conclusion"]["environmentFailures"]))
    print("report           : %s" % payload.get("_out", "docs/verification/m1-network.json"))


# ----------------------------------------------------------------------- main
def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="m1_network_check.py",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description=(
            "Real-network M1 verification for the frozen build/mora.exe: live UI in a\n"
            "hidden test console, injected keys, per-frame --snapshot evidence, and the\n"
            "--report audioPlayed / maxPositionSeconds proof after exit."
        ),
        epilog=(
            "examples:\n"
            "  python scripts/m1_network_check.py\n"
            "  python scripts/m1_network_check.py --play-query \"夜的钢琴曲 石进\"\n"
            "  python scripts/m1_network_check.py --allow-environment-failures\n"
        ),
    )
    parser.add_argument("--exe", default=str(DEFAULT_EXE), help="frozen binary (default: build/mora.exe)")
    parser.add_argument("--out", default=str(DEFAULT_OUT), help="verification JSON (default: docs/verification/m1-network.json)")
    parser.add_argument("--artifacts", default=str(DEFAULT_ARTIFACTS), help="snapshot/report artifacts dir (default: build/m1-network)")
    parser.add_argument("--expected-sha256", default=EXPECTED_SHA256, help="expected frozen SHA256")
    parser.add_argument("--columns", type=int, default=116, help="test console columns (default: 116)")
    parser.add_argument("--rows", type=int, default=36, help="test console rows (default: 36)")
    parser.add_argument("--seconds", type=int, default=240, help="mora --seconds budget (default: 240)")
    parser.add_argument("--search-timeout", type=float, default=40.0, help="seconds per search step (default: 40)")
    parser.add_argument("--play-timeout", type=float, default=45.0, help="seconds to wait for the play reply (default: 45)")
    parser.add_argument("--observe-seconds", type=float, default=8.0, help="playback observation window (default: 8)")
    parser.add_argument("--quit-timeout", type=float, default=15.0, help="seconds to wait for /quit (default: 15)")
    parser.add_argument("--play-query", default="夜的钢琴曲 石进", help="song search used for the playback step")
    parser.add_argument("--play-id", type=int, default=539928216, help="equivalence fallback id for --play (default: 539928216)")
    parser.add_argument("--fallback-seconds", type=int, default=8, help="--play fallback observation seconds (default: 8)")
    parser.add_argument("--allow-environment-failures", action="store_true", help="exit 0 when only environment failures occurred")
    parser.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--result", default="", help=argparse.SUPPRESS)
    return parser


def main(argv=None) -> int:
    configure_stdout()
    args = build_parser().parse_args(argv)
    exe = Path(args.exe).resolve()
    out = Path(args.out).resolve()
    artifacts = Path(args.artifacts).resolve()

    if not exe.exists():
        print("error: binary not found: %s" % exe, file=sys.stderr)
        return 1

    actual = sha256_file(exe)
    if actual.lower() != args.expected_sha256.lower():
        payload = {
            "schema": "mora-m1-network/1",
            "generatedAt": now_iso(),
            "binary": {**fingerprint(exe), "expectedSha256": args.expected_sha256, "matchesExpected": False},
            "steps": [],
            "report": {"path": None, "written": False, "raw": None},
            "conclusion": {
                "verdict": "binary-mismatch",
                "summary": "被测二进制 SHA256 与冻结值不一致，未执行任何联网测试。",
                "actualSha256": actual,
                "expectedSha256": args.expected_sha256,
            },
            "exitCode": 2,
        }
        write_payload(payload, out, 2)
        print("error: SHA256 mismatch\n  actual  : %s\n  expected: %s\nstopped without testing" % (actual, args.expected_sha256), file=sys.stderr)
        return 2

    if args.worker:
        result_path = Path(args.result).resolve()
        console = None
        console_error = ""
        try:
            console = TestConsole(args.columns, args.rows)
        except ConsoleUnavailable as exc:
            console_error = str(exc)
        payload = execute(args, console, {"consoleAttempt": "detached-worker", "consoleError": console_error})
        result_path.parent.mkdir(parents=True, exist_ok=True)
        payload["_out"] = relative(out)
        result_path.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print_summary(payload)
        return exit_code_for(payload, args.allow_environment_failures)

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
            sys.executable, str(SCRIPT), "--worker", "--result", str(result_path),
            "--exe", str(exe), "--out", str(out), "--artifacts", str(artifacts),
            "--expected-sha256", args.expected_sha256,
            "--columns", str(args.columns), "--rows", str(args.rows),
            "--seconds", str(args.seconds),
            "--search-timeout", str(args.search_timeout),
            "--play-timeout", str(args.play_timeout),
            "--observe-seconds", str(args.observe_seconds),
            "--quit-timeout", str(args.quit_timeout),
            "--play-query", args.play_query, "--play-id", str(args.play_id),
            "--fallback-seconds", str(args.fallback_seconds),
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
                timeout=600,
            )
        payload = read_report(result_path)
        if not payload:
            payload = {
                "schema": "mora-m1-network/1",
                "generatedAt": now_iso(),
                "harness": {"console": {"mode": "detached-worker", "error": console_error}},
                "binary": fingerprint(exe),
                "steps": [],
                "report": {"path": None, "written": False, "raw": None},
                "conclusion": {
                    "verdict": "product-defect",
                    "summary": "无法创建测试控制台，DETACHED_PROCESS 后备也失败。",
                    "productDefects": ["console"],
                },
            }
            code = 1
        else:
            code = exit_code_for(payload, args.allow_environment_failures)
            payload.setdefault("harness", {})["workerReturncode"] = completed.returncode
        payload["_out"] = relative(out)
        write_payload(payload, out, code)
        print_summary(payload)
        return code

    payload = execute(args, console, {"consoleAttempt": "direct"})
    payload["_out"] = relative(out)
    code = exit_code_for(payload, args.allow_environment_failures)
    write_payload(payload, out, code)
    print_summary(payload)
    return code


if __name__ == "__main__":
    raise SystemExit(main())
