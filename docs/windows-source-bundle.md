# Windows corresponding source

A distributable GNU/UCRT64 binary needs a corresponding-source bundle tied
to the **same final build**. `New-WindowsSourceBundle.ps1` takes the reviewed
client commit, both executable hashes and build receipt, matching MyCard
ocgcore and miniaudio checkouts, pinned modified Irrlicht and Lua sources,
the four audio archives, and 17 MSYS2 UCRT64 source archives. It records
origin, byte count and SHA-256 for every copied input in
`SOURCE-MANIFEST.json`. `Test-WindowsSourceBundle.ps1` verifies the directory
and ZIP against that manifest.

The source set includes the client GPL license, ocgcore MIT license,
Irrlicht license, Lua and miniaudio notices, five static audio notices,
reviewed dependency notices and recipes for the bundled DLLs. The client
gitlink pins the MyCard core revision used by this candidate; a different
final binary or DLL set requires a fresh receipt and dependency review.

Build both executables from the final public-safe source commit, save the
compiler command and full build log, and verify their hashes before producing
the package and source bundle. Keep card database, effect scripts, personal
decks, settings and unlicensed card art out of both release archives. See
[Windows release packaging](windows-release-packaging.md) for the package
workflow. No source bundle is final until the full-audio build, asset/icon
decision and package tests pass.
