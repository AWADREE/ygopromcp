# Original release fallback assets

This directory contains a functional, neutral startup texture set for a public Windows build. The 27 raster images are original geometric graphics generated from `generate_fallback.py`; no inherited card art, personal cover, user deck, database, replay, or effect script is included. The repository's GPL-2.0 license applies to these original graphics and their generator. The two unmodified Noto Sans CJK SC font files carry their separate SIL OFL 1.1 license in `licenses/NotoSansCJK-OFL-1.1.txt`.

The source asset list and SHA-256 values are pinned in `SOURCE_MANIFEST.json`; the 27 generated image hashes are also in `MANIFEST.json` (SHA-256 `2E761AB68FBE092E7D059C9E550DD9752C3511B2B32017DFF679A3A97223A1F8`). Both covers are 590x860 PNG; every non-cover image preserves the expected loader dimensions. The original theme backdrop and logo are supplied separately by `themes/gilded`, not duplicated here.

## Reproduction inputs

Run `generate_fallback.py` with Python 3.14.9 from MSYS2 UCRT64 (zlib 1.3.2) and libjpeg-turbo `cjpeg` 3.2.0. Set `YGOPROMCP_CJPEG` to the trusted `cjpeg` executable, or put it on PATH. The reviewed cjpeg executable has SHA-256 `1293DE872C37E6D81DDB34FB0A7E973298AAD4876F1BE6E574BFB975AD7783CA`. Keep the UCRT64 runtime libraries on this process's PATH when using its Python. Running with a different Python/zlib can produce byte-different PNG compression with identical decoded pixels; compare both the generated `MANIFEST.json` and decoded image contents before replacing pinned release assets. `licenses/libjpeg-turbo-build-tool-LICENSE.md` records the build tool notice; `cjpeg` is not a player dependency.

The fonts are the unmodified Bold and Regular OTF files from official `notofonts/noto-cjk` commit `f8d157532fbfaeda587e826d4cd5b21a49186f7c`. `fonts/numFont.otf` is Bold and `fonts/textFont.otf` is Regular. Their file hashes and notices are in `SOURCE_MANIFEST.json`. Do not alter the internal font names without reviewing the OFL conditions.

## Tested scope and distribution

A disposable committed diagnostic game build rendered the PNG covers at full 590x860 GPU size with mipmaps and drew the neutral field/card placeholders at 1920x1080 and 1024x640; Modern synthetic picking/event checks passed in both frames. A separate CJK locale fixture rendered Chinese UI labels at 1024x640. These are bounded asset-loader tests, not final integrated release acceptance. The final package must still smoke-test its reviewed core head, first-run bootstrap, and theme. Exact frames and private test receipts stay outside the public source tree.

The package builder should copy only `textures/*`, `fonts/*`, the OFL notice, and any required package license. It should not copy source generator or build-tool notice into the player runtime unless intentionally publishing corresponding source alongside it. Validate each copied byte against `SOURCE_MANIFEST.json`. Do not copy the repository's inherited `textures/` directory into the public binary package.