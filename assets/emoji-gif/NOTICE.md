# Animated Noto emoji (vendored)

These GIFs came from the `78/xiaozhi-fonts` ESP Component Registry package,
version 1.6.0 (repository https://github.com/78/noto-fonts,
commit `d45dbc64052d57048f20ab1770074172ce9eb53b`), where they lived under
`gif/noto-emoji_*`. Licensed Apache-2.0.

Version 2.0.0 of that package dropped the `gif/` directory and now ships only
static PNG color emoji (`png/noto-color-emoji_*`). They're vendored here
because the display code (`main/display/lcd_display.cc`) already knows how to
animate a GIF-backed emoji via `LvglGif` — only the asset source was missing.
See `scripts/build_default_assets.py`'s `get_emoji_collection_path()` for how
a board picks these up via `DEFAULT_EMOJI_COLLECTION`.
