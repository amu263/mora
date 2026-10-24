"""M3 acceptance against a local stub or the configured real provider.

The stub checks WinHTTP headers, tool dispatch into MusicService, and tool-result feedback.
The real mode uses the current user's DPAPI key without printing it.

Usage: python scripts/m3_agent_check.py [--real] [--exe PATH]
The --real mode reuses the current user's DPAPI key, calls the configured HTTPS provider and
writes a separate report with only endpoint/model, result code and tool count.
"""
import json
import argparse
import hashlib
import os
import re
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / 'build' / 'mora.exe'


class Stub(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def do_POST(self):
        length = int(self.headers.get('Content-Length', 0))
        payload = json.loads(self.rfile.read(length) or b'{}')
        self.server.records.append({
            'path': self.path,
            'userAgent': self.headers.get('User-Agent'),
            'sessionHeader': self.headers.get('x-opencode-session'),
            'authorized': bool(self.headers.get('Authorization')),
            'contentType': self.headers.get('Content-Type'),
            'model': payload.get('model'),
            'bodyKeys': sorted(payload.keys()),
            'bodyBytes': length,
            'toolNames': [tool.get('function', {}).get('name')
                          for tool in payload.get('tools', []) if isinstance(tool, dict)],
            'roles': [message.get('role') for message in payload.get('messages', [])],
            'toolResult': next((message.get('content', '') for message in payload.get('messages', [])
                                if message.get('role') == 'tool'), '')})
        if len(self.server.records) == 1:
            body = {'choices': [{'message': {
                'role': 'assistant', 'content': None,
                'tool_calls': [{'id': 'call-1', 'type': 'function', 'function': {
                    'name': 'music_search',
                    'arguments': json.dumps({'query': '夜的钢琴曲', 'limit': 3},
                                            ensure_ascii=False)}}]}}]}
        else:
            tool_result = (self.server.records[-1].get('toolResult') or '')
            summary = '没有拿到搜索结果'
            try:
                parsed = json.loads(tool_result)
                results = parsed.get('items') or parsed.get('results') or []
                if results:
                    first = results[0]
                    summary = (f"第一条是 {first.get('name')} — {first.get('artist')}"
                               f"（id {first.get('id')}）")
            except json.JSONDecodeError:
                pass
            body = {'choices': [{'message': {'role': 'assistant',
                                             'content': '工具结果：' + summary}}]}
        data = json.dumps(body, ensure_ascii=False).encode('utf-8')
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)
        self.close_connection = True

    def log_message(self, *args):
        pass


def run(command, env, stdin=None):
    return subprocess.run(command, env=env, input=stdin, capture_output=True, text=True,
                          encoding='utf-8', errors='replace')


def run_real():
    env = dict(os.environ)
    env.pop('MORA_AGENT_BASE_URL', None)
    env.pop('MORA_AGENT_MODEL', None)
    checked = run([str(EXE), '--agent-check'], env)
    config = json.loads(checked.stdout)
    if not config.get('keyPresent') or not config.get('baseUrl', '').startswith('https://'):
        print('Real provider preflight failed: configured HTTPS endpoint and DPAPI key required')
        return 1
    chat = run([str(EXE), '--chat',
                '帮我找一首《夜的钢琴曲》，告诉我第一条结果的歌手'], env)
    try:
        result = json.loads(chat.stdout)
    except json.JSONDecodeError:
        print('Real provider response was not valid MORA JSON')
        return 1
    tool_names = [item.get('tool') for item in result.get('trace', [])]
    passed = (chat.returncode == 0 and result.get('ok') is True and
              'music_search' in tool_names and bool(result.get('answer')))
    code = result.get('code', '')
    verdict = 'passed' if passed else 'blocked' if code in {'payment_required', 'unauthorized'} \
        else 'failed'
    report = {'schema': 'mora-m3-agent-real/1',
              'sha256': hashlib.sha256(EXE.read_bytes()).hexdigest().upper(),
              'providerBaseUrl': config['baseUrl'], 'model': config['model'],
              'ok': result.get('ok', False), 'toolCalls': result.get('toolCalls', 0),
              'toolNames': tool_names,
              'errorCode': code, 'verdict': verdict}
    out = ROOT / 'docs' / 'verification' / 'm3-agent-real.json'
    out.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, ensure_ascii=False))
    return 0 if passed else 2 if verdict == 'blocked' else 1


