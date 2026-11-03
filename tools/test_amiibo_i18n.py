"""Verify amiibo control translations and the localized result-count format."""
import glob
from pathlib import Path

import i18n_check

KEYS = [
    "All series",
    "Others",
    "Selected figure",
    "Search character, game or series...",
    "Search amiibo",
    "All",
    "Favorites",
    "Recent",
    "Figures: %d",
    "No figures match. Try another search or filter.",
    "Remove favorite",
    "Add favorite",
    "Top button: favorite   L/R: tabs   Confirm: select   Back: cancel",
    "Choose a figure and receive rewards with the amiibo rune.",
    "amiibo rewards",
    "Enables the amiibo rune and automatically scans your selected figure. Use the rune in game to receive rewards.",
    "Choose amiibo",
    "Search by character or series, pin favorites, or pick a recent figure.",
    "Repeat scans",
    "Use the same figure again without waiting until tomorrow. Keeps first-use reward history. Enable amiibo rewards first."
]

for filename in glob.glob(str(Path(i18n_check.ROOT) / 'runtime/ui/i18n/*.inc')):
    language = Path(filename).stem
    entries = i18n_check.table(language)
    for key in KEYS:
        assert entries.get(key), f'{language}: missing or empty amiibo translation: {key}'
    assert entries['Figures: %d'].count('%') == 1
    assert '%d' in entries['Figures: %d'], f'{language}: invalid result-count format'
print('PASS: amiibo controls translated in all 12 language tables')
