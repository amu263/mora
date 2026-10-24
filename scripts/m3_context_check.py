"""M3 context-memory acceptance against a local stub provider (never the internet).

Covers the context window path that `mora --chat` drives:
  * an over-limit prompt first compresses: the oldest turns are folded into one summary system
    message and the newest turns stay verbatim
  * the summary request is one bounded call carrying the compression instruction (keep the user's
    preferences, confirmed facts and unfinished requests)
  * the folded turns really leave the chat request, while the kept turns stay
  * outcome accounting is honest: promptTokens / contextLimitTokens / compressedMessages and the
    compressionDegraded / compressionCode observability fields
  * when the summary call fails the run still answers, the summary is a labelled deterministic
    truncation and the folded history is still present (never silently dropped)
  * a prompt under the limit is not compressed
  * the built-in prompt steers a vague request to "search, then build a multi-track queue"
  * the user cannot select a modality: no request body carries a modality field

The assertions are deliberately independent of how wide the CLI projects its history
(`conversationHistory(conversation, N)`): the fixture is shorter than any projection used by the
CLI and stays over the window, so the folded/kept split is verified from the request itself
(folded messages = projected turns - kept turns), never from a hard-coded projection width.

Usage: python scripts/m3_context_check.py [--exe PATH]
"""
import argparse
import json
import os
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / 'build' / 'mora.exe'
KEY = 'stub-key-not-a-secret'
SUMMARY_MARK = 'SUM-MARK-2026'
ANSWER_MARK = 'ANSWER-MARK-2026'
# A fixture that is longer than the window but shorter than every history projection the CLI
# uses (12 before, 24 now), so the whole fixture reaches the compression planner either way.
FIXTURE_TURNS = 10
KEEP_RECENT = 6  # AgentConfig::keepRecentMessages default
FOLDED_MARK = 'FOLDED-TURN-MARKER-0001'  # oldest fixture turn, must be folded
KEPT_MARK = 'KEPT-TURN-MARKER-0010'      # newest fixture turn, must stay verbatim
FILLER = '这是一段用于把上下文推高的对话内容，记录用户最近听过的歌曲与歌单。' * 20
NEW_USER_TEXT = '请把上次确认过的歌单再念一遍'
VAGUE_TEXT = '来点周杰伦的歌'
# Over the 9000-token window (threshold 6480) but the six retained turns alone still fit; 2500 is
# small enough that even the retained turns do not fit, which exercises the safety net.
CONTEXT_TOKENS = 9000
SAFETY_CONTEXT_TOKENS = 2500


def has_modality_key(value):
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

    def do_POST(self):
        length = int(self.headers.get('Content-Length', 0) or 0)
        raw = self.rfile.read(length) if length else b''
        try:
            payload = json.loads(raw or b'{}')
        except json.JSONDecodeError:
            payload = None
        messages = (payload or {}).get('messages') or []
        first = messages[0].get('content', '') if messages and isinstance(messages[0], dict) else ''
        is_summary = not (payload or {}).get('tools') and '压缩' in str(first)
        self.server.records.append({
            'path': self.path,
            'authorized': bool(self.headers.get('Authorization')),
            'isSummary': bool(is_summary),
            'body': payload,
        })
        if is_summary and self.server.mode == 'summary-fail':
            return self._reply(500, {'error': 'summary backend exploded'})
        if is_summary:
            return self._reply(200, {'choices': [{'message': {
                'role': 'assistant',
                'content': f'用户偏好安静的钢琴曲；已确认《夜的钢琴曲》歌手是石进；'
                           f'未完成：还要一张类似歌单。{SUMMARY_MARK}'}}]})
        return self._reply(200, {'choices': [{'message': {
            'role': 'assistant', 'content': f'好的，已经念过了。{ANSWER_MARK}'}}]})

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


def long_turns():
    """FIXTURE_TURNS alternating turns, long enough that the whole fixture crosses the window."""
    turns = []
    for index in range(FIXTURE_TURNS):
        role = 'user' if index % 2 == 0 else 'assistant'
        marker = ''
        if index == 0:
            marker = FOLDED_MARK + ' '
        elif index == FIXTURE_TURNS - 1:
            marker = KEPT_MARK + ' '
        turns.append({'role': role, 'content': f'{marker}{FILLER}'})
    return turns


