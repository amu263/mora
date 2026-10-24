# Third-party notices

MORA implements the endpoint contracts and encryption described by
[NeteaseCloudMusicApiEnhanced/api-enhanced](https://github.com/NeteaseCloudMusicApiEnhanced/api-enhanced).
Protocol behavior was studied from `util/crypto.js`, `util/request.js`,
`module/register_xeapikey.js`, and the search, player, account and playlist modules.
The upstream MIT license (copyright 2013–2022 Binaryify) is reproduced in
`docs/upstream-LICENSE` in source and `licenses/api-enhanced.LICENSE` in releases.
MORA is an independent third-party client and is not affiliated with NetEase.

Vendored components:

| Component | Version | License | Purpose |
| --- | --- | --- | --- |
| nlohmann/json | 3.11.3 | MIT | Bounded API JSON parsing |
| Nayuki QR Code generator | 1.8.0 | MIT, embedded in source | Local QR encoding |
| Monocypher | 4.0.2 | BSD-2-Clause OR CC0-1.0 | X25519 key exchange; BSD option used |
| miniz | 3.0.2 | MIT | Bounded gzip/deflate response decoding |

Full license text accompanies each component. `vendor/manifest.json` records
the pinned source URLs and SHA-256 hashes. The `miniz_export.h` build stub is generated locally.
Zig 0.14.1 is a build tool; its installed distribution contains its own licenses.
No Zig installation is needed to run the resulting executable. Windows system
components (WinHTTP, CNG, DPAPI, WIC, Media Foundation and waveOut) are used under
the operating system's licenses.

The Amu homepage portrait was generated from the user's latest supplied reference,
then refined with the user-requested [perfectPixel](https://github.com/theamusing/perfectPixel)
NumPy backend at commit `72096de5cf1bff9102687c4f89af6f62c4273a86`. The tool is used only
while preparing the asset; its source and Python runtime are not bundled with the
player. Asset provenance, processing parameters and the generation prompt are in
`assets/amu/`. The previous placeholder mascot and emote references are not used.
