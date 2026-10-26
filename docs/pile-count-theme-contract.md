# Pile count display and theme color

During a duel, Deck, Graveyard, banished, and Extra Deck counts are drawn over the corresponding pile only while that pile is the field hover target. The label uses the existing `ClientField::GetHoverField` result for either player; empty piles show `0`. A nonempty Extra Deck keeps its total and face-up subcount, formatted `total (face-up)`.

`Game::DrawMisc` reads `themeManager.GetPileCountTextColor()` for the text and `EGDC_3D_DARK_SHADOW` from the active Irrlicht skin for its shadow. The accessor supplies the package's optional `pile_count_text` token and restores opaque yellow for Classic or packages without it. Hover and positioning logic remain base-game behavior; the adapter owns color selection and malformed-token fallback.

The label center is projected from the actual pile zone's world-space center and stack height through the active camera. It follows camera, window-size, and aspect changes without screen-specific coordinates.
