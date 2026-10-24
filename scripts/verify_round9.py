"""M12 (task-5) independent adversarial verification for the M8-M11 round.

This harness is written by the independent verifier. It never edits src/, tests/ or another
owner's scripts: it only runs commands, drives a real hidden console, talks to the real MCP
stdio server and to local stub providers, then reports machine-readable evidence.

Status vocabulary (every check carries exactly one):
  pass          - the requirement was exercised and held, with a reproducible command.
  fail          - the requirement was exercised and was violated.
  blocked       - a precondition is missing (peer task not landed, foreign player owns the
                  single-instance pipe, console unavailable). Not a product verdict.
  not_verified  - no test could be constructed for a stated reason.

Usage:
  python scripts/verify_round9.py                      # all areas
  python scripts/verify_round9.py --areas regression,mcp
  python scripts/verify_round9.py --list
  python scripts/verify_round9.py --exe build/mora.exe --out docs/verification/round9-audit.json
"""
from __future__ import annotations

import argparse
import ctypes as c
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from ctypes import wintypes as w
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ARTIFACTS = ROOT / "build" / "round9"
DEFAULT_EXE = ROOT / "build" / "mora.exe"
DEFAULT_OUT = ROOT / "docs" / "verification" / "round9-audit.json"

# Sentinels: unique strings that must never appear in any output, request log or frame.
SENTINEL_KEY = "mora-r9-sentinel-KEY-7f3c9d2e"
SENTINEL_COOKIE = "MUSIC_U=r9-cookie-sentinel-0a1b2c3d"
SENTINEL_UID = "987654321"
SENTINEL_STREAM = "https://m10.music.126.net/r9-stream-sentinel.mp3"
ALL_SENTINELS = (SENTINEL_KEY, SENTINEL_COOKIE, SENTINEL_UID, SENTINEL_STREAM)

PASS, FAIL, BLOCKED, NOT_VERIFIED = "pass", "fail", "blocked", "not_verified"


# --------------------------------------------------------------------------- audit model
def iso_now() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%S%z")


def excerpt(text, limit: int = 1600) -> str:
    if text is None:
        return ""
    text = text if isinstance(text, str) else str(text)
    if len(text) <= limit:
        return text
    return text[: limit // 2] + "\n...[%d bytes omitted]...\n" % (len(text) - limit) + text[-limit // 2 :]


def tail(text, limit: int = 1600) -> str:
    if not text:
        return ""
    return text[-limit:]


def find_secrets(*texts) -> list:
    hits = []
    for index, text in enumerate(texts):
        if not text:
            continue
        for sentinel in ALL_SENTINELS:
            if sentinel in text:
                hits.append({"source": index, "sentinel": sentinel})
    return hits


class Audit:
    def __init__(self, exe: Path):
        self.exe = exe
        self.checks: list = []
        self.notes: list = []
        self.environment: dict = {}
        self.areas: dict = {}

    def add(self, area, cid, status, requirement, command, evidence="", detail="", impact=""):
        assert status in (PASS, FAIL, BLOCKED, NOT_VERIFIED), status
        self.checks.append(
            {
                "id": cid,
                "area": area,
                "status": status,
                "requirement": requirement,
                "command": command,
                "evidence": excerpt(evidence, 2000),
                "detail": detail,
                "impact": impact,
            }
        )
        return self.checks[-1]

    def note(self, area, text):
        self.notes.append({"area": area, "text": text})

    def summary(self):
        counts = {PASS: 0, FAIL: 0, BLOCKED: 0, NOT_VERIFIED: 0}
        for check in self.checks:
            counts[check["status"]] += 1
        return {
            "total": len(self.checks),
            "passed": counts[PASS],
            "failed": counts[FAIL],
            "blocked": counts[BLOCKED],
            "notVerified": counts[NOT_VERIFIED],
        }


# --------------------------------------------------------------------------- process helpers
def run(cmd, timeout: float = 120.0, env=None, cwd=ROOT, stdin_text=None, console: bool = False) -> dict:
    """Run one command, bounded, and return its raw output for evidence."""
    completed = None
    started = time.monotonic()
    flags = 0
    if not console and hasattr(subprocess, "CREATE_NO_WINDOW"):
        flags = subprocess.CREATE_NO_WINDOW
    try:
        completed = subprocess.run(
            [str(item) for item in cmd],
            cwd=str(cwd),
            env=env,
            input=stdin_text.encode("utf-8") if isinstance(stdin_text, str) else stdin_text,
            capture_output=True,
            timeout=timeout,
            creationflags=flags,
        )
        return {
            "argv": [str(item) for item in cmd],
            "returncode": completed.returncode,
            "stdout": completed.stdout.decode("utf-8", "replace"),
            "stderr": completed.stderr.decode("utf-8", "replace"),
            "seconds": round(time.monotonic() - started, 2),
            "timeout": False,
        }
    except subprocess.TimeoutExpired as exc:
        out = exc.stdout.decode("utf-8", "replace") if isinstance(exc.stdout, bytes) else (exc.stdout or "")
        err = exc.stderr.decode("utf-8", "replace") if isinstance(exc.stderr, bytes) else (exc.stderr or "")
        return {
            "argv": [str(item) for item in cmd],
            "returncode": None,
            "stdout": out,
            "stderr": err,
            "seconds": round(time.monotonic() - started, 2),
            "timeout": True,
        }


def command_text(result: dict) -> str:
    return " ".join(result["argv"])


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def new_profile(name: str, session_sentinel: bool = False) -> Path:
    path = ARTIFACTS / ("profile-" + name)
    if path.exists():
        for item in sorted(path.rglob("*"), reverse=True):
            if item.is_file():
                item.unlink()
            else:
                item.rmdir()
    path.mkdir(parents=True, exist_ok=True)
    if session_sentinel:
        # Same trick as scripts/mcp_boundary_test.py: a session file past the size limit makes any
        # accidental MusicService API construction fail before a network request.
        (path / "session.bin").write_bytes(b"x" * 70000)
    return path


def list_mora_pipes() -> list:
    try:
        names = os.listdir("\\\\.\\pipe\\")
    except OSError:
        return []
    return [n for n in names if n.startswith("mora-player")]


def shell_exe() -> str:
    """PowerShell that actually exists on this host (pwsh is not always on PATH)."""
    for name in ("pwsh", "pwsh.exe", "powershell", "powershell.exe"):
        found = shutil.which(name)
        if found:
            return found
    return "powershell.exe"


def mora_processes() -> list:
    result = run(
        [
            shell_exe(),
            "-NoProfile",
            "-Command",
            "Get-Process mora -ErrorAction SilentlyContinue | "
            "Select-Object Id,StartTime,Path | ConvertTo-Json -Compress",
        ],
        timeout=60,
    )
    text = result["stdout"].strip()
    if not text:
        return []
    try:
        parsed = json.loads(text)
    except json.JSONDecodeError:
        return [{"raw": text[:300]}]
    return parsed if isinstance(parsed, list) else [parsed]


def source_contains(relative: str, needles) -> bool:
    path = ROOT / relative
    if not path.exists():
        return False
    text = path.read_text(encoding="utf-8", errors="replace")
    return all(needle in text for needle in (needles if isinstance(needles, (list, tuple)) else [needles]))


def file_present(relative: str) -> bool:
    return (ROOT / relative).exists()


# --------------------------------------------------------------------------- MCP stdio client
class McpServer:
    """Real `mora --mcp` stdio JSON-RPC server (same transport as scripts/m4_bridge_check.py)."""

    def __init__(self, exe: Path, profile: Path, label: str):
        self.exe = exe
        self.profile = profile
        self.label = label
        self.proc = None
        self.stderr_path = ARTIFACTS / ("mcp-%s-stderr.txt" % label)
        self.next_id = 0
        self.requests = 0

    def __enter__(self):
        self.stderr_handle = open(self.stderr_path, "wb")
        self.proc = subprocess.Popen(
            [str(self.exe), "--mcp", "--data-dir", str(self.profile)],
            cwd=str(ROOT),
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self.stderr_handle,
            creationflags=subprocess.CREATE_NO_WINDOW,
        )
        return self

    def __exit__(self, *exc):
        try:
            if self.proc and self.proc.stdin:
                self.proc.stdin.close()
            if self.proc:
                self.proc.wait(timeout=20)
        except (OSError, subprocess.TimeoutExpired):
            if self.proc and self.proc.poll() is None:
                self.proc.kill()
                self.proc.wait()
        try:
            self.stderr_handle.close()
        except OSError:
            pass

    def _send(self, message: dict) -> None:
        self.proc.stdin.write((json.dumps(message, ensure_ascii=False) + "\n").encode("utf-8"))
        self.proc.stdin.flush()
        self.requests += 1

    def _read_response(self, identity, timeout: float = 30.0) -> dict:
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("no answer for id=%r within %.1fs" % (identity, timeout))
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError("MCP server closed stdout before answering id=%r" % (identity,))
            raw = line.decode("utf-8", "replace").rstrip("\r\n")
            if not raw.strip():
                continue
            try:
                parsed = json.loads(raw)
            except json.JSONDecodeError:
                raise RuntimeError("MCP stdout carried a non-JSON line: %r" % raw[:200])
            if parsed.get("id") != identity:
                # A notification or an out-of-order frame; keep reading.
                continue
            return parsed

    def request(self, method: str, params=None, timeout: float = 30.0) -> dict:
        self.next_id += 1
        identity = self.next_id
        message = {"jsonrpc": "2.0", "id": identity, "method": method}
        if params is not None:
            message["params"] = params
        self._send(message)
        return self._read_response(identity, timeout)

    def notify(self, method: str, params=None) -> None:
        message = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            message["params"] = params
        self._send(message)

    def initialize(self) -> dict:
        result = self.request(
            "initialize",
            {
                "protocolVersion": "2025-11-25",
                "capabilities": {},
                "clientInfo": {"name": "mora-round9-verifier", "version": "1"},
            },
        )
        self.notify("notifications/initialized")
        return result

    def tools(self) -> list:
        message = self.request("tools/list", {})
        tools = (message.get("result") or {}).get("tools") or []
        return tools

    def call_tool(self, name: str, arguments=None, timeout: float = 30.0) -> dict:
        arguments = arguments if arguments is not None else {}
        try:
            message = self.request("tools/call", {"name": name, "arguments": arguments}, timeout)
        except (TimeoutError, RuntimeError) as exc:
            return {"tool": name, "arguments": arguments, "transportError": str(exc), "isError": None,
                    "structured": None, "contentText": ""}
        if message.get("error") is not None:
            return {"tool": name, "arguments": arguments, "transportError": message["error"],
                    "isError": None, "structured": None, "contentText": ""}
        result = message.get("result") or {}
        texts = [
            item.get("text", "")
            for item in (result.get("content") or [])
            if isinstance(item, dict) and isinstance(item.get("text"), str)
        ]
        structured = result.get("structuredContent")
        return {
            "tool": name,
            "arguments": arguments,
            "transportError": None,
            "isError": bool(result.get("isError")),
            "structured": structured if isinstance(structured, (dict, list)) else None,
            "contentText": " ".join(texts),
        }

    def tool_error_code(self, record: dict):
        if record.get("structured") and isinstance(record["structured"], dict):
            if "error" in record["structured"]:
                return record["structured"]["error"]
        match = re.search(r'"error"\s*:\s*"([a-z_]+)"', record.get("contentText") or "")
        if match:
            return match.group(1)
        return None

    def stderr_text(self) -> str:
        try:
            return self.stderr_path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            return ""


# --------------------------------------------------------------------------- hidden console TUI
class Coord(c.Structure):
    _fields_ = [("X", w.SHORT), ("Y", w.SHORT)]


class Rect(c.Structure):
    _fields_ = [("Left", w.SHORT), ("Top", w.SHORT), ("Right", w.SHORT), ("Bottom", w.SHORT)]


class _Char(c.Union):
    _fields_ = [("UnicodeChar", w.WCHAR), ("AsciiChar", c.c_char)]


class _Key(c.Structure):
    _fields_ = [("down", w.BOOL), ("repeat", w.WORD), ("vk", w.WORD), ("scan", w.WORD),
                ("char", _Char), ("control", w.DWORD)]


class _Mouse(c.Structure):
    _fields_ = [("position", Coord), ("buttons", w.DWORD), ("control", w.DWORD), ("flags", w.DWORD)]


class _Event(c.Union):
    _fields_ = [("key", _Key), ("mouse", _Mouse), ("raw", c.c_byte * 16)]


class _Input(c.Structure):
    _fields_ = [("type", w.WORD), ("event", _Event)]


class HiddenConsole:
    """Hidden console so the real TUI can be driven through its real input queue."""

    def __init__(self, columns: int = 78, rows: int = 24):
        k = c.WinDLL("kernel32", use_last_error=True)
        u = c.WinDLL("user32", use_last_error=True)
        self.k, self.u = k, u
        k.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, c.c_void_p, w.DWORD, w.DWORD, w.HANDLE]
        k.CreateFileW.restype = w.HANDLE
        k.GetConsoleWindow.restype = w.HWND
        k.GetStdHandle.argtypes = [w.DWORD]
        k.GetStdHandle.restype = w.HANDLE
        k.GetConsoleMode.argtypes = [w.HANDLE, c.POINTER(w.DWORD)]
        k.GetConsoleMode.restype = w.BOOL
        k.SetConsoleScreenBufferSize.argtypes = [w.HANDLE, Coord]
        k.SetConsoleWindowInfo.argtypes = [w.HANDLE, w.BOOL, c.POINTER(Rect)]
        k.WriteConsoleInputW.argtypes = [w.HANDLE, c.POINTER(_Input), w.DWORD, c.POINTER(w.DWORD)]
        k.FlushConsoleInputBuffer.argtypes = [w.HANDLE]
        k.CloseHandle.argtypes = [w.HANDLE]
        u.ShowWindow.argtypes = [w.HWND, c.c_int]
        mode = w.DWORD()
        has_console = bool(k.GetConsoleWindow())
        stdout_is_console = bool(k.GetConsoleMode(k.GetStdHandle(-11), c.byref(mode)))
        self.allocated = False
        self.mode = "reused"
        if has_console and not stdout_is_console:
            k.FreeConsole()
        if not has_console or not stdout_is_console:
            if not k.AllocConsole():
                raise OSError(c.get_last_error(), "AllocConsole failed")
            self.allocated = True
            self.mode = "allocated-hidden"
        window = k.GetConsoleWindow()
        if self.allocated and window:
            u.ShowWindow(window, 0)
        invalid = w.HANDLE(-1).value
        self.conin = k.CreateFileW("CONIN$", 0xC0000000, 3, None, 3, 0, None)
        self.conout = k.CreateFileW("CONOUT$", 0xC0000000, 3, None, 3, 0, None)
        if self.conin == invalid or self.conout == invalid:
            raise OSError(c.get_last_error(), "CONIN$/CONOUT$ unavailable")
        k.SetConsoleScreenBufferSize(self.conout, Coord(max(columns, 116), 120))
        k.SetConsoleWindowInfo(self.conout, True, c.byref(Rect(0, 0, columns - 1, rows - 1)))
        k.SetConsoleScreenBufferSize(self.conout, Coord(columns, 120))
        k.SetConsoleWindowInfo(self.conout, True, c.byref(Rect(0, 0, columns - 1, rows - 1)))

    def send_key(self, vk: int = 0, char: str = "\0") -> None:
        events = (_Input * 2)()
        for index, down in enumerate((True, False)):
            events[index].type = 1
            events[index].event.key = _Key(down, 1, vk, 0, _Char(UnicodeChar=char), 0)
        written = w.DWORD()
        if not self.k.WriteConsoleInputW(self.conin, events, 2, c.byref(written)):
            raise OSError(c.get_last_error(), "WriteConsoleInputW failed")

    def type_text(self, text: str) -> None:
        for char in text:
            self.send_key(0, char)

    def click(self, x: int, y: int) -> None:
        events = (_Input * 2)()
        for index, buttons in enumerate((1, 0)):
            events[index].type = 2
            events[index].event.mouse = _Mouse(Coord(x, y), buttons, 0, 0)
        written = w.DWORD()
        if not self.k.WriteConsoleInputW(self.conin, events, 2, c.byref(written)):
            raise OSError(c.get_last_error(), "console mouse injection failed")

    def flush(self) -> None:
        self.k.FlushConsoleInputBuffer(self.conin)

    def close(self) -> None:
        try:
            if getattr(self, "conin", None):
                self.k.CloseHandle(self.conin)
            if getattr(self, "conout", None):
                self.k.CloseHandle(self.conout)
        finally:
            self.conin = self.conout = None
            if self.allocated:
                self.k.FreeConsole()


