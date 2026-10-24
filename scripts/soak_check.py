"""Long-running soak: real streaming playback in the TUI with an MCP adapter attached.

The script starts the real player in a hidden console, drives it through the real input queue
(/repeat on, search, play), attaches mora --mcp over stdio and polls music_now_playing while
sampling both processes' memory. It writes one JSON report and exits non-zero on a real defect.

Usage: python scripts/soak_check.py --minutes 30 --out docs/verification/m6-soak-live.json
"""
import argparse
import ctypes as c
import hashlib
import json
import subprocess
import threading
import time
from ctypes import wintypes as w
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
kernel32 = c.WinDLL('kernel32', use_last_error=True)
user32 = c.WinDLL('user32', use_last_error=True)
psapi = c.WinDLL('psapi', use_last_error=True)


class Coord(c.Structure):
    _fields_ = [('X', w.SHORT), ('Y', w.SHORT)]


class Rect(c.Structure):
    _fields_ = [('Left', w.SHORT), ('Top', w.SHORT), ('Right', w.SHORT), ('Bottom', w.SHORT)]


class ScreenBufferInfo(c.Structure):
    _fields_ = [('Size', Coord), ('Cursor', Coord), ('Attributes', w.WORD), ('Window', Rect),
                ('MaximumWindow', Coord)]


class Char(c.Union):
    _fields_ = [('UnicodeChar', w.WCHAR), ('AsciiChar', c.c_char)]


class KeyEvent(c.Structure):
    _fields_ = [('down', w.BOOL), ('repeat', w.WORD), ('vk', w.WORD), ('scan', w.WORD),
                ('char', Char), ('control', w.DWORD)]


class Event(c.Union):
    _fields_ = [('key', KeyEvent), ('raw', c.c_byte * 16)]


class Input(c.Structure):
    _fields_ = [('type', w.WORD), ('event', Event)]


class ProcessMemory(c.Structure):
    _fields_ = [('cb', w.DWORD), ('PageFaultCount', w.DWORD), ('PeakWorkingSetSize', c.c_size_t),
                ('WorkingSetSize', c.c_size_t), ('QuotaPeakPagedPoolUsage', c.c_size_t),
                ('QuotaPagedPoolUsage', c.c_size_t), ('QuotaPeakNonPagedPoolUsage', c.c_size_t),
                ('QuotaNonPagedPoolUsage', c.c_size_t), ('PagefileUsage', c.c_size_t),
                ('PeakPagefileUsage', c.c_size_t)]


kernel32.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, c.c_void_p, w.DWORD, w.DWORD,
                                 w.HANDLE]
kernel32.CreateFileW.restype = w.HANDLE
kernel32.GetConsoleWindow.restype = w.HWND
kernel32.WriteConsoleInputW.argtypes = [w.HANDLE, c.POINTER(Input), w.DWORD, c.POINTER(w.DWORD)]
kernel32.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
kernel32.OpenProcess.restype = w.HANDLE
psapi.GetProcessMemoryInfo.argtypes = [w.HANDLE, c.POINTER(ProcessMemory), w.DWORD]
invalid = w.HANDLE(-1).value


class Console:
    """Private hidden console so the child gets a stable stdin/stdout without stealing ours."""

    def __enter__(self):
        if kernel32.GetConsoleWindow():
            kernel32.FreeConsole()
        self.allocated = bool(kernel32.AllocConsole())
        window = kernel32.GetConsoleWindow()
        if self.allocated and window:
            user32.ShowWindow(window, 0)
        self.conin = kernel32.CreateFileW('CONIN$', 0xC0000000, 3, None, 3, 0, None)
        self.conout = kernel32.CreateFileW('CONOUT$', 0xC0000000, 3, None, 3, 0, None)
        if self.conin == invalid or self.conout == invalid:
            raise OSError(c.get_last_error(), 'soak console unavailable')
        info = ScreenBufferInfo()
        if kernel32.GetConsoleScreenBufferInfo(self.conout, c.byref(info)):
            kernel32.SetConsoleScreenBufferSize(self.conout, Coord(120, 90))
            kernel32.SetConsoleWindowInfo(
                self.conout, True, c.byref(Rect(0, 0, 115, 35)))
        return self

    def __exit__(self, *exception):
        kernel32.CloseHandle(self.conin)
        kernel32.CloseHandle(self.conout)
        if self.allocated:
            kernel32.FreeConsole()

    def type(self, text):
        for character in text:
            self.key(0, character)

    def key(self, vk=0, character='\0'):
        events = (Input * 2)()
        for index, down in enumerate((True, False)):
            events[index].type = 1
            events[index].event.key = KeyEvent(down, 1, vk, 0, Char(UnicodeChar=character), 0)
        written = w.DWORD()
        if not kernel32.WriteConsoleInputW(self.conin, events, 2, c.byref(written)):
            raise OSError(c.get_last_error(), 'soak key injection failed')