def main():
    global EXE
    parser = argparse.ArgumentParser(description='MORA M3 agent acceptance')
    parser.add_argument('--real', action='store_true',
                        help='use the configured real HTTPS provider and existing DPAPI key')
    parser.add_argument('--exe', default=str(EXE), help='binary to test')
    args = parser.parse_args()
    EXE = Path(args.exe).resolve()
    if args.real:
        return run_real()
    report = {'schema': 'mora-m3-agent/1', 'steps': [], 'failures': [],
              'note': 'local stub validates the tool loop independently of provider quota'}

    def step(name, ok, detail):
        report['steps'].append({'name': name, 'passed': bool(ok), 'detail': detail})
        if not ok:
            report['failures'].append(name)
        print(('PASS ' if ok else 'FAIL ') + name)

    profile = ROOT / 'build' / 'm3-profile'
    if profile.exists():
        for item in sorted(profile.rglob('*'), reverse=True):
            item.unlink() if item.is_file() else item.rmdir()
    profile.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env['MORA_DATA_DIR'] = str(profile)
    stored = run([str(EXE), '--set-agent-key'], env, stdin='test-key-not-a-secret\n')
    report['keyStored'] = json.loads(stored.stdout) if stored.stdout.strip() else {}
    step('key-vault', stored.returncode == 0 and report['keyStored'].get('keyPresent', True) is not False
         and report['keyStored'].get('keyLength') == 21, json.dumps(report['keyStored']))

    server = ThreadingHTTPServer(('127.0.0.1', 0), Stub)
    server.records = []
    port = server.server_address[1]
    threading.Thread(target=server.serve_forever, daemon=True).start()
    env['MORA_AGENT_BASE_URL'] = f'http://127.0.0.1:{port}/v1/chat/completions'
    chat = run([str(EXE), '--chat', '帮我找一首《夜的钢琴曲》，告诉我第一条结果的歌手'],
               env)
    server.shutdown()
    report['chatExitCode'] = chat.returncode
    try:
        payload = json.loads(chat.stdout)
    except json.JSONDecodeError:
        step('chat-json', False, (chat.stdout or chat.stderr)[:400])
        payload = {}
    if payload:
        report['chat'] = payload
        step('chat-json', True, f"ok={payload.get('ok')} toolCalls={payload.get('toolCalls')}")
        step('model-loop-completed', payload.get('ok') is True and payload.get('toolCalls') == 1,
             json.dumps(payload.get('trace'), ensure_ascii=False)[:300])
        answer = payload.get('answer') or ''
        step('answer-uses-real-tool-result', bool(re.search(r'\d{4,}', answer)), answer)
    records = server.records
    for record in records:
        if 'toolResult' in record:
            record['toolResultBytes'] = len(record.pop('toolResult'))
    report['providerRequests'] = records
    step('required-headers', len(records) == 2 and
         all(r.get('authorized') for r in records) and
         all((r.get('userAgent') or '').startswith('mora-player/') for r in records) and
         all((r.get('sessionHeader') or '').startswith('mora-') for r in records),
         json.dumps(records[0], ensure_ascii=False) if records else 'no requests')
    step('tools-advertised',
         bool(records) and len(records[0].get('toolNames', [])) >= 13 and
         records[0]['toolNames'][-1] == 'mora_create_persona' and
         'music_queue_list' in records[0]['toolNames'] and
         'music_queue_edit' in records[0]['toolNames'],
         f"{len(records[0].get('toolNames', [])) if records else 0} tools")
    step('tool-result-fed-back', len(records) == 2 and 'tool' in records[1].get('roles', []),
         f"second request roles={records[1].get('roles') if len(records) > 1 else []}")
    step('key-never-echoed', 'test-key' not in json.dumps(report.get('chat', {}), ensure_ascii=False),
         'the chat report contains no key material')
    report['verdict'] = 'passed' if not report['failures'] else 'failed'
    out = ROOT / 'docs' / 'verification' / 'm3-agent.json'
    out.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'verdict': report['verdict'], 'failures': report['failures']},
                     ensure_ascii=True))
    return 0 if report['verdict'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
