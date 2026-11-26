# Cheats, assists and amiibo

Savage Wind adds options that make the game easier, cheats that hand you items or change the world, a teleport, and
a virtual amiibo reader. None of them is on until you turn it on, and none of them changes your game files.

| What | Where | Needs |
|---|---|---|
| [Colour vision](#colour-vision) | The game's Options > Accessibility | Nothing |
| [Assists](#assists) | The game's Options > Accessibility | Nothing |
| [Cheats tab](#cheats-tab) | Savage Wind menu (F1) > Cheats and Teleport | Being in game, started with `-dev` |
| [Teleport and console cheats](#teleport-and-console-cheats) | The console (` or F12) | Started with `-dev` |
| [amiibo](#amiibo) | Savage Wind menu (F1) > amiibo | Your own figure dumps |
| [Save profiles](#keep-a-separate-save-for-cheats) | Savage Wind Configuration > Settings > Saves | The game closed |

## Colour vision

For colour-blind players. Open the game's own menu, then **Options > Accessibility**; Colour vision is at the top.
It shifts the colours of the whole picture, the game's HUD and menus included, so colours that look alike to you come
apart: a red Chuchu from a green one, the stamina wheel turning red when it runs out. The change shows at once, so you can try each type and strength while the menu is open.

| Option | What it does |
|---|---|
| Colour vision | Off, or the kind of colour blindness you have: **Protanopia** (red looks dark and close to green), **Deuteranopia** (green and red look alike, the most common), **Tritanopia** (blue and green, and yellow and pink, look alike). |
| Correction strength | How strongly the colours are shifted: 10% to 100%. Milder colour blindness often needs less; lower it if the picture looks too changed. |

Notes:

- It works with HDR, the upscalers and frame generation, and adds no extra pass: the frame rate stays the same.
- The Savage Wind menu (F1) and the photos you take with the camera rune keep their real colours. Screenshots and
  recordings of the window (Steam, OBS) show the corrected picture.
- Savage Wind's colour look and the Video > Colour settings still apply; the correction comes after them.

## Assists

Open the game's own menu, then **Options > Accessibility**. Changes apply at once, while you play, and are saved with
your settings.

| Option | What it does |
|---|---|
| God mode | Link takes no damage. |
| One-hit kill | Enemies go down in one hit. |
| Silent Link | Link stays quiet, so enemies do not hear him. |
| Unlock champion abilities | The four champion abilities are usable anywhere from the start of the game, in shrines and in the Trial of the Sword too; the pause menu cannot switch them off. |
| Damage taken | How much damage Link takes: 25% to 400%. 100% is the game's own. |
| Damage dealt | How much damage enemies take: 25% to 400%. 100% is the game's own. |
| Weapon wear | How fast weapons, bows and shields wear out: Fairy (never), Peter Pan (0.5x), Hylian (1x, the game's own), Hero (1.5x), Master (2x), Legend (2.5x), Dude, are you ok? (3x). |
| Infinite arrows | Shooting a bow uses up no arrows. You still need one arrow to shoot. |
| Infinite stamina | The stamina wheel never runs out. |

## Cheats tab

Open the Savage Wind menu (**F1**, **Esc**, the controller's Home / Xbox / PlayStation button, or Back + Start) and
go to the **Cheats** tab. The tab, and the Teleport tab next to it, only show up while you are in game and when
Savage Wind was started with `-dev` (see [Teleport and console cheats](#teleport-and-console-cheats) for how).

### Items

1. Pick a **Category** (weapons, bows, shields, arrows, armour, materials, critters and fairies, or all), or type part
   of a name in **Search**. The list stays empty until you do one of the two.
2. Set the **Amount** (1 to 99). Stackable items such as arrows and materials take the whole amount at once.
3. Press **Add** next to the item. It goes into your pouch through the game's own item pickup.

**Searching a Chinese name** goes by the name's pinyin. The search is typed on the on-screen keyboard, which a
controller drives and which has no input method, so the hanzi themselves can't be typed there: type the readings
instead, without tones, run together or apart. `luyu` finds 鲈鱼, `kaoluyu` 烤鲈鱼, `bingleng` the frozen fish, and `yu`
every name with 鱼 in it. With a keyboard the hanzi can be typed directly, as before; ü is also typed `v`, as in a
pinyin input method.

If the pouch is full, you get as many as fit, and the tab says how many were added. If you are in a menu or a
cutscene, the request waits until you are back in play. The item list comes from your own game files; nothing of the
game is shipped with Savage Wind.

### Rupees, assists, time, weather and more

| Option | What it does |
|---|---|
| Infinite rupees | Keeps your wallet at 9999 rupees, topped up every few seconds. |
| Set rupees | Sets your wallet to an exact number (0 to 999999). Turn off Infinite rupees first. |
| God mode, Infinite stamina, One-hit kill, Infinite arrows, Weapon wear, Damage taken, Damage dealt | The same settings as the [Assists](#assists): changing one here changes it there too. |
| Set hearts | Sets your heart containers to an exact number (3 to 30). It is written into your save. |
| Time of day | Sets the game's clock to an hour. The day goes on from there. |
| Freeze time | Stops the clock at the hour it is. Off: the day goes on. |
| Blood moon now | The game's own blood moon: its cutscene plays, and the monsters and materials come back as after any blood moon. It starts once you close the menu, in the open world only. |
| Weather | Holds the weather you pick (clear, cloudy, rain, heavy rain, snow, heavy snow, thunderstorm, thunder rain, rain in sunshine), or keeps out rain or thunderstorms. Automatic is the game's own. Cutscenes keep their weather. |
| Free sprint out of combat | Sprinting on the ground takes no stamina while no enemy is fighting you. Once enemies start the battle music, sprinting costs stamina again until the music fades. Climbing, swimming, gliding and attacks cost stamina as usual. |
| Item pickup multiplier | Multiplies the stackable items you pick up (materials, monster parts, arrows) by 2, 4 or 8, up to 999. |
| Sure-footed horse | A running horse no longer stops dead and rears when it runs into something, even a small rock. It no longer stops at cliff edges either: mind where you ride. |
| Show shrines and towers | Shows every shrine, tower, stable and ancient lab on the map, and makes the shrines, towers and labs travel destinations (a stable is shown but never travelled to). Nothing is saved and no tower is activated, so the map fog stays as it is. |

### Collectible hints on the map

**Collectible hints on the map** helps you find the Korok seeds and shrines you have left, without giving their exact
spots away. Turn it on in the Cheats tab, open the game's map and hold **D-pad Down**, or its key on the keyboard (**4**
unless you changed it in Controls). The map's prompt bar shows it as **Hints**, with your button. The hints fade in
while you hold it and fade out when you let go:

- a soft pale circle around each Korok seed you don't have yet, and a stronger amber one around each shrine you haven't
  found yet. The spot is somewhere inside its circle, not at its centre. Hovering a circle with the map's cursor
  says which it is;
- top right, the region under the cursor: its name and how many of its Korok seeds and shrines you have found. A region
  where you have found everything is shown in a muted blue, with a check after its name.

Only regions whose tower you have activated show anything: the rest of the map stays as the game shows it. Nothing is
shown in Hero's Path mode, or while the Sheikah Sensor's settings or a stamp's window is open. It is off until you turn
it on, and needs version 1.9.0 of the game.

The counts come from your game while the map is open and follow it as you play: the first time, reading them takes a
moment. They count the 900 Korok seeds and the 120 shrines of the main game; the shrines of The Champions' Ballad are
not counted. The list of what to count, and the regions' names, come from your own game files: Savage Wind reads them
once, when the game starts with the option on (or the first time the map asks, a fraction of a second). If your files
don't have what it expects, no hints show and the log says why.

## Teleport and console cheats

The teleport and a few more cheats are console commands for developers. To use them:

1. Start Savage Wind with `-dev`: in Steam, right-click the game > Properties > Launch options, or after the path
   in a shortcut. Or add the launch option `DEV` with the value `1` on the Savage Wind start screen:
   [Launch options](launch-options.md).
2. Start the game and load your save.
3. Press **`** (the key under Esc) or **F12** to open the console. The game gets no input while it is open; press the
   same key to close it.

| Command | What it does |
|---|---|
| `tp shrine 5` | Teleport to a shrine, a tower, a stable, a village, a landmark or a Korok, by number or name: `tp tower 3`, `tp stable wetland`, `tp hateno`, `tp korok 10`, `tp nearest korok`. `goto` works the same. Every place with its number: [Teleport places](teleport-places.md). |
| `places` | Lists the kinds of places and how many there are. `places shrine` lists the shrines with their numbers and names, and more words narrow the list: `places shrine akkala`. |
| `tp <x> <y> <z>` | Teleport to coordinates. |
| `pos` | Shows where Link is and copies a line you can give to `tp` later to come back. |
| `give <name> [count]` | Puts an item in the pouch, the same as the Cheats tab: `give arrow 20`. With several matches it lists them. |
| `time <0-24>` | Sets the hour of the day. `time freeze` stops the clock and `time unfreeze` starts it again. |
| `bloodmoon` | The game's own blood moon, as Cheats > Blood moon now. It starts once you close the console. |
| `collectibles` | Shows the Korok seeds and shrines you have, in all of Hyrule and in each region, and which towers you have activated: the counts the map's hints use. |
| `help` | Lists every command. |

Known limits of the teleport:

- Some places, Koroks above all, can land a little low or off to one side.
- After a long jump the camera can look odd for 1 to 3 seconds while the area loads.
- Rarely, the game crashes after long jumps. Save before you teleport often; restarting the game fixes it.

Developer mode also turns on other developer tools, and the Cheats and Teleport tabs of the Savage Wind menu.

## amiibo

Savage Wind has a virtual amiibo reader: the game's amiibo rune finds the figure you choose in the Savage Wind menu.
It reads dumps of your own amiibo figures. Savage Wind does not include any amiibo data and never will.

1. Turn on the amiibo rune in the game, as on the console.
2. Put your figure dumps (`.bin` files) in the `amiibo` folder of Savage Wind's data folder: next to
   `savagewind.exe` on Windows, and `~/.local/share/savagewind/amiibo/` on Linux and the Steam Deck. Folders inside it
   work too, for example one per series.
3. For the figures to give game data (their rewards, the nickname, Wolf Link's hearts), also put the amiibo keys file
   `key_retail.bin`, dumped from your own console, in the same folder. Without it the reader still finds the figure,
   but the game gets no data from it.
4. Open the Savage Wind menu (F1) and go to the **amiibo** tab. Pick a figure from the list; Search and the series
   filter help with a big collection. The figure you pick stays on the reader until you pick another one or **None**.
5. In the game, use the amiibo rune as usual. The game finds the figure on the reader.

The game's own limits apply: each figure gives its rewards once a day.

## Keep a separate save for cheats

Cheats change your save for good: items you add stay in the pouch, and a teleport can skip parts of the story. To
keep your own playthrough clean, use a second save profile for cheating. A save profile is a whole set of saves, with
its own slots, options, Hero's Path, album and compendium, and profiles never touch each other.

1. Close the game. Profiles are only on the start screen, because the game must not be running while saves change.
2. Open **Savage Wind Configuration** and go to **Settings > Saves**. The **Profiles** section is at the top. Your
   existing saves are the profile called **Default**; they are not moved or changed.
3. To cheat on a copy of your playthrough: select **Default**, type a name in **Name for a new profile** (for example
   `Cheats`) and choose **Copy this profile**. To start from scratch instead, type the name and choose **Create empty
   profile**.
4. Select the new profile and choose **Use this profile**, then press **Play**. The game now loads and saves only in
   that profile.
5. To go back to your own playthrough, close the game, select **Default** and choose **Use this profile**.

Switching only changes which profile the game uses: no save is copied, moved or deleted. Renaming, deleting, where the
files are and the naming rules are in [Save profiles](save-profiles.md).
