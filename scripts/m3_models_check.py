"""M3 model discovery acceptance against a local stub provider (never the internet).

Covers `mora --models` and the cached capability report that `mora --agent-check` shows:
  * the /models endpoint is derived from the configured chat baseUrl on the same host/port
  * it is a GET with the same Authorization header, one bounded request, no third party
  * OpenAI-compatible data[] parsing: id, modalities, context_length/context_window,
    max_output_tokens, reasoning support
  * machine-readable failure codes: missing_key / unauthorized / not_supported / bad_response /
    network
  * the catalog is cached in agent.json without the key
  * the key is never printed anywhere
  * the user cannot pick a modality: no request body carries a modality field and --help
    advertises no such switch

Usage: python scripts/m3_models_check.py [--exe PATH]
"""
import argparse
import json
import os
import subprocess
import sys
import threading
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / 'build' / 'mora.exe'
KEY = 'stub-key-not-a-secret'

CATALOG = {
    'object': 'list',
    'data': [
        {
            'id': 'deepseek-v4.1-flash',
            'object': 'model',
            'context_length': 65536,
            'max_output_tokens': 8192,
            'architecture': {'input_modalities': ['text'], 'output_modalities': ['text'],
                             'modality': 'text->text'},
        },
        {
            'id': 'vision-omni-preview',
            'context_window': 131072,
            'top_provider': {'context_length': 131072, 'max_completion_tokens': 4096},
            'input_modalities': ['text', 'image'],
            'output_modalities': ['text'],
            'supported_parameters': ['tools', 'reasoning'],
        },
    ],
}


def has_modality_key(value):
    """True when any nested object key mentions a modality (the user must not select one)."""
    if isinstance(value, dict):
        for key, item in value.items():
            if 'modalit' in str(key).lower():
                return True
            if has_modality_key(item):
                return True
    elif isinstance(value, list):
        return any(has_modality_key(item) for item in value)
    return False


class Stub(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def _reply(self, status, payload):
        data = payload if isinstance(payload, bytes) else \
            json.dumps(payload, ensure_ascii=False).encode('utf-8')
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)
        self.close_connection = True

    def do_GET(self):
        self.server.records.append({
            'method': 'GET',
            'path': self.path,
            'authorized': bool(self.headers.get('Authorization')),
            'userAgent': self.headers.get('User-Agent') or '',
            'contentLength': int(self.headers.get('Content-Length', 0) or 0),
            'body': None,
        })
        mode = self.server.mode
        if mode == 'unauthorized':
            return self._reply(401, {'error': 'bad key'})
        if mode == 'not_supported':
            return self._reply(404, {'error': 'no such route'})
        if mode == 'server_error':
            return self._reply(500, {'error': 'boom'})
        if mode == 'bad':
            return self._reply(200, b'<html>this is not json</html>')
        if mode == 'empty':
            return self._reply(200, {'object': 'list', 'data': []})
        return self._reply(200, CATALOG)

    def do_POST(self):
        length = int(self.headers.get('Content-Length', 0) or 0)
        raw = self.rfile.read(length) if length else b''
        try:
            body = json.loads(raw or b'{}')
        except json.JSONDecodeError:
            body = None
        self.server.records.append({
            'method': 'POST',
            'path': self.path,
            'authorized': bool(self.headers.get('Authorization')),
            'userAgent': self.headers.get('User-Agent') or '',
            'contentLength': length,
            'body': body,
        })
        return self._reply(200, {'choices': [{'message': {'role': 'assistant',
                                                          'content': '你好，我是 MORA。'}}]})

    def log_message(self, *args):
        pass


def run(command, env, stdin=None):
    return subprocess.run(command, env=env, input=stdin, capture_output=True, text=True,
                          encoding='utf-8', errors='replace')


def fresh_profile(name):
    profile = ROOT / 'build' / name
    if profile.exists():
        for item in sorted(profile.rglob('*'), reverse=True):
            item.unlink() if item.is_file() else item.rmdir()
    profile.mkdir(parents=True, exist_ok=True)
    return profile


