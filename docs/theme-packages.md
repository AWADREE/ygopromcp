# Theme packages

The client offers Classic and Gilded Duel Terminal in Settings. Classic uses
the original Irrlicht skin. Selecting a missing or invalid Gilded package
falls back to Classic without changing cards, decks or gameplay. The selected
theme is saved in `system.conf` as `theme = classic` or `theme = gilded`.

Theme packages are UTF-8 JSON data, not scripts. A manifest must provide its
schema version, ID, name, complete Irrlicht color palette and asset map.
Colors use `#AARRGGBB`. Optional `pile_count_text`, `ui` and `spacing` values
style existing controls and hover-only pile counts. Unknown keys, invalid
colors, arbitrary paths and partial palettes are rejected. Asset paths stay
under that package's `assets/` directory; the loader checks the resolved
filesystem target and decodes bounded PNGs before making the theme available.

The bundled Gilded package contains original backdrop and logo images. Its
backdrop is 1920 by 1080 and is rendered with its aspect ratio preserved.
The logo uses a fixed viewport header anchor. No reference screenshot is
distributed. Layout and color changes leave card, hand, field and pile
geometry and game/network behavior unchanged. The shared controls are
described in [Gilded controls](gilded-control-system.md).
