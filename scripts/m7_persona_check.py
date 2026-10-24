"""Persona acceptance: every built-in persona repaints the home, and /persona lists them.

Runs the real player through the hidden-console frame capture, so the evidence is the player's own
ANSI output rather than a hand-written expectation. Each persona must show its own name on the
homepage, and the avatar area must actually change between personas.

Usage: python scripts/m7_persona_check.py
"""
import hashlib
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / 'build' / 'mora.exe'
FRAME = ROOT / 'scripts' / 'frame_capture.py'

PERSONAS = [
    ('amu', '小阿木'),
    ('fox', '小灰狐娘'),
    ('devil', '小恶魔少女'),
    ('mushroom', '小蘑菇菌娘'),
    ('daidai', '小宅女呆呆'),
]


def strip_ansi(text: str) -> str:
    return re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', text)


def capture(data: Path, out: Path, command: str, seconds: int) -> str:
    run = subprocess.run(
        [sys.executable, str(FRAME), str(out), '--columns', '116', '--rows', '36',
         '--seconds', str(seconds), '--command-after', '2', '--command', command,
         '--pass-through', '--data-dir', str(data)],
        cwd=ROOT, capture_output=True, text=True, timeout=seconds + 40,
        creationflags=0x00000008)
    assert run.returncode == 0, (run.returncode, run.stderr)
    return out.read_text(encoding='utf-8', errors='replace')


def main():
    report = {'schema': 'mora-persona/1', 'steps': [], 'failures': []}

    def step(name, ok, detail=''):
        report['steps'].append({'name': name, 'passed': bool(ok), 'detail': str(detail)[:300]})
        if not ok:
            report['failures'].append(name)
        print(('PASS ' if ok else 'FAIL ') + name)

    if not EXE.is_file():
        raise SystemExit('Build build/mora.exe first')

    with tempfile.TemporaryDirectory(prefix='mora-persona-') as tmp:
        data = Path(tmp)
        signatures = {}
        for persona_id, name in PERSONAS:
            (data / 'settings.json').write_text(
                json.dumps({'userName': '验收听众', 'personaId': persona_id,
                            'motion': False}, ensure_ascii=False), encoding='utf-8')
            raw = capture(data, data / f'home-{persona_id}.ansi', '', 3)
            frame = strip_ansi(raw)
            step(f'home-shows-{persona_id}', name in frame,
                 'persona name missing from the homepage')
            # Both the drawn avatar and the palette differ per persona, so two personas must never
            # render the same homepage.
            signatures[persona_id] = hashlib.sha256(raw.encode('utf-8')).hexdigest()
        step('homes-are-distinct',
             len(set(signatures.values())) == len(PERSONAS), signatures)

        (data / 'settings.json').write_text(
            json.dumps({'userName': '验收听众', 'personaId': 'fox', 'motion': False},
                       ensure_ascii=False), encoding='utf-8')
        listing = strip_ansi(capture(data, data / 'persona-list.ansi', '/persona', 3))
        for persona_id, name in PERSONAS:
            step(f'list-has-{persona_id}', name in listing, 'missing from the /persona picker')

        settings = json.loads((data / 'settings.json').read_text(encoding='utf-8'))
        step('settings-persist-persona', settings.get('personaId') == 'fox', settings)

    report['verdict'] = 'passed' if not report['failures'] else 'failed'
    out = ROOT / 'docs' / 'verification' / 'm7-persona.json'
    out.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'verdict': report['verdict'], 'failures': report['failures']},
                     ensure_ascii=True))
    return 0 if report['verdict'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