class TuiSession:
    def __init__(self, exe: Path, console: HiddenConsole, label: str, seconds: int = 40, extra=None,
                 profile: Path = None, demo: bool = True):
        self.exe, self.console = exe, console
        self.profile = profile
        self.snapshot = ARTIFACTS / ("tui-%s.txt" % label)
        self.report = ARTIFACTS / ("tui-%s-report.json" % label)
        self.stderr_path = ARTIFACTS / ("tui-%s-stderr.txt" % label)
        self.argv = [str(exe)]
        if demo:
            self.argv += ["--demo"]
        self.argv += ["--seconds", str(seconds), "--snapshot", str(self.snapshot),
                      "--report", str(self.report)]
        if profile is not None:
            # Bridge pipes are per data directory (bridgePipeName), so an isolated profile keeps the
            # user's own running player out of the way without ending it.
            self.argv += ["--data-dir", str(profile)]
        self.argv += list(extra or [])
        for stale in (self.snapshot, self.report, self.stderr_path):
            try:
                stale.unlink()
            except FileNotFoundError:
                pass
        self.proc = None

    def start(self):
        startup = subprocess.STARTUPINFO()
        startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        self.stdin = open("CONIN$", "rb", buffering=0)
        self.stdout = open("CONOUT$", "wb", buffering=0)
        self.stderr = open(self.stderr_path, "wb")
        self.proc = subprocess.Popen(self.argv, cwd=str(ROOT), stdin=self.stdin, stdout=self.stdout,
                                     stderr=self.stderr, startupinfo=startup,
                                     env=dict(os.environ))
        return self

    def frame(self) -> str:
        try:
            return self.snapshot.read_text(encoding="utf-8", errors="replace")
        except OSError:
            return ""

    def wait_for(self, predicate, timeout: float = 6.0, interval: float = 0.1):
        deadline = time.monotonic() + timeout
        frame = ""
        while time.monotonic() < deadline:
            frame = self.frame()
            if frame and predicate(frame):
                return frame
            if self.proc.poll() is not None:
                frame = self.frame()
                if frame and predicate(frame):
                    return frame
                return None
            time.sleep(interval)
        frame = self.frame()
        return frame if frame and predicate(frame) else None

    def command(self, text: str) -> None:
        self.console.type_text(text)
        self.console.send_key(13)

    def key(self, vk: int, char: str = "\0") -> None:
        self.console.flush()
        self.console.send_key(vk, char)

    def click(self, x: int, y: int) -> None:
        self.console.click(x, y)

    def stop(self, timeout: float = 15.0) -> int:
        if self.proc is None:
            return -1
        try:
            if self.proc.poll() is None:
                self.console.flush()
                self.command("/quit")
                self.proc.wait(timeout=timeout)
        except (OSError, subprocess.TimeoutExpired):
            pass
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        for handle in (getattr(self, "stdin", None), getattr(self, "stdout", None),
                       getattr(self, "stderr", None)):
            try:
                if handle:
                    handle.close()
            except OSError:
                pass
        return self.proc.returncode


# --------------------------------------------------------------------------- stub provider
class StubProvider(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def _record(self, kind: str, body: bytes):
        parsed = None
        try:
            parsed = json.loads(body or b"{}")
        except (json.JSONDecodeError, UnicodeDecodeError):
            parsed = None
        entry = {
            "kind": kind,
            "path": self.path,
            "method": self.command,
            "authorized": bool(self.headers.get("Authorization")),
            "authorizationPrefix": (self.headers.get("Authorization") or "")[:12],
            "contentType": self.headers.get("Content-Type"),
            "bytes": len(body or b""),
            "body": parsed,
        }
        self.server.records.append(entry)
        return entry

    def _respond(self, status: int, payload, raw: bytes = None):
        if raw is None:
            raw = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "text/plain" if self.server.raw_reply else "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)
        self.close_connection = True

    def do_GET(self):
        body = b""
        entry = self._record("GET", body)
        self.server.handle(entry, self)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0) or 0)
        body = self.rfile.read(length) if length else b""
        entry = self._record("POST", body)
        self.server.handle(entry, self)


def start_stub(handler) -> tuple:
    server = ThreadingHTTPServer(("127.0.0.1", 0), StubProvider)
    server.records = []
    server.handle = handler
    server.raw_reply = False
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, server.server_address[1]


def stub_stop(server):
    try:
        server.shutdown()
    except Exception:  # noqa: BLE001 - best effort shutdown
        pass


# =========================================================================== areas
def area_regression(audit: Audit, args) -> None:
    area = "regression"

    # --- native tests
    native = run([shell_exe(), "-NoProfile", "-NonInteractive", "-File", str(ROOT / "build.ps1"), "-Test"],
                 timeout=args.build_timeout)
    text = native["stdout"] + native["stderr"]
    match = re.search(r"(PASS|FAILED)\s+(\d+)\s+checks,\s+(\d+)\s+failures", text)
    command = "powershell -NoProfile -File build.ps1 -Test"
    if native["timeout"]:
        audit.add(area, "REG-01", BLOCKED, "./build.ps1 -Test completes and is green", command,
                  tail(text), detail="build/test command timed out after %.0fs" % args.build_timeout)
    elif match and match.group(1) == "PASS" and int(match.group(3)) == 0 and native["returncode"] == 0:
        audit.add(area, "REG-01", PASS, "./build.ps1 -Test green",
                  command, "exit=%s %s" % (native["returncode"], match.group(0)),
                  detail="compiled and ran %s checks" % match.group(2))
    elif match:
        failures = re.findall(r"FAIL[^\n]*", text)
        audit.add(area, "REG-01", FAIL, "./build.ps1 -Test green", command,
                  match.group(0) + "\n" + "\n".join(failures[:8]),
                  detail="exit=%s" % native["returncode"],
                  impact="any task whose acceptance gate is a green native suite is blocked")
    else:
        audit.add(area, "REG-01", FAIL, "./build.ps1 -Test green", command, tail(text, 2000),
                  detail="no PASS/FAILED summary line; exit=%s" % native["returncode"])

    # --- ui smoke
    smoke = run([sys.executable, str(ROOT / "scripts" / "ui_smoke.py")], timeout=args.ui_timeout,
                console=True)
    command = "python scripts/ui_smoke.py"
    payload = last_json_object(smoke["stdout"])
    if smoke["returncode"] == 0 and payload and payload.get("passed") is True:
        audit.add(area, "REG-02", PASS, "python scripts/ui_smoke.py green", command,
                  "exit=0 checks=%s" % payload.get("count"),
                  detail=json.dumps(payload.get("checks", [])[:6], ensure_ascii=False))
    else:
        audit.add(area, "REG-02", FAIL, "python scripts/ui_smoke.py green", command,
                  (tail(smoke["stdout"], 1200) + "\n" + tail(smoke["stderr"], 1200)),
                  detail="exit=%s payload=%s" % (smoke["returncode"],
                                                 json.dumps(payload, ensure_ascii=False)[:300]
                                                 if payload else None))

    # --- mcp boundary
    boundary = run([sys.executable, str(ROOT / "scripts" / "mcp_boundary_test.py")], timeout=args.mcp_timeout)
    command = "python scripts/mcp_boundary_test.py"
    payload = last_json_object(boundary["stdout"])
    pipes = audit.environment.get("moraPipes") or []
    if smoke_secrets := find_secrets(boundary["stdout"], boundary["stderr"]):
        audit.add(area, "REG-03", FAIL, "MCP boundary test leaks no secret", command,
                  json.dumps(smoke_secrets), detail="sentinel material appeared in boundary output")
    if boundary["returncode"] == 0 and payload and payload.get("passed") is True:
        audit.add(area, "REG-03", PASS, "python scripts/mcp_boundary_test.py green", command,
                  "exit=0 checks=%s" % payload.get("checks"))
    elif pipes and payload:
        audit.add(area, "REG-03", BLOCKED,
                  "python scripts/mcp_boundary_test.py green", command,
                  json.dumps(payload, ensure_ascii=False)[:1200],
                  detail="a foreign MORA player owns %s, so the no-player assertions cannot hold" % pipes,
                  impact="re-run after the user closes the running player")
    else:
        audit.add(area, "REG-03", FAIL, "python scripts/mcp_boundary_test.py green", command,
                  (tail(boundary["stdout"], 1500) + "\n" + tail(boundary["stderr"], 800)),
                  detail="exit=%s" % boundary["returncode"])

    # --- packaging
    packaging = run([shell_exe(), "-NoProfile", "-NonInteractive", "-File",
                     str(ROOT / "scripts" / "packaging_tests.ps1")], timeout=args.packaging_timeout)
    command = "powershell -NoProfile -File scripts/packaging_tests.ps1"
    match = re.search(r"PASS\s+(\d+)\s+packaging and installer checks", packaging["stdout"])
    if packaging["returncode"] == 0 and match:
        audit.add(area, "REG-04", PASS, "python/ps packaging tests green", command,
                  "exit=0 %s" % match.group(0))
    else:
        audit.add(area, "REG-04", FAIL, "packaging tests green", command,
                  tail(packaging["stdout"], 1500) + "\n" + tail(packaging["stderr"], 1500),
                  detail="exit=%s" % packaging["returncode"])


def last_json_object(text: str):
    """Parse a compact one-line JSON object (scripts print it as the final line)."""
    if not text:
        return None
    for line in reversed(text.splitlines()):
        line = line.strip()
        if line.startswith("{") and line.endswith("}"):
            try:
                return json.loads(line)
            except json.JSONDecodeError:
                continue
    return None


def extract_json_object(text: str):
    """Parse a pretty-printed JSON object from the start of a command's stdout."""
    if not text:
        return None
    decoder = json.JSONDecoder()
    stripped = text.strip()
    for offset, char in enumerate(stripped[:4000]):
        if char != "{":
            continue
        try:
            parsed, _ = decoder.raw_decode(stripped[offset:])
        except json.JSONDecodeError:
            continue
        if isinstance(parsed, dict):
            return parsed
    return None


def area_capability(audit: Audit, args) -> None:
    area = "capability"
    help_result = run([str(audit.exe), "--help"], timeout=60)
    help_text = help_result["stdout"] + help_result["stderr"]
    audit.add(area, "CAP-01", PASS if help_result["returncode"] == 0 else FAIL,
              "binary answers --help", "%s --help" % audit.exe.name, excerpt(help_text, 1200))

    profile = new_profile("capability")
    with McpServer(audit.exe, profile, "capability") as server:
        try:
            server.initialize()
            tools = server.tools()
            names = sorted(tool.get("name") for tool in tools)
            audit.add(area, "CAP-02", PASS, "MCP advertises its tools", "%s --mcp → tools/list" % audit.exe.name,
                      json.dumps(names, ensure_ascii=False),
                      detail="%d tools" % len(names))
            audit.environment["mcpTools"] = names
        except Exception as exc:  # noqa: BLE001 - reported as evidence
            audit.add(area, "CAP-02", FAIL, "MCP advertises its tools",
                      "%s --mcp → tools/list" % audit.exe.name, "%s: %s" % (type(exc).__name__, exc))

    agent_check = run([str(audit.exe), "--agent-check"], timeout=60, env=dict(os.environ, MORA_DATA_DIR=str(profile)))
    audit.add(area, "CAP-03", PASS if agent_check["returncode"] == 0 else FAIL,
              "--agent-check answers on an isolated profile",
              "%s --agent-check (MORA_DATA_DIR isolated)" % audit.exe.name,
              excerpt(agent_check["stdout"] or agent_check["stderr"], 1000))
    audit.environment["agentCheck"] = last_json_object(agent_check["stdout"])

    inventory = {
        "src/stats.cpp": file_present("src/stats.cpp"),
        "src/context.cpp": file_present("src/context.cpp"),
        "scripts/m8_queue_check.py": file_present("scripts/m8_queue_check.py"),
        "scripts/m3_context_check.py": file_present("scripts/m3_context_check.py"),
        "scripts/m3_models_check.py": file_present("scripts/m3_models_check.py"),
        "scripts/m9_stats_check.py": file_present("scripts/m9_stats_check.py"),
        "flag:--models": "--models" in help_text,
        "agent.cpp:planCompression-body": source_contains("src/agent.cpp", "planCompression"),
        "agent.cpp:fetchModelCatalog-body": source_contains("src/agent.cpp", "fetchModelCatalog"),
        "service.cpp:music_queue_edit": source_contains("src/service.cpp", "music_queue_edit"),
        "ui.cpp:/stats": source_contains("src/ui.cpp", "/stats"),
        "ui.cpp:/mode": source_contains("src/ui.cpp", "/mode"),
        "ui.cpp:/review": source_contains("src/ui.cpp", "/review"),
    }
    audit.environment["inventory"] = inventory
    audit.add(area, "CAP-04", PASS, "capability inventory recorded", "file/flag/source probe",
              json.dumps(inventory, ensure_ascii=False, indent=2))


