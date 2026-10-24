#!/usr/bin/env python3
"""MORA M1 acceptance harness: memory budget, console sizes and local audio progress.

What it does
------------
1. Copies the current candidate binary (build/mora.exe by default) to
   build/m1-harness/mora-harness.exe and records size / SHA256 / LastWriteTime.
   The private copy is the measurement object, because a concurrent build may
   replace build/mora.exe while the scenarios run.
2. Runs --demo --seconds 15 and --demo --seconds 60 inside a hidden (or reused)
   test console sized 80x24 and 116x36, in the style of scripts/ui_smoke.py.
3. Generates two low-volume (0.02) 440 Hz sine WAV fixtures with the Python
   stdlib wave module - 16-bit / 44100 Hz and 24-bit / 96000 Hz - and runs
   --local <wav> --seconds 10 --report ... for each. No ffmpeg is used.
4. Parses every --report JSON, aggregates one record per scenario and writes
   docs/verification/m1-acceptance.json with per-scenario numbers plus the
   top-level budgetPassed / failedScenarios verdict and the memory-scope note.
5. --soak-seconds N additionally runs --demo --seconds N (disabled by default).

Any scenario that does not meet its criteria makes the script exit non-zero.
Environment problems (no audio endpoint, missing Media Foundation, no console)
are recorded separately from product failures, but still fail the run unless
--allow-environment-failures is given.

This script writes only build/m1-harness/** and the --out JSON (default
docs/verification/m1-acceptance.json). It never invokes build.ps1.
"""

from __future__ import annotations

import argparse
import ctypes as c
from ctypes import wintypes as w
from datetime import datetime
import hashlib
import json
import math
import platform
import shutil
import struct
import subprocess
import sys
import time
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUDGET_MB = 50.0
DEFAULT_EXE = ROOT / "build" / "mora.exe"
DEFAULT_HARNESS_DIR = ROOT / "build" / "m1-harness"
DEFAULT_OUT = ROOT / "docs" / "verification" / "m1-acceptance.json"

TONE_16_NAME = "tone-16bit-44100.wav"
TONE_24_NAME = "tone-24bit-96000.wav"
TONE_SECONDS = 12.0
TONE_AMPLITUDE = 0.02
TONE_FREQUENCY = 440.0
TONE_CHANNELS = 2

# Message fragments the player emits when the failure is the host environment,
# not the product: no audio endpoint or no system media stack.
ENVIRONMENT_ERROR_MARKERS = (
    "音频输出设备不可用",
    "Media Foundation 不可用",
)


# --------------------------------------------------------------------- helpers
def now_iso() -> str:
    return datetime.now().astimezone().isoformat(timespec="seconds")


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def file_fingerprint(path: Path) -> dict:
    stat = path.stat()
    try:
        relative = str(path.relative_to(ROOT))
    except ValueError:
        relative = str(path)
    return {
        "path": str(path),
        "relativePath": relative,
        "sizeBytes": stat.st_size,
        "sha256": sha256_of(path),
        "lastWriteTime": datetime.fromtimestamp(stat.st_mtime).astimezone().isoformat(timespec="seconds"),
    }


