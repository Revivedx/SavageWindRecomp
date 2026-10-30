// The achievements. Flag names are the game's own GameData names (the save stores their crc32).
//
// Confirmed against this repo: IsGet_PlayerStole2 (the paraglider, runtime/services/test_warp.c) and MaxHartValue (4
// per heart: 120 = 30 hearts, docs/SAVAGE-WIND.md). The other names come from how BOTW names its save flags and
// still need checking against a real save: `tools/ach_tool verify <game_data.sav>` lists each one found / not found.
// The Korok seed counter's name isn't known: "#8a94e07a" is its crc32, found with `ach_tool counter` on saves with 2, 3 and 4 seeds.
// The table is in order of difficulty, easiest first (a rough guess of the effort in a normal playthrough): it is also the order
// of the lists in the menus. The ids are what is saved, so moving a line never loses anything.
// An achievement whose flag isn't in the save simply never unlocks (nothing breaks). A flag whose name isn't known can be
// written as "#<hex crc32>" (what `tools/ach_tool diff` prints), e.g. "#1a2b3c4d".
#include "achievements.h"
#include "achievements_sets.h"

// (the table leaves the trailing `list` fields out for the single-flag kinds)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

const AchDef g_ach_defs[] = {
    // id, name, description, kind, flag, first, last, min

    // ---- the first hour: the Great Plateau ----
    // The cutscene of leaving the Shrine of Resurrection is Demo103_0 (zeldamods.org, WorldMgr / Time: the game itself checks it
    // to start the clock); IsPlayed_Demo<N>_<k> is the save flag of a cutscene having played. Check with `ach_tool get`.
    {"leave_shrine", "Awakening", "Leave the Shrine of Resurrection.", ACH_FLAG, "IsPlayed_Demo103_0", 0, 0, 1},
    // (Defeated_<actor>_Num counts the kills of each enemy)
    {"first_enemy", "What's That?", "Defeat your first enemy.", ACH_SUM, "", 0, 0, 1, L_defeated, ACH_N(L_defeated)},
    {"shrine_1", "First Trial", "Complete a shrine.", ACH_COUNT, "Clear_Dungeon%03d", 0, 120, 1},
    {"shrine_4", "Four Trials", "Complete 4 shrines.", ACH_COUNT, "Clear_Dungeon%03d", 0, 120, 4},
    {"paraglider", "Take Flight", "Get the Paraglider.", ACH_FLAG, "IsGet_PlayerStole2", 0, 0, 1},
    // Beedle's first talk (Npc_TripMaster: Beedle, "Terry" in Spanish)
    {"beedle", "My Best Friend", "Talk to Beedle.", ACH_FLAG, "Npc_TripMaster_00_Talk", 0, 0, 1},

    // ---- the first hours of the open world ----
    // A Flurry Rush (a perfect dodge: time slows down): once the Actions list has one of its dodges, or the first
    // Flurry Rush (GuideSPlus_KaihiJust, kaihi: dodge, seen with SAVAGEWIND_LOG_FLAGS while doing one): L_flurry
    {"flurry_rush", "Bullet Time", "Dodge an attack at the last moment and slow down time.", ACH_ANY, "", 0, 0, 0,
     L_flurry, ACH_N(L_flurry)},
    // ... and a Perfect Guard (a parry with the shield): GuideSPlus_GurdJust, its twin (set with Guide_GurdJust, the
    // Perfect Guard hint, as an enemy attacks with the shield up: no flag records the parry itself)
    {"perfect_guard", "Not Today, Thanks", "Parry an attack with your shield.", ACH_FLAG, "GuideSPlus_GurdJust", 0, 0, 1},
    // Shield surfing: GuideSPlus_Surfing, set with Guide_Surfing the first time (seen with SAVAGEWIND_LOG_FLAGS)
    {"shield_surf", "Just Like the Beach", "Surf down a slope on your shield.", ACH_FLAG, "GuideSPlus_Surfing", 0, 0, 1},
    {"korok_1", "Is That Its Poop?", "Find your first Korok seed.", ACH_FLAG, "#8a94e07a", 0, 0, 1},
    // Horse names: the save keeps 6 names of 64 characters (16 entries each) under crc32 0x7b74e117 (zeldamods.org, Save Files).
    // A slot has a name once a horse is registered at a stable. `ach_tool horses <save>` prints the slots.
    {"horse_registered", "A Little Horse", "Register a horse at a stable.", ACH_STR, "#7b74e117", 16, 0, 1},
    {"horse_iwata", "Satoru Iwata", "Register a horse named Satoru.", ACH_STR, "#7b74e117", 16, 0, 1, L_horse_legend,
     ACH_N(L_horse_legend)},
    // Dishes: IsGet_Item_Cook_* (dishes you have had)
    {"cook_10", "Improvised Chef", "Cook 10 different recipes.", ACH_SET, "", 0, 0, 10, L_cook, ACH_N(L_cook)},
    // (the save records getting a rune, not using it)
    {"runes", "Field Engineer", "Get every Sheikah Slate rune.", ACH_ALL,  // (round and cube bombs: one rune, no Cube flag)
     "IsGet_Obj_RemoteBomb,IsGet_Obj_Magnetglove,IsGet_Obj_StopTimer,IsGet_Obj_IceMaker,IsGet_Obj_Camera", 0, 0, 5},
    {"guardians_10", "Guardian Hunter", "Destroy 10 Guardians.", ACH_SUM, "", 0, 0, 10, L_guardians, ACH_N(L_guardians)},

    // ---- the first stretch of the adventure ----
    {"shrine_25", "Shrine Wanderer", "Complete 25 shrines.", ACH_COUNT, "Clear_Dungeon%03d", 0, 120, 25},
    // The 15 Sheikah Towers are what reveals the map (MapTower_01..15, 1 once activated).
    {"map_towers", "Cartographer", "Reveal the whole map: activate all 15 Sheikah Towers.", ACH_COUNT, "MapTower_%02d", 1, 15, 15},
    // The Korok seed counter's name isn't known: "#8a94e07a" is its crc32 (see the top of the file).
    {"korok_100", "Seed Gatherer", "Find 100 Korok seeds.", ACH_FLAG, "#8a94e07a", 0, 0, 100},
    {"beast_ruta", "Waters Calmed", "Free Divine Beast Vah Ruta.", ACH_FLAG, "Clear_RemainsWater", 0, 0, 1},
    {"beast_rudania", "Fire Quelled", "Free Divine Beast Vah Rudania.", ACH_FLAG, "Clear_RemainsFire", 0, 0, 1},
    {"beast_naboris", "Storm Stilled", "Free Divine Beast Vah Naboris.", ACH_FLAG, "Clear_RemainsElectric", 0, 0, 1},
    {"beast_medoh", "Winds Freed", "Free Divine Beast Vah Medoh.", ACH_FLAG, "Clear_RemainsWind", 0, 0, 1},
    {"beast_all", "Four Beasts Freed", "Free all four Divine Beasts.", ACH_ALL,
     "Clear_RemainsWater,Clear_RemainsFire,Clear_RemainsElectric,Clear_RemainsWind", 0, 0, 4},

    // ---- mid game: gear and the long ones start ----
    // IsGet_<actor> = "you have had that item" (like IsGet_Weapon_Sword_070 below). Actor names, from BOTW's item tables:
    //   Item_Enemy_26..31 = Ancient Gear, Screw, Spring, Shaft, Core, Giant Ancient Core (the six ancient parts)
    //   Armor_160_Head / _Upper / _Lower = Dark Hood, Dark Tunic, Dark Trousers (the Dark Set: Dark Link's look)
    //   Weapon_Shield_030 = Hylian Shield
    // They follow the pattern of the flags already checked against a real save, but run `tools/ach_tool verify` on yours.
    {"hylian_shield", "The Legendary Shield", "Get the Hylian Shield.", ACH_FLAG, "IsGet_Weapon_Shield_030", 0, 0, 1},
    {"master_sword", "Blade That Seals the Darkness", "Pull the Master Sword.", ACH_FLAG, "IsGet_Weapon_Sword_070", 0, 0, 1},
    // The Lost Woods are crossed when the Korok Forest (map marker Cokiri) has been reached
    {"lost_woods", "Been Here Before", "Make your way through the Lost Woods.", ACH_FLAG, "Location_Cokiri", 0, 0, 1},
    // Hestu's maracas (stolen in "Hestu's Concerto" style: IsGet_<actor> of the item)
    {"maracas", "Shake 'Em", "Get Hestu's maracas back.", ACH_FLAG, "IsGet_Obj_Maracas", 0, 0, 1},
    // The Gerudo vai: veil, top and sirwal are Armor_053
    {"gerudo_outfit", "Strange Tastes", "Get the outfit to sneak into Gerudo Town.", ACH_ALL,
     "IsGet_Armor_053_Head,IsGet_Armor_053_Upper,IsGet_Armor_053_Lower", 0, 0, 3},
    {"arrows", "Fletcher", "Get every type of arrow.", ACH_ALL,
     "IsGet_NormalArrow,IsGet_FireArrow,IsGet_IceArrow,IsGet_ElectricArrow,IsGet_BombArrow_A,IsGet_AncientArrow", 0, 0, 6},
    {"old_memories", "Old Memories", "Get the Master Sword and the Hylian Shield.", ACH_ALL, "IsGet_Weapon_Sword_070,IsGet_Weapon_Shield_030", 0, 0, 2},
    {"ancient_parts", "Ancient Collection", "Get every type of ancient part.", ACH_ALL,
     "IsGet_Item_Enemy_26,IsGet_Item_Enemy_27,IsGet_Item_Enemy_28,IsGet_Item_Enemy_29,IsGet_Item_Enemy_30,IsGet_Item_Enemy_31", 0, 0, 6},
    {"cook_50", "Camp Cook", "Cook 50 different recipes.", ACH_SET, "", 0, 0, 50, L_cook, ACH_N(L_cook)},

    // ---- late game ----
    // Dark Beast Ganon is Calamity Ganon's last form: beating it ends the game.
    {"ganon", "End of the Calamity", "Defeat Calamity Ganon.", ACH_FLAG, "Defeated_Enemy_GanonBeast_Num", 0, 0, 1},
    {"hearts_20", "Stronger Heart", "Reach 20 hearts.", ACH_FLAG, "MaxHartValue", 0, 4, 20},
    {"shrine_60", "Shrine Veteran", "Complete 60 shrines.", ACH_COUNT, "Clear_Dungeon%03d", 0, 120, 60},
    // "Stranded on Eventide" (QL_NakedIsland): the island's trial finished
    {"eventide", "The Lost Island", "Complete Eventide Island.", ACH_FLAG, "NakedIsland_Finish", 0, 0, 1},
    {"stables", "Horse Lover", "Visit every stable.", ACH_SET, "", 0, 0, 0, L_stables, ACH_N(L_stables)},
    {"lynel_all", "Kings of the Wild", "Defeat one Lynel of each type.", ACH_ALL,
     "Defeated_Enemy_Lynel_Junior_Num,Defeated_Enemy_Lynel_Middle_Num,Defeated_Enemy_Lynel_Senior_Num,Defeated_Enemy_Lynel_Dark_Num",
     0, 0, 4},
    // "Captured Memories" (PictureMemory): every memory recovered
    {"memories", "Captured Memories", "Recover every memory.", ACH_FLAG, "PictureMemory_Finish", 0, 0, 1},

    // ---- the downloadable content ----
    // (actor names and flags from the game's own texts and GameData, 1.9.0 with the DLC: Armor_172_Head Majora's Mask,
    // 176_Head Korok Mask, 179 Royal Guard Cap / Uniform / Boots, 180 Phantom Ganon Skull / Armor / Greaves; the
    // Champions' Ballad: BalladOfHero<Zora/Rito/Goron/Gerudo> are the four Champions' songs, BalladOfHeroes the whole;
    // FinalTrial is the last shrine, Maz Koshia's)
    {"korok_mask", "Spitting Image", "Get the Korok Mask.", ACH_FLAG, "IsGet_Armor_176_Head", 0, 0, 1},
    {"majora_mask", "A Moon?", "Get Majora's Mask.", ACH_FLAG, "IsGet_Armor_172_Head", 0, 0, 1},
    {"phantom_ganon", "Feel the Evil", "Get the Phantom Ganon armor.", ACH_ALL,
     "IsGet_Armor_180_Head,IsGet_Armor_180_Upper,IsGet_Armor_180_Lower", 0, 0, 3},
    {"royal_guard", "Royal Protector", "Get the Royal Guard armor.", ACH_ALL,
     "IsGet_Armor_179_Head,IsGet_Armor_179_Upper,IsGet_Armor_179_Lower", 0, 0, 3},
    {"obliterator", "The Divine Annihilator", "Get the most powerful weapon in all of Hyrule.", ACH_FLAG,
     "IsGet_Weapon_Sword_502", 0, 0, 1},
    {"song_mipha", "Mipha's Song", "Complete Mipha's Song.", ACH_FLAG, "BalladOfHeroZora_Finish", 0, 0, 1},
    {"song_revali", "Revali's Song", "Complete Revali's Song.", ACH_FLAG, "BalladOfHeroRito_Finish", 0, 0, 1},
    {"song_daruk", "Daruk's Song", "Complete Daruk's Song.", ACH_FLAG, "BalladOfHeroGoron_Finish", 0, 0, 1},
    {"song_urbosa", "Urbosa's Song", "Complete Urbosa's Song.", ACH_FLAG, "BalladOfHeroGerudo_Finish", 0, 0, 1},
    {"maz_koshia", "Why You?", "Defeat Maz Koshia.", ACH_FLAG, "Die_Boss_FinalTrial,Clear_FinalTrial", 0, 0, 1},
    {"ballad", "The Champions' Ballad", "Complete every Champion's song.", ACH_FLAG, "BalladOfHeroes_Finish", 0, 0, 1},
    {"champions_photo", "A Picture for the Memories", "Get the photo of the Champions.", ACH_FLAG,
     "IsGet_Get_TwnObj_DLC_MemorialPicture_A_01", 0, 0, 1},
    {"master_cycle", "Why a Motorcycle?", "Get the Master Cycle Zero.", ACH_FLAG, "IsGet_Obj_Motorcycle", 0, 0, 1},

    // ---- the completionist ones ----
    {"great_fairy", "Thank You, Great Fairy!", "Fully upgrade an armor set.", ACH_ANY, "", 0, 0, 0, L_great_fairy,
     ACH_N(L_great_fairy)},
    {"korok_450", "Seed Hoarder", "Find 450 Korok seeds.", ACH_FLAG, "#8a94e07a", 0, 0, 450},
    {"hearts_30", "Heart of a Hero", "Reach 30 hearts.", ACH_FLAG, "MaxHartValue", 0, 4, 30},
    {"cook_100", "Master Chef", "Cook 100 different recipes.", ACH_SET, "", 0, 0, 100, L_cook, ACH_N(L_cook)},
    {"dark_armor", "The Dark Side", "Get Dark Link's armor.", ACH_ALL,
     "IsGet_Armor_160_Head,IsGet_Armor_160_Upper,IsGet_Armor_160_Lower", 0, 0, 3},
    {"dex_food", "Flavor Hunter", "Complete the Compendium's food ingredients.", ACH_SET, "", 0, 0, 0, L_dex_food, ACH_N(L_dex_food)},
    {"shrine_120", "Shrine Master", "Complete all 120 shrines.", ACH_COUNT, "Clear_Dungeon%03d", 0, 120, 120},
    {"dex_creatures", "Photographer of the Realm", "Fill the Compendium's creatures.", ACH_SET, "", 0, 0, 0, L_dex_creatures,
     ACH_N(L_dex_creatures)},
    {"korok_900", "Seed Legend", "Find all 900 Korok seeds.", ACH_FLAG, "#8a94e07a", 0, 0, 900},
    // Calamity Ganon beaten in a Master Mode save: kept for every save, a new normal game's too
    {"master_mode", "Master Hylian", "Beat the game in Master Mode.", ACH_MASTER, "Defeated_Enemy_GanonBeast_Num", 0, 0, 1},

    // every other one: the last, with a sound of its own (achievements_sound.c)
    {"all", "King of Hyrule", "Unlock every achievement.", ACH_META, "", 0, 0, 0},

    // "plus_ultra" (Slated for Upgrades: upgrade the Sheikah Sensor, Remote Bombs and Stasis at Purah's) is NOT here yet: the
    // names of its flags aren't known. docs/ACHIEVEMENTS.md ("Waiting for its flags") says how to find them with ach_tool diff.
};
const int g_ach_count = (int)(sizeof g_ach_defs / sizeof g_ach_defs[0]);

