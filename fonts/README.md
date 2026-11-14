# System fonts

The game draws its text with the Switch's built-in system font, which isn't part of the game files.
Without it, Savage Wind uses Hiragino Sans (built into macOS, the same style of Japanese gothic as the Switch's
Morisawa UD Shin Go), then Arial.

For the exact look, put a dump from your own console here (Switch system archive *FontStandard*, title
0100000000000811, and *FontNintendoExtension*, 0100000000000810):

- `nintendo_udsg-r_std_003.bfttf`: the main font (a `.ttf` with the same name, or spelled `nintendo_udsgr_std_003`, also works)
- `nintendo_ext_003.bfttf`: button glyphs (optional)

`SAVAGEWIND_FONT=<file>` overrides everything (.ttf / .otf / .ttc / .bfttf).

## Chinese and Korean text

The Standard font above is a Japanese gothic (or Arial): it has no Simplified/Traditional Chinese hanzi or Korean
hangul, so a Chinese or Korean text language shows Japanese glyphs, blank boxes or `?`. The game asks for those
through separate shared fonts (*ChineseSimple*, *ChineseSimpleExt*, *ChineseTraditional*, *Korean*), and Savage Wind
fills them from a CJK font when it finds one:

1. `SAVAGEWIND_CJK_FONT=<file>` (any of .ttf / .ttc face 0 / .bfttf), or
2. a CJK font installed on the system, in which case nothing has to be set. Chinese: Microsoft YaHei / SimSun
   (Windows), PingFang (macOS), WenQuanYi Micro Hei / Zen Hei (Linux). Korean: Malgun Gothic (Windows),
   Apple SD Gothic Neo (macOS), WenQuanYi (Linux). Microsoft YaHei has no hangul, so Korean is looked up apart from
   Chinese.

The game's own font engine only reads TrueType (`glyf`) outlines: it crashes on CFF fonts, which Noto Sans CJK
(`.otf` / most `.ttc`) are. Use a TrueType CJK font (WenQuanYi, SimSun, Microsoft YaHei, PingFang are TrueType).

The menu overlay and the start screen (Dear ImGui) use their own font path: there, `SAVAGEWIND_CJK_FONT` is also the
override, otherwise a font in the data folder's `fonts/` (e.g. `NotoSansCJK-Regular.ttc`, `wqy-microhei.ttc`), otherwise
a system CJK font. ImGui reads CFF too, so Noto Sans CJK is fine there.