def write_tone(path: Path, sample_rate: int, bits: int) -> None:
    """Write a deterministic low-amplitude sine WAV using only the stdlib."""
    frames = int(round(sample_rate * TONE_SECONDS))
    peak = (1 << (bits - 1)) - 1
    step = 2.0 * math.pi * TONE_FREQUENCY / sample_rate
    payload = bytearray()
    for index in range(frames):
        value = int(round(TONE_AMPLITUDE * peak * math.sin(step * index)))
        if bits == 16:
            frame = struct.pack("<h", value)
        elif bits == 24:
            frame = (value & 0xFFFFFF).to_bytes(3, "little")
        else:
            raise ValueError("unsupported bit depth: %d" % bits)
        payload += frame * TONE_CHANNELS
    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(TONE_CHANNELS)
        handle.setsampwidth(bits // 8)
        handle.setframerate(sample_rate)
        handle.writeframes(bytes(payload))


# ------------------------------------------------------------------- test console
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


class TestConsole:
    """Owns the console mora.exe attaches to and resizes it per scenario.

    Mirrors scripts/ui_smoke.py: AllocConsole when the process has no console,
    CreateFileW('CONIN$') / ('CONOUT$') for the child std handles, ShowWindow(0)
    to hide an allocated window. When the process is attached to a console whose
    stdout is *not* a console handle (piped/redirected output), the old console
    is released first so the harness gets a private, resizable one.
    """

    GENERIC_READ_WRITE = 0xC0000000
    OPEN_EXISTING = 3
    SHARE = 3
    STD_OUTPUT_HANDLE = -11

    def __init__(self) -> None:
        kernel32 = c.WinDLL("kernel32", use_last_error=True)
        user32 = c.WinDLL("user32", use_last_error=True)
        self.kernel32 = kernel32
        self.user32 = user32

        kernel32.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, c.c_void_p, w.DWORD, w.DWORD, w.HANDLE]
        kernel32.CreateFileW.restype = w.HANDLE
        kernel32.GetConsoleWindow.restype = w.HWND
        kernel32.GetStdHandle.argtypes = [w.DWORD]
        kernel32.GetStdHandle.restype = w.HANDLE
        kernel32.GetConsoleMode.argtypes = [w.HANDLE, c.POINTER(w.DWORD)]
        kernel32.GetConsoleMode.restype = w.BOOL
        kernel32.SetConsoleScreenBufferSize.argtypes = [w.HANDLE, _Coord]
        kernel32.SetConsoleScreenBufferSize.restype = w.BOOL
        kernel32.SetConsoleWindowInfo.argtypes = [w.HANDLE, w.BOOL, c.POINTER(_Rect)]
        kernel32.SetConsoleWindowInfo.restype = w.BOOL
        kernel32.GetConsoleScreenBufferInfo.argtypes = [w.HANDLE, c.POINTER(_ScreenBufferInfo)]
        kernel32.GetConsoleScreenBufferInfo.restype = w.BOOL
        kernel32.CloseHandle.argtypes = [w.HANDLE]
        user32.ShowWindow.argtypes = [w.HWND, c.c_int]
        user32.ShowWindow.restype = w.BOOL

        self.mode = "unavailable"
        self.allocated = False
        if kernel32.GetConsoleWindow() == 0:
            self.allocated = bool(kernel32.AllocConsole())
            self.mode = "allocated-hidden" if self.allocated else "unavailable"
        elif not self._stdout_is_console():
            # Piped output survives FreeConsole, so take a private console whose
            # window geometry we fully control.
            kernel32.FreeConsole()
            self.allocated = bool(kernel32.AllocConsole())
            self.mode = "allocated-hidden" if self.allocated else "unavailable"
        else:
            self.mode = "reused"

        self.window = kernel32.GetConsoleWindow()
        if self.allocated and self.window:
            user32.ShowWindow(self.window, 0)

        invalid = w.HANDLE(-1).value
        self.conin = kernel32.CreateFileW(
            "CONIN$", self.GENERIC_READ_WRITE, self.SHARE, None, self.OPEN_EXISTING, 0, None
        )
        self.conout = kernel32.CreateFileW(
            "CONOUT$", self.GENERIC_READ_WRITE, self.SHARE, None, self.OPEN_EXISTING, 0, None
        )
        if self.conin == invalid or self.conout == invalid:
            raise OSError(c.get_last_error(), "test console unavailable (CONIN$/CONOUT$)")
        self.original = self.geometry()

    def _stdout_is_console(self) -> bool:
        handle = self.kernel32.GetStdHandle(self.STD_OUTPUT_HANDLE)
        mode = w.DWORD()
        return bool(self.kernel32.GetConsoleMode(handle, c.byref(mode)))

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

    def _set_window(self, left: int, top: int, right: int, bottom: int) -> bool:
        rect = _Rect(left, top, right, bottom)
        return bool(self.kernel32.SetConsoleWindowInfo(self.conout, True, c.byref(rect)))

    def _set_buffer(self, width: int, height: int) -> bool:
        return bool(self.kernel32.SetConsoleScreenBufferSize(self.conout, _Coord(width, height)))

    def set_size(self, columns: int, rows: int) -> dict:
        before = self.geometry() or {}
        current_width = max(int(before.get("bufferWidth", columns)), 1)
        current_height = max(int(before.get("bufferHeight", rows)), 1)
        steps = []
        # 1. shrink the visible window inside the current buffer first, otherwise
        #    a narrower buffer is rejected with ERROR_INVALID_PARAMETER.
        steps.append(self._set_window(0, 0, min(columns, current_width) - 1, min(rows, current_height) - 1))
        # 2. set the buffer to the target width (generous height keeps scrollback).
        steps.append(self._set_buffer(columns, max(rows, 100)))
        # 3. pin the exact target window.
        steps.append(self._set_window(0, 0, columns - 1, rows - 1))
        observed = self.geometry()
        return {
            "requested": {"columns": columns, "rows": rows},
            "observed": observed,
            "failedSteps": [index for index, ok in enumerate(steps) if not ok],
        }

    def restore(self) -> None:
        if not self.original:
            return
        try:
            original = self.original
            self._set_window(
                original["windowLeft"],
                original["windowTop"],
                original["windowLeft"] + original["windowWidth"] - 1,
                original["windowTop"] + original["windowHeight"] - 1,
            )
            self._set_buffer(original["bufferWidth"], original["bufferHeight"])
        except Exception:
            pass

    def close(self) -> None:
        self.restore()
        try:
            if self.conin:
                self.kernel32.CloseHandle(self.conin)
            if self.conout:
                self.kernel32.CloseHandle(self.conout)
        finally:
            self.conin = self.conout = None
            if self.allocated:
                self.kernel32.FreeConsole()


