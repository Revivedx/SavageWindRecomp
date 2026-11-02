# Glitch compatibility tracker

Use this checklist to record which Breath of the Wild glitches and speedrun techniques behave like the
Nintendo Switch game in Savage Wind. Entries start untested until results are reported; inclusion is not a claim that a technique
works on Switch version 1.9.0 or in the recomp.

Savage Wind targets **Switch version 1.9.0**. Record the exact Switch version used as a baseline: an older
video or a Wii U result alone does not establish expected behavior on the supported version.

## Filling in the tables

- Replace `Untested` with `Works`, `Partial`, `Fails`, or `Retest` for each frame rate you test.
- `Works` means the expected effect was reproduced; `Partial` means it differs or only some variants work.
  `Fails` means the documented setup was attempted without the expected effect. `Retest` means an earlier
  result needs checking on the current build. A failed attempt alone does not establish a regression.
- In **Switch baseline**, record the version and result, with evidence when available.
- In **Build / report**, add the recomp commit or release and a link to a test record below, clip, or issue.
- Use **Notes** for the exact variant, limitations, and other frame rates (90, 120, uncapped, etc.).
  Add separate rows when variants or builds give different results.

Start at 30 FPS, then repeat the same setup at 60 FPS and any higher cap you use. Record the cap and actual
FPS when possible. Keep the location, equipment, input device, and setup consistent between runs.

## Movement, launches, and clipping

| Glitch / technique | Switch baseline | 30 FPS | 60 FPS | Build / report | Notes / other FPS |
|---|---|---|---|---|---|
| Windbomb (general test; variant not recorded) | — | Works | Works | Built from `test` branch commit [`ac47f65e3f9ee05641fff35f8d683fd36c306513`](https://github.com/Revivedx/SavageWindRecomp/commit/ac47f65e3f9ee05641fff35f8d683fd36c306513); [video evidence](https://youtu.be/7JGTN7cCYoY) | **120 FPS: Works.** Tester reports that higher FPS makes execution harder because the trick is timing-sensitive; expected behavior, not an issue. |
| Windbomb (bomb impact launch) — ledge setup | — | Untested | Untested | — | — |
| Windbomb — midair setup | — | Untested | Untested | — | — |
| Bullet Time Bounce (BTB) | — | Works | Works | Built from `test` branch commit [`ac47f65e3f9ee05641fff35f8d683fd36c306513`](https://github.com/Revivedx/SavageWindRecomp/commit/ac47f65e3f9ee05641fff35f8d683fd36c306513); [video evidence](https://youtu.be/A6n5opLTVng) | **120 FPS: Works.** |
| Bow Lift Smuggling Slide (BLSS) | — | Untested | Untested | — | — |
| Whistle sprint | — | Untested | Untested | — | — |
| Skew storage / shield clipping | — | Untested | Untested | — | Record wall and skew setup. |
| Skew bounce | — | Untested | Untested | — | — |
| Extended shield clip (ESC) | — | Untested | Untested | — | Record location and setup. |
| Stasis launch | — | Untested | Untested | — | Record object and hits. |
| Stasis clipping | — | Untested | Untested | — | — |
| Cryonis launch | — | Untested | Untested | — | — |
| Moon jump | — | Untested | Untested | — | Record mounted archery setup. |
| Wrong warp using moon jump | — | Untested | Untested | — | Record source and destination. |
| Infinite jump / climbing using whistle | — | Untested | Untested | — | Record exact method. |

## Inventory, equipment, and progression

| Glitch / technique | Switch baseline | 30 FPS | 60 FPS | Build / report | Notes / other FPS |
|---|---|---|---|---|---|
| Menu overload | — | Untested | Untested | — | Record bows, arrows, and setup count. |
| Equipment duplication through overload | — | Untested | Untested | — | Test weapon, bow, and shield separately. |
| Durability transfer | — | Untested | Untested | — | Record source and target equipment. |
| Weapon smuggling | — | Untested | Untested | — | — |
| Bow smuggling | — | Untested | Untested | — | — |
| Inventory Slot Transfer (IST) | — | Untested | Untested | — | Record exact method and transferred items. |
| Weapon Modifier Corruption (WMC) | — | Untested | Untested | — | Record intended and actual modifiers. |
| Apparatus storage | — | Untested | Untested | — | Record shrine and setup. |
| Heart / stamina duplication | — | Untested | Untested | — | Record exact apparatus / essence method. |
| Early Master Sword — campfire method | — | Untested | Untested | — | Record hearts and prerequisites. |
| Trial of the Sword skip | — | Untested | Untested | — | Requires DLC; record trial and route. |
| Eventide storage | — | Untested | Untested | — | Record inventory and quest effects. |
| Minigame storage | — | Untested | Untested | — | Record minigame and exact variant. |

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
