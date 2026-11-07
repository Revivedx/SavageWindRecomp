# Menu language packs

Savage Wind's own menus (the start screen and the in-game Options) come in 14 languages, built into the program
(`runtime/ui/i18n/<lang>.inc`). A **language pack** lets a player fix or finish one of them without waiting for a new
build: a text file in the data folder that the menus read over the built-in table.

- **Where:** `lang/<language>.txt` in the data folder (next to `savagewind.ini`). `<language>` is the menu language's
  code: `en`, `de`, `es`, `fr`, `it`, `nl`, `pt`, `ru`, `ja`, `ko`, `zh-Hans`, `zh-Hant`, `th` or `he`.
- **Format:** UTF-8, one text per line: the English text exactly as in the program, a tab, then the translation.
  `\n` stands for a line break, `\t` for a tab and `\\` for a backslash. Lines starting with `#` and empty lines are
  skipped. A line without a tab is skipped too (the log says so).
- **Which texts:** the English side is the first string of each `T("English", "...")` line in
  `runtime/ui/i18n/<language>.inc`. A text the pack doesn't name keeps the built-in translation.
- **When:** the pack is read the first time its language is shown, so restart Savage Wind after changing it. The log
  shows `[i18n] lang/<language>.txt: <n> texts`.

The game's own texts (items, dialogue, the game's menus) aren't part of this: they come from the game files.

Better translations are welcome back in the built-in tables, so every player gets them: see `tools/i18n_check.py`.
