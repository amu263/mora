"""Verify configured reasoning effort reaches the actual HTTP request body."""
import json
import os
import subprocess
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / 'build' / 'mora.exe'


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers['Content-Length']))
        self.server.requests.append(json.loads(body))
        payload = json.dumps({'choices': [{'message': {'content': '好的'}}]}).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, *args):
        pass


def run(*args, env, input=None):
    return subprocess.run([str(EXE), *args], env=env, input=input,
                          text=True, encoding='utf-8', errors='replace',
                          capture_output=True)


def main():
    if not EXE.is_file():
        raise SystemExit('Build build/mora.exe first')
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    server.requests = []
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        with tempfile.TemporaryDirectory(prefix='mora-reasoning-') as temp:
            profile = Path(temp)
            env = dict(os.environ)
            env['MORA_DATA_DIR'] = str(profile)
            env['MORA_AGENT_BASE_URL'] = f'http://127.0.0.1:{server.server_port}/v1/chat/completions'
            env.pop('MORA_AGENT_REASONING_EFFORT', None)
            key = run('--set-agent-key', env=env, input='test-key-not-a-secret\n')
            assert key.returncode == 0, key.stderr
            # DST's ladder. `off` and an empty value both mean "send no reasoning field".
            for level in ('', 'off', 'minimal', 'low', 'medium', 'high', 'xhigh', 'max'):
                (profile / 'agent.json').write_text(
                    json.dumps({'reasoningEffort': level}), encoding='utf-8')
                chat = run('--chat', '打个招呼', env=env)
                assert chat.returncode == 0, (level, chat.stdout, chat.stderr)
                request = server.requests[-1]
                expected = None if level in ('', 'off') else level
                assert request.get('reasoning_effort') == expected, (level, request)
                print('PASS', level or 'provider-default')
            env['MORA_AGENT_REASONING_EFFORT'] = 'high'
            (profile / 'agent.json').write_text(
                json.dumps({'reasoningEffort': 'low'}), encoding='utf-8')
            chat = run('--chat', '打个招呼', env=env)
            assert chat.returncode == 0 and server.requests[-1]['reasoning_effort'] == 'high'
            print('PASS environment-override')
            env['MORA_AGENT_REASONING_EFFORT'] = 'invalid'
            count = len(server.requests)
            bad = run('--chat', '打个招呼', env=env)
            assert bad.returncode != 0 and len(server.requests) == count
            print('PASS invalid-value-rejected-before-request')
    finally:
        server.shutdown()


if __name__ == '__main__':
    main()
