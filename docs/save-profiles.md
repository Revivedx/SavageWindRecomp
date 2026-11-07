# Save profiles

A save profile is a whole set of saves: its own slots, its own option file, its own Hero's Path, album and compendium.
You can keep several and switch between them, for example your own playthrough and a second one for testing, and they
never touch each other.

## Where they are

In **Savage Wind Configuration**, go to **Settings > Saves**. The **Profiles** section is at the top. The tab is only on
the start screen, not in the in-game menu, because the game must not be running while saves change.

**Your existing saves need nothing from you.** The `save` folder you have always used shows up as the profile called
**Default**. It is not moved, renamed or changed, and it is used until you pick another profile.

## Use it

- **Switch:** close the game, open Savage Wind Configuration, go to **Settings > Saves**, select a profile in the list
  and choose **Use this profile**. The next time you press Play the game uses that profile's saves. Switching changes
  only which profile is used. No save is copied, moved or deleted.
- **New empty profile:** type a name in **Name for a new profile**, then choose **Create empty profile**. The game
  starts a new adventure in it.
- **Copy a profile:** select the profile to copy, type the new name, choose **Copy this profile**. You get a complete
  second set of saves to experiment with. The copy is made under a temporary name and only appears in the list when it
  is whole. If the disk is too full, nothing is copied and you are told.
- **Rename** and **Delete** work on the selected profile. Delete asks first and removes that profile's saves for good,
  so check the name. The profile you are using and the Default profile can't be renamed or deleted: switch to another
  one first.
- **Open this profile's folder** shows the profile's saves in your file manager.

The other buttons on the tab (**Import from Wii U / Cemu...**, **Back up saves**, the slot list) work on the profile in
use. Importing a Wii U save replaces the saves of that profile only.

## Names

A name can have letters, digits, spaces and `- _ . ( )`, up to 32 characters. It can't start or end with a space or a
dot, can't be `Default`, and can't be a name Windows keeps for itself (`CON`, `NUL`, `COM1`, ...). Upper and lower case
count as the same: `Test` and `test` can't both exist.

## Where the files are

```
save/                         the Default profile (as always)
save-profiles/<name>/save/    every other profile
save_profile.txt              the name of the profile in use
```

A profile folder has the same layout as `save`: `save_0` with the slots `0` to `5`, `tracker` and `option.sav`.
To back up one profile, close the game and copy its folder. To add a set of saves you already have, create a profile,
close Savage Wind Configuration, and copy the files into `save-profiles/<name>/save/`.

## Good to know

- A profile that a running game is using can't be switched to, copied, renamed or deleted from another Savage Wind.
  Close the game first.
- If `SAVAGEWIND_SAVE_DIR` is set (a testing setting), it decides the save folder and profiles are switched off. The tab
  says so.
- Old profile folders and `save_profile.txt` come along when you move the data folder: copy `save-profiles/` and
  `save_profile.txt` together with `save/` (see [Moving the data folder](data-folder.md)).
