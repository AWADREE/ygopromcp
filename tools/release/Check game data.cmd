@echo off
rem Optional manual diagnostic. The native launcher handles first-run setup.
rem Send all diagnostics to stderr; MCP stdout belongs to JSON-RPC only.
if not exist "%~dp0cards.cdb" goto missing
if not exist "%~dp0strings.conf" goto missing
if not exist "%~dp0script\constant.lua" goto missing
for %%F in (act.png attack.png bg.jpg bg_deck.jpg bg_menu.jpg chain.png chaintarget.png equip.png f1.jpg f2.jpg f3.jpg field.png field2.png field3.png field-transparent2.png field-transparent3.png lim.png lp.png lpf.png mask.png negated.png number.png ot.png target.png unknown.jpg) do if not exist "%~dp0textures\%%F" goto missing
if not exist "%~dp0textures\cover.png" if not exist "%~dp0textures\cover.jpg" goto missing
if not exist "%~dp0textures\cover2.png" if not exist "%~dp0textures\cover2.jpg" goto missing
exit /b 0
:missing
>&2 echo YGOPro game data is incomplete. Run Play YGOPro.cmd for native first-run setup.
exit /b 1
