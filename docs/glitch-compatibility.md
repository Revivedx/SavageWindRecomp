# Glitch compatibility tracker

Use this checklist to record which Breath of the Wild glitches and speedrun techniques behave like the
Nintendo Switch **1.9.0** game in Savage Wind. Only include glitches with a documented working 1.9.0
baseline or a successful test using the supported 1.9.0 game in the recomp. `Untested` refers to the
recomp result, not whether the glitch exists in 1.9.0.

Savage Wind targets **Switch version 1.9.0**. Record the exact Switch version used as a baseline: an older
video or a Wii U result alone does not establish expected behavior on the supported version.

## Filling in the tables

- Replace `Untested` with `Works`, `Partial`, `Fails`, or `Retest` for each frame rate you test.
- `Works` means the expected effect was reproduced; `Partial` means it differs or only some variants work.
  `Fails` means the documented setup was attempted without the expected effect. `Retest` means an earlier
  result needs checking on the current build. A failed attempt alone does not establish a regression.
- In **Switch baseline**, link the working 1.9.0 reference. Recomp evidence alone must be labeled as
  such; it does not establish console parity. Do not add patched or unverified techniques to the tables.
- In **Build / report**, add the recomp commit or release and a link to a test record below, clip, or issue.
- Use **Notes** for the exact variant, limitations, and other frame rates (90, 120, uncapped, etc.).
  Add separate rows when variants or builds give different results.

Start at 30 FPS, then repeat the same setup at 60 FPS and any higher cap you use. Record the cap and actual
FPS when possible. Keep the location, equipment, input device, and setup consistent between runs.

## Movement, launches, and clipping

| Glitch / technique | Switch baseline | 30 FPS | 60 FPS | Build / report | Notes / other FPS |
|---|---|---|---|---|---|
| Windbomb (general test; variant not recorded) | [1.9.0 reference][baseline-190] | Works | Works | Built from `test` branch commit [`ac47f65e3f9ee05641fff35f8d683fd36c306513`](https://github.com/Revivedx/SavageWindRecomp/commit/ac47f65e3f9ee05641fff35f8d683fd36c306513); [video evidence](https://youtu.be/7JGTN7cCYoY) | **120 FPS: Works.** Tester reports that higher FPS makes execution harder because the trick is timing-sensitive; expected behavior, not an issue. |
| Bullet Time Bounce (BTB) | 1.9.0 recomp confirmed; console baseline not recorded | Works | Works | Built from `test` branch commit [`ac47f65e3f9ee05641fff35f8d683fd36c306513`](https://github.com/Revivedx/SavageWindRecomp/commit/ac47f65e3f9ee05641fff35f8d683fd36c306513); [video evidence](https://youtu.be/A6n5opLTVng) | **120 FPS: Works.** |
| Whistle sprint | [1.9.0 reference][baseline-190] | Untested | Untested | — | — |
| Skew storage / shield clipping | [1.9.0 reference][baseline-190] | Untested | Untested | — | Record wall and skew setup. |
| Extended shield clip (ESC) | [1.9.0 reference][baseline-190] | Untested | Untested | — | Record location and setup. |
| Moon jump | [1.9.0 reference][baseline-190] | Untested | Untested | — | Record mounted archery setup. |

## Inventory, equipment, and progression

| Glitch / technique | Switch baseline | 30 FPS | 60 FPS | Build / report | Notes / other FPS |
|---|---|---|---|---|---|
| Menu overload | [1.9.0 reference][baseline-190] | Untested | Untested | — | Record bows, arrows, and setup count. |
| Equipment duplication through overload | [1.9.0 reference][baseline-190] | Untested | Untested | — | Test weapon, bow, and shield separately. |
| Durability transfer | [1.9.0 reference][baseline-190] | Untested | Untested | — | Record source and target equipment. |
| Inventory Slot Transfer (IST) | [1.9.0 reference][baseline-190] | Untested | Untested | — | Record exact method and transferred items. |
| Heart / stamina duplication | [1.9.0 reference][baseline-190] | Untested | Untested | — | Record exact apparatus / essence method. |

## Inclusion references

[baseline-190]: https://questfern.com/botw/guides/glitches/

The [1.9.0 compatibility guide][baseline-190] reports the retained console-baseline techniques as working
on original Switch 1.9.0. Its Switch 2 Edition results are separate: plain shield clips differ, while the
extended method is reported working. Recomp frame-rate behavior still needs testing.

BLSS is excluded because it was patched in 1.8.0. Other entries without a specific working 1.9.0 reference
have been removed; add them back only with version-specific evidence. BTB is retained on the strength of
the contributor's successful 1.9.0 recomp tests linked in its row, rather than an assumed console result.

## Detailed test records

Copy this template for each test session. Back up your saves before testing inventory or progression
glitches; keep the backup local rather than attaching game files to the tracker.

```text
Test ID / date / tester:
Glitch and exact variant:
Recomp release or full commit:
Game version / build ID:
OS / CPU / GPU:
Input device (controller or keyboard/mouse):
Mode (Normal / Master) and DLC:
FPS cap / observed FPS:
Relevant settings or mods:
Location / starting state / equipment:
Steps to reproduce:
Expected result:
Actual result:
Successful attempts / total attempts:
Switch baseline version / result / reference:
Clip or screenshots:
Related issue:
```

When a result differs from a verified Switch baseline, open a bug report with these details and link it
in the table. Keep earlier results in the test records when updating the checklist for a newer build.

## Technique references

These references help identify techniques and setups; they are not confirmation of 1.9.0 compatibility.

- [ZeldaSpeedRuns: Wind Bomb](https://www.zeldaspeedruns.com/botw/techniques/wind-bomb)
- [ZeldaSpeedRuns: Bullet Time Glitches](https://www.zeldaspeedruns.com/botw/techniques/bullet-time-glitches)
- [ZeldaSpeedRuns: Whistle Sprint](https://www.zeldaspeedruns.com/botw/techniques/whistle-sprint)
- [ZeldaSpeedRuns: Skew Bounce](https://www.zeldaspeedruns.com/botw/techniques/skew-bounce)
- [Zelda Wiki: Glitches in Breath of the Wild](https://zeldawiki.wiki/wiki/Glitches_in_Breath_of_the_Wild)
