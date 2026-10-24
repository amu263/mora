"""End-to-end /chat check with a local model stub and real console input."""
import json
import ctypes as c
import hashlib
import os
import re
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / 'build' / 'mora.exe'


class ProcessMemory(c.Structure):
    _fields_ = [('cb', c.c_ulong), ('faults', c.c_ulong),
                ('peakWorking', c.c_size_t), ('working', c.c_size_t),
                ('peakPagedPool', c.c_size_t), ('pagedPool', c.c_size_t),
                ('peakNonPagedPool', c.c_size_t), ('nonPagedPool', c.c_size_t),
                ('pagefile', c.c_size_t), ('peakPagefile', c.c_size_t),
                ('private', c.c_size_t)]


kernel32 = c.WinDLL('kernel32', use_last_error=True)
psapi = c.WinDLL('psapi', use_last_error=True)
kernel32.OpenProcess.argtypes = [c.c_ulong, c.c_int, c.c_ulong]
kernel32.OpenProcess.restype = c.c_void_p
kernel32.CloseHandle.argtypes = [c.c_void_p]
psapi.GetProcessMemoryInfo.argtypes = [c.c_void_p, c.POINTER(ProcessMemory), c.c_ulong]


def memory(pid):
    handle = kernel32.OpenProcess(0x1010, 0, pid)
    if not handle:
        return None
    try:
        info = ProcessMemory()
        info.cb = c.sizeof(info)
        if not psapi.GetProcessMemoryInfo(handle, c.byref(info), info.cb):
            return None
        return info.working / 1048576, info.private / 1048576
    finally:
        kernel32.CloseHandle(handle)


class Provider(BaseHTTPRequestHandler):
    calls = []
    result_seen = False

    def do_POST(self):
        size = int(self.headers.get('Content-Length', '0'))
        body = json.loads(self.rfile.read(size))
        Provider.calls.append(body)
        messages = body['messages']
        if len(Provider.calls) == 1:
            answer = {'choices': [{'message': {'role': 'assistant', 'content': None,
                      'tool_calls': [{'id': 'search-1', 'type': 'function',
                                      'function': {'name': 'music_search',
                                                   'arguments': json.dumps(
                                                       {'query': '夜的钢琴曲', 'limit': 1},
                                                       ensure_ascii=False)}},
                                     {'id': 'volume-1', 'type': 'function',
                                      'function': {'name': 'music_control',
                                                   'arguments': json.dumps(
                                                       {'action': 'volume', 'value': 37})}},
                                     {'id': 'play-1', 'type': 'function',
                                      'function': {'name': 'music_play',
                                                   'arguments': json.dumps(
                                                       {'kind': 'song', 'id': 539928216})}}]}}]}
        else:
            search = json.loads(messages[-3]['content'])
            volume = json.loads(messages[-2]['content'])
            play = json.loads(messages[-1]['content'])
            Provider.result_seen = (bool(search.get('items')) and
                                    volume.get('volumePercent') == 37 and
                                    play.get('status') == 'accepted' and
                                    play.get('id') == 539928216)
            answer = {'choices': [{'message': {'role': 'assistant',
                                                'content': '模型已确认音量 37' if Provider.result_seen
                                                else '工具结果未确认'}}]}
        payload = json.dumps(answer, ensure_ascii=False).encode('utf-8')
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, *_):
        pass


