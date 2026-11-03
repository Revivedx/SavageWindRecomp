# Colour LUTs (CC0)

The LUTs Savage Wind ships for Options > Colour > LUT (`runtime/color_grade.h`). Each comes from
[FreshLUTs](https://freshluts.com), where its uploader released it under
[CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/) ("free for commercial use, no attribution required").
The files are unchanged; only their names are Savage Wind's own. The game shows LUTs by file name, underscores as
spaces. They ship next to the executable as `colour-luts/` (players' own LUTs go in `luts/` in the data folder).

| File | FreshLUTs page | Name there | Uploaded by | Size |
|---|---|---|---|---|
| `Savage_Wind.cube` | <https://freshluts.com/luts/219> | CINEMA 3 | Andy | 16³ |
| `Dormant.cube` | <https://freshluts.com/luts/217> | CARIBE | Andy | 16³ |
| `Ember.cube` | <https://freshluts.com/luts/107> | powerful cold | sagi hershkovich | 33³ |

All three are made for Rec. 709 input, which is what BOTW's picture is. They were picked by eye on the Great Plateau
(day and evening) from six CC0 candidates. Savage Wind is the recommended look: the first setup offers it (the start
screen's "Savage Wind colour look", on by default, sets it at 50% intensity), and the LUT list shows it first. At full
intensity it draws a hard-edged arc in the bright daytime sky around the sun (in the LUT's own data, at 16³); 50%
softens the arc, and it looks best in the evening. Dormant is warm and golden with teal shadows, strong by day (about
70% intensity suits it). Ember is a little deeper and cooler. (Until 2026-10-28 Savage Wind and Ember had each
other's names.) Left out: ROYALS (/luts/596: the daytime sky blows out to white), Cinematic/Forest
(/luts/611) and KCTRK_222 (/luts/2705: strong cyan / yellow casts by day); and, before any test, LUTs whose uploader
is not their author, that carry a brand name, or that expect camera log input.