def memory(pid):
    handle = kernel32.OpenProcess(0x0410, False, pid)
    if not handle:
        return None
    try:
        counters = ProcessMemory()
        counters.cb = c.sizeof(ProcessMemory)
        if not psapi.GetProcessMemoryInfo(handle, c.byref(counters), counters.cb):
            return None
        return counters.WorkingSetSize, counters.PagefileUsage
    finally:
        kernel32.CloseHandle(handle)


class Mcp:
    """Minimal JSON-RPC stdio client with a reader thread."""

    def __init__(self, exe, data_dir=None):
        command = [exe, '--mcp'] + (['--data-dir', data_dir] if data_dir else [])
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, text=True, encoding='utf-8',
                                        errors='replace', bufsize=1)
        self.lines = []
        self.raw = []
        self.lock = threading.Lock()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()
        self.serial = 0

    def _read(self):
        for line in self.process.stdout:
            with self.lock:
                self.raw.append(line)
                if line.strip():
                    self.lines.append(line.strip())

    def send(self, method, params=None):
        self.serial += 1
        message = {'jsonrpc': '2.0', 'id': self.serial, 'method': method}
        if params is not None:
            message['params'] = params
        self.process.stdin.write(json.dumps(message, ensure_ascii=False) + '\n')
        self.process.stdin.flush()
        return self.serial

    def notify(self, method):
        self.process.stdin.write(json.dumps({'jsonrpc': '2.0', 'method': method}) + '\n')
        self.process.stdin.flush()

    def call(self, method, params=None, timeout=30):
        identity = self.send(method, params)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with self.lock:
                for index, line in enumerate(self.lines):
                    try:
                        message = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    if message.get('id') == identity:
                        self.lines.pop(index)
                        return message
            time.sleep(0.05)
        raise TimeoutError(f'{method} did not answer in {timeout}s')

    def close(self):
        try:
            self.process.stdin.close()
        except OSError:
            pass
        try:
            self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            self.process.wait(timeout=5)
        self.reader.join(timeout=5)
        stderr = self.process.stderr.read()
        with self.lock:
            raw = list(self.raw)
        return stderr, raw