// The game's own picture for each achievement (runtime/ui/ach_icon.mm), from the player's RomFS: an inventory icon
// (UI/StockItem/<name>.sbitemico) or, with "@", a Hyrule Compendium photo (UI/PictureBook/<name>.jpg)
const AchGameIcon g_ach_game_icons[] = {
    {"leave_shrine", "Obj_DRStone_Get"}, {"first_enemy", "@Enemy_Bokoblin_Junior"}, {"shrine_1", "Item_Enemy_40"},
    {"shrine_4", "Obj_HeartUtuwa_A_01"}, {"paraglider", "PlayerStole2"}, {"beedle", "PutRupee"},
    {"flurry_rush", "Weapon_Sword_001"}, {"perfect_guard", "Weapon_Shield_003"}, {"shield_surf", "Weapon_Shield_001"},
    {"korok_1", "Obj_KorokNuts"}, {"horse_registered", "GameRomHorseSaddle_00"}, {"horse_iwata", "@GameRomHorse"},
    {"cook_10", "Item_Roast_01"}, {"runes", "Obj_RemoteBombBall"}, {"guardians_10", "@Enemy_Guardian_A"},
    {"shrine_25", "Obj_StaminaUtuwa_A_01"}, {"map_towers", "Obj_Photo_Other"}, {"korok_100", "Obj_KorokNuts"},
    {"beast_ruta", "Weapon_Spear_050"}, {"beast_rudania", "Item_Ore_B"}, {"beast_naboris", "Item_Ore_D"},
    {"beast_medoh", "Weapon_Bow_071"}, {"beast_all", "Item_Ore_J"}, {"hylian_shield", "Weapon_Shield_030"},
    {"master_sword", "Weapon_Sword_070"}, {"lost_woods", "@Npc_HiddenKorokGround"}, {"maracas", "Obj_Maracas"},
    {"gerudo_outfit", "Armor_053_Upper"}, {"arrows", "Obj_ArrowBundle_A_01"}, {"old_memories", "Weapon_Shield_030"},
    {"ancient_parts", "Item_Enemy_31"}, {"cook_50", "Item_Cook_A_01"}, {"ganon", "@Enemy_GanonBeast"},
    {"hearts_20", "Obj_HeartUtuwa_A_01"}, {"shrine_60", "Item_Ore_E"}, {"eventide", "Obj_FireWoodBundle"},
    {"stables", "GameRomHorseReins_00"}, {"lynel_all", "@Enemy_Lynel_Senior"}, {"memories", "Obj_Camera"},
    {"korok_mask", "Armor_176_Head"}, {"majora_mask", "Armor_172_Head"}, {"phantom_ganon", "Armor_180_Head"},
    {"royal_guard", "Armor_179_Head"}, {"obliterator", "Weapon_Sword_502"}, {"song_mipha", "Weapon_Spear_050"},
    {"song_revali", "Weapon_Bow_071"}, {"song_daruk", "Item_Ore_B"}, {"song_urbosa", "Item_Ore_D"},
    {"maz_koshia", "@Priest_Boss_Normal"}, {"ballad", "Obj_Head_024"},
    {"champions_photo", "Get_TwnObj_DLC_MemorialPicture_A_01"}, {"master_cycle", "Obj_Motorcycle"},
    {"great_fairy", "Item_Ore_A"}, {"korok_450", "Obj_KorokNuts"}, {"hearts_30", "Obj_HeartUtuwa_A_01"},
    {"cook_100", "Item_Cook_P_01"}, {"dark_armor", "Armor_160_Head"}, {"dex_food", "Item_Fruit_A"},
    {"shrine_120", "Item_Ore_A"}, {"dex_creatures", "Obj_Photo_Animal"}, {"korok_900", "Obj_ProofKorok"},
    {"master_mode", "@Enemy_Ganon"}, {"all", "Obj_Armor_115_Head"},
};
const int g_ach_game_icon_count = (int)(sizeof g_ach_game_icons / sizeof g_ach_game_icons[0]);
