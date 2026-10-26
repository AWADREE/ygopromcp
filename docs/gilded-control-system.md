# Gilded controls

Gilded Duel Terminal styles the game's existing Irrlicht controls through a
shared skin. Buttons, edit fields, combo boxes, lists, tabs, menus, toolbars,
scrollbars and dialogs use flat dark surfaces, rounded corners, gold framing
and cyan focus. The controls retain their native hit boxes, keyboard behavior,
caret, selection and password masking. Disabled controls and error dialogs
have distinct colors. Classic restores the original Irrlicht skin.

The Settings theme selector loads only validated, data-only packages. Missing
or invalid Gilded data returns to Classic. The original Gilded logo is anchored
to the viewport header; opening Home, LAN, Replay or Settings does not move it.
Theme layout changes leave card placement, picking and gameplay rules intact.

The package format, asset rules and fallback behavior are described in
[theme packages](theme-packages.md). This guide describes the implementation;
it is not a claim that every control state, window size or input device has
been tested in a final packaged build.