def area_mcp(audit: Audit, args) -> None:
    area = "mcp"
    profile = new_profile("mcp-boundary", session_sentinel=True)
    foreign_pipes = audit.environment.get("moraPipes") or []
    with McpServer(audit.exe, profile, "round9") as server:
        try:
            server.initialize()
            tools = server.tools()
            names = {tool.get("name"): tool for tool in tools}
            queue_names = {"music_queue_list", "music_queue_edit"}
            if queue_names <= set(names):
                audit.add(area, "MCP-01", PASS, "queue tools are advertised", "tools/list",
                          json.dumps(sorted(names), ensure_ascii=False))
            else:
                audit.add(area, "MCP-01", BLOCKED, "queue tools are advertised (task-1)", "tools/list",
                          json.dumps(sorted(names), ensure_ascii=False),
                          detail="missing %s" % sorted(queue_names - set(names)),
                          impact="queue E2E cannot be exercised yet")

            # Safety gate before any tool that could act on a real player. The status call is
            # read-only. If the isolated profile still sees a player while a foreign MORA pipe
            # exists, --data-dir does not scope the pipe in this binary, and driving session tools
            # would mutate the user's live playback. In that case nothing but reads is sent.
            status = server.call_tool("music_get_status", {})
            attached = None
            if isinstance(status.get("structured"), dict):
                attached = status["structured"].get("playerSessionAttached")
            isolated = not (foreign_pipes and attached)
            audit.environment["mcpIsolatedProfile"] = {
                "foreignPipes": foreign_pipes,
                "playerSessionAttached": attached,
                "isolated": isolated,
            }
            audit.add(area, "MCP-ISO", PASS if isolated else FAIL,
                      "an isolated --data-dir owns its own bridge pipe",
                      "mora --mcp --data-dir <profile> → music_get_status",
                      json.dumps({"foreignPipes": foreign_pipes, "playerSessionAttached": attached},
                                 ensure_ascii=False),
                      detail="writing to an attached foreign player would be unsafe"
                      if not isolated else "no foreign player is reachable from this profile",
                      impact="session-write checks are skipped while not isolated" if not isolated else "")

            # Parameter pollution: a field the contract does not define must be invalid_arguments.
            pollution = [
                ("music_play", {"id": 1, "kind": "song", "surprise": 1}),
                ("music_now_playing", {"mode": "repeat_one"}),
                ("music_control", {"action": "pause", "force": True}),
                ("music_get_status", {"verbose": True}),
            ]
            for tool, arguments in pollution:
                record = server.call_tool(tool, arguments)
                code = server.tool_error_code(record)
                ok = record["isError"] is True and code == "invalid_arguments"
                audit.add(area, "MCP-02:%s" % tool, PASS if ok else FAIL,
                          "unknown field on %s → invalid_arguments" % tool,
                          "tools/call %s %s" % (tool, json.dumps(arguments, ensure_ascii=False)),
                          json.dumps(record, ensure_ascii=False)[:900],
                          detail="error code=%r" % code)

            # Session tools without a player must answer needs_player, never pretend to play.
            session_calls = [
                ("music_play", {"id": 123}),
                ("music_control", {"action": "pause"}),
                ("music_now_playing", {}),
                ("music_play_random", {"candidateSetId": "rec-1"}),
            ]
            if queue_names <= set(names):
                session_calls += [
                    ("music_queue_list", {}),
                    ("music_queue_edit", {"op": "set_mode", "mode": "repeat_one"}),
                    ("music_queue_edit", {"op": "clear"}),
                    ("music_queue_edit", {"op": "remove", "index": 0}),
                    ("music_queue_edit", {"op": "play_index", "index": 0}),
                ]
            if not isolated:
                audit.add(area, "MCP-03", BLOCKED, "session tools report needs_player with no player",
                          "tools/call … (no player expected)",
                          "foreign pipes=%s but isolated profile still reports attached=%r"
                          % (foreign_pipes, attached),
                          detail="refusing to send session writes: they would act on the user's real player",
                          impact="needs_player evidence deferred")
            else:
                for tool, arguments in session_calls:
                    record = server.call_tool(tool, arguments)
                    code = server.tool_error_code(record)
                    ok = record["isError"] is True and code == "needs_player"
                    if tool == "music_queue_list":
                        # A read-only listing may legitimately answer an empty queue instead.
                        ok = ok or (record["isError"] is False and isinstance(record["structured"], dict))
                    audit.add(area, "MCP-03:%s" % tool, PASS if ok else FAIL,
                              "%s is honest with no player" % tool,
                              "tools/call %s %s" % (tool, json.dumps(arguments, ensure_ascii=False)),
                              json.dumps(record, ensure_ascii=False)[:900],
                              detail="error code=%r" % code)

            # Malformed queue arguments must be rejected without touching a player.
            if queue_names <= set(names):
                malformed = [
                    ({"op": "set_mode"}, "set_mode without mode"),
                    ({"op": "set_mode", "mode": "nonsense"}, "unknown mode"),
                    ({"op": "play_index"}, "play_index without index"),
                    ({"op": "play_index", "index": "first"}, "non-integer index"),
                    ({"op": "remove", "index": -1}, "negative index"),
                    ({"op": "explode"}, "unknown op"),
                    ({"op": "append", "surprise": 1}, "unknown field"),
                    ({"op": "play_index", "index": 999999}, "index beyond the declared bound"),
                    ({"op": "move", "from": 2, "to": -3}, "negative move target"),
                    ({"op": "set_mode", "mode": 5}, "non-string mode"),
                ]
                for arguments, label in malformed:
                    record = server.call_tool("music_queue_edit", arguments)
                    code = server.tool_error_code(record)
                    ok = record["isError"] is True and code == "invalid_arguments"
                    audit.add(area, "MCP-04:%s" % label, PASS if ok else FAIL,
                              "music_queue_edit rejects %s" % label,
                              "tools/call music_queue_edit %s" % json.dumps(arguments, ensure_ascii=False),
                              json.dumps(record, ensure_ascii=False)[:900],
                              detail="error code=%r" % code)

            # One request must produce exactly one response: no automatic retry inside the server.
            before = server.requests
            repeated = [server.call_tool("music_now_playing", {}) for _ in range(3)]
            delta = server.requests - before
            audit.add(area, "MCP-05", PASS if delta == 3 else FAIL,
                      "no hidden retry/duplicate request per call",
                      "3× tools/call music_now_playing", "requests sent=%d (expected 3)" % delta,
                      detail=json.dumps([r["transportError"] for r in repeated], ensure_ascii=False))

            stderr_text = server.stderr_text()
            hits = find_secrets(server.stderr_text())
            audit.add(area, "MCP-06", PASS if not hits else FAIL,
                      "MCP stderr carries no sentinel material", "scan mcp stderr",
                      "no sentinel found" if not hits else json.dumps(hits),
                      detail=excerpt(stderr_text, 400))
        except Exception as exc:  # noqa: BLE001 - reported as evidence
            audit.add(area, "MCP-00", FAIL, "offline MCP session is usable", "%s --mcp" % audit.exe.name,
                      "%s: %s" % (type(exc).__name__, exc))


