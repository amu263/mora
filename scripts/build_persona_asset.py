"""Convert one persona portrait into the project's 48x50 indexed pixel asset.

The source is a normal (non-pixel) anime portrait. The conversion keeps the framing used by the
existing Amu asset: a head close-up that fills the frame, a transparent background and a small
indexed palette embedded in C++ as `namespace mora::<name>_asset`.

Run: python scripts/build_persona_asset.py --source build/persona-raw/fox.png --name fox
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter

ROOT = Path(__file__).resolve().parents[1]

parser = argparse.ArgumentParser()
parser.add_argument('--source', required=True)
parser.add_argument('--name', required=True, help='asset namespace suffix, e.g. fox')
parser.add_argument('--size', type=int, default=48)
parser.add_argument('--height', type=int, default=50,
                    help='avatar box height; transparent rows are appended to reach it')
parser.add_argument('--colors', type=int, default=32)
parser.add_argument('--crop-top', type=float, default=0.0,
                    help='fraction of image height skipped above the crop')
parser.add_argument('--crop-height', type=float, default=0.72,
                    help='fraction of image height kept as the head close-up')
parser.add_argument('--crop-center-x', type=float, default=0.5,
                    help='horizontal centre of the square crop, as a fraction of image width')
parser.add_argument('--keep-background', action='store_true',
                    help='debug: skip the transparent background pass')
parser.add_argument('--background-tolerance', type=float, default=48.0)
parser.add_argument('--saturation', type=float, default=1.06)
parser.add_argument('--contrast', type=float, default=1.10)
parser.add_argument('--gamma', type=float, default=1.0,
                    help='below 1 deepens the midtones so features read at 48 px')
parser.add_argument('--sharpen-percent', type=int, default=40,
                    help='unsharp mask amount applied after the downscale')
parser.add_argument('--auto-levels', dest='auto_levels', action='store_true', default=True,
                    help='stretch the character luminance range (default on)')
parser.add_argument('--no-auto-levels', dest='auto_levels', action='store_false')
parser.add_argument('--out-dir', default='')
args = parser.parse_args()

if not 32 <= args.size <= 64 or not 16 <= args.colors <= 64:
    parser.error('size must be 32..64 and colors 16..64')

source = Path(args.source).resolve()
name = args.name
asset_dir = Path(args.out_dir).resolve() if args.out_dir else ROOT / 'assets' / 'personas' / name
asset_dir.mkdir(parents=True, exist_ok=True)
image = Image.open(source).convert('RGB')
rgb = np.asarray(image).astype(np.float32)
height, width = rgb.shape[:2]

# The generator puts the subject on a flat background. Sample the border ring to learn it.
ring = np.concatenate([
    rgb[0:2].reshape(-1, 3), rgb[-2:].reshape(-1, 3),
    rgb[:, 0:2].reshape(-1, 3), rgb[:, -2:].reshape(-1, 3),
])
background = np.median(ring, axis=0)
distance = np.linalg.norm(rgb - background, axis=2)
subject = distance > args.background_tolerance
rows = np.where(subject.any(axis=1))[0]
cols = np.where(subject.any(axis=0))[0]
if rows.size == 0 or cols.size == 0:
    raise SystemExit('no subject found above the background threshold')
top, bottom = int(rows[0]), int(rows[-1])
left, right = int(cols[0]), int(cols[-1])

# The generated portraits share one composition (head and shoulders, centred), so the avatar
# framing is a fixed fraction of the frame rather than of a bounding box: the shoulders and the
# props in the lower third would otherwise drag the crop down off the face.
side = max(16, int(height * args.crop_height))
centre_x = int(width * args.crop_center_x)
crop_top = int(height * args.crop_top)
crop_left = centre_x - side // 2

# Pad when the square runs past an edge so the subject is never clipped by the crop.
padded = np.full((side * 3, side * 3, 3), background, dtype=np.float32)
offset_x = side - crop_left
offset_y = side - crop_top
y0, y1 = max(0, offset_y), min(padded.shape[0], offset_y + height)
x0, x1 = max(0, offset_x), min(padded.shape[1], offset_x + width)
padded[y0:y1, x0:x1] = rgb[y0 - offset_y:y1 - offset_y, x0 - offset_x:x1 - offset_x]
crop = padded[side:side + side, side:side + side]

# Cut the background at full resolution and flood fill from the crop border only, so a light
# highlight inside the hair is never punched out. Doing this before the downscale is what keeps
# the avatar free of the pale halo that mixing hair with a cream background would produce.
crop_distance = np.linalg.norm(crop - background, axis=2)
background_mask = crop_distance <= args.background_tolerance
label = np.zeros(background_mask.shape, dtype=np.uint8)
label[0, :] |= background_mask[0, :]
label[-1, :] |= background_mask[-1, :]
label[:, 0] |= background_mask[:, 0]
label[:, -1] |= background_mask[:, -1]
while True:
    grown = label.copy()
    grown[1:, :] |= label[:-1, :]
    grown[:-1, :] |= label[1:, :]
    grown[:, 1:] |= label[:, :-1]
    grown[:, :-1] |= label[:, 1:]
    grown &= background_mask
    if np.array_equal(grown, label):
        break
    label = grown
if args.keep_background:
    alpha_full = np.full(label.shape, 255.0, dtype=np.float32)
else:
    alpha_full = np.where(label > 0, 0.0, 255.0).astype(np.float32)

# Downscaling straight RGB would let the cream background bleed into the hair, and
# unpremultiplying a half-covered edge pixel amplifies its colour into a bright fringe. So the
# subject's colours are first spread into the background region, and then both the colour and the
# alpha are averaged with a plain box filter. The final alpha is binary, so the spread only has to
# keep the half-covered edge pixels plausible.
filled = crop.copy()
opaque = alpha_full > 0
for _ in range(8):
    if opaque.all():
        break
    totals = np.zeros_like(filled)
    weights = np.zeros(filled.shape[:2], dtype=np.float32)
    for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        totals += np.roll(filled, (dy, dx), axis=(0, 1)) * np.roll(opaque, (dy, dx),
                                                                   axis=(0, 1))[:, :, None]
        weights += np.roll(opaque, (dy, dx), axis=(0, 1))
    growing = (~opaque) & (weights > 0)
    filled[growing] = totals[growing] / weights[growing][:, None]
    opaque = opaque | growing

small = np.asarray(Image.fromarray(filled.astype(np.uint8)).resize(
    (args.size, args.size), Image.BOX)).astype(np.float32)
small_alpha = np.asarray(Image.fromarray(alpha_full.astype(np.uint8)).resize(
    (args.size, args.size), Image.BOX)).astype(np.float32)

# Legibility at 48 px: more colour, deeper midtones and a light unsharp pass after the
# downscale are what keep the eyes and jaw readable at avatar size.
grey = small.mean(axis=2, keepdims=True)
small = np.clip((grey + (small - grey) * args.saturation - 128.0) * args.contrast + 128.0,
                0, 255)
small = np.clip(255.0 * np.power(small / 255.0, args.gamma), 0, 255)
# A high-key source (white hair, pale skin) and a low-key one need different amounts of pull, so
# the opaque range is stretched to the full scale instead of relying on one fixed contrast.
if args.auto_levels:
    opaque = small_alpha >= 128.0
    if opaque.any():
        luminance = small[opaque].mean(axis=1)
        low, high = np.percentile(luminance, 2), np.percentile(luminance, 98)
        if high - low > 8:
            small = np.clip((small - low) * (255.0 / (high - low)), 0, 255)
if args.sharpen_percent > 0:
    sharpened = Image.fromarray(small.astype(np.uint8)).filter(
        ImageFilter.UnsharpMask(radius=1.1, percent=args.sharpen_percent, threshold=0))
    small = np.asarray(sharpened).astype(np.float32)
alpha = np.where(small_alpha >= 128.0, 255, 0).astype(np.uint8)

rgba = Image.fromarray(np.dstack((small.astype(np.uint8), alpha)), 'RGBA')
quantized = rgba.quantize(colors=args.colors, method=Image.Quantize.FASTOCTREE,
                          dither=Image.Dither.NONE).convert('RGBA')
# Every avatar shares one terminal box so the homepage never has to special-case a persona.
if quantized.size[1] < args.height:
    padded_image = Image.new('RGBA', (quantized.size[0], args.height), (0, 0, 0, 0))
    padded_image.paste(quantized, (0, 0))
    quantized = padded_image
elif quantized.size[1] % 2:
    padded_image = Image.new('RGBA', (quantized.size[0], quantized.size[1] + 1), (0, 0, 0, 0))
    padded_image.paste(quantized, (0, 0))
    quantized = padded_image
width_out, height_out = quantized.size

pixels = [tuple(map(int, p)) for p in np.asarray(quantized).reshape(-1, 4)]
palette = sorted({p[:3] for p in pixels if p[3] >= 128})
lookup = {color: index for index, color in enumerate(palette)}
indices = [lookup[p[:3]] if p[3] >= 128 else 255 for p in pixels]
if not any(index == 255 for index in indices):
    raise SystemExit('persona asset must keep a transparent background')
if len(palette) > 64 or width_out > 64 or height_out > 64:
    raise SystemExit('persona asset exceeds the embedded budget')

quantized.save(asset_dir / f'{name}-pixel.png')
preview = quantized.resize((width_out * 12, height_out * 12), Image.NEAREST)
preview.save(asset_dir / f'{name}-pixel-preview.png')

header = [
    f'// Generated by scripts/build_persona_asset.py from {source.name}. Do not hand-edit.',
    '#pragma once',
    '#include <array>',
    '#include <cstdint>',
    f'namespace mora::{name}_asset {{',
    f'inline constexpr int width={width_out}, height={height_out};',
    f'inline constexpr std::array<std::array<uint8_t,3>,{len(palette)}> palette = {{{{',
]
for color in palette:
    header.append(f'    {{{color[0]},{color[1]},{color[2]}}},')
header.append('}};')
header.append(f'inline constexpr std::array<uint8_t,{width_out * height_out}> pixels = {{{{')
for row in range(height_out):
    chunk = indices[row * width_out:(row + 1) * width_out]
    header.append('    ' + ','.join(str(v) for v in chunk) + ',')
header.append('}};')
header.append(f'}} // namespace mora::{name}_asset')
target = asset_dir / f'{name}_sprite.hpp'
target.write_text('\n'.join(header) + '\n', encoding='utf-8')

provenance = {
    'persona': name,
    'source': source.name,
    'sourceSha256': hashlib.sha256(source.read_bytes()).hexdigest(),
    'grid': [args.size, args.size],
    'crop': {
        'subjectBox': [left, top, right, bottom],
        'cropTopFraction': args.crop_top,
        'cropHeightFraction': args.crop_height,
        'cropCenterXFraction': args.crop_center_x,
        'squareSide': side,
        'cropTopPixels': crop_top,
        'cropLeftPixels': crop_left,
    },
    'background': {'sampledRgb': [int(v) for v in background],
                   'tolerance': args.background_tolerance,
                   'alphaRule': 'flood fill from the crop border only'},
    'postprocess': {'paletteColors': len(palette), 'dither': False,
                    'saturation': args.saturation, 'contrast': args.contrast,
                    'gamma': args.gamma, 'sharpenPercent': args.sharpen_percent,
                    'autoLevels': args.auto_levels},
    'output': {'width': width_out, 'height': height_out,
               'embeddedPixelsBytes': width_out * height_out,
               'paletteBytes': len(palette) * 3},
    'sha256': {
        f'{name}-pixel.png': hashlib.sha256((asset_dir / f'{name}-pixel.png').read_bytes()).hexdigest(),
        f'{name}_sprite.hpp': hashlib.sha256(target.read_bytes()).hexdigest(),
    },
}
(asset_dir / 'provenance.json').write_text(
    json.dumps(provenance, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print(json.dumps({'name': name, 'size': [width_out, height_out], 'colors': len(palette),
                  'subjectBox': [left, top, right, bottom], 'assetDir': str(asset_dir)},
                 ensure_ascii=False))
