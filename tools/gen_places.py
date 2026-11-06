#!/usr/bin/env python3
"""Regenerate runtime/control_places_data.inc, the table behind `goto shrine 5`, `goto korok H23`, `goto stable ...`.

The coordinates are the game's own world coordinates (the same frame as `pos`), taken from the data behind the ZeldaMods
object map (https://objmap.zeldamods.org, https://github.com/zeldamods/objmap):
  game_files/map_summary/MainField/static.json   shrines, towers, villages, stables, Koroks (marker positions)
  game_files/text/StaticMsg/*.json               English names
  radar.zeldamods.org /objs/MainField/           the LocationTag object of each marker (the marker icons float above the
                                                 ground, the tag sits where the place really is)

Where the game has a start position for a place (shrine entrances, towers, goddess statues, ...) that is used instead, because
it is a spot Link can stand on (tools/places_startpos.json, see tools/dump_startpos.py).

Run it when you want newer data; it needs network access and only the Python standard library:
  python3 tools/gen_places.py [--out runtime/control_places_data.inc]
"""
import argparse
import json
import re
import sys
import urllib.parse
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

GAME_FILES = "https://objmap.zeldamods.org/game_files"
RADAR = "https://radar.zeldamods.org"

# The game's own start positions (tools/places_startpos.json, made by tools/dump_startpos.py from your game files) are where
# Link really stands, so they win over the map data for every place that has one. Key of the place -> name of the StartPos.
START_OF = {
    "ForestHatago": "OldKorok_ForestStable",
    "NorthHatelHatago": "OldKorok_WetlandsStable",
    "RiverSideHatago": "OldKorok_RiversideStable",
    "FaronHatago000": "Hatago_Ch",
    "TamourHatago": "HigakkareBF_Goal",
    "Gerudo": "Gerudo001",
    "Goron": "GoddessGoron",
    "Hateno": "GoddessHateno",
    "Kakariko": "GoddessKakariko",
    "Cokiri": "GoddessKorok",
    "Taura": "GoddessUotori",
    "Rito": "GoddessRito",
    "UMiiVillage": "GoddessIchikara",
    "WhiteZora": "GoddessZora",
    "AncientLabo": "WarpAkkareLabo",
    "ValleyVillage": "ParashawlTalk",
    "HatenoLabo": "WarpHatenoLabo",
    "HyruleCastle": "HyruleCastleExit",
    "Oasis": "Oasis001",
}
START_OF.update({"Dungeon%03d" % n: "StartPos_CDungeon%03d" % n for n in range(120)})  # (the DLC shrines have none)
START_OF.update({"Tower%02d" % n: "StartPos_Tower%02d" % n for n in range(1, 16)})
START_NAMES = set(START_OF.values())
START_FILE = Path(__file__).with_name("places_startpos.json")
# Places with no start position of the game (the DLC shrines, a few landmarks): the map marker floats, sometimes 100 above the
# ground. tools/places_measured.json holds the height Link really stood at after `goto` dropped him from the marker
# (measured in a running game with tools/check_places.py, plus 1), so these places get the same small lift as the rest.
MEASURED_FILE = Path(__file__).with_name("places_measured.json")
START_MAX_OFF = 450.0  # a start position further than this from the map data's idea of the place is some other spot


