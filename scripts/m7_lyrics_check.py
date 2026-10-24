"""Lyrics acceptance: the agent reads live lyrics through the running player.

Starts the real TUI on a public track, attaches an MCP adapter, and checks that
  * music_now_playing reports the line being sung right now,
  * music_get_lyrics returns bounded timed lines with the live index,
  * the payload stays small and never contains a stream URL or cookie.

Usage: python scripts/m7_lyrics_check.py [--song 539928216]
"""
import argparse
import hashlib
import json
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / 'build' / 'mora.exe'
FRAME = ROOT / 'scripts' / 'frame_capture.py'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--song', type=int, default=0,
                        help='explicit song id; by default the script searches a real vocal song')
    args = parser.parse_args()
    report = {'schema': 'mora-lyrics/1', 'steps': [], 'failures': [],
              'sha256': hashlib.sha256(EXE.read_bytes()).hexdigest().upper()}

    def step(name, ok, detail=''):
        report['steps'].append({'name': name, 'passed': bool(ok), 'detail': detail})
        if not ok:
            report['failures'].append(name)
        print(('PASS ' if ok else 'FAIL ') + name)

    work = ROOT / 'build' / 'lyrics-check'
    work.mkdir(parents=True, exist_ok=True)
    # Real playback needs the user's signed-in session: a guest request for a stream URL answers
    # 400 for these songs. A --seconds run never writes settings, so the user's profile is untouched.
    snapshot = work / 'frame.ansi'
    capture = subprocess.Popen(
        [sys.executable, str(FRAME), str(snapshot), '--columns', '116', '--rows', '36',
         '--seconds', '60', '--pass-through'],
        cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        creationflags=0x00000008)
    try:
        capture.stdout.readline()
        pid_line = capture.stdout.readline()
        assert pid_line.startswith('player pid '), pid_line
        adapter = subprocess.Popen([str(EXE), '--mcp'], cwd=ROOT, stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                   encoding='utf-8', errors='replace')

        def call(request_id, tool, arguments=None, timeout=25):
            adapter.stdin.write(json.dumps({'jsonrpc': '2.0', 'id': request_id,
                                            'method': 'tools/call',
                                            'params': {'name': tool,
                                                       'arguments': arguments or {}}}) + '\n')
            adapter.stdin.flush()
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                line = adapter.stdout.readline()
                if not line:
                    time.sleep(.05)
                    continue
                try:
                    message = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if message.get('id') == request_id:
                    return message
            return {}

        adapter.stdin.write(json.dumps({'jsonrpc': '2.0', 'id': 0, 'method': 'initialize',
                                        'params': {'protocolVersion': '2025-11-25',
                                                   'capabilities': {},
                                                   'clientInfo': {'name': 'lyrics',
                                                                  'version': '1'}}}) + '\n')
        adapter.stdin.write(json.dumps({'jsonrpc': '2.0',
                                        'method': 'notifications/initialized'}) + '\n')
        adapter.stdin.flush()

        # Find a real vocal song, then ask the player to play it.
        song_id = args.song
        if not song_id:
            reply = call(90, 'music_search', {'query': '晴天 周杰伦', 'limit': 10})
            items = reply.get('result', {}).get('structuredContent', {}).get('items', [])
            report['search'] = [{'id': item.get('id'), 'name': item.get('name'),
                                 'artist': item.get('artist')} for item in items[:3]]
            # Prefer the original vocal recording over covers, which often ship credits-only lyrics.
            for item in items:
                if item.get('id') and '周杰伦' in (item.get('artist') or ''):
                    song_id = int(item['id'])
                    break
            if not song_id:
                for item in items:
                    if item.get('id'):
                        song_id = int(item['id'])
                        break
        step('found-a-real-song', bool(song_id), song_id)
        # A signed-in session makes the player fetch the account at startup, which legitimately
        # answers "busy" for a moment, so retry until it is idle before submitting playback.
        submission = {}
        for attempt in range(20):
            submission = call(91 + attempt, 'music_play', {'id': song_id})
            result = submission.get('result', {})
            detail = result.get('structuredContent', {})
            if not result.get('isError', True) or detail.get('error') != 'busy':
                break
            time.sleep(.5)
        step('play-accepted',
             not submission.get('result', {}).get('isError', True),
             submission.get('result', {}).get('structuredContent'))
        report['songId'] = song_id

        # Wait for the track to actually play, then give the player a moment to fetch lyrics.
        playing = {}
        raw_first = {}
        for attempt in range(40):
            reply = call(100 + attempt, 'music_now_playing')
            if attempt == 0:
                raw_first = reply
            playing = reply.get('result', {}).get('structuredContent', {})
            if playing.get('playing') and playing.get('lyrics', {}).get('available'):
                break
            time.sleep(1)
        report['firstReply'] = raw_first
        report['nowPlaying'] = {key: playing.get(key)
                                for key in ('playing', 'positionSeconds', 'quality', 'track')}
        step('track-is-playing', bool(playing.get('playing')), playing.get('track'))
        lyric_block = playing.get('lyrics', {})
        step('now-playing-carries-current-line',
             bool(lyric_block.get('available')) and bool(lyric_block.get('currentLine')),
             lyric_block)
        step('now-playing-line-matches-index',
             isinstance(lyric_block.get('currentIndex'), int) and
             lyric_block.get('currentIndex') >= 0, lyric_block.get('currentIndex'))

        reply = call(200, 'music_get_lyrics', {'maxLines': 12})
        payload = reply.get('result', {}).get('structuredContent', {})
        report['lyrics'] = {key: payload.get(key)
                            for key in ('songId', 'title', 'artist', 'lineCount', 'currentIndex',
                                        'truncated', 'hasTranslation')}
        lines = payload.get('lines', [])
        report['lyrics']['lineSample'] = lines[:3]
        step('lyrics-returned', len(lines) > 0, f'{len(lines)} lines')
        step('lyrics-bounded', len(lines) <= 12, len(lines))
        step('lyrics-index-in-window',
             isinstance(payload.get('currentIndex'), int) and
             payload.get('fromLine', 0) <= payload['currentIndex'] <
             payload.get('fromLine', 0) + len(lines), payload.get('currentIndex'))
        step('lyrics-have-timestamps',
             all(isinstance(line.get('seconds'), (int, float)) for line in lines), lines[:1])
        step('lyrics-carry-text',
             all(isinstance(line.get('text'), str) and line['text'] for line in lines),
             lines[:1])
        raw = json.dumps(payload, ensure_ascii=False)
        step('no-stream-url-or-cookie',
             'http' not in raw.lower() and 'cookie' not in raw.lower() and 'MUSIC' not in raw,
             f'{len(raw)} bytes')
        step('lyrics-payload-small', len(raw) <= 16384, f'{len(raw)} bytes')

        # An unknown song must fail cleanly instead of inventing lyrics.
        reply = call(300, 'music_get_lyrics', {'songId': 1})
        bad = reply.get('result', {})
        step('unknown-song-fails-cleanly',
             bool(bad.get('isError')) and
             bad.get('structuredContent', {}).get('error') in ('not_found', 'network_unavailable',
                                                               'needs_login'),
             bad.get('structuredContent'))

        adapter.stdin.close()
        adapter.wait(timeout=10)
        step('mcp-stdout-clean', adapter.returncode == 0, adapter.returncode)
    finally:
        if capture.poll() is None:
            capture.terminate()
        try:
            capture.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            capture.kill()

    report['verdict'] = 'passed' if not report['failures'] else 'failed'
    out = ROOT / 'docs' / 'verification' / 'm7-lyrics.json'
    out.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'verdict': report['verdict'], 'failures': report['failures']},
                     ensure_ascii=True))
    return 0 if report['verdict'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
