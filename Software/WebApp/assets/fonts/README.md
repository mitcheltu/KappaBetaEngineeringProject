# Locally bundled fonts

Downloaded on October 30, 2026 from the official [Google Fonts repository](https://github.com/google/fonts). The original TTF files and their complete license notices are bundled without modification. No external font-service request is needed at runtime.

| Font family | Style / CSS weight | File | Source | License |
| --- | --- | --- | --- | --- |
| Barlow Condensed | Bold / `700` | [BarlowCondensed-Bold.ttf](BarlowCondensed-Bold.ttf) | [Official Google Fonts file](https://github.com/google/fonts/blob/main/ofl/barlowcondensed/BarlowCondensed-Bold.ttf) | [SIL Open Font License 1.1](BarlowCondensed-OFL.txt) |
| Space Mono | Regular / `400` | [SpaceMono-Regular.ttf](SpaceMono-Regular.ttf) | [Official Google Fonts file](https://github.com/google/fonts/blob/main/ofl/spacemono/SpaceMono-Regular.ttf) | [SIL Open Font License 1.1](SpaceMono-OFL.txt) |

The accompanying license files were downloaded from each family's official repository directory: [Barlow Condensed OFL](https://github.com/google/fonts/blob/main/ofl/barlowcondensed/OFL.txt) and [Space Mono OFL](https://github.com/google/fonts/blob/main/ofl/spacemono/OFL.txt).

[`public/fonts/fonts.css`](../../public/fonts/fonts.css) declares the exact CSS family names `'Barlow Condensed'` and `'Space Mono'`, with `font-display: swap`. The web entry point imports `src/fonts.web.ts`, which adds a stylesheet link prefixed by Expo's build-time `EXPO_BASE_URL`. Barlow Condensed is used for display headings, and Space Mono for numbers and technical labels. Only the weights listed above are bundled.

The originals and licenses remain in this directory. Identical copies of both TTF files and both OFL notices are also bundled in [`public/fonts/`](../../public/fonts/) for Expo's static serving and production export. Relative URLs such as `./BarlowCondensed-Bold.ttf` resolve beside the stylesheet, including when the app is hosted under `/KappaBetaEngineeringProject/` on GitHub Pages. Keep these public copies and their notices synchronized if the fonts are updated.

The fonts' internal family/style metadata was inspected after downloading and matches these declarations.

## File integrity

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| `BarlowCondensed-Bold.ttf` | 109,912 | `e476562ec9c1e16cf16475895b511f08c804f438cc9a9f80a44ea50a0eeb5b65` |
| `SpaceMono-Regular.ttf` | 99,356 | `95837e182baeeada83368f7748db28357f0a1b75c6b84ff7065b5edf933c8e18` |