# ------------------------------------------------------------------- scenarios
def plan_scenarios(args, harness_dir: Path) -> list:
    wav16 = harness_dir / TONE_16_NAME
    wav24 = harness_dir / TONE_24_NAME
    scenarios = [
        {
            "name": "demo-15s-80x24",
            "description": "--demo offline UI, 15 s, narrow 80x24 console",
            "args": ["--demo", "--seconds", "15"],
            "seconds": 15,
            "columns": 80,
            "rows": 24,
            "timeoutSeconds": 90,
        },
        {
            "name": "demo-60s-116x36",
            "description": "--demo offline UI, 60 s, 116x36 console",
            "args": ["--demo", "--seconds", "60"],
            "seconds": 60,
            "columns": 116,
            "rows": 36,
            "timeoutSeconds": 150,
        },
        {
            "name": "local-16bit-44100-10s",
            "description": "--local 16-bit/44100 Hz sine WAV, 10 s",
            "args": ["--local", str(wav16), "--seconds", "10"],
            "seconds": 10,
            "columns": 116,
            "rows": 36,
            "timeoutSeconds": 90,
        },
        {
            "name": "local-24bit-96000-10s",
            "description": "--local 24-bit/96000 Hz sine WAV, 10 s",
            "args": ["--local", str(wav24), "--seconds", "10"],
            "seconds": 10,
            "columns": 116,
            "rows": 36,
            "timeoutSeconds": 90,
        },
    ]
    if args.soak_seconds and args.soak_seconds > 0:
        scenarios.append(
            {
                "name": "soak-demo-%ds-116x36" % args.soak_seconds,
                "description": "--demo soak, %d s, 116x36 console" % args.soak_seconds,
                "args": ["--demo", "--seconds", str(args.soak_seconds)],
                "seconds": args.soak_seconds,
                "columns": 116,
                "rows": 36,
                "timeoutSeconds": args.soak_seconds + 120,
            }
        )
    return scenarios