def main():
    global EXE
    parser = argparse.ArgumentParser(description='MORA M3 model discovery acceptance')
    parser.add_argument('--exe', default=str(EXE), help='binary to test')
    args = parser.parse_args()
    EXE = Path(args.exe).resolve()

    report = {'schema': 'mora-m3-models/1', 'steps': [], 'failures': [],
              'note': 'local stub provider; no third-party endpoint is contacted'}
    transcript = []

    def step(name, ok, detail):
        report['steps'].append({'name': name, 'passed': bool(ok), 'detail': detail})
        if not ok:
            report['failures'].append(name)
        print(('PASS ' if ok else 'FAIL ') + name)

    def call(arguments, env, stdin=None, label=''):
        result = run([str(EXE)] + arguments, env, stdin)
        transcript.append({'label': label or ' '.join(arguments), 'exit': result.returncode,
                           'stdout': result.stdout, 'stderr': result.stderr})
        return result

    profile = fresh_profile('m3-models-profile')
    env = dict(os.environ)
    env.pop('MORA_AGENT_MODEL', None)
    env['MORA_DATA_DIR'] = str(profile)

    stored = call(['--set-agent-key'], env, stdin=KEY + '\n', label='set-agent-key')
    step('key-vault', stored.returncode == 0 and
         json.loads(stored.stdout or '{}').get('keyLength') == len(KEY),
         f"exit={stored.returncode}")

    server = ThreadingHTTPServer(('127.0.0.1', 0), Stub)
    server.records = []
    server.mode = 'ok'
    port = server.server_address[1]
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f'http://127.0.0.1:{port}/v1/chat/completions'
    env['MORA_AGENT_BASE_URL'] = base

    models = call(['--models'], env, label='models-ok')
    try:
        payload = json.loads(models.stdout)
    except json.JSONDecodeError:
        payload = {}
    report['discovery'] = payload
    expected = f'http://127.0.0.1:{port}/v1/models'
    step('models-ok', models.returncode == 0 and payload.get('ok') is True,
         json.dumps(payload, ensure_ascii=False)[:300])
    step('endpoint-derived-same-host', payload.get('endpoint') == expected and
         urllib.parse.urlparse(payload.get('endpoint', '')).netloc ==
         urllib.parse.urlparse(base).netloc,
         f"endpoint={payload.get('endpoint')} expected={expected}")

    gets = [r for r in server.records if r['method'] == 'GET']
    step('models-get-authorized',
         len(gets) == 1 and gets[0]['path'] == '/v1/models' and gets[0]['authorized'] and
         gets[0]['userAgent'].startswith('mora-player/') and gets[0]['contentLength'] == 0,
         json.dumps(gets[0], ensure_ascii=False) if gets else 'no GET')
    step('no-third-party', len(server.records) == 1 and
         all(urllib.parse.urlparse(base).hostname == '127.0.0.1' for _ in server.records),
         f"{len(server.records)} request(s) to the stub only")

    models_by_id = {m.get('id'): m for m in payload.get('models', [])}
    flash = models_by_id.get('deepseek-v4.1-flash', {})
    omni = models_by_id.get('vision-omni-preview', {})
    step('parse-context-window', flash.get('contextTokens') == 65536 and
         omni.get('contextTokens') == 131072,
         json.dumps({k: v.get('contextTokens') for k, v in models_by_id.items()}))
    step('parse-max-output', flash.get('maxOutputTokens') == 8192 and
         omni.get('maxOutputTokens') == 4096,
         json.dumps({k: v.get('maxOutputTokens') for k, v in models_by_id.items()}))
    step('parse-modalities', flash.get('inputModalities') == ['text'] and
         flash.get('outputModalities') == ['text'] and
         omni.get('inputModalities') == ['text', 'image'] and
         omni.get('outputModalities') == ['text'],
         json.dumps({k: [v.get('inputModalities'), v.get('outputModalities')]
                     for k, v in models_by_id.items()}, ensure_ascii=False))
    step('parse-reasoning', flash.get('reasoning') is False and omni.get('reasoning') is True,
         json.dumps({k: v.get('reasoning') for k, v in models_by_id.items()}))
    step('reported-not-requested', payload.get('reportOnly') is True,
         f"reportOnly={payload.get('reportOnly')}")

    try:
        cached_file = json.loads((profile / 'agent.json').read_text(encoding='utf-8'))
    except (OSError, json.JSONDecodeError):
        cached_file = {}
    cached = cached_file.get('modelCatalog', {})
    step('catalog-cached', cached.get('ok') is True and len(cached.get('models', [])) == 2 and
         any(m.get('contextTokens') == 65536 for m in cached.get('models', [])),
         json.dumps(cached, ensure_ascii=False)[:240])
    step('key-not-cached', KEY not in json.dumps(cached_file, ensure_ascii=False),
         'agent.json holds no key material')

    # The cache must survive a restart and be readable offline: shut the stub down first.
    server.shutdown()
    checked = call(['--agent-check'], env, label='agent-check-offline')
    try:
        check = json.loads(checked.stdout)
    except json.JSONDecodeError:
        check = {}
    report['agentCheck'] = check
    step('cached-window-reported',
         (check.get('cachedModel') or {}).get('contextTokens') == 65536,
         json.dumps(check.get('cachedModel'), ensure_ascii=False))
    step('cached-modalities-reported', check.get('modelModalities') == ['text'] and
         check.get('cachedCatalog', {}).get('modelCount') == 2,
         json.dumps({'modelModalities': check.get('modelModalities'),
                     'cachedCatalog': check.get('cachedCatalog')}, ensure_ascii=False))
    step('effective-limit-reported', check.get('contextLimitTokens') == 65536,
         json.dumps({k: check.get(k) for k in ('contextTokens', 'contextLimitTokens',
                                              'contextFallbackTokens')}))

    # Failure codes. Each run gets the same stub with a different mode.
    server = ThreadingHTTPServer(('127.0.0.1', 0), Stub)
    server.records = []
    server.mode = 'ok'
    port = server.server_address[1]
    threading.Thread(target=server.serve_forever, daemon=True).start()
    env['MORA_AGENT_BASE_URL'] = f'http://127.0.0.1:{port}/v1/chat/completions'

    def failing(mode, expected_code):
        server.mode = mode
        server.records = []
        result = call(['--models'], env, label='models-' + mode)
        try:
            body = json.loads(result.stdout)
        except json.JSONDecodeError:
            body = {}
        step(f'code-{expected_code}',
             result.returncode != 0 and body.get('ok') is False and
             body.get('code') == expected_code,
             f"exit={result.returncode} code={body.get('code')} error={str(body.get('error'))[:120]}")
        return body

    failing('unauthorized', 'unauthorized')
    failing('not_supported', 'not_supported')
    failing('bad', 'bad_response')

    # A dead port must be reported as a network failure, not crash or hang.
    server.shutdown()
    dead = call(['--models'], env, label='models-network')
    try:
        dead_body = json.loads(dead.stdout)
    except json.JSONDecodeError:
        dead_body = {}
    step('code-network', dead.returncode != 0 and dead_body.get('code') == 'network',
         f"exit={dead.returncode} code={dead_body.get('code')}")

    keyless = fresh_profile('m3-models-nokey')
    keyless_env = dict(env)
    keyless_env['MORA_DATA_DIR'] = str(keyless)
    keyless_env['MORA_AGENT_BASE_URL'] = base
    no_key = call(['--models'], keyless_env, label='models-missing-key')
    try:
        no_key_body = json.loads(no_key.stdout)
    except json.JSONDecodeError:
        no_key_body = {}
    step('code-missing_key', no_key.returncode != 0 and no_key_body.get('code') == 'missing_key',
         f"exit={no_key.returncode} code={no_key_body.get('code')}")

    # An empty but valid catalog is a success with zero models, never an invented model.
    server = ThreadingHTTPServer(('127.0.0.1', 0), Stub)
    server.records = []
    server.mode = 'empty'
    port = server.server_address[1]
    threading.Thread(target=server.serve_forever, daemon=True).start()
    env['MORA_AGENT_BASE_URL'] = f'http://127.0.0.1:{port}/v1/chat/completions'
    empty = call(['--models'], env, label='models-empty')
    try:
        empty_body = json.loads(empty.stdout)
    except json.JSONDecodeError:
        empty_body = {}
    step('empty-catalog-honest', empty_body.get('ok') is True and
         empty_body.get('models') == [], json.dumps(empty_body, ensure_ascii=False)[:200])

    # Modality is a report, never a request field: the chat body must not carry one.
    server.mode = 'ok'
    server.records = []
    chat = call(['--chat', '你好'], env, label='chat')
    posts = [r for r in server.records if r['method'] == 'POST']
    step('chat-runs', chat.returncode == 0 and json.loads(chat.stdout or '{}').get('ok') is True,
         (chat.stdout or chat.stderr)[:200])
    step('no-modality-in-request',
         bool(posts) and all(r['body'] is not None and not has_modality_key(r['body'])
                             for r in posts),
         f"{len(posts)} POST body(ies) scanned")
    help_text = call(['--help'], env, label='help').stdout.lower()
    step('no-modality-switch', '--modalit' not in help_text and 'set-modalit' not in help_text,
         '--help advertises no modality switch')

    server.shutdown()

    printed = json.dumps(transcript, ensure_ascii=False) + json.dumps(report, ensure_ascii=False)
    report['keyMaterialPrinted'] = KEY in printed
    step('key-never-printed', KEY not in printed, 'no stdout/stderr/report carries the key')

    report['verdict'] = 'passed' if not report['failures'] else 'failed'
    out = ROOT / 'docs' / 'verification' / 'm3-models.json'
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, ensure_ascii=False, indent=2, default=str) + '\n',
                   encoding='utf-8')
    print(json.dumps({'verdict': report['verdict'], 'passed': len(report['steps']) -
                      len(report['failures']), 'total': len(report['steps']),
                      'failures': report['failures']}, ensure_ascii=True))
    return 0 if report['verdict'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