def main():
    parser = argparse.ArgumentParser(description='Soak real playback with an MCP adapter.')
    parser.add_argument('--minutes', type=float, default=30)
    parser.add_argument('--sample-seconds', type=float, default=10)
    parser.add_argument('--query', default='夜的钢琴曲 石进')
    parser.add_argument('--exe', default=str(ROOT / 'build' / 'mora.exe'))
    parser.add_argument('--out', default=str(ROOT / 'docs' / 'verification' / 'm6-soak-live.json'))
    parser.add_argument('--data-dir', default=str(ROOT / 'build' / 'soak-profile'))
    args = parser.parse_args()
    started = time.time()
    seconds = int(args.minutes * 60)
    report = {'schema': 'mora-m6-soak/1', 'startedAt': time.strftime('%Y-%m-%dT%H:%M:%S%z'),
              'plannedSeconds': seconds, 'sampleSeconds': args.sample_seconds,
              'query': args.query, 'memory': [], 'playback': [], 'failures': [],
              'environment': 'Windows hidden test console; real audio device; real NetEase search'}
    Path(args.data_dir).mkdir(parents=True, exist_ok=True)
    key = Path(args.exe)
    report['binary'] = {'path': str(key), 'sha256': hashlib.sha256(key.read_bytes()).hexdigest(),
                        'sizeBytes': key.stat().st_size}
    with Console() as console:
        startup = subprocess.STARTUPINFO()
        startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        tui_report = ROOT / 'build' / 'soak-tui-report.json'
        if tui_report.exists():
            tui_report.unlink()
        with open('CONIN$', 'rb', buffering=0) as stdin_file, \
                open('CONOUT$', 'wb', buffering=0) as stdout_file:
            tui = subprocess.Popen([args.exe, '--data-dir', args.data_dir, '--seconds',
                                    str(seconds), '--report', str(tui_report)],
                                   stdin=stdin_file, stdout=stdout_file,
                                   stderr=subprocess.DEVNULL, startupinfo=startup)
            mcp = None
            try:
                time.sleep(1.5)
                console.type('/repeat on')
                console.key(0x0D)
                time.sleep(0.6)
                console.type('/search ' + args.query)
                console.key(0x0D)
                time.sleep(4.0)
                console.key(0x0D)  # play the first real result
                time.sleep(2.0)
                mcp = Mcp(args.exe, args.data_dir)
                mcp.call('initialize', {'protocolVersion': '2025-11-25', 'capabilities': {},
                                        'clientInfo': {'name': 'mora-soak', 'version': '1'}})
                mcp.notify('notifications/initialized')
                deadline = started + seconds
                last_track = None
                last_position = None
                stalls = 0
                tracks_seen = []
                while time.time() < deadline and tui.poll() is None:
                    now = time.time()
                    tui_mem = memory(tui.pid)
                    mcp_mem = memory(mcp.process.pid)
                    if tui_mem and mcp_mem:
                        report['memory'].append({
                            'at': round(now - started, 1),
                            'tuiWorkingSetMB': round(tui_mem[0] / 1048576, 3),
                            'tuiPrivateMB': round(tui_mem[1] / 1048576, 3),
                            'mcpWorkingSetMB': round(mcp_mem[0] / 1048576, 3),
                            'mcpPrivateMB': round(mcp_mem[1] / 1048576, 3)})
                    try:
                        state = mcp.call('tools/call', {'name': 'music_now_playing',
                                                        'arguments': {}}, timeout=20)
                        view = state.get('result', {}).get('structuredContent', {})
                        track = view.get('track') or {}
                        position = view.get('positionSeconds', 0)
                        report['playback'].append({'at': round(now - started, 1),
                                                   'playing': view.get('playing'),
                                                   'paused': view.get('paused'),
                                                   'trackId': track.get('id'),
                                                   'name': track.get('name'),
                                                   'positionSeconds': round(position, 2),
                                                   'quality': view.get('quality'),
                                                   'lastError': view.get('lastError', '')})
                        if not view.get('playing'):
                            stalls += 1
                        else:
                            stalls = 0
                        same_track = track.get('id') == last_track
                        if track.get('id') and not same_track:
                            tracks_seen.append(track.get('id'))
                        # Position only has to advance inside one track; an EOF transition resets it.
                        if (same_track and last_position is not None and view.get('playing')
                                and position <= last_position + 0.5):
                            report['failures'].append({'kind': 'position-stalled',
                                                       'at': round(now - started, 1),
                                                       'previous': last_position,
                                                       'current': position})
                        last_track = track.get('id')
                        last_position = position
                        if stalls >= 3:
                            report['failures'].append({'kind': 'playback-stopped',
                                                       'at': round(now - started, 1)})
                            break
                    except TimeoutError as error:
                        report['failures'].append({'kind': 'mcp-timeout', 'detail': str(error),
                                                   'at': round(now - started, 1)})
                    time.sleep(args.sample_seconds)
                report['tracksSeen'] = tracks_seen
            finally:
                if mcp:
                    stderr, raw = mcp.close()
                    report['mcp'] = {
                        'stderrEmpty': not stderr.strip(),
                        'stdoutLines': len(raw),
                        'stdoutAllJsonRpc': all(
                            (json.loads(line).get('jsonrpc') == '2.0') for line in raw
                            if line.strip()),
                        'ansiFree': all('\x1b' not in line for line in raw)}
                if tui.poll() is None:
                    tui.terminate()
                tui.wait(timeout=20)
                report['tuiExitCode'] = tui.returncode
    if Path(tui_report).exists():
        report['tuiReport'] = json.loads(Path(tui_report).read_text(encoding='utf-8'))
    working = [entry['tuiWorkingSetMB'] + entry['mcpWorkingSetMB'] for entry in report['memory']]
    private = [entry['tuiPrivateMB'] + entry['mcpPrivateMB'] for entry in report['memory']]
    report['summary'] = {
        'samples': len(report['memory']),
        'elapsedSeconds': round(time.time() - started, 1),
        'peakSumWorkingSetMB': round(max(working), 3) if working else None,
        'peakSumPrivateMB': round(max(private), 3) if private else None,
        'budgetMB': 50,
        'tracksPlayed': len(report['tracksSeen']),
        'playbackSamples': len(report['playback']),
        'failures': report['failures'][:20],
    }
    budget_ok = (report['summary']['peakSumWorkingSetMB'] or 0) < 50 and \
                (report['summary']['peakSumPrivateMB'] or 0) < 50
    flags = [entry['playing'] for entry in report['playback']]
    start = next((index for index, value in enumerate(flags) if value), None)
    report['summary']['startedAtSample'] = start
    report['summary']['playingAfterStart'] = start is not None and all(flags[start:])
    report['summary']['budgetPassed'] = budget_ok
    report['verdict'] = ('passed' if budget_ok and not report['failures'] and
                         report['summary']['playingAfterStart'] else 'failed')
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'verdict': report['verdict'], **report['summary']}, ensure_ascii=True))
    return 0 if report['verdict'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