def main():
    with tempfile.TemporaryDirectory(prefix='mora-tui-chat-') as tmp:
        data = Path(tmp)
        snapshot = data / 'chat.ansi'
        playback_report = data / 'playback.json'
        stored = subprocess.run([str(EXE), '--data-dir', str(data), '--set-agent-key'],
                                input='local-test-key\n', text=True, capture_output=True,
                                timeout=15)
        assert stored.returncode == 0, stored.stderr
        # The first real launch now asks for the user's name; seed it so the modal does not
        # swallow the injected /chat keystrokes.
        (data / 'settings.json').write_text(
            json.dumps({'userName': '测试听众', 'personaId': 'amu'}, ensure_ascii=False),
            encoding='utf-8')
        server = ThreadingHTTPServer(('127.0.0.1', 0), Provider)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        env = os.environ.copy()
        env['MORA_AGENT_BASE_URL'] = f'http://127.0.0.1:{server.server_port}/v1/chat/completions'
        peak_working = peak_private = 0.0
        samples = 0
        try:
            capture = subprocess.Popen(
                [sys.executable, str(ROOT / 'scripts' / 'frame_capture.py'), str(snapshot),
                 '--columns', '116', '--rows', '36', '--seconds', '16',
                 '--command', '/chat 把音量调到37', '--pass-through', '--data-dir', str(data),
                 '--report', str(playback_report)],
                cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                creationflags=0x00000008)
            console_line = capture.stdout.readline()
            pid_line = capture.stdout.readline()
            assert pid_line.startswith('player pid '), (console_line, pid_line)
            player_pid = int(pid_line.split()[-1])
            adapter_log = data / 'mcp.jsonl'
            adapter_output = adapter_log.open('wb')
            adapter = subprocess.Popen([str(EXE), '--mcp'], cwd=ROOT, stdin=subprocess.PIPE,
                                       stdout=adapter_output, stderr=subprocess.DEVNULL)
            try:
                adapter.stdin.write((json.dumps({'jsonrpc': '2.0', 'id': 1, 'method': 'initialize',
                                                   'params': {'protocolVersion': '2025-11-25',
                                                              'capabilities': {},
                                                              'clientInfo': {'name': 'test',
                                                                             'version': '1'}}})
                                     + '\n').encode())
                adapter.stdin.write((json.dumps({'jsonrpc': '2.0',
                                                   'method': 'notifications/initialized'})
                                     + '\n').encode())
                # The player binds its per-user pipe during startup, so poll the status tool until
                # it reports an attached session instead of assuming the very first call wins.
                status_id = 2
                attached = False
                for _ in range(24):
                    adapter.stdin.write((json.dumps({'jsonrpc': '2.0', 'id': status_id,
                                                       'method': 'tools/call',
                                                       'params': {'name': 'music_get_status',
                                                                  'arguments': {}}}) + '\n').encode())
                    adapter.stdin.flush()
                    time.sleep(.4)
                    if not adapter_log.exists():
                        status_id += 1
                        continue
                    for line in adapter_log.read_text(encoding='utf-8').splitlines():
                        try:
                            reply = json.loads(line)
                        except json.JSONDecodeError:
                            continue
                        if reply.get('result', {}).get('structuredContent', {}).get(
                                'playerSessionAttached'):
                            attached = True
                    if attached:
                        break
                    status_id += 1
                adapter.stdin.flush()
                deadline = time.monotonic() + 25
                while capture.poll() is None and time.monotonic() < deadline:
                    a, b = memory(player_pid), memory(adapter.pid)
                    if a and b:
                        samples += 1
                        peak_working = max(peak_working, a[0] + b[0])
                        peak_private = max(peak_private, a[1] + b[1])
                    time.sleep(.05)
                tail, errors = capture.communicate(timeout=5)
                output = console_line + pid_line + tail
                returncode = capture.returncode
            finally:
                adapter.stdin.close()
                adapter.wait(timeout=5)
                adapter_output.close()
        finally:
            server.shutdown()
            server.server_close()
        assert returncode == 0, errors
        frame = snapshot.read_text(encoding='utf-8', errors='replace')
        assert len(Provider.calls) == 2, (f'provider calls={len(Provider.calls)} '
                                                  f'helper={output} frame={frame[-1200:]}')
        assert Provider.result_seen, 'control result did not come from the live player'
        assert '模型已确认音量 37' in frame, 'model reply absent from the terminal frame'
        assert 'local-test-key' not in frame
        played = json.loads(playback_report.read_text(encoding='utf-8'))
        plain_frame = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', frame)
        plain_frame = re.sub(r'\s+', ' ', plain_frame)
        assert played['audioPlayed'] and played['maxPositionSeconds'] > .1, (
            played, plain_frame[-700:])
        assert samples > 0 and peak_working < 50 and peak_private < 50, (
            peak_working, peak_private, samples)
        adapter_replies = [json.loads(line) for line in adapter_log.read_text(encoding='utf-8').splitlines()]
        assert any(reply.get('result', {}).get('structuredContent', {})
                   .get('playerSessionAttached') for reply in adapter_replies), adapter_replies
        empty_profile = data / 'no-key'
        empty_profile.mkdir()
        no_key_frame = data / 'no-key.ansi'
        no_key = subprocess.run(
            [sys.executable, str(ROOT / 'scripts' / 'frame_capture.py'), str(no_key_frame),
             '--columns', '116', '--rows', '36', '--seconds', '3',
             '--command', '/chat 测试未配置密钥', '--pass-through', '--data-dir', str(empty_profile)],
            cwd=ROOT, env=env, capture_output=True, text=True, timeout=15,
            creationflags=0x00000008)
        assert no_key.returncode == 0, no_key.stderr
        assert 'missing_key' in no_key_frame.read_text(encoding='utf-8', errors='replace')
        assert len(Provider.calls) == 2, 'missing-key case must not contact the provider'
        report = {'schema': 'mora-m3-tui-chat/1',
                  'sha256': hashlib.sha256(EXE.read_bytes()).hexdigest().upper(),
                  'provider': 'local stub', 'providerCalls': len(Provider.calls),
                  'tools': ['music_search', 'music_control', 'music_play'],
                  'modelReplyVisible': True, 'playerToolResultsVerified': True,
                  'missingKeyShownWithoutProviderCall': True,
                  'mcpAttachedDuringChat': True,
                  'audioPlayed': played['audioPlayed'],
                  'maxPositionSeconds': round(played['maxPositionSeconds'], 2),
                  'memory': {'scope': 'TUI + MCP, sampled simultaneously',
                             'samples': samples, 'peakWorkingSetMB': round(peak_working, 2),
                             'peakPrivateMB': round(peak_private, 2)}, 'passed': True}
        out = ROOT / 'docs' / 'verification' / 'm3-tui-chat.json'
        out.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        print(f'PASS TUI /chat: network search, main-thread volume/play, real reply; '
              f'TUI+MCP {peak_working:.2f}/{peak_private:.2f} MB')


if __name__ == '__main__':
    main()
