# Windows release packaging

The Windows GNU/UCRT64 package is assembled from a tested game executable and
its matching `YGOProUpdateLauncher.exe`. The launcher is the player entry
point. It handles native first-run setup, validated card-data updates,
recovery and startup health confirmation; players do not run updater scripts.
The package includes the original Gilded theme, 27 reviewed functional
textures, two licensed fonts, their notice, and required DLLs. It excludes personal decks,
settings, replays, card artwork, card database and effect scripts. The launcher
offers a pinned compatible base-data download on first run.

Release preparation uses the scripts under `tools/release/`:

1. Build both executables from the same reviewed source commit using the
   pinned GNU/UCRT64 toolchain and save the complete build log. For the audio
   profile, retain the exact miniaudio, Ogg, Opus, opusfile and Vorbis inputs.
2. Run `New-WindowsBuildReceipt.ps1` with both executables. Its receipt pins
   hashes and sizes; it does not prove source provenance by itself.
3. Run `New-WindowsPackage.ps1` with that receipt, the matching UCRT64 DLL,
   `objdump` and license directories, and a new output directory. It scans
   transitive imports from **both** executables. The fallback source manifest
   and every one of its 30 assets are mandatory.
4. Run `Test-WindowsPayload.ps1` and the package checks against the extracted
   directory and ZIP. Then test the packaged launcher, stdio and localhost
   MCP, first-run setup, update, Restore, audio and visible UI in a disposable
   runtime. Do not use a player's settings or data as a test fixture.
5. Create and verify the matching [corresponding-source bundle](windows-source-bundle.md)
   and review distribution rights before publication.

`MANIFEST.json` records the source revision, executable and DLL hashes,
licenses, audio profile and asset inventory. The release tools do not publish
or upload a package. The reviewed full-audio Windows candidate includes the
approved app icon;
this document is a build procedure, not a release announcement. The theme
package format is in [theme packages](theme-packages.md), and MCP connection
details are in [local MCP setup](mcp-local-http.md).
