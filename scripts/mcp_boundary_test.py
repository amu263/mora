"""Offline MCP boundary regressions, including cases that must never reach an API.

An oversized, non-secret session sentinel makes accidental MusicService API
construction fail before any network request, including against older builds.
"""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--exe', type=Path, default=ROOT / 'build/mora.exe')
options = parser.parse_args()
ROOT.joinpath('build').mkdir(exist_ok=True)
checks = 0
failures = []


def check(condition, description):
    global checks
    checks += 1
    if not condition:
        failures.append(description)


def req(identity, method, params=None):
    message = {'jsonrpc': '2.0', 'id': identity, 'method': method}
    if params is not None:
        message['params'] = params
    return message


initialize = req(1, 'initialize', {
    'protocolVersion': '2025-11-25', 'capabilities': {},
    'clientInfo': {'name': 'mora-boundary-test', 'version': '1'},
})
ready = {'jsonrpc': '2.0', 'method': 'notifications/initialized'}


with tempfile.TemporaryDirectory(prefix='mcp-boundary-', dir=ROOT / 'build') as directory:
    profile = Path(directory)
    (profile / 'session.bin').write_bytes(b'x' * 70000)

    def run(messages):
        payload = '\n'.join(
            item if isinstance(item, str) else json.dumps(item, ensure_ascii=False)
            for item in messages
        ) + '\n'
        result = subprocess.run(
            [str(options.exe.resolve()), '--mcp', '--data-dir', str(profile)],
            input=payload, encoding='utf-8', capture_output=True, timeout=10,
        )
        check(result.returncode == 0, 'server exits cleanly after input EOF')
        check('\x1b' not in result.stdout, 'stdout contains no terminal escape sequences')
        try:
            return [json.loads(line) for line in result.stdout.splitlines()]
        except json.JSONDecodeError:
            failures.append('stdout contains only JSON messages')
            return []

    def code(response):
        return response.get('error', {}).get('code')

    responses = run([
        req(0, 'tools/list'), ready, initialize, req(2, 'tools/list'), ready,
        req(3, 'tools/list'), initialize, req(4, 'tools/call', {
            'name': 'music_get_status', 'arguments': {},
        }),
    ])
    check(len(responses) == 6, 'notifications produce no responses')
    if len(responses) == 6:
        check(code(responses[0]) == -32002, 'tools require initialize')
        check(code(responses[2]) == -32002, 'tools require initialized notification')
        tools = responses[3].get('result', {}).get('tools', [])
        check({tool['name'] for tool in tools} == {
            'music_search', 'music_open_collection', 'music_play', 'music_control',
            'music_now_playing', 'music_get_lyrics', 'music_recommend', 'music_play_random',
            'music_list_playlists', 'music_set_like', 'music_add_to_playlist',
            'music_get_status', 'music_queue_list', 'music_queue_edit'},
              'advertised tools match the implemented contract')
        check(code(responses[4]) == -32600, 'duplicate initialize cannot reset session')
        status = responses[5].get('result', {}).get('structuredContent', {})
        check(status.get('playerSessionAttached') is False,
              'MCP status does not claim a live playback connection')

    ids = [None, True, 1.5, {}, []]
    responses = run([initialize, ready] + [req(identity, 'ping') for identity in ids]
                    + [req('请求-1', 'ping'), req(0, 'ping')])
    check(len(responses) == 8, 'invalid IDs do not terminate the stream')
    if len(responses) == 8:
        for response in responses[1:6]:
            check(code(response) == -32600 and response.get('id') is None,
                  'invalid request ID is rejected without reflecting its value')
        check(responses[6].get('id') == '请求-1', 'Unicode string ID is preserved')
        check(responses[7].get('id') == 0, 'zero integer ID is preserved')

    invalid_params = [
        {'name': 'music_get_status', 'arguments': None},
        {'name': 'music_get_status', 'arguments': []},
        {'name': 7, 'arguments': {}},
    ]
    responses = run([initialize, ready] + [
        req(index + 2, 'tools/call', params) for index, params in enumerate(invalid_params)
    ] + [req(9, 'tools/list', {'cursor': 'not-a-real-cursor'})])
    check(len(responses) == 5, 'malformed tool parameters keep the server usable')
    if len(responses) == 5:
        for response in responses[1:]:
            check(code(response) == -32602, 'malformed protocol parameters are protocol errors')

    bad_searches = [
        ({'query': 'x', 'limit': 1.5}, 'limit must be an integer'),
        ({'query': 'x', 'limit': 4294967297}, 'limit must be 1..30'),
        ({'query': 'x', 'limit': 0}, 'limit must be 1..30'),
        ({'query': 'x', 'limit': True}, 'limit must be an integer'),
        ({'query': 'x', 'offset': 0.5}, 'offset must be an integer'),
        ({'query': 'x', 'offset': 4294967296}, 'offset must be 0..3000'),
        ({'query': '   \t\n'}, 'query must contain visible text'),
        ({'query': 'x\x1b[31m'}, 'query must not contain control characters'),
        ({'query': 'x', 'kind': 5}, 'kind must be song, album or playlist'),
    ]
    responses = run([initialize, ready] + [
        req(index + 2, 'tools/call', {'name': 'music_search', 'arguments': arguments})
        for index, (arguments, _) in enumerate(bad_searches)
    ])
    check(len(responses) == len(bad_searches) + 1, 'invalid searches do not terminate server')
    for response, (_, expected) in zip(responses[1:], bad_searches):
        result = response.get('result', {})
        text = ' '.join(item.get('text', '') for item in result.get('content', []))
        check(result.get('isError') is True and expected in text,
              f'search rejects invalid input before API construction: {expected}')
        check('Session file too large' not in text,
              'search validation does not reach the isolated API sentinel')

    invalid_tool_inputs = [
        ('music_play', {}, 'invalid_arguments'),
        ('music_play', {'id': 0}, 'invalid_arguments'),
        ('music_play', {'id': 1, 'kind': 'artist'}, 'invalid_arguments'),
        ('music_control', {'action': 'explode'}, 'invalid_arguments'),
        ('music_control', {'action': 'seek'}, 'invalid_arguments'),
        ('music_control', {'action': 'volume', 'value': 'loud'}, 'invalid_arguments'),
        ('music_open_collection', {'kind': 'album'}, 'invalid_arguments'),
        ('music_open_collection', {'kind': 'artist', 'id': 1}, 'invalid_arguments'),
        ('music_now_playing', {'x': 1}, 'invalid_arguments'),
        ('music_recommend', {'source': 'library'}, 'invalid_arguments'),
        ('music_recommend', {'source': 'search'}, 'invalid_arguments'),
        ('music_play_random', {}, 'invalid_arguments'),
        ('music_set_like', {'songId': 1}, 'invalid_arguments'),
        ('music_set_like', {'songId': 1, 'liked': 'yes'}, 'invalid_arguments'),
        ('music_add_to_playlist', {'songId': 1}, 'invalid_arguments'),
        ('music_list_playlists', {'limit': 0}, 'invalid_arguments'),
    ]
    responses = run([initialize, ready] + [
        req(index + 2, 'tools/call', {'name': name, 'arguments': arguments})
        for index, (name, arguments, _) in enumerate(invalid_tool_inputs)
    ])
    check(len(responses) == len(invalid_tool_inputs) + 1,
          'invalid tool input does not terminate the server')
    for response, (_, _, expected) in zip(responses[1:], invalid_tool_inputs):
        result = response.get('result', {})
        structured = result.get('structuredContent', {})
        check(result.get('isError') is True and structured.get('error') == expected,
              f'tool input is validated before any side effect: {expected}')

    no_player = [
        ('music_play', {'id': 123}),
        ('music_control', {'action': 'pause'}),
        ('music_now_playing', {}),
        ('music_play_random', {'candidateSetId': 'rec-1'}),
        ('music_set_like', {'songId': 1, 'liked': True}),
        ('music_add_to_playlist', {'songId': 1, 'playlistId': 42}),
    ]
    responses = run([initialize, ready] + [
        req(index + 2, 'tools/call', {'name': name, 'arguments': arguments})
        for index, (name, arguments) in enumerate(no_player)
    ])
    check(len(responses) == len(no_player) + 1,
          'session tools answer instead of hanging when no player is attached')
    for response in responses[1:]:
        result = response.get('result', {})
        structured = result.get('structuredContent', {})
        check(result.get('isError') is True and structured.get('error') == 'needs_player',
              'session tools report needs_player instead of pretending to play')

    responses = run([
        req(1, 'initialize', {'protocolVersion': None}), req(2, 'tools/list'),
        initialize, ready, req(3, 'ping'),
    ])
    check(len(responses) == 4, 'failed initialize can be followed by valid initialize')
    if len(responses) == 4:
        check(code(responses[0]) == -32602, 'protocolVersion must be a string')
        check(code(responses[1]) == -32002, 'failed initialize does not open capabilities')
        check(responses[3].get('result') == {}, 'valid session still answers ping')

    responses = run([initialize, ready, '{', 'x' * 65537, req('after-errors', 'ping')])
    check(len(responses) == 4, 'framing recovers after malformed and oversized messages')
    if len(responses) == 4:
        check(code(responses[1]) == -32700, 'invalid JSON has a parse error')
        check(code(responses[2]) == -32600, 'oversized message has an invalid request error')
        check(responses[3].get('id') == 'after-errors', 'next frame survives oversize discard')

report = {'passed': not failures, 'checks': checks, 'failures': failures,
          'realNetworkUsed': False}
ROOT.joinpath('build/mcp-boundary.json').write_text(
    json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print(json.dumps(report, ensure_ascii=True))
raise SystemExit(bool(failures))