def write_memory(profile, turns, summary=''):
    payload = {'version': 1, 'summary': summary, 'compressedTurns': 0, 'turns': turns}
    (profile / 'chat-memory.json').write_text(
        json.dumps(payload, ensure_ascii=False), encoding='utf-8')


def main():
    global EXE
    parser = argparse.ArgumentParser(description='MORA M3 context memory acceptance')
    parser.add_argument('--exe', default=str(EXE), help='binary to test')
    args = parser.parse_args()
    EXE = Path(args.exe).resolve()

    report = {'schema': 'mora-m3-context/1', 'steps': [], 'failures': [],
              'fixtureTurns': FIXTURE_TURNS, 'keepRecentMessages': KEEP_RECENT,
              'note': 'local stub provider; assertions do not depend on the projection width'}
    transcript = []

    def step(name, ok, detail):
        report['steps'].append({'name': name, 'passed': bool(ok), 'detail': detail})
        if not ok:
            report['failures'].append(name)
        print(('PASS ' if ok else 'FAIL ') + name)

    profile = fresh_profile('m3-context-profile')
    env = dict(os.environ)
    env['MORA_DATA_DIR'] = str(profile)
    stored = run([str(EXE), '--set-agent-key'], env, stdin=KEY + '\n')
    step('key-vault', stored.returncode == 0, f'exit={stored.returncode}')

    server = ThreadingHTTPServer(('127.0.0.1', 0), Stub)
    server.records = []
    server.mode = 'ok'
    port = server.server_address[1]
    threading.Thread(target=server.serve_forever, daemon=True).start()
    env['MORA_AGENT_BASE_URL'] = f'http://127.0.0.1:{port}/v1/chat/completions'
    env['MORA_AGENT_MODEL'] = 'deepseek-v4.1-flash'

    def chat(context_tokens, label, user_text=NEW_USER_TEXT):
        server.records = []
        run_env = dict(env)
        if context_tokens:
            run_env['MORA_AGENT_CONTEXT_TOKENS'] = str(context_tokens)
        else:
            run_env.pop('MORA_AGENT_CONTEXT_TOKENS', None)
        result = run([str(EXE), '--chat', user_text], run_env)
        transcript.append({'label': label, 'exit': result.returncode,
                           'stdout': result.stdout, 'stderr': result.stderr})
        try:
            payload = json.loads(result.stdout)
        except json.JSONDecodeError:
            payload = {}
        summaries = [r for r in server.records if r['isSummary']]
        chats = [r for r in server.records if not r['isSummary']]
        return result, payload, summaries, chats

    def split(record):
        """The request as the planner built it: system, optional summary, kept turns, user."""
        messages = (record or {}).get('body', {}).get('messages', [])
        compressed = (len(messages) > 2 and isinstance(messages[1], dict) and
                      messages[1].get('role') == 'system')
        summary = messages[1].get('content', '') if compressed else ''
        kept = messages[2:-1] if compressed else messages[1:-1]
        return messages, summary, kept

    # 1) Over the limit: the oldest fixture turns are folded, the newest stay verbatim.
    fixture = long_turns()
    write_memory(profile, fixture)
    server.mode = 'ok'
    result, payload, summaries, chats = chat(CONTEXT_TOKENS, 'compress-ok')
    report['compressOk'] = payload
    step('chat-ok-after-compression', result.returncode == 0 and payload.get('ok') is True and
         ANSWER_MARK in payload.get('answer', ''), (result.stdout or result.stderr)[:240])
    step('compression-triggered', len(summaries) == 1 and len(chats) == 1,
         f'summaryRequests={len(summaries)} chatRequests={len(chats)}')
    instruction = summaries[0]['body']['messages'][0]['content'] if summaries else ''
    step('summary-instruction',
         '偏好' in instruction and '未完成' in instruction and '确认' in instruction and
         '压缩' in instruction and '保留' in instruction, instruction[:200])
    step('summary-single-bounded-call',
         len(summaries) == 1 and not summaries[0]['body'].get('tools') and
         summaries[0]['body']['messages'][-1]['role'] == 'user',
         f"summaryRequests={len(summaries)} tools={summaries[0]['body'].get('tools') if summaries else None}")

    messages, summary_text, kept = split(chats[0] if chats else {})
    step('summary-system-message-injected',
         len(messages) >= 3 and messages[0].get('role') == 'system' and
         messages[1].get('role') == 'system' and '[早前对话摘要]' in summary_text and
         SUMMARY_MARK in summary_text,
         json.dumps(messages[1], ensure_ascii=False)[:200] if len(messages) > 1 else 'no messages')
    step('recent-turns-kept-verbatim',
         [turn.get('content') for turn in kept] == [turn['content'] for turn in fixture[-KEEP_RECENT:]],
         f'kept={len(kept)} expected={KEEP_RECENT}')
    # folded messages = projected turns - kept turns, and the fixture is fully projected.
    step('folded-count-matches-retention',
         payload.get('compressedMessages') == FIXTURE_TURNS - len(kept) and
         payload.get('compressedMessages') == FIXTURE_TURNS - KEEP_RECENT,
         f"compressedMessages={payload.get('compressedMessages')} kept={len(kept)} "
         f"fixture={FIXTURE_TURNS}")
    step('oldest-turns-folded-away', FOLDED_MARK not in json.dumps(messages, ensure_ascii=False),
         'the folded turn no longer travels as a raw message')
    step('new-user-turn-last', messages and messages[-1] == {'role': 'user',
                                                             'content': NEW_USER_TEXT},
         json.dumps(messages[-1], ensure_ascii=False) if messages else 'no messages')
    step('accounting-honest',
         payload.get('contextLimitTokens') == CONTEXT_TOKENS and
         0 < payload.get('promptTokens', 0) <= CONTEXT_TOKENS and
         payload.get('compressedMessages') == FIXTURE_TURNS - len(kept) and
         payload.get('compressionDegraded') is False and payload.get('compressionCode') == '' and
         not [item for item in payload.get('trace', []) if item.get('compression')],
         json.dumps({k: payload.get(k) for k in ('promptTokens', 'contextLimitTokens',
                                                 'compressedMessages', 'compressionDegraded',
                                                 'compressionCode')}))

    # 2) The summary call fails: the answer must still come back and the history must be kept.
    write_memory(profile, fixture)
    server.mode = 'summary-fail'
    result, payload, summaries, chats = chat(CONTEXT_TOKENS, 'compress-degraded')
    report['degraded'] = payload
    messages, summary_text, kept = split(chats[0] if chats else {})
    step('degraded-summary-labelled', '[压缩降级' in summary_text, summary_text[:200])
    step('degraded-keeps-old-history', FOLDED_MARK in summary_text and
         len(summary_text.encode('utf-8')) <= 1400,
         f'summaryBytes={len(summary_text.encode("utf-8"))}')
    step('degraded-still-answers', result.returncode == 0 and payload.get('ok') is True and
         ANSWER_MARK in payload.get('answer', '') and
         payload.get('compressedMessages') == FIXTURE_TURNS - len(kept),
         (result.stdout or result.stderr)[:240])
    # The observability fields must agree with the labelled summary, without parsing its text.
    step('degraded-flags-reported',
         payload.get('compressionDegraded') is True and
         payload.get('compressionCode') == 'server_error' and not payload.get('code'),
         json.dumps({k: payload.get(k) for k in ('ok', 'compressionDegraded',
                                                 'compressionCode', 'code')}))
    degraded_trace = [item for item in payload.get('trace', [])
                      if item.get('compression') == 'degraded']
    step('degraded-trace-readable',
         len(degraded_trace) == 1 and degraded_trace[0].get('code') == 'server_error' and
         degraded_trace[0].get('foldedTurns') == payload.get('compressedMessages'),
         json.dumps(degraded_trace, ensure_ascii=False)[:200])

    # 3) Under the limit nothing is compressed.
    write_memory(profile, [{'role': 'user', 'content': '你好'},
                           {'role': 'assistant', 'content': '你好，我是 MORA。'}])
    server.mode = 'chat-only'
    result, payload, summaries, chats = chat(CONTEXT_TOKENS, 'under-limit')
    messages, summary_text, kept = split(chats[0] if chats else {})
    step('under-limit-not-compressed',
         result.returncode == 0 and payload.get('ok') is True and
         payload.get('compressedMessages') == 0 and payload.get('compressionDegraded') is False and
         len(summaries) == 0 and len(chats) == 1 and
         not any('[早前对话摘要]' in (m.get('content') or '') for m in messages),
         json.dumps({k: payload.get(k) for k in ('compressedMessages', 'promptTokens',
                                                 'compressionDegraded')}) +
         f' summaryRequests={len(summaries)}')

    # 3b) A vague request must be steered to "search, then build a multi-track queue".
    write_memory(profile, [{'role': 'user', 'content': '你好'},
                           {'role': 'assistant', 'content': '你好，我是 MORA。'}])
    server.mode = 'chat-only'
    result, payload, summaries, chats = chat(CONTEXT_TOKENS, 'vague-request', VAGUE_TEXT)
    system_text = chats[0]['body']['messages'][0].get('content', '') if chats else ''
    rules = ['music_search', 'music_queue_edit', 'replace', 'music_play', 'music_now_playing',
             'accepted', 'append', 'add_next', '只播一首', '5..20']
    missing = [rule for rule in rules if rule not in system_text]
    step('vague-request-steers-to-multi-queue', not missing,
         f'missing={missing} systemBytes={len(system_text.encode("utf-8"))}')
    step('vague-request-reaches-model',
         bool(chats) and chats[0]['body']['messages'][-1].get('content') == VAGUE_TEXT,
         json.dumps(chats[0]['body']['messages'][-1], ensure_ascii=False) if chats
         else 'no request')
    step('system-prompt-bounded', 0 < len(system_text.encode('utf-8')) <= 2400,
         f'systemPromptBytes={len(system_text.encode("utf-8"))}')

    # 4) Even the retained turns do not fit: fold more, never drop the newest.
    write_memory(profile, fixture)
    server.mode = 'ok'
    result, payload, summaries, chats = chat(SAFETY_CONTEXT_TOKENS, 'safety-net')
    messages, summary_text, kept = split(chats[0] if chats else {})
    step('over-limit-still-fits',
         1 <= len(kept) < KEEP_RECENT and
         payload.get('compressedMessages') == FIXTURE_TURNS - len(kept) and
         payload.get('contextLimitTokens') == SAFETY_CONTEXT_TOKENS and
         0 < payload.get('promptTokens', 0) <= SAFETY_CONTEXT_TOKENS,
         json.dumps({k: payload.get(k) for k in ('promptTokens', 'contextLimitTokens',
                                                 'compressedMessages')}) + f' kept={len(kept)}')
    step('newest-turn-never-dropped', KEPT_MARK in json.dumps(kept, ensure_ascii=False),
         f'kept={len(kept)} messages')

    # 5) The turn is appended to chat-memory.json for the next launch.
    try:
        memory = json.loads((profile / 'chat-memory.json').read_text(encoding='utf-8'))
    except (OSError, json.JSONDecodeError):
        memory = {}
    contents = [turn.get('content') for turn in memory.get('turns', [])]
    step('memory-appended', NEW_USER_TEXT in contents, f'{len(contents)} stored turns')

    # 6) Modality is reported, never requested.
    bodies = [r['body'] for r in server.records if r.get('body')]
    step('no-modality-in-request', bool(bodies) and
         all(not has_modality_key(body) for body in bodies),
         f'{len(bodies)} request body(ies) scanned')
    step('no-modality-field-anywhere',
         'modalit' not in json.dumps(bodies, ensure_ascii=False).lower(),
         'no modality field reached any endpoint')

    server.shutdown()
    printed = json.dumps(transcript, ensure_ascii=False) + json.dumps(report, ensure_ascii=False)
    step('key-never-printed', KEY not in printed, 'no stdout/stderr/report carries the key')

    report['verdict'] = 'passed' if not report['failures'] else 'failed'
    out = ROOT / 'docs' / 'verification' / 'm3-context.json'
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, ensure_ascii=False, indent=2, default=str) + '\n',
                   encoding='utf-8')
    print(json.dumps({'verdict': report['verdict'], 'passed': len(report['steps']) -
                      len(report['failures']), 'total': len(report['steps']),
                      'failures': report['failures']}, ensure_ascii=True))
    return 0 if report['verdict'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
