"""Verify amiibo control translations and the localized result-count format."""
import glob
from pathlib import Path

import i18n_check

KEYS = [
    "Wolf Link data",
    "Synthetic: 20 hearts. Own amiibo: place wolf_link.bin and key_retail.bin in the amiibo folder. Original files are never overwritten.",
    "Own amiibo",
    "Synthetic (20 hearts)",
    "Own Wolf Link data loaded.",
    "Missing amiibo/wolf_link.bin.",
    "The dump is not a Wolf Link amiibo.",
    "Missing amiibo/key_retail.bin.",
    "The amiibo keys file is invalid.",
    "The dump failed authentication. Check your dump and keys.",
    "Could not read amiibo data. Try again.",
    "The saved Wolf Link application data is invalid or unreadable.",
    "The amiibo dump is invalid. Use a 532, 540 or 572-byte NTAG215 dump.",
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
    "Preview",
    "Series",
    "amiibo ID",
    "Clear search",
    "Reset filters",
    "Choose a figure and receive rewards with the amiibo rune.",
    "amiibo rewards",
    "Enables the amiibo rune and virtual reader. Use the rune in game to choose a figure and receive rewards.",
    "Automatic scanning",
    "Off: choose a figure each time you use the rune. On: scan your saved selection automatically.",
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