def fetch_json(url):
    req = urllib.request.Request(url, headers={"User-Agent": "swr-gen-places"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.load(r)


def location_tag(message_id):
    """Position of the LocationTag with this message id, or None."""
    url = RADAR + "/objs/MainField/?q=" + urllib.parse.quote(message_id)
    for o in fetch_json(url):
        if o.get("name") == "LocationTag" and o.get("messageid") == message_id:
            return o["pos"]
    return None


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def grid_key(name):
    m = re.fullmatch(r"([A-Z])-(\d+)", name)
    return (m.group(1), int(m.group(2))) if m else (name, 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="runtime/control_places_data.inc")
    args = ap.parse_args()

    markers = fetch_json(GAME_FILES + "/map_summary/MainField/static.json")["markers"]
    loc_names = fetch_json(GAME_FILES + "/text/StaticMsg/LocationMarker.json")
    dungeon_names = fetch_json(GAME_FILES + "/text/StaticMsg/Dungeon.json")

    # Each entry: kind, number (0 = assigned by order), key, name, sub, position, source of the position
    entries = []

    def add(kind, number, key, name, sub, pos, ground_tag):
        entries.append({"kind": kind, "number": number, "key": key, "name": name, "sub": sub, "pos": pos, "tag": ground_tag})

    # Everything that has a message id also has a LocationTag in the world; fetch them all in parallel.
    ids = [m["MessageID"] for k in ("Dungeon", "Place", "Labo") for m in markers[k]]
    ids += [m["MessageID"] for m in markers["Tower"]]
    with ThreadPoolExecutor(8) as pool:
        tags = dict(zip(ids, pool.map(location_tag, ids)))

    def position(m, prefer_tag):
        t = tags.get(m["MessageID"]) if prefer_tag else None
        tr = m["Translate"]
        return (t, True) if t else ([tr["X"], tr["Y"], tr["Z"]], False)

    for m in markers["Dungeon"]:
        mid = m["MessageID"]
        n = int(mid.replace("Dungeon", ""))
        pos, tag = position(m, True)
        add("shrine", n, mid, dungeon_names[mid], dungeon_names.get(mid + "_sub", ""), pos, tag)
    for m in markers["Tower"]:
        # The tower's marker sits on the top platform, its tag at the foot inside the tower: use the marker.
        pos, tag = position(m, False)
        add("tower", int(m["MessageID"].replace("Tower", "")), m["MessageID"], loc_names[m["MessageID"]], "", pos, tag)
    for m in markers["Place"]:
        pos, tag = position(m, True)
        kind = {"Hatago": "stable", "Village": "village"}.get(m["Icon"], "landmark")
        add(kind, 0, m["MessageID"], loc_names[m["MessageID"]], "", pos, tag)
    for m in markers["Labo"]:
        pos, tag = position(m, True)
        add("landmark", 0, m["MessageID"], loc_names[m["MessageID"]], "", pos, tag)

    # Koroks: the game's ids (A01 ... Z99) are unique; number them in map grid order, then id
    for m in sorted(markers["Korok"], key=lambda k: (grid_key(k["map_name"]), k["id"])):
        tr = m["Translate"]
        add("korok", 0, m["id"], m["id"], m["map_name"], [tr["X"], tr["Y"], tr["Z"]], False)

    order = ["shrine", "tower", "stable", "village", "landmark", "korok"]
    out = []
    for kind in order:
        group = [e for e in entries if e["kind"] == kind]
        if kind in ("stable", "village", "landmark"):
            group.sort(key=lambda e: e["name"].lower())
        elif kind in ("shrine", "tower"):
            group.sort(key=lambda e: e["number"])
        for i, e in enumerate(group, 1):
            e["number"] = e["number"] if kind in ("shrine", "tower") else i
            out.append(e)

    # The game's own start positions replace the map data where there are any.
    starts = json.load(open(START_FILE)) if START_FILE.exists() else {}
    used = 0
    for e in out:
        s = starts.get(START_OF.get(e["key"], ""))
        if not s:
            continue
        off = ((s[0] - e["pos"][0]) ** 2 + (s[2] - e["pos"][2]) ** 2) ** 0.5
        if off > START_MAX_OFF:
            print("warning: %s: start position %s is %.0f away from the map data, not used" % (e["key"], START_OF[e["key"]], off), file=sys.stderr)
            continue
        e["pos"], e["tag"], e["ground"] = list(s), True, True
        used += 1
    print("start positions of the game used for %d places" % used, file=sys.stderr)

    measured = json.load(open(MEASURED_FILE)) if MEASURED_FILE.exists() else {}
    for e in out:
        m = measured.get(e["key"])
        if m and not e.get("ground"):
            e["pos"], e["ground"] = [e["pos"][0], m["y"], e["pos"][2]], True
    print("measured ground height used for %d places" % sum(1 for e in out if e["key"] in measured and e.get("ground")), file=sys.stderr)

    missing = [e["key"] for e in out if e["kind"] in ("shrine", "stable", "village") and not e["tag"]]
    if missing:
        print("warning: no LocationTag for", ", ".join(missing), "(marker position used, it floats above the ground)", file=sys.stderr)

    with open(args.out, "w") as f:
        f.write("// Generated by tools/gen_places.py, do not edit.\n")
        f.write("// Game world coordinates (the frame of `pos`) from the ZeldaMods object map data (https://objmap.zeldamods.org).\n")
        f.write("// PLACE(kind, number, key, name, sub, x, y, z, ground)\n")
        for e in out:
            x, y, z = e["pos"]
            f.write("PLACE(PLACE_%s, %d, %s, %s, %s, %.2ff, %.2ff, %.2ff, %s)\n" % (
                e["kind"].upper(), e["number"], c_str(e["key"]), c_str(e["name"]), c_str(e["sub"]), x, y, z,
                "true" if e.get("ground") else "false"))
    counts = {k: sum(1 for e in out if e["kind"] == k) for k in order}
    print("wrote %s: %s" % (args.out, counts))


if __name__ == "__main__":
    main()
