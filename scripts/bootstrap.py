"""Download pinned build dependencies into this project only."""
import hashlib
import io
import json
from pathlib import Path
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
FILES = {
    'json.hpp': 'https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp',
    'json.LICENSE': 'https://raw.githubusercontent.com/nlohmann/json/v3.11.3/LICENSE.MIT',
    'qrcodegen.hpp': 'https://raw.githubusercontent.com/nayuki/QR-Code-generator/v1.8.0/cpp/qrcodegen.hpp',
    'qrcodegen.cpp': 'https://raw.githubusercontent.com/nayuki/QR-Code-generator/v1.8.0/cpp/qrcodegen.cpp',
    'monocypher.h': 'https://raw.githubusercontent.com/LoupVaillant/Monocypher/4.0.2/src/monocypher.h',
    'monocypher.c': 'https://raw.githubusercontent.com/LoupVaillant/Monocypher/4.0.2/src/monocypher.c',
    'monocypher.LICENSE': 'https://raw.githubusercontent.com/LoupVaillant/Monocypher/4.0.2/LICENCE.md',
    'miniz_tinfl.c': 'https://raw.githubusercontent.com/richgel999/miniz/3.0.2/miniz_tinfl.c',
    'miniz.h': 'https://raw.githubusercontent.com/richgel999/miniz/3.0.2/miniz.h',
    'miniz_tdef.h': 'https://raw.githubusercontent.com/richgel999/miniz/3.0.2/miniz_tdef.h',
    'miniz_zip.h': 'https://raw.githubusercontent.com/richgel999/miniz/3.0.2/miniz_zip.h',
    'miniz_tinfl.h': 'https://raw.githubusercontent.com/richgel999/miniz/3.0.2/miniz_tinfl.h',
    'miniz_common.h': 'https://raw.githubusercontent.com/richgel999/miniz/3.0.2/miniz_common.h',
    'miniz.LICENSE': 'https://raw.githubusercontent.com/richgel999/miniz/3.0.2/LICENSE',
}

def download(url):
    print('Downloading', url, flush=True)
    with urllib.request.urlopen(url, timeout=120) as response:
        return response.read()

manifest = {}
(ROOT / 'vendor' / 'miniz_export.h').write_text('#define MINIZ_EXPORT\n', encoding='ascii')
for name, url in FILES.items():
    target = ROOT / 'vendor' / name
    if not target.exists():
        target.write_bytes(download(url))
    manifest[name] = {'url': url, 'sha256': hashlib.sha256(target.read_bytes()).hexdigest()}
(ROOT / 'vendor' / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
zig = ROOT / 'tools' / 'zig-windows-x86_64-0.14.1'
if not (zig / 'zig.exe').exists():
    payload = download('https://ziglang.org/download/0.14.1/zig-x86_64-windows-0.14.1.zip')
    # Release checksum is checked against the official release index before extraction.
    index = json.loads(download('https://ziglang.org/download/index.json'))
    expected = index['0.14.1']['x86_64-windows']['shasum']
    if hashlib.sha256(payload).hexdigest() != expected:
        raise RuntimeError('Zig archive checksum mismatch')
    with zipfile.ZipFile(io.BytesIO(payload)) as archive:
        for item in archive.infolist():
            dest = (ROOT / 'tools' / item.filename).resolve()
            if not dest.is_relative_to((ROOT / 'tools').resolve()):
                raise RuntimeError('Invalid archive path')
        archive.extractall(ROOT / 'tools')
    extracted = ROOT / 'tools' / 'zig-x86_64-windows-0.14.1'
    extracted.rename(zig)
print('Dependencies ready.', flush=True)