def _number(report, key):
    value = report.get(key) if isinstance(report, dict) else None
    return float(value) if isinstance(value, (int, float)) else None


def run_scenario(console, measured_exe: Path, measured_fp: dict, scenario: dict, reports_dir: Path) -> dict:
    report_path = reports_dir / (scenario["name"] + ".json")
    log_path = reports_dir / (scenario["name"] + ".stderr.txt")
    reports_dir.mkdir(parents=True, exist_ok=True)
    report_path.unlink(missing_ok=True)
    log_path.unlink(missing_ok=True)

    cmd = [str(measured_exe)] + list(scenario["args"]) + ["--report", str(report_path)]
    if console is not None:
        console_state = console.set_size(scenario["columns"], scenario["rows"])
    else:
        console_state = {
            "requested": {"columns": scenario["columns"], "rows": scenario["rows"]},
            "observed": None,
            "failedSteps": [],
        }

    startup = subprocess.STARTUPINFO()
    startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0

    started = time.monotonic()
    timed_out = False
    with open(log_path, "wb") as stderr_file:
        if console is not None:
            stdin_file = open("CONIN$", "rb", buffering=0)
            stdout_file = open("CONOUT$", "wb", buffering=0)
        else:
            stdin_file = open("NUL", "rb")
            stdout_file = open("NUL", "wb")
        try:
            child = subprocess.Popen(
                cmd,
                cwd=str(ROOT),
                stdin=stdin_file,
                stdout=stdout_file,
                stderr=stderr_file,
                startupinfo=startup,
            )
            try:
                returncode = child.wait(timeout=scenario["timeoutSeconds"])
            except subprocess.TimeoutExpired:
                timed_out = True
                child.terminate()
                try:
                    returncode = child.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    child.kill()
                    returncode = child.wait()
        finally:
            stdin_file.close()
            stdout_file.close()
    elapsed = time.monotonic() - started

    stderr_text = ""
    try:
        stderr_text = log_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        pass
    if len(stderr_text) > 4000:
        stderr_text = stderr_text[-4000:]

    report = None
    report_error = ""
    if report_path.exists():
        try:
            report = json.loads(report_path.read_text(encoding="utf-8"))
        except Exception as exc:  # noqa: BLE001 - recorded, not raised
            report_error = "report parse error: %s" % exc
    else:
        report_error = "report file missing"

    audio_required = "--local" in scenario["args"]
    peak_ws = _number(report, "peakWorkingSetMB")
    peak_private = _number(report, "peakPrivateMB")
    report_budget = _number(report, "budgetMB") or BUDGET_MB
    audio_played = bool(report.get("audioPlayed")) if isinstance(report, dict) else False
    audio = bool(report.get("audio")) if isinstance(report, dict) else None
    max_position = _number(report, "maxPositionSeconds")
    audio_error = (report.get("audioError") or "") if isinstance(report, dict) else ""
    report_passed = bool(report.get("passed")) if isinstance(report, dict) else False

    observed = console_state.get("observed") or {}
    if console is None:
        console_ok = None
    else:
        console_ok = (
            observed.get("windowWidth") == scenario["columns"] and observed.get("windowHeight") == scenario["rows"]
        )

    memory_ok = (
        peak_ws is not None and peak_private is not None and peak_ws <= BUDGET_MB and peak_private <= BUDGET_MB
    )
    audio_ok = (not audio_required) or (audio_played and (max_position or 0) > 0 and not audio_error)

    reasons = []
    failure_kind = "none"

    def escalate(kind: str, reason: str) -> None:
        nonlocal failure_kind
        if failure_kind == "none" or (failure_kind == "harness" and kind != "harness"):
            failure_kind = kind
        reasons.append(reason)

    if console is None:
        escalate("environment", "隐藏测试控制台不可用：场景在无控制台回退模式下运行，控制台尺寸无法验证")
    if timed_out:
        escalate("product", "进程在 %ss 超时限制内未退出" % scenario["timeoutSeconds"])
    if returncode != 0:
        if any(marker in stderr_text for marker in ENVIRONMENT_ERROR_MARKERS):
            escalate("environment", "环境：mora 报告系统音频/媒体组件不可用（退出码 %d）" % returncode)
        else:
            escalate("product", "mora 退出码 %d" % returncode)
    if report is None:
        escalate("harness", report_error)
    if console is not None and not console_ok:
        escalate(
            "harness",
            "控制台尺寸未设置到请求值（观察值 %sx%s）"
            % (observed.get("windowWidth"), observed.get("windowHeight")),
        )
    if peak_ws is None or peak_private is None:
        escalate("harness", "报告缺少内存峰值（peakWorkingSetMB/peakPrivateMB）")
    elif not memory_ok:
        escalate(
            "product",
            "内存超过 %.0f MB 预算：peakWorkingSetMB=%s peakPrivateMB=%s" % (BUDGET_MB, peak_ws, peak_private),
        )
    if audio_required and not audio_ok:
        if any(marker in audio_error for marker in ENVIRONMENT_ERROR_MARKERS):
            escalate(
                "environment",
                "环境：本地音频未播放 audioPlayed=%s maxPositionSeconds=%s audioError=%r"
                % (audio_played, max_position, audio_error),
            )
        else:
            escalate(
                "product",
                "本地音频未播放 audioPlayed=%s maxPositionSeconds=%s audioError=%r"
                % (audio_played, max_position, audio_error),
            )

    passed = failure_kind == "none" and not reasons

    return {
        "name": scenario["name"],
        "description": scenario["description"],
        "args": list(scenario["args"]),
        "commandLine": " ".join('"%s"' % part if " " in part else part for part in cmd),
        "seconds": scenario["seconds"],
        "binary": dict(measured_fp),
        "console": {
            "mode": console.mode if console is not None else "unavailable",
            "requested": {"columns": scenario["columns"], "rows": scenario["rows"]},
            "observed": console_state.get("observed"),
            "matched": console_ok,
            "failedSteps": console_state.get("failedSteps", []),
        },
        "returncode": returncode,
        "timedOut": timed_out,
        "elapsedSeconds": round(elapsed, 3),
        "reportPath": str(report_path),
        "reportWritten": report is not None,
        "reportError": report_error,
        "peakWorkingSetMB": peak_ws,
        "peakPrivateMB": peak_private,
        "budgetMB": report_budget,
        "reportPassed": report_passed,
        "passed": passed,
        "audioRequired": audio_required,
        "audio": audio,
        "audioPlayed": audio_played,
        "maxPositionSeconds": max_position,
        "audioError": audio_error,
        "mode": report.get("mode") if isinstance(report, dict) else None,
        "sampleRate": report.get("sampleRate") if isinstance(report, dict) else None,
        "sampleBits": report.get("sampleBits") if isinstance(report, dict) else None,
        "actualQuality": report.get("actualQuality") if isinstance(report, dict) else None,
        "failureKind": failure_kind,
        "failureReasons": reasons,
        "stderrTail": stderr_text,
    }


