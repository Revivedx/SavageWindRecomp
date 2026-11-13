// The game versions Savage Wind knows, by the build id of their main module (its first 8 bytes as hex, as
// tools/sw_probe.py prints it). The configuration screen sets up the supported ones; a known one that isn't supported
// yet is refused with its own message. tools/game_sites.py VERSIONS is the same list for the code generator
// (tools/test_game_sites.py checks they match); docs/SAVAGE-WIND.md, "Versiones del juego", has the steps for a
// new version. X(version, build id, supported, RomFS files, RomFS bytes)
// RomFS files and bytes: how many files its RomFS has with the update applied over the base game, and their sizes added
// up (0: not counted). Before copying, the setup checks that the chosen RomFS says this version in System/Version.txt
// and has all of these files and bytes (runtime/ui/launcher.mm, check_romfs): the cartridge's RomFS without the update
// says its own version, the update's RomFS alone has far fewer files, and an extraction that lost some files or cut
// some short (#472: 180 files missing, the game crashed before its first frame) comes up short of either number.
#pragma once

#define GAME_VERSIONS(X)                                      \
    X("1.9.0", "cd57b23fa4bbad65", 1, 41062, 15129626830ull)
