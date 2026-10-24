"""M4-B acceptance: account read/write tools against the real signed-in account.

Drives the real TUI in a hidden console, attaches mora --mcp and exercises
music_list_playlists / music_set_like / music_add_to_playlist. The like test is reversible
(like then unlike). The playlist-add test only runs when --playlist is passed by the user, so no
unrequested change is ever written.

Usage: python scripts/m4b_account_check.py [--playlist 12345] [--song 149229]
"""
import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from soak_check import Console, Mcp  # reuse the hidden-console and stdio helpers

ROOT = Path(__file__).resolve().parents[1]


def call(mcp, name, arguments):
    response = mcp.call('tools/call', {'name': name, 'arguments': arguments})
    result = response.get('result', {})
    return result, result.get('structuredContent', {}), result.get('isError', False)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--playlist', type=int, default=0, help='own playlist id for the add test')
    parser.add_argument('--song', type=int, default=149229)
    parser.add_argument('--exe', default=str(ROOT / 'build' / 'mora.exe'))
    parser.add_argument('--out', default=str(ROOT / 'docs' / 'verification' / 'm4b-account.json'))
    parser.add_argument('--seconds', type=int, default=150)
    args = parser.parse_args()
    report = {'schema': 'mora-m4b-account/1', 'song': args.song, 'playlist': args.playlist,
              'steps': [], 'failures': []}

    def step(name, ok, detail):
        report['steps'].append({'name': name, 'passed': bool(ok), 'detail': detail})
        if not ok:
            report['failures'].append(name)
        print(('PASS ' if ok else 'FAIL ') + name)

    with Console() as console:
        startup = subprocess.STARTUPINFO()
        startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        tui_report = ROOT / 'build' / 'm4b-tui-report.json'
        with open('CONIN$', 'rb', buffering=0) as stdin_file, \
                open('CONOUT$', 'wb', buffering=0) as stdout_file:
            tui = subprocess.Popen([args.exe, '--seconds', str(args.seconds), '--report',
                                    str(tui_report)], stdin=stdin_file, stdout=stdout_file,
                                   stderr=subprocess.DEVNULL, startupinfo=startup)
            mcp = None
            try:
                time.sleep(1.5)
                mcp = Mcp(args.exe)
                mcp.call('initialize', {'protocolVersion': '2025-11-25', 'capabilities': {},
                                        'clientInfo': {'name': 'mora-m4b', 'version': '1'}})
                mcp.notify('notifications/initialized')
                # The player may still be binding its pipe; retry briefly like a real host would.
                report['attached'] = False
                report['statusRaw'] = []
                for _ in range(10):
                    raw = mcp.call('tools/call', {'name': 'music_get_status',
                                                  'arguments': {}})
                    report['statusRaw'].append(raw)
                    status = raw.get('result', {}).get('structuredContent', {})
                    report['attached'] = bool(status.get('playerSessionAttached'))
                    if report['attached']:
                        break
                    time.sleep(0.5)
                step('mcp-attached', report['attached'], 'playerSessionAttached')

                _, playlists, error = call(mcp, 'music_list_playlists', {'ownedOnly': True,
                                                                        'limit': 5})
                items = playlists.get('items', [])
                report['playlists'] = items
                step('owned-playlists', not error and len(items) > 0 and
                     all(item.get('canWrite') for item in items),
                     f'{len(items)} owned playlists, canWrite={[i.get("canWrite") for i in items]}')

                _, liked, error = call(mcp, 'music_set_like', {'songId': args.song,
                                                               'liked': True})
                step('like-on', not error and liked.get('liked') is True,
                     json.dumps(liked, ensure_ascii=False)[:200])
                _, unliked, error = call(mcp, 'music_set_like', {'songId': args.song,
                                                                 'liked': False})
                step('like-off', not error and unliked.get('liked') is False,
                     json.dumps(unliked, ensure_ascii=False)[:200])

                # A playlist this account does not own must be refused without writing.
                result, payload, error = call(mcp, 'music_add_to_playlist',
                                              {'songId': args.song, 'playlistId': 3778678})
                step('foreign-playlist-refused', error and payload.get('error') == 'no_permission',
                     json.dumps(payload, ensure_ascii=False)[:200])

                if args.playlist:
                    result, payload, error = call(mcp, 'music_add_to_playlist',
                                                  {'songId': args.song,
                                                   'playlistId': args.playlist,
                                                   'operationId': 'm4b-check-1'})
                    step('add-to-own-playlist', not error and
                         payload.get('status') in ('added', 'already_present'),
                         json.dumps(payload, ensure_ascii=False)[:200])
                else:
                    step('add-to-own-playlist', True,
                         'skipped: pass --playlist <your test playlist id> to run the real write')
                    report['steps'][-1]['skipped'] = True
            finally:
                if mcp:
                    stderr, raw = mcp.close()
                    report['mcp'] = {'stderrEmpty': not stderr.strip(),
                                     'stdoutLines': len(raw),
                                     'allJsonRpc': all(json.loads(line).get('jsonrpc') == '2.0'
                                                       for line in raw if line.strip())}
                if tui.poll() is None:
                    tui.terminate()
                tui.wait(timeout=20)
                report['tuiExitCode'] = tui.returncode
    report['verdict'] = 'passed' if not report['failures'] else 'failed'
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'verdict': report['verdict'], 'failures': report['failures']},
                     ensure_ascii=True))
    return 0 if report['verdict'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