# ------------------------------------------------------------------------ main
def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="m1_acceptance.py",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description=(
            "Reproducible MORA M1 acceptance run: copies the candidate binary to\n"
            "build/m1-harness, measures peak working set / private commit for the\n"
            "demo UI at 80x24 and 116x36, generates 16-bit/44100 and 24-bit/96000\n"
            "sine WAVs with the stdlib (no ffmpeg) and checks real local playback\n"
            "progress, then aggregates docs/verification/m1-acceptance.json.\n"
            "\n"
            "Exit code is 1 when any scenario fails, 0 when all scenarios pass."
        ),
        epilog=(
            "examples:\n"
            "  python scripts/m1_acceptance.py\n"
            "  python scripts/m1_acceptance.py --soak-seconds 600\n"
            "  python scripts/m1_acceptance.py --exe build/mora.exe --out docs/verification/m1-acceptance.json\n"
            "  python scripts/m1_acceptance.py --plan\n"
            "  python scripts/m1_acceptance.py --allow-environment-failures\n"
            "\n"
            "memory scope: DEVELOPMENT_PLAN.md section 7 counts every required MORA\n"
            "process at one instant; this harness currently measures the single\n"
            "mora.exe process, and the JSON records that limitation.\n"
            "\n"
            "outputs: build/m1-harness/** (private binary, WAV fixtures, raw reports)\n"
            "and the --out JSON (default docs/verification/m1-acceptance.json)."
        ),
    )
    parser.add_argument("--exe", default=str(DEFAULT_EXE), help="candidate binary to copy and measure (default: build/mora.exe)")
    parser.add_argument("--harness-dir", default=str(DEFAULT_HARNESS_DIR), help="private harness directory (default: build/m1-harness)")
    parser.add_argument("--out", default=str(DEFAULT_OUT), help="aggregated JSON output (default: docs/verification/m1-acceptance.json)")
    parser.add_argument("--soak-seconds", type=int, default=0, help="also run --demo --seconds N; disabled by default")
    parser.add_argument("--allow-environment-failures", action="store_true", help="exit 0 when only environment failures occurred")
    parser.add_argument("--plan", action="store_true", help="print the planned scenarios and exit without running")
    return parser


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)

    source_exe = Path(args.exe).resolve()
    harness_dir = Path(args.harness_dir).resolve()
    out_path = Path(args.out).resolve()
    reports_dir = harness_dir / "reports"

    scenarios = plan_scenarios(args, harness_dir)

    if args.plan:
        for scenario in scenarios:
            print("%-28s %-5ss %sx%s  %s" % (
                scenario["name"], scenario["seconds"], scenario["columns"], scenario["rows"], scenario["description"]))
        return 0

    if not source_exe.exists():
        print("error: candidate binary not found: %s" % source_exe, file=sys.stderr)
        return 2

    harness_dir.mkdir(parents=True, exist_ok=True)
    reports_dir.mkdir(parents=True, exist_ok=True)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    # 1. freeze the measurement object: build/mora.exe can be replaced by a
    #    concurrent build at any moment, so every scenario measures the copy.
    source_before = file_fingerprint(source_exe)
    measured_exe = harness_dir / "mora-harness.exe"
    shutil.copyfile(source_exe, measured_exe)
    measured_fp = file_fingerprint(measured_exe)
    source_after = file_fingerprint(source_exe)
    binary_info = {
        "source": source_before,
        "sourceAfterCopy": source_after,
        "measured": measured_fp,
        "copyMatchesSourceAtCopyTime": source_before["sha256"] == measured_fp["sha256"],
        "sourceStableDuringCopy": source_before["sha256"] == source_after["sha256"],
        "measurementTarget": measured_fp["relativePath"],
        "note": "build/mora.exe may be overwritten by a concurrent build; mora-harness.exe is the frozen measurement object",
    }

    # 3. deterministic low-volume sine fixtures, stdlib wave only.
    fixtures = []
    for name, sample_rate, bits in ((TONE_16_NAME, 44100, 16), (TONE_24_NAME, 96000, 24)):
        path = harness_dir / name
        write_tone(path, sample_rate, bits)
        fixture = file_fingerprint(path)
        fixture.update(
            {
                "sampleRate": sample_rate,
                "bits": bits,
                "channels": TONE_CHANNELS,
                "seconds": TONE_SECONDS,
                "amplitude": TONE_AMPLITUDE,
                "frequencyHz": TONE_FREQUENCY,
                "generator": "python stdlib wave + struct (no ffmpeg)",
            }
        )
        fixtures.append(fixture)

    console = None
    console_error = ""
    try:
        console = TestConsole()
    except OSError as exc:
        console_error = str(exc)

    records = []
    try:
        for scenario in scenarios:
            print("running %-28s ..." % scenario["name"], flush=True)
            record = run_scenario(console, measured_exe, measured_fp, scenario, reports_dir)
            records.append(record)
            peak = record["peakWorkingSetMB"]
            private = record["peakPrivateMB"]
            print(
                "  %-4s %-28s ws=%s priv=%s audio=%s maxPos=%s"
                % (
                    "OK" if record["passed"] else "FAIL",
                    record["name"],
                    "%.3f" % peak if peak is not None else "n/a",
                    "%.3f" % private if private is not None else "n/a",
                    record["audioPlayed"],
                    record["maxPositionSeconds"],
                ),
                flush=True,
            )
            for reason in record["failureReasons"]:
                print("    - %s" % reason, flush=True)
    finally:
        if console is not None:
            console.close()

    measured_records = [r for r in records if r["peakWorkingSetMB"] is not None and r["peakPrivateMB"] is not None]
    budget_passed = len(measured_records) == len(records) and all(
        r["peakWorkingSetMB"] <= BUDGET_MB and r["peakPrivateMB"] <= BUDGET_MB for r in records
    )

    failed = [r for r in records if not r["passed"]]
    environment_failures = [r["name"] for r in failed if r["failureKind"] == "environment"]
    product_failures = [r["name"] for r in failed if r["failureKind"] == "product"]
    harness_failures = [r["name"] for r in failed if r["failureKind"] == "harness"]

    payload = {
        "schema": "mora-m1-acceptance/1",
        "generatedAt": now_iso(),
        "harness": {
            "script": "scripts/m1_acceptance.py",
            "command": " ".join(sys.argv),
            "pythonVersion": sys.version.split()[0],
            "platform": platform.platform(),
            "host": platform.node(),
            "console": {
                "mode": console.mode if console is not None else "unavailable",
                "error": console_error,
                "hidden": bool(console is not None and console.allocated),
            },
            "budgetMB": BUDGET_MB,
        },
        "memoryScope": {
            "policy": "统计范围=MORA 全部必需进程（前端 + MCP 进程/代理 + MORA 必需后台进程）在同一时间点求和后的峰值；两者各自不超过 50 MB，不把两者相加",
            "measuredThisRun": "仅 mora.exe 单进程（每个场景独立启动一个进程）",
            "coveredProcesses": ["mora.exe (build/m1-harness/mora-harness.exe)"],
            "notCoveredProcesses": ["MORA MCP 进程/代理", "MORA 必需后台进程"],
            "limitation": "本次只覆盖 mora.exe 单进程；多进程合计口径未在本轮测量，需在 M4/M6 的多进程场景补齐",
            "emptyWorkingSetUsed": False,
        },
        "binary": binary_info,
        "fixtures": fixtures,
        "scenarios": records,
        "budgetPassed": budget_passed,
        "failedScenarios": [r["name"] for r in failed],
        "environmentFailures": environment_failures,
        "productFailures": product_failures,
        "harnessFailures": harness_failures,
        "verdict": "passed" if not failed else "failed",
    }

    out_path.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    print("")
    print("binary           : %s (sha256 %s)" % (binary_info["measured"]["relativePath"], binary_info["measured"]["sha256"][:16]))
    print("budgetPassed     : %s" % budget_passed)
    print("failedScenarios  : %s" % (", ".join(payload["failedScenarios"]) or "none"))
    if environment_failures:
        print("environmentFailures: %s" % ", ".join(environment_failures))
    if product_failures:
        print("productFailures  : %s" % ", ".join(product_failures))
    if harness_failures:
        print("harnessFailures  : %s" % ", ".join(harness_failures))
    print("report           : %s" % out_path)

    if args.allow_environment_failures:
        blocking = product_failures + harness_failures
        return 1 if blocking else 0
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
