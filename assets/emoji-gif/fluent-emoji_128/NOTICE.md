# Animated Fluent emoji (vendored)

Source: `microsoft/fluentui-emoji-animated` (https://github.com/microsoft/fluentui-emoji-animated),
`assets/<Emoji Name>/animated/*.png`. Licensed MIT.

Upstream ships each animation as a 256x256 animated PNG (APNG) stored via Git
LFS, roughly 1-3MB apiece — too large and the wrong format for this project's
GIF-based emoji pipeline (`LcdDisplay`/`LvglGif`, magic-byte `GIF` detection).
Each file here was converted with ffmpeg:

```
ffmpeg -i <name>_animated.png -filter_complex \
  "[0:v] fps=12,scale=128:128:flags=lanczos,split [a][b]; \
   [a] palettegen=max_colors=128:stats_mode=diff:reserve_transparent=1 [p]; \
   [b][p] paletteuse=dither=none:alpha_threshold=200" \
  -loop 0 <name>.gif
```

(128x128, 12fps, 128-color adaptive palette — brings the set from ~55MB of
source APNGs down to ~3.0MB of GIFs.)

GIF transparency is strictly binary (a pixel is either fully opaque or fully
transparent — no partial blending), so the source's anti-aliased edges have
to be hard-thresholded. An early version of this conversion used Bayer
dithering with `alpha_threshold=128` (ffmpeg's default), which left a visible
light fringe/halo around each character on dark backgrounds (fine on white,
noticeable on black). Raising `alpha_threshold` to 200 snaps ambiguous
semi-transparent edge pixels to fully transparent instead of fully opaque,
which removes the fringe; dropping dithering (`dither=none`) removed a visible
noise pattern that was more obvious against black than white, and also
compressed better than the dithered version despite the higher color count.

## Name mapping

This project's emotion names don't match Fluent's emoji names 1:1; each file
here was picked as the closest semantic match:

| this project | Fluent Emoji source          |
|---------------|-------------------------------|
| angry         | Angry face                    |
| confident     | Smirking face                 |
| confused      | Confused face                 |
| cool          | Smiling face with sunglasses  |
| crying        | Loudly crying face            |
| delicious     | Face savoring food            |
| embarrassed   | Flushed face                  |
| funny         | Grinning squinting face       |
| happy         | Beaming face with smiling eyes|
| kissy         | Kissing face with smiling eyes|
| laughing      | Face with tears of joy        |
| loving        | Smiling face with heart-eyes  |
| neutral       | Face without mouth             |
| relaxed       | Relieved face                 |
| sad           | Frowning face                 |
| shocked       | Astonished face               |
| silly         | Zany face                     |
| sleepy        | Sleepy face                   |
| surprised     | Face with open mouth          |
| thinking      | Thinking face                 |
| winking       | Winking face                  |
