# YGOPROMCP banner visual direction

This note records the original banner direction for possible future UI work.
It is a reference, not an implemented client theme or a license to reuse
third-party Yu-Gi-Oh! card art. The banner itself is
[`docs/assets/ygopromcp-banner.jpg`](assets/ygopromcp-banner.jpg).

## Palette

These colors are sampled from the original 2172 × 724 generated PNG, using sRGB pixel values. The committed JPEG is a quality-95, full-resolution export of that same approved image; compression can slightly change individual pixel values. They capture the approved visual direction, not yet an accessible production theme. Coordinates below are zero-based (x, y); check contrast and state differentiation before implementation.

| Role | Sampled color | Intended use |
|---|---|---|
| Near-black charcoal | `#07151C` | Quiet background |
| Deep blue charcoal | `#102A35` | Panels and shadowed depth |
| Electric cyan | `#34DBEC` | Primary circuit lines and focus glow |
| Muted teal | `#299FAB` | Secondary line work |
| Warm gold | `#E9B969` | Sparse highlights and card-edge accents |
| Soft ice | `#DDF7F8` | Readable text on dark surfaces |

Sample positions: charcoal (1859, 174), deep blue (1433, 91), cyan (1705, 499), teal (1497, 588), gold (1843, 196), ice (849, 337).

Suggested visual gradients: background `#07151C` → `#102A35`; cyan highlight `#299FAB` → `#34DBEC` → `#DDF7F8`; gold accents may fade from transparent into `#E9B969`. These gradient recipes are design suggestions, not extracted shader settings.

## Treatment for a future UI exploration

- Let the background fade from near-black charcoal to deep blue charcoal.
  Reserve brighter cyan for active boundaries, not every panel edge.
- Use thin, directional circuit-like lines with restrained bloom. Keep
  important text and card effects above the decorative line layer.
- Place warm gold in small focal accents; it should complement the cyan
  instead of competing with it across entire screens.
- Preserve card readability: full-card art, frame colors, English names and
  effect text must remain legible at normal gameplay sizes. Check contrast
  and color-blind readability in any actual UI prototype.
- Keep the banner's abstract card/circuit language separate from inherited
  YGOPro attribution. This note authorizes no UI implementation or public
  distribution of card assets.