def settle(session: TuiSession, previous: str, timeout: float = 6.0) -> str:
    """Wait until the frame differs from `previous`, then let it paint once more."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        current = session.frame()
        if current and current != previous:
            time.sleep(0.35)
            return session.frame()
        time.sleep(0.1)
    return session.frame()


def area_ui(audit: Audit, args) -> None:
    area = "ui"
    try:
        console = HiddenConsole(78, 24)
    except OSError as exc:
        audit.add(area, "UI-00", BLOCKED, "real console available for frame capture",
                  "AllocConsole/CONIN$", str(exc), impact="UI frame checks cannot run")
        return
    session = None
    try:
        session = TuiSession(audit.exe, console, "frames", seconds=120,
                             profile=new_profile("ui")).start()
        frame = session.wait_for(lambda text: "阿木" in text or "M O R A" in text, timeout=15)
        if not frame:
            audit.add(area, "UI-01", FAIL, "home renders at 78×24", "mora --demo --seconds 120 (78×24)",
                      excerpt(session.frame(), 1200),
                      detail="no home marker within 15s; stderr=%s"
                      % excerpt(session.stderr_path.read_text(encoding="utf-8", errors="replace"), 400))
            return
        save_frame("home", frame)
        home_problems = layout_problems(frame)
        audit.add(area, "UI-01", PASS if not home_problems else FAIL,
                  "the 78×24 home frame fits and shows exactly one of each layout band",
                  "mora --demo --seconds 120 --snapshot (78 column window)",
                  "rows=%d overwide=%s\n%s" % (len(frame.splitlines()),
                                               [p for p in home_problems if p.startswith("overwide")],
                                               excerpt(frame, 1100)),
                  detail="; ".join(home_problems) if home_problems else
                  "no over-wide row, no duplicated banner/hint/input band")
        audit.add(area, "UI-02", PASS if "\x1b" not in frame else FAIL,
                  "snapshot carries no raw ANSI escape (cell text only)",
                  "grep ESC in the snapshot", "escape present=%s" % ("\x1b" in frame))

        # Page probes: a command that changes nothing is reported as a missing surface, not a pass.
        # (label, kind, input, required markers, page-exclusive marker, requirement)
        probes = [
            ("ai-panel", "command", "/ai", ["音乐 Agent", "模型"], "模型设置", "/ai model panel"),
            ("route-1-songs", "key", "1", ["单曲"], None, "route page 1 (songs)"),
            ("route-3-playlists", "key", "3", ["歌单"], None, "route page 3 (playlists)"),
            ("route-5-queue", "key", "5", ["队列"], None, "route page 5 (queue)"),
            ("detail-key", "key", "D", ["歌曲详情"], None, "song detail page via the D shortcut"),
            ("detail-from-route", "keys", "5D", ["歌曲详情"], "歌曲详情",
             "song detail via D after leaving the home composer (route page 5)"),
            ("detail-cmd", "command", "/detail", ["歌曲详情"], "歌曲详情", "/detail command"),
            ("stats-day", "command", "/stats day", ["统计"], None, "/stats day report"),
            ("mode-repeat-one", "command", "/mode repeat_one", ["单曲循环"], None,
             "/mode switches the queue mode"),
            ("review", "command", "/review", ["锐评"], None, "/review writes a model review"),
        ]
        pages = {"home": frame}
        for label, kind, payload, markers, exclusive, requirement in probes:
            previous = session.frame()
            if kind == "command":
                session.command(payload)
                shown = "%r" % payload
            elif kind == "keys":
                for character in payload:
                    session.key(0, character)
                    time.sleep(0.5)
                shown = "keys %s" % payload
            else:
                # Printable shortcuts travel as a character (vk 0), the same way the composer
                # receives typed text; vk-coded events were not delivered to the handler.
                session.key(0, payload)
                shown = "key %s" % payload
            current = settle(session, previous)
            pages[label] = current
            save_frame(label, current)
            changed = current != previous
            present = [marker for marker in markers if marker in current]
            problems = layout_problems(current)
            refusal = next((phrase for phrase in
                            ("演示模式不写入听歌记录", "演示模式不能写锐评")
                            if phrase in current), None)
            if refusal:
                audit.add(area, "UI-%s" % label, BLOCKED, requirement,
                          "mora --demo → %s" % shown, excerpt(current, 900),
                          detail="demo mode refuses on purpose (%r); the live surface is verified "
                                 "in a live session instead" % refusal)
            elif "未知命令" in current:
                audit.add(area, "UI-%s" % label, BLOCKED, requirement,
                          "mora --demo → %s" % shown, excerpt(current, 900),
                          detail="the command is not registered yet (the player answers 未知命令)")
            elif not changed:
                audit.add(area, "UI-%s" % label, BLOCKED, requirement,
                          "mora --demo → %s" % shown, excerpt(current, 900),
                          detail="the frame did not change after %s: the surface does not exist yet"
                          % shown)
            elif present and not problems:
                audit.add(area, "UI-%s" % label, PASS, requirement, "mora --demo → %s" % shown,
                          excerpt(current, 1100), detail="markers=%s" % present)
            else:
                audit.add(area, "UI-%s" % label, FAIL, requirement, "mora --demo → %s" % shown,
                          excerpt(current, 1100),
                          detail="missing markers=%s layout=%s"
                          % ([m for m in markers if m not in current], problems))
            # Close any modal, return home, and prove no page-exclusive marker is left painted.
            session.key(27)  # Esc
            settle(session, current)
            previous = session.frame()
            session.command("/home")
            back = settle(session, previous)
            if exclusive and exclusive in back:
                audit.add(area, "UI-%s:stale" % label, FAIL,
                          "leaving %s repaints the home frame" % label,
                          "mora --demo → %s → Esc → /home" % shown, excerpt(back, 700),
                          detail="exclusive marker %r is still painted after returning home" % exclusive)

        # The advertised `D 详情` shortcut is swallowed by the composer, so click the button itself:
        # this separates "the page is unreachable" from "only the keyboard shortcut is broken".
        previous = session.frame()
        session.key(0, "5")
        route = settle(session, previous)
        target = locate_text(route, "D 详情")
        if target:
            session.click(target[0], target[1])
            clicked = settle(session, route)
            audit.add(area, "UI-detail-click",
                      PASS if "歌曲详情" in clicked else FAIL,
                      "the detail page is reachable by clicking its advertised button",
                      "mora --demo → key 5 → click (%d,%d) 'D 详情'" % target,
                      excerpt(clicked, 1000),
                      detail="located the button at 0-based (%d,%d)" % target)
        else:
            audit.add(area, "UI-detail-click", BLOCKED,
                      "the detail page is reachable by clicking its advertised button",
                      "mora --demo → key 5 → locate 'D 详情'",
                      excerpt(route, 700), detail="the hint text was not found in the frame")

        # The now-playing block must be present on home and the queue route page, and the mode label
        # must be readable there (task-3 item 1/3).
        for label in ("home", "route-5-queue"):
            page = pages.get(label, "")
            has_progress = bool(re.search(r"\d{2}:\d{2}\s*/\s*\d{2}:\d{2}", page))
            has_mode = any(word in page for word in ("顺序", "随机", "单曲循环", "列表循环"))
            audit.add(area, "UI-NOW-%s" % label, PASS if (has_progress and has_mode) else FAIL,
                      "now-playing block with progress and queue mode on %s" % label,
                      "capture %s frame" % label,
                      "progress=%s mode=%s" % (has_progress, has_mode) + "\n" + excerpt(page, 500))

        audit.environment["frames"] = {label: {"path": str(ARTIFACTS / ("frame-%s.txt" % label)),
                                               "rows": len(text.splitlines())}
                                       for label, text in pages.items()}
    except Exception as exc:  # noqa: BLE001 - reported as evidence
        audit.add(area, "UI-00", FAIL, "real console session usable", "mora --demo (78×24)",
                  "%s: %s" % (type(exc).__name__, exc))
    finally:
        if session is not None:
            session.stop()
        console.close()


def layout_problems(frame: str) -> list:
    """Bounded layout invariants for a fixed-size frame: width, and no duplicated band."""
    problems = []
    if not frame:
        return ["empty frame"]
    rows = frame.splitlines()
    over = [index for index, row in enumerate(rows) if display_width(row) > 78]
    if over:
        problems.append("overwide rows %s" % over[:5])
    for marker, limit in (("浏览", 1), ("Enter 发送", 1), ("╭", 1)):
        count = sum(1 for row in rows if marker in row)
        if count > limit:
            problems.append("marker %r appears %d times" % (marker, count))
    if rows and "Enter 发送" in rows[0]:
        problems.append("hint band painted on the first row")
    return problems


def display_width(text: str) -> int:
    import unicodedata
    width = 0
    for char in text:
        if unicodedata.combining(char):
            continue
        width += 2 if unicodedata.east_asian_width(char) in ("W", "F") else 1
    return width


def locate_text(frame: str, needle: str):
    """0-based console (x, y) of a hint label, counting CJK glyphs as two cells."""
    for row_index, row in enumerate(frame.splitlines()):
        column = row.find(needle)
        if column >= 0:
            return (min(display_width(row[:column]) + 1, 76), row_index)
    return None


def save_frame(label: str, text: str) -> dict:
    path = ARTIFACTS / ("frame-%s.txt" % label)
    path.write_text(text or "", encoding="utf-8")
    return {"path": str(path), "rows": len((text or "").splitlines()), "text": text or ""}


def area_agent(audit: Audit, args) -> None:
    area = "agent"
    profile = new_profile("agent")
    key_result = run([str(audit.exe), "--set-agent-key"], timeout=60,
                     env=dict(os.environ, MORA_DATA_DIR=str(profile)), stdin_text=SENTINEL_KEY + "\n")
    if key_result["returncode"] != 0:
        audit.add(area, "AGT-00", FAIL, "sentinel key can be stored in the DPAPI vault",
                  "--set-agent-key", excerpt(key_result["stdout"] + key_result["stderr"], 800))
        return

    def chat_handler(entry, handler):
        body = entry["body"] if isinstance(entry["body"], dict) else {}
        messages = body.get("messages") or []
        joined = " ".join(str(message.get("content") or "")[:4000] for message in messages
                          if isinstance(message, dict))
        looks_like_summary = any(token in joined for token in ("压缩", "摘要", "summary", "compact"))
        handler.server.summary_requests += 1 if looks_like_summary else 0
        if looks_like_summary:
            reply = {"choices": [{"message": {"role": "assistant",
                                              "content": "[ROUND9-SUMMARY] 用户偏好安静的钢琴曲"}}]}
        else:
            reply = {"choices": [{"message": {"role": "assistant", "content": "好的，继续为你推荐。"}}]}
        handler._respond(200, reply)

    server, port = start_stub(chat_handler)
    server.summary_requests = 0
    env = dict(os.environ, MORA_DATA_DIR=str(profile),
               MORA_AGENT_BASE_URL="http://127.0.0.1:%d/v1/chat/completions" % port,
               MORA_AGENT_MODEL="round9-stub-model")
    try:
        baseline = run([str(audit.exe), "--chat", "你好，推荐一首安静的歌"], timeout=90, env=env)
        command = "mora --chat '你好，推荐一首安静的歌' (stub baseUrl)"
        payload = extract_json_object(baseline["stdout"])
        hits = find_secrets(baseline["stdout"], baseline["stderr"], server.records and
                            json.dumps(server.records, ensure_ascii=False))
        audit.add(area, "AGT-01", PASS if not hits else FAIL,
                  "sentinel key never appears in chat stdout/stderr/request log", command,
                  "no sentinel found" if not hits else json.dumps(hits),
                  detail=excerpt(baseline["stdout"] + baseline["stderr"], 500))
        if not payload:
            audit.add(area, "AGT-02", FAIL, "--chat answers JSON", command, excerpt(
                baseline["stdout"] + baseline["stderr"], 1000), detail="exit=%s" % baseline["returncode"])
        else:
            audit.add(area, "AGT-02", PASS if payload.get("ok") else FAIL, "--chat answers JSON", command,
                      json.dumps(payload, ensure_ascii=False)[:900])
            modality_fields = collect_modality_fields(server.records)
            audit.add(area, "AGT-03", PASS if not modality_fields else FAIL,
                      "request body carries no modality field (users cannot choose modality)", command,
                      "modality-like keys found: %s" % modality_fields if modality_fields
                      else "body keys: %s" % json.dumps(
                          sorted((server.records[-1]["body"] or {}).keys()) if server.records else [],
                          ensure_ascii=False))
            audit.add(area, "AGT-04", PASS if "promptTokens" in payload and "contextLimitTokens" in payload
                      else BLOCKED,
                      "--chat reports promptTokens/contextLimitTokens", command,
                      json.dumps({k: payload.get(k) for k in
                                  ("promptTokens", "contextLimitTokens", "compressedMessages")},
                                 ensure_ascii=False),
                      detail="context accounting fields are added by task-2",
                      impact="context-budget evidence deferred")

            # --- adversarial: a tool round whose assistant message carries content:null (the shape
            # OpenAI-compatible providers send with tool_calls). The estimator must not throw and the
            # loop must still finish.
            server.records.clear()

            def tool_round_handler(entry, handler):
                body = entry.get("body") if isinstance(entry.get("body"), dict) else {}
                posts = [record for record in handler.server.records if record.get("kind") == "POST"]
                if len(posts) <= 1:
                    handler._respond(200, {"choices": [{"message": {
                        "role": "assistant", "content": None,
                        "tool_calls": [{"id": "call-null", "type": "function", "function": {
                            "name": "music_search",
                            "arguments": json.dumps({"query": "随便来点歌", "limit": 5},
                                                    ensure_ascii=False)}}]}}]})
                else:
                    handler._respond(200, {"choices": [{"message": {
                        "role": "assistant", "content": "给你找了几首，正在播放。"}}]})

            server.handle = tool_round_handler
            round_run = run([str(audit.exe), "--chat", "随便来点歌"], timeout=90, env=env)
            round_payload = extract_json_object(round_run["stdout"]) or {}
            bodies = [record["body"] for record in server.records
                      if isinstance(record.get("body"), dict)]
            null_assistant = any(
                message.get("role") == "assistant" and message.get("content") is None
                for body in bodies for message in (body.get("messages") or [])
                if isinstance(message, dict))
            tool_round_travelled = any(
                message.get("role") == "assistant" and message.get("tool_calls")
                for body in bodies for message in (body.get("messages") or [])
                if isinstance(message, dict))
            contents = [(message.get("role"), message.get("content"))
                        for body in bodies for message in (body.get("messages") or [])
                        if isinstance(message, dict) and message.get("role") == "assistant"]
            ok = (round_payload.get("ok") is True and round_payload.get("toolCalls") == 1
                  and tool_round_travelled)
            audit.add(area, "AGT-10", PASS if ok else FAIL,
                      "a null-content assistant tool round neither throws nor stalls",
                      "mora --chat '随便来点歌' with a stub that replies content:null + tool_calls",
                      json.dumps({k: round_payload.get(k) for k in
                                  ("ok", "toolCalls", "code", "error", "promptTokens")},
                                 ensure_ascii=False)
                      + " toolRoundTravelled=%s nullContentVerbatim=%s assistantContents=%s"
                      % (tool_round_travelled, null_assistant,
                         json.dumps(contents, ensure_ascii=False)[:200]),
                      detail="estimateMessageTokens used to throw on content:null; the loop must "
                             "report the round honestly")
            server.handle = chat_handler

            # --- prompt accounting grows with history (never shrinks) at a fixed window
            sizes = {}
            for turns in (0, 12, 24):
                history = synthesize_history(turns, 300) if turns else []
                (profile / "chat-memory.json").write_text(
                    json.dumps({"version": 1, "summary": "", "compressedTurns": 0, "turns": history},
                               ensure_ascii=False), encoding="utf-8")
                server.records.clear()
                probe = run([str(audit.exe), "--chat", "继续"], timeout=90, env=env)
                sizes[turns] = (extract_json_object(probe["stdout"]) or {}).get("promptTokens")
            monotonic = (all(isinstance(sizes[turn], int) for turn in (0, 12, 24))
                         and sizes[0] < sizes[12] < sizes[24])
            audit.add(area, "AGT-11", PASS if monotonic else FAIL,
                      "promptTokens grows monotonically with the injected history",
                      "mora --chat '继续' with 0 / 12 / 24 history turns (stub)",
                      json.dumps(sizes, ensure_ascii=False),
                      detail="strictly increasing expected at a fixed 32768 window")

            # --- the projection width itself: 24 turns now (UI and CLI were aligned after the
            # measured 12-turn ceiling). Small turns keep the prompt under the threshold, so the
            # request is not compacted and its history length is directly observable.
            server.records.clear()
            width_history = synthesize_history(30, 200)
            (profile / "chat-memory.json").write_text(
                json.dumps({"version": 1, "summary": "", "compressedTurns": 0,
                            "turns": width_history}, ensure_ascii=False), encoding="utf-8")
            width_run = run([str(audit.exe), "--chat", "继续"], timeout=90, env=env)
            width_payload = extract_json_object(width_run["stdout"]) or {}
            width_bodies = [record["body"] for record in server.records
                            if isinstance(record.get("body"), dict)]
            counts = []
            for body in width_bodies:
                messages = body.get("messages") or []
                counts.append(sum(1 for message in messages
                                  if isinstance(message, dict)
                                  and message.get("role") in ("user", "assistant")))
            projected = (max(counts) - 1) if counts else 0  # minus the new user message
            audit.add(area, "AGT-12", PASS if projected == 24 else FAIL,
                      "the CLI projects 24 history turns, consistent with the terminal UI",
                      "mora --chat '继续' with a 30-turn chat-memory.json (stub)",
                      "requests=%d historyCounts=%s projectedHistoryTurns=%d compressedMessages=%s"
                      % (len(counts), counts, projected, width_payload.get("compressedMessages")),
                      detail="expected 24 projected turns excluding the new user message")

        # --- oversized history must trigger compression, keep recent turns and inject a summary
        server.records.clear()
        # The CLI now projects 24 turns (was 12; measured and then widened), so the default window
        # is crossed by this fixture too. A small fallback window is still pinned first so the
        # small-window path is exercised independently of the default one (AGT-08c).
        (profile / "agent.json").write_text(
            json.dumps({"contextFallbackTokens": 8192}, ensure_ascii=False), encoding="utf-8")
        # 40 turns × ~3980 bytes: after the 4000-byte per-turn cap the projection is still far past
        # 72% of the 8192-token window.
        history = synthesize_history(turns=40, chars=3980)
        (profile / "chat-memory.json").write_text(
            json.dumps({"version": 1, "summary": "", "compressedTurns": 0, "turns": history},
                       ensure_ascii=False), encoding="utf-8")
        compressed = run([str(audit.exe), "--chat", "继续推荐"], timeout=120, env=env)
        command = "mora --chat '继续推荐' with 40×3980-char chat-memory.json (stub)"
        payload = extract_json_object(compressed["stdout"])
        bodies = [record["body"] for record in server.records if isinstance(record.get("body"), dict)]
        summary_seen = any("ROUND9-SUMMARY" in json.dumps(body, ensure_ascii=False) for body in bodies)
        reformatted = [body for body in bodies if is_summary_request(body)]
        recent_ok = recent_turns_kept(bodies, history)
        if not bodies:
            audit.add(area, "AGT-05", FAIL, "oversized history reaches the provider at all", command,
                      excerpt(compressed["stdout"] + compressed["stderr"], 800))
        else:
            # The final POST is the answer request; anything before it is the compaction call.
            reformatted = [body for body in bodies[:-1]] or [body for body in bodies if is_summary_request(body)]
            audit.add(area, "AGT-05", PASS if reformatted else FAIL,
                      "compression request is actually made when history exceeds the window", command,
                      "requests=%d compaction-looking=%d summaryMarkerInjected=%s"
                      % (len(bodies), len(reformatted), summary_seen),
                      detail="message roles of last request: %s"
                      % json.dumps([m.get("role") for m in (bodies[-1].get("messages") or [])],
                                   ensure_ascii=False)[:400],
                      impact="if this fails, the model silently loses older context")
            audit.add(area, "AGT-06", PASS if summary_seen else FAIL,
                      "compaction summary is injected into the request body", command,
                      "summaryMarkerInjected=%s" % summary_seen)
            recent = recent_turns_kept(bodies, history)
            audit.add(area, "AGT-07", PASS if recent["kept"] else FAIL,
                      "most recent turns stay verbatim after compression", command,
                      json.dumps(recent, ensure_ascii=False)[:500])
            fields = {k: (payload or {}).get(k) for k in
                      ("ok", "promptTokens", "contextLimitTokens", "compressedMessages", "code")}
            if fields["compressedMessages"] not in (None, 0):
                status = PASS
            elif fields["compressedMessages"] is None:
                status = BLOCKED
            else:
                status = FAIL
            audit.add(area, "AGT-08", status,
                      "--chat reports compressedMessages>0 when it compressed", command,
                      json.dumps(fields, ensure_ascii=False),
                      detail="context accounting fields are added by task-2"
                      if status == BLOCKED else "foldedTurns=%s" % fields["compressedMessages"])
            audit.add(area, "AGT-08b",
                      PASS if fields["contextLimitTokens"] == 8192 else FAIL,
                      "contextLimitTokens matches the configured window", command,
                      "configured contextFallbackTokens=8192 reported=%r" % fields["contextLimitTokens"],
                      detail="promptTokens=%r" % fields["promptTokens"])

            # Default-window behaviour: after the measured 12-turn ceiling was reported, the CLI/UI
            # projection was widened to 24 turns, so a long session now crosses 72% of a 32768-token
            # window and compaction really engages. Both properties are asserted here: it happened,
            # and it brought the reported prompt back under the threshold.
            (profile / "agent.json").unlink(missing_ok=True)
            server.records.clear()
            default_run = run([str(audit.exe), "--chat", "继续推荐"], timeout=120, env=env)
            default_payload = extract_json_object(default_run["stdout"]) or {}
            threshold = int(32768 * 0.72)
            engaged = default_payload.get("ok") is True and \
                (default_payload.get("compressedMessages") or 0) > 0
            shrank = isinstance(default_payload.get("promptTokens"), int) and \
                default_payload["promptTokens"] <= threshold
            audit.add(area, "AGT-08c", PASS if (engaged and shrank) else FAIL,
                      "at the default window a long history triggers compaction and drops the prompt "
                      "under the threshold (24-turn projection)",
                      "mora --chat '继续推荐' with the same fixture, default 32768 window",
                      json.dumps({k: default_payload.get(k) for k in
                                  ("ok", "promptTokens", "contextLimitTokens", "compressedMessages")},
                                 ensure_ascii=False) + "\nthreshold=%d" % threshold,
                      detail="engaged=%s shrankUnderThreshold=%s; the 40-turn fixture now exceeds "
                             "0.72×32768 through the 24-turn projection" % (engaged, shrank))
            audit.note("agent",
                       "实测边界（已修复并复验）：修复前 --chat 只投影 conversationHistory(...,12) 轮、"
                       "每个 turn 又被 4000 字节截断，默认 32768 窗口下 promptTokens≈13855 < 阈值 23592，"
                       "压缩休眠；Lead 据此把 CLI/UI 的投影统一放宽到 24 轮。本轮实测 "
                       "promptTokens=%s / compressedMessages=%s（默认窗口），压缩已在默认窗口真实触发并把 "
                       "prompt 压回阈值以内。"
                       % (default_payload.get("promptTokens"),
                          default_payload.get("compressedMessages")))

        # --- compression failure must be loud or lossless, never a silent history drop
        server.records.clear()
        server.raw_reply = True

        def failing_handler(entry, handler):
            body = entry.get("body") if isinstance(entry.get("body"), dict) else {}
            if is_summary_request(body):
                handler._respond(500, None, raw=b'{"error":{"message":"stub summary failure"}}')
            else:
                handler._respond(200, {"choices": [{"message": {
                    "role": "assistant", "content": "好的，继续为你推荐。"}}]})

        server.handle = failing_handler
        profile2 = new_profile("agent-fail")
        (profile2 / "agent.json").write_text(
            json.dumps({"contextFallbackTokens": 8192}, ensure_ascii=False), encoding="utf-8")
        shutil.copyfile(profile / "chat-memory.json", profile2 / "chat-memory.json")
        shutil.copyfile(profile / "agent.bin", profile2 / "agent.bin")
        run([str(audit.exe), "--set-agent-key"], timeout=60,
            env=dict(os.environ, MORA_DATA_DIR=str(profile2)), stdin_text=SENTINEL_KEY + "\n")
        env2 = dict(env, MORA_DATA_DIR=str(profile2))
        failed = run([str(audit.exe), "--chat", "继续推荐"], timeout=120, env=env2)
        payload = extract_json_object(failed["stdout"])
        command = "mora --chat where only the summarization POST returns HTTP 500 (stub)"
        if payload is None:
            audit.add(area, "AGT-09", BLOCKED, "compression failure is reported, not silent", command,
                      excerpt(failed["stdout"] + failed["stderr"], 800),
                      detail="no JSON answer at all")
        else:
            requests = [record["body"] for record in server.records
                        if isinstance(record.get("body"), dict)]
            summary_requests = [body for body in requests if is_summary_request(body)]
            main_requests = [body for body in requests if not is_summary_request(body)]
            history_present = False
            if main_requests:
                joined = json.dumps(main_requests[-1], ensure_ascii=False)
                history_present = any(marker in joined for marker in
                                      ("[turn-", "早期对话", "截断", "压缩降级", "确定性"))
            loud = payload.get("ok") is False and bool(payload.get("code"))
            lossless = payload.get("ok") is True and bool(main_requests) and history_present
            silent_drop = payload.get("ok") is True and not history_present
            audit.add(area, "AGT-09", PASS if (loud or lossless) else FAIL,
                      "compression failure is loud or lossless (never silent context loss)", command,
                      json.dumps({k: payload.get(k) for k in ("ok", "code", "error", "toolCalls",
                                                              "compressedMessages")}, ensure_ascii=False)
                      + "\nsummaryRequests=%d mainRequests=%d historyPresent=%s"
                      % (len(summary_requests), len(main_requests), history_present),
                      detail="loud=%s lossless=%s silentDrop=%s" % (loud, lossless, silent_drop),
                      impact="silentDrop would mean the model answered while the folded history vanished")
            # The degraded flag must now be observable from the CLI itself.
            source_sets_flag = source_contains("src/agent.cpp", "compressionDegraded")
            cli_prints_flag = source_contains("src/main.cpp", "compressionDegraded")
            degraded_flag = (payload or {}).get("compressionDegraded")
            degraded_code = (payload or {}).get("compressionCode")
            flag_ok = cli_prints_flag and degraded_flag is True and bool(degraded_code)
            audit.add(area, "AGT-09b",
                      PASS if flag_ok else (BLOCKED if not cli_prints_flag else FAIL),
                      "a degraded compression is observable from the CLI",
                      "grep compressionDegraded in src/main.cpp + inspect --chat JSON",
                      "agent.cpp sets it=%s; main.cpp prints it=%s; compressionDegraded=%r; "
                      "compressionCode=%r" % (source_sets_flag, cli_prints_flag, degraded_flag,
                                              degraded_code),
                      detail="the deterministic degradation keeps the folded turns and labels itself "
                             "in the outcome")
    except Exception as exc:  # noqa: BLE001 - reported as evidence
        audit.add(area, "AGT-00", FAIL, "stub-provider chat session usable", "mora --chat with stub",
                  "%s: %s" % (type(exc).__name__, exc))
    finally:
        stub_stop(server)


def collect_modality_fields(records) -> list:
    found = []
    for record in records:
        body = record.get("body")
        if isinstance(body, dict):
            for key in body:
                if "modalit" in key.lower() or key.lower() == "modality":
                    found.append(key)
    return sorted(set(found))


def is_summary_request(body: dict) -> bool:
    """The compaction call is tool-free; the answer request always carries the tool list.

    Without that discriminator the *degraded summary text* inside the answer request (it contains
    words like 压缩/截断) makes the answer request look like a compaction call.
    """
    if body.get("tools"):
        return False
    messages = body.get("messages") or []
    joined = " ".join(str(message.get("content") or "")[:2000] for message in messages
                      if isinstance(message, dict))
    return any(token in joined for token in ("压缩", "摘要", "summary", "compact"))


def recent_turns_kept(bodies, history) -> dict:
    probes = [turn["content"][:60] for turn in history[-3:]]
    found = {probe: False for probe in probes}
    for body in bodies:
        for message in (body.get("messages") or []):
            content = str(message.get("content") or "")
            for probe in probes:
                if probe in content:
                    found[probe] = True
    newest = probes[-1] if probes else ""
    return {"kept": bool(found.get(newest)), "newest": newest, "found": found}


def synthesize_history(turns: int, chars: int) -> list:
    history = []
    for index in range(turns):
        role = "user" if index % 2 == 0 else "assistant"
        history.append({"role": role,
                        "content": "[turn-%03d]%s" % (index, "安静钢琴曲" * (chars // 6))})
    return history


def area_models(audit: Audit, args) -> None:
    area = "models"
    help_text = run([str(audit.exe), "--help"], timeout=60)["stdout"]
    if "--models" not in help_text:
        audit.add(area, "MOD-00", BLOCKED, "model discovery is reachable from the CLI",
                  "%s --help | findstr --models" % audit.exe.name,
                  "no --models flag in --help; running it would launch the TUI by accident",
                  detail="task-2 adds --models / extends --agent-check",
                  impact="discovery parsing and error codes cannot be exercised through the binary yet")
    profile = new_profile("models")
    run([str(audit.exe), "--set-agent-key"], timeout=60,
        env=dict(os.environ, MORA_DATA_DIR=str(profile)), stdin_text=SENTINEL_KEY + "\n")

    def models_handler(entry, handler):
        if handler.server.delay:
            time.sleep(handler.server.delay)
        status, payload = handler.server.scripted.pop(0) if handler.server.scripted else (200, {})
        if isinstance(payload, bytes):
            handler._respond(status, None, raw=payload)
        else:
            handler._respond(status, payload)

    server, port = start_stub(models_handler)
    server.scripted = []
    server.delay = 0.0
    env = dict(os.environ, MORA_DATA_DIR=str(profile),
               MORA_AGENT_BASE_URL="http://127.0.0.1:%d/v1/chat/completions" % port)
    try:
        catalog_body = {"data": [
            {"id": "round9-multimodal", "context_length": 131072, "max_output_tokens": 8192,
             "architecture": {"input_modalities": ["text", "image"], "output_modalities": ["text"]}},
            {"id": "round9-text", "context_length": 32768},
        ]}
        cases = [
            ("MOD-01 multi-modal + context_length parsed", 200, catalog_body, {"ok": True}),
            ("MOD-02 401 refused", 401, {"error": {"message": "no key"}}, {"code": "unauthorized"}),
            ("MOD-03 403 refused", 403, {"error": {"message": "forbidden"}}, {"code": "unauthorized"}),
            ("MOD-04 404 missing route", 404, {"error": {"message": "not found"}}, {"code": "not_supported"}),
            ("MOD-05 non-JSON body", 200, b"<html>not json</html>", {"code": "bad_response"}),
        ]
        for label, status, payload, expected in cases:
            server.scripted = [(status, payload)]
            result = run([str(audit.exe), "--models"], timeout=60, env=env) if "--models" in help_text \
                else run([str(audit.exe), "--agent-check"], timeout=60, env=env)
            command = "%s --models (stub /models → HTTP %s)" % (audit.exe.name, status)
            if "--models" not in help_text:
                audit.add(area, label, BLOCKED, label, command, "no --models flag",
                          detail="task-2 has not landed --models")
                continue
            parsed = extract_json_object(result["stdout"])
            hits = find_secrets(result["stdout"], result["stderr"])
            detail = "exit=%s" % result["returncode"]
            if "code" in expected:
                code = (parsed or {}).get("code")
                ok = code == expected["code"] and not hits
                detail += " code=%r expected=%r" % (code, expected["code"])
            else:
                ok = bool(parsed) and parsed.get("ok") is True and not hits
                detail += " ok=%r" % ((parsed or {}).get("ok"))
            audit.add(area, label, PASS if ok else FAIL, label, command,
                      excerpt(result["stdout"] + result["stderr"], 900), detail=detail)
            if hits:
                audit.add(area, label + ":secret-scan", FAIL, "no sentinel key in model output",
                          command, json.dumps(hits))

        # 200 with a valid catalog: parsed capability range, then confirm the cache contents.
        server.scripted = [(200, catalog_body)]
        catalog_result = run([str(audit.exe), "--models"], timeout=60, env=env) if "--models" in help_text \
            else run([str(audit.exe), "--agent-check"], timeout=60, env=env)
        parsed = extract_json_object(catalog_result["stdout"])
        command = "%s --models (stub 200 with two models)" % audit.exe.name
        models = (parsed or {}).get("models") if isinstance(parsed, dict) else None
        models = models if isinstance(models, list) else []
        first = next((model for model in models if model.get("id") == "round9-multimodal"), None)
        audit.add(area, "MOD-06",
                  PASS if (first and first.get("contextTokens") == 131072
                           and "image" in (first.get("inputModalities") or [])
                           and "text" in (first.get("outputModalities") or [])) else FAIL,
                  "multi-modal and context_length are parsed into the report", command,
                  json.dumps(models, ensure_ascii=False)[:800],
                  detail="first multimodal entry=%s" % json.dumps(first, ensure_ascii=False)[:300])

        cached_file = profile / "agent.json"
        if cached_file.exists():
            cached_text = cached_file.read_text(encoding="utf-8", errors="replace")
            hits = find_secrets(cached_text)
            audit.add(area, "MOD-07", PASS if not hits else FAIL,
                      "cached catalog never stores the model key", "read %s" % cached_file,
                      "no sentinel in agent.json" if not hits else json.dumps(hits))
            audit.add(area, "MOD-08", PASS if "131072" in cached_text else FAIL,
                      "cached catalog keeps the context window across restarts",
                      "read %s" % cached_file, excerpt(cached_text, 600))
        else:
            audit.add(area, "MOD-07", FAIL, "cached catalog file exists", "read agent.json",
                      "agent.json missing after a successful discovery")

        # A hanging endpoint must degrade to a machine-readable network code, bounded.
        if "--models" in help_text:
            server.delay = 25.0
            server.scripted = [(200, catalog_body)]
            started = time.monotonic()
            result = run([str(audit.exe), "--models"], timeout=90, env=env)
            elapsed = round(time.monotonic() - started, 1)
            parsed = extract_json_object(result["stdout"])
            code = (parsed or {}).get("code")
            audit.add(area, "MOD-09", PASS if code == "network" else FAIL,
                      "a hanging /models endpoint reports a network code within a bounded time",
                      "%s --models (stub sleeps 25s)" % audit.exe.name,
                      json.dumps({"code": code, "elapsedSeconds": elapsed, "exit": result["returncode"]},
                                 ensure_ascii=False),
                      detail="bounded=%s" % (elapsed < 60))
            server.delay = 0.0
    finally:
        stub_stop(server)


def stats_harness(audit: Audit, area: str):
    """The real src/stats.cpp is compiled into the owner's generated harness.

    The harness is only a *driver* (stdin ops -> JSON); every assertion below is the verifier's.
    """
    harness = ROOT / "build" / "m9-stats-check" / "harness.exe"
    if harness.exists():
        return harness, "existing harness built from src/stats.cpp"
    try:
        sys.path.insert(0, str(ROOT / "scripts"))
        import m9_stats_check as owner  # noqa: PLC0415 - optional build helper
        path, error = owner.ensure_harness()
        return path, error or "built through scripts/m9_stats_check.ensure_harness"
    except Exception as exc:  # noqa: BLE001 - reported as evidence
        audit.add(area, "STA-00", BLOCKED, "the stats engine can be driven", "harness build",
                  "%s: %s" % (type(exc).__name__, exc),
                  impact="no independent stats evidence")
        return None, str(exc)


def stats_run(harness: Path, directory: Path, ops: list, timeout: float = 300.0):
    payload = json.dumps({"dir": str(directory), "ops": ops}, ensure_ascii=False).encode("utf-8")
    proc = subprocess.run([str(harness)], cwd=str(ROOT), input=payload, capture_output=True,
                          timeout=timeout)
    text = proc.stdout.decode("utf-8", "replace")
    return extract_json_object(text), proc, text


def local_ts(year, month, day, hour=0, minute=0, second=0) -> int:
    return int(time.mktime((year, month, day, hour, minute, second, 0, 0, -1)))


def play_op(song, name, start, *, duration=200, position=200, completed=1, artist="本地歌手"):
    return {"op": "play", "id": song, "name": name, "artist": artist, "duration": duration,
            "position": position, "completed": completed, "start": start}


def report_of(out, index=0):
    reports = out.get("reports") if isinstance(out, dict) else None
    if not isinstance(reports, list) or index >= len(reports):
        return {}
    return reports[index]


def log_entry(song, name, start, *, artist="本地歌手", listened=60, duration=60, completed=True,
              source="search", **extra):
    """One listen-log.jsonl row in the on-disk schema the stats engine reads back."""
    entry = {"v": 1, "song": int(song), "name": name, "artist": artist, "album": "",
             "source": source, "start": int(start), "listened": int(listened),
             "duration": int(duration), "completed": bool(completed)}
    entry.update(extra)
    return entry


def write_log(directory: Path, rows) -> Path:
    path = directory / "listen-log.jsonl"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(json.dumps(row, ensure_ascii=False) for row in rows) + "\n",
                    encoding="utf-8", newline="")
    return path


def area_stats(audit: Audit, args) -> None:
    area = "stats"
    if not file_present("src/stats.cpp"):
        audit.add(area, "STA-00", BLOCKED, "statistics engine exists", "test -f src/stats.cpp",
                  "src/stats.cpp is absent", impact="aggregation cannot be exercised")
        return
    harness, harness_note = stats_harness(audit, area)
    if harness is None:
        return
    audit.add(area, "STA-00", PASS, "the stats engine is driven directly from src/stats.cpp",
              "generated harness (stdin ops → JSON)", harness_note)

    # --- local-time boundaries: day, month, year, with entries straddling each edge.
    # The op-driven "play" call stamps the wall clock, so boundary rows are written to the log
    # file in the on-disk schema instead; only reading/reporting goes through the engine.
    directory = new_profile("stats-boundary")
    write_log(directory, [
        log_entry(1, "D-2359", local_ts(2026, 10, 24, 23, 59, 50)),
        log_entry(2, "D-0000", local_ts(2026, 10, 25, 0, 0, 10)),
        log_entry(3, "M-0930", local_ts(2026, 9, 30, 23, 59, 59)),
        log_entry(4, "M-1001", local_ts(2026, 10, 1, 0, 0, 1)),
        log_entry(5, "Y-2025", local_ts(2025, 12, 31, 23, 59, 59)),
        log_entry(6, "Y-2026", local_ts(2026, 1, 1, 0, 0, 1)),
    ])
    ops = [{"op": "limit", "limit": 5000},
           {"op": "report", "period": "day", "key": "2026-10-24"},
           {"op": "report", "period": "day", "key": "2026-10-25"},
           {"op": "report", "period": "month", "key": "2026-09"},
           {"op": "report", "period": "month", "key": "2026-10"},
           {"op": "report", "period": "year", "key": "2025"},
           {"op": "report", "period": "year", "key": "2026"}]
    out, proc, raw = stats_run(harness, directory, ops)
    if not isinstance(out, dict) or out.get("ok") is not True:
        audit.add(area, "STA-01", FAIL, "the engine answers a boundary script", "harness ops",
                  excerpt(raw, 900), detail="exit=%s" % proc.returncode)
    else:
        expectations = [
            ("day 2026-10-24", 0, 1, ["D-2359"]),
            ("day 2026-10-25", 1, 1, ["D-0000"]),
            ("month 2026-09", 2, 1, ["M-0930"]),
            ("month 2026-10", 3, 3, ["D-2359", "M-1001"]),
            ("year 2025", 4, 1, ["Y-2025"]),
            ("year 2026", 5, 5, ["Y-2026", "M-0930"]),
        ]
        for label, index, plays, songs in expectations:
            record = report_of(out, index)
            labels = record.get("topSongLabels") or []
            ok = record.get("plays") == plays and all(song in labels for song in songs)
            audit.add(area, "STA-01:%s" % label, PASS if ok else FAIL,
                      "local %s bucket counts exactly its own rows" % label,
                      "harness report period=%s key=%s" % (label.split()[0], label.split()[1]),
                      json.dumps({k: record.get(k) for k in
                                  ("key", "plays", "listenedSeconds", "topSongLabels", "byHourSize")},
                                 ensure_ascii=False)[:600],
                      detail="expected plays=%d with %s" % (plays, songs))
        errors = out.get("errors") or []
        audit.add(area, "STA-02", PASS if not errors else FAIL,
                  "no engine exception during the boundary script", "harness errors[]",
                  json.dumps(errors, ensure_ascii=False)[:400])

    # --- torn / invalid / oversized / CRLF lines must not break the reader
    directory = new_profile("stats-torn")
    log = write_log(directory, [log_entry(11, "OK-1", local_ts(2026, 10, 20, 9, 0, 0))])
    with log.open("a", encoding="utf-8", newline="") as handle:
        handle.write('{"op":"play","id":12,"name":"TORN"\n')
        handle.write(json.dumps(log_entry(13, "HUGE", local_ts(2026, 10, 20, 9, 5, 0),
                                          blob="x" * (2 * 1024 * 1024)), ensure_ascii=False) + "\n")
        handle.write(json.dumps(log_entry(14, "CRLF", local_ts(2026, 10, 20, 9, 10, 0)),
                                ensure_ascii=False) + "\r\n")
    out, proc, raw = stats_run(harness, directory,
                              [{"op": "limit", "limit": 5000},
                               {"op": "report", "period": "day", "key": "2026-10-20"},
                               {"op": "entries", "limit": 50}])
    if not isinstance(out, dict) or out.get("ok") is not True:
        audit.add(area, "STA-03", FAIL, "torn/oversized lines are tolerated", "harness ops",
                  excerpt(raw, 900), detail="exit=%s" % proc.returncode)
    else:
        record = report_of(out, 0)
        entries = (out.get("duringEntries") or [{}])[0]
        names = [item.get("name") for item in (entries.get("items") or [])]
        errors = out.get("errors") or []
        ok = (not errors and "OK-1" in names and "CRLF" in names and record.get("plays") in (2, 3))
        audit.add(area, "STA-03", PASS if ok else FAIL,
                  "a torn line, a 2 MB line and a CRLF line do not break the reader",
                  "fixture: valid + torn + 2MB + CRLF → harness report/entries",
                  "plays=%s names=%s errors=%s hugeKept=%s"
                  % (record.get("plays"), names, errors, "HUGE" in names),
                  detail="no exception, intact rows survive; an oversized row is dropped by design")

    # --- Top-N bound and the 8 KB projection with many distinct songs
    directory = new_profile("stats-topn")
    write_log(directory, [log_entry(100 + index, "TOP-%02d" % index,
                                    local_ts(2026, 10, 21, 8, index % 60, 0),
                                    listened=120, duration=120) for index in range(60)])
    out, proc, raw = stats_run(harness, directory,
                              [{"op": "limit", "limit": 5000},
                               {"op": "report", "period": "day", "key": "2026-10-21"}])
    record = report_of(out, 0)
    labels = record.get("topSongLabels") or []
    size = record.get("projectionBytes")
    audit.add(area, "STA-04", PASS if len(labels) <= 20 else FAIL,
              "Top songs stay at or below 20 rows", "60 distinct plays → report",
              "rows=%d projectionBytes=%s" % (len(labels), size),
              detail="first=%s last=%s" % (labels[:2], labels[-2:]))
    audit.add(area, "STA-05", PASS if isinstance(size, int) and size <= 8192 else FAIL,
              "the model projection stays within 8 KB", "60 distinct plays → listenStatsJson",
              "projectionBytes=%s" % size)
    audit.add(area, "STA-06", PASS if record.get("uniqueSongs") == 60 else FAIL,
              "unique song count is exact", "60 distinct plays → report",
              "uniqueSongs=%s uniqueArtists=%s" % (record.get("uniqueSongs"), record.get("uniqueArtists")))
    audit.add(area, "STA-07", PASS if (len(record.get("availableKeys") or []) == 1) else FAIL,
              "availableKeys lists only buckets that have data", "60 plays on one local day",
              json.dumps(record.get("availableKeys"), ensure_ascii=False)[:300])

    # --- the projection and the engine-written rows must carry no cookie/uid/stream/path
    directory = new_profile("stats-sensitive")
    write_log(directory, [log_entry(201, "SENS", local_ts(2026, 10, 22, 12, 0, 0),
                                    cookie=SENTINEL_COOKIE, uid=SENTINEL_UID,
                                    url=SENTINEL_STREAM)])
    out, proc, raw = stats_run(harness, directory,
                              [{"op": "report", "period": "day", "key": "2026-10-22"},
                               {"op": "play", "id": 202, "name": "AFTER", "artist": "本地",
                                "duration": 60, "position": 60, "completed": 1,
                                "start": local_ts(2026, 10, 22, 12, 5, 0), "source": "queue"},
                               {"op": "flush"}])
    projection = json.dumps((report_of(out, 0) or {}).get("projection"), ensure_ascii=False)
    log_text = (directory / "listen-log.jsonl").read_text(encoding="utf-8", errors="replace")
    engine_text = "\n".join(line for line in log_text.splitlines() if "AFTER" in line)
    hits = find_secrets(projection, engine_text) + [
        {"source": "engine-row", "sentinel": token} for token in ("http://", "https://")
        if token in engine_text
    ]
    audit.add(area, "STA-08", PASS if not hits else FAIL,
              "the projection and the engine-written row carry no cookie/uid/stream URL",
              "fixture with cookie/uid/url fields → report + read the engine-written row",
              "hits=%s" % json.dumps(hits, ensure_ascii=False) if hits
              else "no sentinel in the projection or in the row the engine wrote; projectionBytes=%s"
              % (report_of(out, 0) or {}).get("projectionBytes"))
    audit.add(area, "STA-09", PASS if str(directory) not in engine_text else FAIL,
              "the log never stores the local data path", "read the engine-written row",
              "engine row=%s" % excerpt(engine_text, 300))
    errors = out.get("errors") or []
    audit.add(area, "STA-10", PASS if not errors else FAIL,
              "an entry with unknown fields does not raise", "harness errors[]",
              json.dumps(errors, ensure_ascii=False)[:300])

    # --- compaction above the 16 MB cap keeps the newest rows and shrinks the file
    directory = new_profile("stats-compact")
    log = directory / "listen-log.jsonl"
    base = local_ts(2026, 10, 1, 0, 0, 0)
    with log.open("w", encoding="utf-8", newline="\n") as handle:
        for index in range(20000):
            handle.write(json.dumps({
                "v": 1, "song": 1000 + index, "name": "OLD-%05d" % index, "artist": "批量",
                "album": "", "source": "search", "start": base + index * 60,
                "listened": 60, "duration": 60, "completed": True,
                "note": "padding-" + "p" * 700}, ensure_ascii=False) + "\n")
    size_before = log.stat().st_size
    out, proc, raw = stats_run(harness, directory,
                              [{"op": "play", "id": 99999, "name": "NEWEST", "artist": "本地",
                                "duration": 60, "position": 60, "completed": 1,
                                "start": local_ts(2026, 10, 24, 12, 0, 0), "source": "queue"},
                               {"op": "flush"},
                               {"op": "report", "period": "month", "key": "2026-10"}])
    size_after = log.stat().st_size if log.exists() else -1
    text_after = log.read_text(encoding="utf-8", errors="replace")
    newest_kept = "NEWEST" in text_after
    audit.add(area, "STA-11", PASS if size_before > 16 * 1024 * 1024 and newest_kept and
              (size_after < size_before) else FAIL,
              "a log above 16 MB is compacted without losing the newest listen",
              "write ~%d MB synthetic log → play → flush" % (size_before // (1024 * 1024)),
              "before=%s bytes after=%s bytes newestKept=%s" % (size_before, size_after, newest_kept),
              detail="report plays=%s" % (report_of(out, 0) or {}).get("plays"))
    errors = out.get("errors") or []
    audit.add(area, "STA-12", PASS if not errors else FAIL,
              "compaction does not raise", "harness errors[]",
              json.dumps(errors, ensure_ascii=False)[:300])

    # --- the /stats surface itself is still a UI task; report its state honestly
    if not source_contains("src/ui.cpp", "/stats"):
        audit.add(area, "STA-13", BLOCKED, "/stats renders the report in the terminal",
                  "mora --demo → /stats day", "src/ui.cpp does not register /stats yet",
                  detail="task-3 has not landed the report surface; only the engine is verified here")
    regression = run([sys.executable, str(ROOT / "scripts" / "m9_stats_check.py")], timeout=900)
    audit.add(area, "STA-14", PASS if regression["returncode"] == 0 else FAIL,
              "the owner's stats acceptance script is green", "python scripts/m9_stats_check.py",
              tail(regression["stdout"], 500) + tail(regression["stderr"], 400),
              detail="exit=%s" % regression["returncode"])


def queue_schema_fields(tools, name) -> set:
    for tool in tools:
        if tool.get("name") == name:
            schema = tool.get("inputSchema") or {}
            props = schema.get("properties") or {}
            return set(props)
    return set()


def queue_state(server) -> dict:
    record = server.call_tool("music_queue_list", {})
    structured = record.get("structured")
    if not isinstance(structured, dict):
        return {"_error": record}
    for key in ("queue", "state", "result"):
        if isinstance(structured.get(key), dict):
            structured = structured[key]
            break
    items = structured.get("items")
    if not isinstance(items, list):
        items = []
    ids = []
    for item in items:
        if isinstance(item, dict):
            ids.append(item.get("id"))
    return {
        "total": structured.get("total", structured.get("queueSize")),
        "index": structured.get("index", structured.get("queueIndex")),
        "mode": structured.get("mode"),
        "ids": ids,
        "raw": structured,
    }


def queue_edit(server, arguments) -> dict:
    return server.call_tool("music_queue_edit", arguments)


def queue_edit_retry(server, arguments, attempts: int = 4, wait: float = 1.2):
    """The UI guards its own request queue and answers `busy` while it finishes a write.

    Retrying here is the *client's* bounded behaviour; the tool itself must not retry. The full
    response list is kept so the report can show whether `busy` was seen.
    """
    responses = []
    for _ in range(attempts):
        record = queue_edit(server, arguments)
        responses.append(record)
        if queue_error(server, record) != "busy":
            break
        time.sleep(wait)
    return responses[-1], responses


def queue_error(server, record) -> str:
    code = server.tool_error_code(record)
    if code is None and record.get("transportError"):
        return "transport"
    return code


def keyword_groups(text: str, groups) -> dict:
    """Each group is satisfied when any of its candidate tokens appears in `text`."""
    return {name: any(token in text for token in candidates) for name, candidates in groups}


MUSIC_SENSE_GROUPS = [
    ("act-on-vague", ("直接", "不必追问", "不要反问", "无需确认", "不用确认", "别问")),
    ("series-not-single", ("整张", "整个歌单", "整张专辑", "歌单", "专辑")),
    ("series-count", ("5", "20", "多首", "整组")),
    ("replace", ("替换",)),
    ("append", ("追加", "append", "add_next", "接着放", "再加", "排在后面")),
    ("confirm-playback", ("music_now_playing",)),
]


def area_intent(audit: Audit, args) -> None:
    """task-6: agent music common sense, series playback, replace vs append."""
    area = "intent"
    profile = new_profile("intent")
    run([str(audit.exe), "--set-agent-key"], timeout=60,
        env=dict(os.environ, MORA_DATA_DIR=str(profile)), stdin_text=SENTINEL_KEY + "\n")

    def sense_handler(entry, handler):
        handler._respond(200, {"choices": [{"message": {"role": "assistant",
                                                        "content": "好的。"}}]})

    server, port = start_stub(sense_handler)
    env = dict(os.environ, MORA_DATA_DIR=str(profile),
               MORA_AGENT_BASE_URL="http://127.0.0.1:%d/v1/chat/completions" % port)
    try:
        result = run([str(audit.exe), "--chat", "随便来点歌"], timeout=90, env=env)
        bodies = [record["body"] for record in server.records if isinstance(record.get("body"), dict)]
        system_messages = [message.get("content") or ""
                           for body in bodies for message in (body.get("messages") or [])
                           if isinstance(message, dict) and message.get("role") == "system"]
        system_text = "\n".join(system_messages)
        groups = keyword_groups(system_text, MUSIC_SENSE_GROUPS)
        missing = [name for name, ok in groups.items() if not ok]
        audit.add(area, "INT-01", PASS if not missing else FAIL,
                  "the built-in system prompt carries the music-operation common sense",
                  "mora --chat (stub) → read the system message",
                  json.dumps(groups, ensure_ascii=False) + "\n" + excerpt(system_text, 1200),
                  detail="missing=%s" % missing,
                  impact="without this the model says it cannot act on vague requests")

        # Every built-in persona prompt must carry the same semantics (task-6 defines this as a
        # contract for all built-ins plus a shared custom fallback). The window is bounded to the
        # function body so digits in the sprite code cannot satisfy the check by accident.
        persona_source = (ROOT / "src" / "persona.cpp").read_text(encoding="utf-8", errors="replace")
        marker = "personaSystemPrompt"
        prompt_body = ""
        if marker in persona_source:
            start = persona_source.index(marker)
            prompt_body = persona_source[start:start + 6000]
        persona_groups = keyword_groups(prompt_body, MUSIC_SENSE_GROUPS)
        missing = [name for name, ok in persona_groups.items() if not ok]
        audit.add(area, "INT-02", PASS if not missing else FAIL,
                  "personaSystemPrompt declares the same music-operation common sense",
                  "read src/persona.cpp personaSystemPrompt (6000-char window)",
                  json.dumps(persona_groups, ensure_ascii=False) + "\n" + excerpt(prompt_body, 900),
                  detail="missing=%s (source-level window; the runtime persona prompt is not "
                         "captured in this area)" % missing)

        # Tool contract: replace must exist and append/add_next must be described as non-replacing.
        tools = []
        with McpServer(audit.exe, profile, "intent-tools") as mcp:
            mcp.initialize()
            tools = mcp.tools()
            edit = next((tool for tool in tools if tool.get("name") == "music_queue_edit"), None)
            schema = (edit or {}).get("inputSchema") or {}
            props = schema.get("properties") or {}
            op_enum = ((props.get("op") or {}).get("enum")) or []
            description = (edit or {}).get("description", "")
            id_field = next((key for key in ("ids", "items", "tracks", "songs") if key in props), None)
            id_schema = props.get(id_field) if id_field else {}
            audit.add(area, "INT-03", PASS if "replace" in op_enum else FAIL,
                      "music_queue_edit advertises op=replace", "tools/list → inputSchema",
                      json.dumps({"op": op_enum, "idField": id_field,
                                  "idSchema": id_schema}, ensure_ascii=False)[:600],
                      detail="replace is the series-playback entry point")
            audit.add(area, "INT-04",
                      PASS if ("append" in description and "add_next" in description
                               and ("替换" in description or "replace" in description)) else FAIL,
                      "the tool description states replace vs append/add_next semantics",
                      "tools/list → music_queue_edit.description", excerpt(description, 900))
            search = next((tool for tool in tools if tool.get("name") == "music_search"), None)
            search_description = (search or {}).get("description", "")
            audit.add(area, "INT-05",
                      PASS if any(token in search_description for token in ("10", "20", "多", "整")) else FAIL,
                      "music_search tells the model how to fetch a series", "tools/list → description",
                      excerpt(search_description, 700))
            malformed = [
                ({"op": "replace"}, "replace without ids"),
                ({"op": "replace", "ids": []}, "replace with an empty id list"),
                ({"op": "replace", "ids": [0]}, "replace with a non-positive id"),
                ({"op": "replace", "ids": "1,2"}, "replace with a non-list payload"),
            ]
            for arguments, label in malformed:
                record = mcp.call_tool("music_queue_edit", arguments)
                code = mcp.tool_error_code(record)
                audit.add(area, "INT-06:%s" % label, PASS if code == "invalid_arguments" else FAIL,
                          "music_queue_edit rejects %s" % label,
                          "tools/call music_queue_edit %s" % json.dumps(arguments, ensure_ascii=False),
                          json.dumps(record, ensure_ascii=False)[:500], detail="code=%r" % code)
            valid = mcp.call_tool("music_queue_edit", {"op": "replace", "ids": [1, 2, 3, 4, 5]})
            code = mcp.tool_error_code(valid)
            audit.add(area, "INT-07", PASS if code == "needs_player" else FAIL,
                      "a valid replace with no player reports needs_player",
                      "tools/call music_queue_edit {op:replace,ids:[1..5]}",
                      json.dumps(valid, ensure_ascii=False)[:500], detail="code=%r" % code)
    finally:
        stub_stop(server)

    # Real-model intent mapping is the only way to observe "fuzzy understanding" itself. It spends
    # the user's own provider quota, so it is opt-in and bounded to four requests.
    prompts = [
        ("vague", "随便来点歌", ("music_search", "music_recommend", "music_queue_edit", "music_play")),
        ("artist", "来点周杰伦的", ("music_search", "music_recommend", "music_queue_edit")),
        ("album", "播放整张专辑 叶惠美", ("music_open_collection", "music_play", "music_search",
                                          "music_queue_edit")),
        ("append", "再加上一首周杰伦的歌", ("music_queue_edit", "music_play")),
    ]
    if not args.real_intent or not os.environ.get("MORA_VERIFIER_ALLOW_REAL_INTENT"):
        reason = ("not approved: the four prompts would spend the user's own provider quota, which "
                  "the user has not authorised (Lead ruling). Re-run only with explicit user "
                  "authorisation and MORA_VERIFIER_ALLOW_REAL_INTENT=1."
                  if args.real_intent else
                  "not run: real-model language mapping needs the user's own provider key/quota, so "
                  "it is opt-in and not approved by default")
        audit.add(area, "INT-08", NOT_VERIFIED,
                  "the real model acts on vague / series / append requests",
                  "python scripts/verify_round9.py --areas intent --real-intent "
                  "(+ MORA_VERIFIER_ALLOW_REAL_INTENT=1, user authorisation required)",
                  reason,
                  detail="everything offline is covered by INT-01..INT-07 and the live queue "
                         "semantics in the queue area; only the model's own language mapping is "
                         "unobservable without a real provider")
    else:
        for label, prompt, expected_tools in prompts:
            real = run([str(audit.exe), "--chat", prompt], timeout=180)
            parsed = extract_json_object(real["stdout"]) or {}
            trace = parsed.get("trace") or []
            called = [item.get("tool") for item in trace if isinstance(item, dict)]
            audit.add(area, "INT-08:%s" % label,
                      PASS if any(tool in expected_tools for tool in called) else FAIL,
                      "real model maps %r to a music action" % prompt,
                      "mora --chat %r (real provider)" % prompt,
                      json.dumps({"ok": parsed.get("ok"), "code": parsed.get("code"),
                                  "tools": called, "answer": (parsed.get("answer") or "")[:160]},
                                 ensure_ascii=False),
                      detail="expected one of %s" % (expected_tools,))


def area_queue(audit: Audit, args) -> None:
    area = "queue"
    tools = []
    exe = audit.exe
    profile = new_profile("queue")
    with McpServer(exe, profile, "queue-probe") as probe:
        try:
            probe.initialize()
            tools = probe.tools()
        except Exception as exc:  # noqa: BLE001 - reported below
            audit.add(area, "QUE-00", FAIL, "MCP server starts for queue verification", "mora --mcp",
                      "%s: %s" % (type(exc).__name__, exc))
            return
    audit.environment["mcpTools"] = sorted(tool.get("name") for tool in tools)
    missing = [name for name in ("music_queue_list", "music_queue_edit") if name not in audit.environment["mcpTools"]]
    if missing:
        audit.add(area, "QUE-00", BLOCKED, "queue tools exist", "tools/list",
                  "missing: %s (advertised: %s)" % (missing, audit.environment["mcpTools"]),
                  detail="task-1 has not landed music_queue_list/music_queue_edit",
                  impact="every queue behaviour check is deferred")
        return

    default_profile = Path(os.environ.get("LOCALAPPDATA", "")) / "MORA"
    session_file = default_profile / "session.bin"
    if not session_file.exists():
        audit.add(area, "QUE-00", BLOCKED, "an authenticated player can build a queue",
                  "test -f %LOCALAPPDATA%\\MORA\\session.bin", "no saved session in the default profile",
                  detail="a live TUI needs a real account session to search/play; the queue tools "
                         "themselves are present",
                  impact="queue behaviour deferred")
        return
    # A copy, never the original: the isolated profile keeps the user's own player untouched.
    shutil.copyfile(session_file, profile / "session.bin")

    # Listen-history fixture for the live /stats surface: today's local rows, with sentinel cookie/
    # uid/url fields that must never be rendered. Written before the player starts, and the stats
    # probe runs before any live playback so the report contains exactly this fixture.
    today = time.localtime()
    fixture_base = local_ts(today.tm_year, today.tm_mon, today.tm_mday, 8, 0, 0)
    write_log(profile, [
        log_entry(9001, "夹具曲一", fixture_base + 60, artist="夹具歌手"),
        log_entry(9002, "夹具曲二", fixture_base + 200, artist="夹具歌手", listened=90, duration=120),
        log_entry(9001, "夹具曲一", fixture_base + 400, artist="夹具歌手",
                  cookie=SENTINEL_COOKIE, uid=SENTINEL_UID, url=SENTINEL_STREAM),
    ])

    try:
        console = HiddenConsole(110, 34)
    except OSError as exc:
        audit.add(area, "QUE-00", BLOCKED, "console for the live TUI", "AllocConsole", str(exc))
        return

    tui = None
    try:
        tui = TuiSession(exe, console, "queue", seconds=240, profile=profile, demo=False).start()
        frame = tui.wait_for(lambda text: "保存过网易云音乐登录" in text or "阿木" in text
                             or "M O R A" in text, timeout=20)
        if not frame:
            audit.add(area, "QUE-01", BLOCKED, "live TUI starts with the copied session",
                      "mora --data-dir <profile> --snapshot", excerpt(tui.frame(), 800),
                      detail="stderr=%s" % excerpt(tui.stderr_path.read_text(encoding="utf-8",
                                                                          errors="replace"), 400))
            return
        if "保存过网易云音乐登录" in frame:
            console.send_key(13)  # Enter: use the saved account for this profile only
            frame = tui.wait_for(lambda text: "保存过网易云音乐登录" not in text, timeout=15) or tui.frame()
        audit.add(area, "QUE-01", PASS, "live TUI owns its isolated bridge pipe",
                  "mora --data-dir <queue profile> (live, copied session)",
                  save_frame("queue-live-home", frame)["path"])

        with McpServer(exe, profile, "queue") as server:
            server.initialize()
            status = server.call_tool("music_get_status")
            structured = status.get("structured") if isinstance(status.get("structured"), dict) else {}
            if structured.get("playerSessionAttached") is not True:
                audit.add(area, "QUE-02", BLOCKED, "MCP drives the isolated live player",
                          "mora --mcp --data-dir <same profile> → music_get_status",
                          json.dumps(status, ensure_ascii=False)[:800],
                          detail="playerSessionAttached is not true",
                          impact="queue E2E deferred")
                return
            audit.add(area, "QUE-02", PASS, "MCP drives the isolated live player",
                      "music_get_status", json.dumps(structured, ensure_ascii=False)[:600])

            # --- live terminal surfaces (task-3): stats from a real seeded log, honest review and
            # models. Probed before any playback so /stats day contains exactly the fixture.
            live_probes = [
                ("stats-day", "/stats day", ["夹具"],
                 "the /stats day terminal report renders the seeded local history"),
                ("stats-month", "/stats month", ["夹具"],
                 "the /stats month terminal report renders the seeded local history"),
                ("review", "/review", ["锐评"],
                 "/review refuses honestly without data or a model key"),
                ("models", "/models", ["模型"],
                 "/models reports the cached catalogue honestly"),
            ]
            for label, command, markers, requirement in live_probes:
                # The previous surface (stats panel / chat view) would swallow the typed command.
                tui.key(27)
                settle(tui, tui.frame(), timeout=2.0)
                before = tui.frame()
                tui.command(command)
                shown = settle(tui, before)
                save_frame("live-%s" % label, shown)
                present = [marker for marker in markers if marker in shown]
                honest = any(token in shown for token in
                             ("未配置", "失败", "错误", "无法", "没有", "空", "缓存", "0"))
                hits = find_secrets(shown)
                if label.startswith("stats"):
                    ok = bool(present)
                elif label == "review":
                    ok = bool(present) and honest
                else:
                    ok = bool(present) and honest
                audit.add(area, "LIVE-%s" % label, PASS if (ok and not hits) else FAIL,
                          requirement, "live mora --data-dir <profile> → %r" % command,
                          "markers=%s honestMarker=%s sentinelHits=%s\n%s"
                          % (present, honest, hits, excerpt(shown, 1000)),
                          detail="fixture plays=3 (two distinct tracks) seeded in %s" % profile)

            # Silent from here on: the user may be listening to their own player.
            server.call_tool("music_control", {"action": "volume", "value": 0})

            fields = queue_schema_fields(tools, "music_queue_edit")
            list_fields = queue_schema_fields(tools, "music_queue_list")
            audit.environment["queueSchemas"] = {
                "music_queue_edit": sorted(fields),
                "music_queue_list": sorted(list_fields),
            }

            # --- build a queue from the user's real, permitted catalogue (bounded: one search)
            search = server.call_tool("music_search", {"query": args.queue_query, "limit": 5}, timeout=60)
            found = (search.get("structured") or {}).get("items") if isinstance(search.get("structured"), dict) else None
            found = found if isinstance(found, list) else []
            if not found:
                audit.add(area, "QUE-03", BLOCKED, "queue can be populated from a real search",
                          "music_search query=%r limit=5" % args.queue_query,
                          json.dumps(search, ensure_ascii=False)[:900],
                          detail="no search results (account/quota/network); nothing to queue")
                return
            play = server.call_tool("music_play", {"id": found[0].get("id")}, timeout=60)
            if play.get("isError"):
                audit.add(area, "QUE-03", BLOCKED, "queue can be populated from a real search",
                          "music_play id=%s" % found[0].get("id"),
                          json.dumps(play, ensure_ascii=False)[:900],
                          detail="playback refused (account/quota/network)")
                return
            time.sleep(2.0)
            state = queue_state(server)
            audit.add(area, "QUE-03", PASS if state.get("total", 0) >= 1 else FAIL,
                      "playing a real track builds a queue", "music_search → music_play → music_queue_list",
                      json.dumps(state.get("raw"), ensure_ascii=False)[:900],
                      detail="total=%s index=%s mode=%s" % (state.get("total"), state.get("index"),
                                                            state.get("mode")))

            # --- mode round-trip
            for mode, expected in (("sequential", "Sequential"), ("repeat_all", "RepeatAll"),
                                   ("repeat_one", "RepeatOne"), ("shuffle", "Shuffle")):
                record, attempts = queue_edit_retry(server, {"op": "set_mode", "mode": mode})
                text = json.dumps(record, ensure_ascii=False)
                busy = sum(1 for item in attempts if queue_error(server, item) == "busy")
                text_l = text.lower()
                ok = (record.get("isError") is not True
                      and (expected.lower() in text_l or mode.lower() in text_l))
                audit.add(area, "QUE-04:%s" % mode, PASS if ok else FAIL,
                          "set_mode(%s) is accepted and reflected" % mode,
                          "music_queue_edit {op:set_mode,mode:%s}" % mode, text[:800],
                          detail="expected canonical %s; busyResponses=%d" % (expected, busy))

            # --- single loop: the same track must be re-played, not skipped away
            queue_edit_retry(server, {"op": "set_mode", "mode": "repeat_one"})
            before = server.call_tool("music_now_playing")
            before_id = now_playing_id(before)
            next_record = server.call_tool("music_control", {"action": "next"})
            time.sleep(2.0)
            after = server.call_tool("music_now_playing")
            after_id = now_playing_id(after)
            audit.add(area, "QUE-05", PASS if before_id and before_id == after_id else FAIL,
                      "repeat_one keeps the same track on advance", "set_mode repeat_one → music_control next",
                      "before=%r after=%r\n%s\n%s" % (before_id, after_id,
                                                      json.dumps(before, ensure_ascii=False)[:300],
                                                      json.dumps(after, ensure_ascii=False)[:300]),
                      detail="repeat_one must replay the track instead of jumping to the next row")
            # End-of-track replay, if a real duration is known.
            duration = now_playing_duration(after)
            if duration and duration > 6:
                seek = server.call_tool("music_control", {"action": "seek", "value": max(1, duration - 3)})
                restarted = None
                deadline = time.monotonic() + min(30, duration + 5)
                while time.monotonic() < deadline:
                    time.sleep(1.5)
                    probe = server.call_tool("music_now_playing")
                    if (now_playing_id(probe) == after_id
                            and (now_playing_position(probe) or 999) < 5):
                        restarted = probe
                        break
                audit.add(area, "QUE-05b", PASS if restarted else FAIL,
                          "repeat_one restarts the same track after it finishes",
                          "seek to duration-3 → wait → music_now_playing",
                          json.dumps(restarted or server.call_tool("music_now_playing"),
                                     ensure_ascii=False)[:600],
                          detail="seek=%s" % json.dumps(seek, ensure_ascii=False)[:200])
            else:
                audit.add(area, "QUE-05b", NOT_VERIFIED,
                          "repeat_one restarts the same track after it finishes",
                          "seek to duration-3 → wait → music_now_playing",
                          "duration reported as %r" % duration,
                          detail="cannot wait out a track without a usable duration")

            # --- index semantics and boundaries
            queue_edit_retry(server, {"op": "set_mode", "mode": "sequential"})
            state = queue_state(server)
            total = state.get("total") or 0
            base = None
            for candidate in (0, 1):
                record, _ = queue_edit_retry(server, {"op": "play_index", "index": candidate})
                if record.get("isError") is not True:
                    observed = queue_state(server)
                    if observed.get("index") == 0:
                        base = candidate
                        break
            audit.add(area, "QUE-06", PASS if (base == 0 or total < 2) else FAIL,
                      "play_index is 0-based as the contract documents",
                      "music_queue_edit {op:play_index,index:0|1}",
                      "total=%s observed base=%r\n%s" % (total, base,
                                                         json.dumps(state.get("raw"), ensure_ascii=False)[:400]),
                      detail="index 0 selected the first row" if base == 0 else
                      "index 1 selected the first row (contradicts the documented 0 base)"
                      if base == 1 else "neither index selected row 0")
            out_of_range = [total, total + 5, -1, 4294967296]
            for index in out_of_range:
                record = queue_edit(server, {"op": "play_index", "index": index})
                code = queue_error(server, record)
                ok = record.get("isError") is True and code in ("invalid_arguments", "queue_boundary")
                audit.add(area, "QUE-07:%s" % index, PASS if ok else FAIL,
                          "out-of-range play_index(%s) is refused, not a crash" % index,
                          "music_queue_edit {op:play_index,index:%s}" % index,
                          json.dumps(record, ensure_ascii=False)[:600], detail="code=%r" % code)
            record = queue_edit(server, {"op": "remove", "index": total + 3})
            audit.add(area, "QUE-07:remove", PASS if (record.get("isError") is True and
                      queue_error(server, record) in ("invalid_arguments", "queue_boundary")) else FAIL,
                      "out-of-range remove is refused", "music_queue_edit {op:remove,index:total+3}",
                      json.dumps(record, ensure_ascii=False)[:600])
            record = queue_edit(server, {"op": "move", "from": 0, "to": total + 9})
            audit.add(area, "QUE-07:move", PASS if record.get("isError") is True else FAIL,
                      "out-of-range move is refused", "music_queue_edit {op:move,from:0,to:total+9}",
                      json.dumps(record, ensure_ascii=False)[:600])

            # --- replace / append / add_next semantics (task-6 §3/§4 and task-5 队列项)
            pool = [item for item in found if item.get("id") != found[0].get("id")]
            key_items = None
            for candidate in ("ids", "items", "tracks", "songs"):
                if candidate in fields:
                    key_items = candidate
                    break
            if pool and key_items:
                def items_payload(items):
                    return [item.get("id") for item in items] if key_items == "ids" else items

                # replace: the whole queue becomes the given set and playback restarts at its first row
                chosen = [found[0]] + pool[:4]
                wanted = [item.get("id") for item in chosen]
                current_before = now_playing_id(server.call_tool("music_now_playing"))
                replaced, replace_attempts = queue_edit_retry(
                    server, {"op": "replace", key_items: items_payload(chosen)})
                time.sleep(1.5)
                after_replace = queue_state(server)
                ids_after = after_replace.get("ids") or []
                ok_replace = (replaced.get("isError") is not True
                              and after_replace.get("total") == len(chosen)
                              and set(ids_after) == set(wanted)
                              and after_replace.get("index") in (0, 1))
                audit.add(area, "QUE-16", PASS if ok_replace else FAIL,
                          "op=replace swaps the whole queue and starts from its first row",
                          "music_queue_edit {op:replace,%s:[%d ids]}" % (key_items, len(chosen)),
                          "playing before=%s after total=%s index=%s ids=%s\n%s"
                          % (current_before, after_replace.get("total"), after_replace.get("index"),
                             ids_after, json.dumps(replaced, ensure_ascii=False)[:400]),
                          detail="replace must not leave older rows behind")

                # append grows the queue exactly once per call and never replaces the playing row
                playback_before = now_playing_id(server.call_tool("music_now_playing"))
                size_before = after_replace.get("total")
                first, _ = queue_edit_retry(server, {"op": "append", key_items: items_payload(pool[:1])})
                size_mid = queue_state(server).get("total")
                second, _ = queue_edit_retry(server, {"op": "append", key_items: items_payload(pool[:1])})
                after_append = queue_state(server)
                size_after = after_append.get("total")
                playback_after = now_playing_id(server.call_tool("music_now_playing"))
                expected_step = len(pool[:1])
                no_retry = (isinstance(size_before, int) and isinstance(size_mid, int)
                            and isinstance(size_after, int)
                            and size_mid - size_before == expected_step
                            and size_after - size_mid == expected_step)
                audit.add(area, "QUE-08", PASS if no_retry else FAIL,
                          "append writes exactly once per call (no automatic retry/duplicate)",
                          "music_queue_edit {op:append,...} twice",
                          "total %s → %s → %s (payload=%d)\n%s"
                          % (size_before, size_mid, size_after, expected_step,
                             json.dumps(second, ensure_ascii=False)[:400]),
                          detail="field=%s" % key_items)
                audit.add(area, "QUE-17", PASS if playback_before == playback_after else FAIL,
                          "append does not replace or restart the current queue",
                          "music_queue_edit {op:append,...} twice",
                          "playing %r → %r, total %s → %s"
                          % (playback_before, playback_after, size_before, size_after))

                # add_next inserts the new row right after the current one
                inserted_items = pool[1:2] or pool[:1]
                inserted_ids = [item.get("id") for item in inserted_items]
                added, _ = queue_edit_retry(server, {"op": "add_next", key_items: items_payload(inserted_items)})
                after_next = queue_state(server)
                ids_next = after_next.get("ids") or []
                index_next = after_next.get("index")
                current_id = now_playing_id(server.call_tool("music_now_playing"))
                pos_current = ids_next.index(current_id) if current_id in ids_next else -1
                pos_inserted = ids_next.index(inserted_ids[0]) if inserted_ids and \
                    inserted_ids[0] in ids_next else -1
                ok_next = (added.get("isError") is not True and pos_inserted == pos_current + 1
                           and current_id == playback_after)
                audit.add(area, "QUE-09", PASS if ok_next else FAIL,
                          "add_next inserts the row right after the current one",
                          "music_queue_edit {op:add_next,...}",
                          "ids=%s current=%s pos(current)=%s pos(inserted)=%s index=%s\n%s"
                          % (ids_next, current_id, pos_current, pos_inserted, index_next,
                             json.dumps(added, ensure_ascii=False)[:400]),
                          detail="add_next must not touch the playing row")
            else:
                audit.add(area, "QUE-08", NOT_VERIFIED, "append/add_next exercise a second track",
                          "music_queue_edit {op:append}",
                          "fields=%s pool=%d" % (sorted(fields), len(pool)),
                          detail="the tool schema exposes no item/id field the verifier can drive")

            # --- remove / move keep the playing row, clear empties the queue
            state = queue_state(server)
            if (state.get("total") or 0) >= 3:
                playing = now_playing_id(server.call_tool("music_now_playing"))
                moved, move_attempts = queue_edit_retry(server, {"op": "move", "from": 0, "to": 2})
                still = now_playing_id(server.call_tool("music_now_playing"))
                audit.add(area, "QUE-10", PASS if moved.get("isError") is not True and playing == still else FAIL,
                          "move keeps the listener on the same row",
                          "music_queue_edit {op:move,from:0,to:2}",
                          "playing %r → %r busy=%d\n%s"
                          % (playing, still, sum(1 for item in move_attempts
                                                 if queue_error(server, item) == "busy"),
                             json.dumps(moved, ensure_ascii=False)[:400]))
                removed, remove_attempts = queue_edit_retry(server, {"op": "remove", "index": 0})
                audit.add(area, "QUE-11", PASS if removed.get("isError") is not True else FAIL,
                          "remove drops one row", "music_queue_edit {op:remove,index:0}",
                          json.dumps(queue_state(server).get("raw"), ensure_ascii=False)[:500])
            else:
                audit.add(area, "QUE-10", NOT_VERIFIED, "remove/move on a ≥3-row queue",
                          "music_queue_edit {op:move}/{op:remove}",
                          "total=%s" % state.get("total"), detail="queue too short to move rows meaningfully")

            # --- shuffle: with an explicit seed and the same starting state the permutation must be
            # reproducible, and one full pass must visit every row exactly once. Track changes are
            # asynchronous, so each advance is confirmed by polling now-playing before sampling.
            shuffle_ids = [item.get("id") for item in found[:6] if item.get("id")]
            if len(shuffle_ids) >= 3:
                def advance_and_read(previous, timeout: float = 10.0):
                    for _ in range(4):
                        move = server.call_tool("music_control", {"action": "next"})
                        if queue_error(server, move) != "busy":
                            break
                        time.sleep(1.0)
                    deadline = time.monotonic() + timeout
                    while time.monotonic() < deadline:
                        time.sleep(0.4)
                        current = now_playing_id(server.call_tool("music_now_playing"))
                        if current is not None and current != previous:
                            return current
                    remaining = now_playing_id(server.call_tool("music_now_playing"))
                    return remaining if remaining is not None else previous

                def shuffle_order(seed, steps):
                    queue_edit_retry(server, {"op": "replace", "ids": shuffle_ids})
                    queue_edit_retry(server, {"op": "play_index", "index": 0})
                    time.sleep(1.2)
                    start_state = queue_state(server)
                    start_id = now_playing_id(server.call_tool("music_now_playing"))
                    queue_edit_retry(server, {"op": "set_mode", "mode": "shuffle", "seed": seed})
                    order = [now_playing_id(server.call_tool("music_now_playing"))]
                    for _ in range(steps - 1):
                        order.append(advance_and_read(order[-1]))
                    return order, {"index": start_state.get("index"), "startId": start_id,
                                   "total": start_state.get("total"),
                                   "afterSeedIndex": queue_state(server).get("index")}

                steps = len(shuffle_ids)
                orders = []
                states = []
                for _ in range(3):
                    order, state = shuffle_order(7, steps)
                    orders.append(order)
                    states.append(state)
                same_state = all(state == states[0] for state in states)
                reproducible = all(order == orders[0] for order in orders)
                audit.add(area, "QUE-12", PASS if reproducible else FAIL,
                          "shuffle is reproducible for the same seed and start row",
                          "replace(ids) → play_index 0 → set_mode shuffle seed=7 → next ×N, three times",
                          "orders=%s\nstates=%s\nseed ids=%s" % (orders, states, shuffle_ids),
                          detail="identical start state across passes=%s; requirement: an explicit "
                                 "seed must always re-shuffle to the same order, and a shuffle wrap "
                                 "must reuse that permutation (the earlier PlaybackSession::setMode "
                                 "early-return is fixed)" % same_state,
                          impact="同一 seed 不复现会让随机模式无法被调用方解释/重放")
                cycle_ok = sorted(x for x in orders[0] if x is not None) == sorted(shuffle_ids)
                audit.add(area, "QUE-13", PASS if cycle_ok else FAIL,
                          "one shuffle cycle visits every queued row exactly once",
                          "replace(ids) → shuffle seed=7 → next ×N",
                          "order=%s\nexpected multiset=%s" % (orders[0], sorted(shuffle_ids)))

                # Regression for the exact D2 root cause: an explicit seed must rebuild the
                # permutation even when the mode does not change. `changed:false` is not evidence
                # either way (TOOL_CONTRACTS); the order itself is.
                seed_orders = {}
                for seed in (1, 7, 13):
                    order_a, _ = shuffle_order(seed, steps)
                    order_b, _ = shuffle_order(seed, steps)
                    seed_orders[seed] = (order_a, order_b)
                deterministic = all(pair[0] == pair[1] for pair in seed_orders.values())
                distinct = len({tuple(pair[0]) for pair in seed_orders.values()}) >= 2
                audit.add(area, "QUE-19", PASS if (deterministic and distinct) else FAIL,
                          "an explicit seed always reshuffles, even with the mode unchanged",
                          "for seed in {1,7,13}: replace → play_index 0 → set_mode shuffle(seed) → "
                          "next ×N, twice per seed",
                          "seedOrders=%s\ndeterministic=%s distinctPermutations=%s"
                          % (json.dumps({str(k): v for k, v in seed_orders.items()}, ensure_ascii=False),
                             deterministic, distinct),
                          detail="a no-op set_mode would make two different seeds visit the same "
                                 "order (the pre-fix behaviour); queue-tools fixed the early return")
            else:
                audit.add(area, "QUE-12", NOT_VERIFIED, "shuffle determinism",
                          "replace(ids) → shuffle seed=7", "only %d ids available" % len(shuffle_ids))

            # --- clear
            cleared, clear_attempts = queue_edit_retry(server, {"op": "clear"})
            time.sleep(1.0)
            after_clear = queue_state(server)
            busy = sum(1 for item in clear_attempts if queue_error(server, item) == "busy")
            audit.add(area, "QUE-14", PASS if (cleared.get("isError") is not True and
                      (after_clear.get("total") in (0, None))) else FAIL,
                      "clear empties the queue", "music_queue_edit {op:clear}",
                      "busyResponses=%d response=%s\nstate=%s"
                      % (busy, json.dumps(cleared, ensure_ascii=False)[:300],
                         json.dumps(after_clear.get("raw"), ensure_ascii=False)[:400]))

            hits = find_secrets(server.stderr_text(), tui.frame())
            audit.add(area, "QUE-15", PASS if not hits else FAIL,
                      "queue session leaks no cookie/uid/stream sentinel", "scan MCP stderr + TUI frame",
                      "no sentinel found" if not hits else json.dumps(hits))
    except Exception as exc:  # noqa: BLE001 - reported as evidence
        audit.add(area, "QUE-CRASH", FAIL, "queue E2E runs to completion", "python verify_round9.py --areas queue",
                  "%s: %s" % (type(exc).__name__, exc))
    finally:
        if tui is not None:
            tui.stop()
        console.close()


def _structured(record):
    return record.get("structured") if isinstance(record.get("structured"), dict) else {}


def now_playing_id(record):
    data = _structured(record)
    for key in ("track", "current", "song", "nowPlaying"):
        if isinstance(data.get(key), dict):
            data = data[key]
            break
    return data.get("id")


def now_playing_duration(record):
    data = _structured(record)
    value = data.get("durationSeconds", data.get("duration"))
    return value if isinstance(value, (int, float)) and value > 0 else None


def now_playing_position(record):
    data = _structured(record)
    value = data.get("positionSeconds", data.get("position"))
    return value if isinstance(value, (int, float)) else None


AREAS = {
    "regression": area_regression,
    "capability": area_capability,
    "mcp": area_mcp,
    "ui": area_ui,
    "agent": area_agent,
    "models": area_models,
    "stats": area_stats,
    "queue": area_queue,
    "intent": area_intent,
}


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description="MORA M12 independent verification (task-5)")
    parser.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--areas", default="all", help="comma list or 'all': " + ",".join(AREAS))
    parser.add_argument("--list", action="store_true", help="list areas and exit")
    parser.add_argument("--build-timeout", type=float, default=900)
    parser.add_argument("--ui-timeout", type=float, default=420)
    parser.add_argument("--mcp-timeout", type=float, default=180)
    parser.add_argument("--packaging-timeout", type=float, default=900)
    parser.add_argument("--queue-query", default="夜的钢琴曲",
                        help="bounded real search used to build the queue in the queue area")
    parser.add_argument("--real-intent", action="store_true",
                        help="task-6: spend the user's real provider quota on four bounded prompts")
    return parser.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    if args.list:
        for name in AREAS:
            print(name)
        return 0
    ARTIFACTS.mkdir(parents=True, exist_ok=True)
    exe = Path(args.exe).resolve()
    audit = Audit(exe)
    audit.environment = {
        "generatedBy": "scripts/verify_round9.py",
        "moraPipes": list_mora_pipes(),
        "moraProcesses": mora_processes(),
        "startedAt": iso_now(),
    }
    if not exe.exists():
        audit.add("capability", "CAP-00", BLOCKED, "binary under test exists", str(exe), "missing",
                  impact="run ./build.ps1 first")
    else:
        audit.environment["binary"] = {
            "path": str(exe),
            "sha256": sha256_file(exe),
            "sizeBytes": exe.stat().st_size,
            "lastWriteTime": time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(exe.stat().st_mtime)),
        }
        selected = list(AREAS) if args.areas == "all" else [a.strip() for a in args.areas.split(",") if a.strip()]
        for name in selected:
            if name not in AREAS:
                raise SystemExit("unknown area: %s" % name)
        for name in selected:
            try:
                AREAS[name](audit, args)
            except Exception as exc:  # noqa: BLE001 - a crashing area must not lose the report
                audit.add(name, name.upper() + "-CRASH", FAIL, "verification area runs",
                          "python scripts/verify_round9.py --areas %s" % name,
                          "%s: %s" % (type(exc).__name__, exc))
    audit.environment["finishedAt"] = iso_now()
    summary = audit.summary()
    report = {
        "schema": "mora-round9-audit/1",
        "task": "task-5",
        "verifier": "verifier",
        "summary": summary,
        "environment": audit.environment,
        "checks": audit.checks,
        "defects": [c for c in audit.checks if c["status"] == FAIL],
        "blocked": [c["id"] for c in audit.checks if c["status"] == BLOCKED],
        "notVerified": [c["id"] for c in audit.checks if c["status"] == NOT_VERIFIED],
        "notes": audit.notes,
    }
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"summary": summary, "defects": [c["id"] for c in report["defects"]],
                      "blocked": report["blocked"], "notVerified": report["notVerified"],
                      "out": str(out)}, ensure_ascii=False))
    return 1 if summary["failed"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
