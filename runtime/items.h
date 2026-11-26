// The game's own items (runtime/items.c; the give command, the overlay's Cheats tab), read from the player's RomFS (their
// actors in Actor/ActorInfo.product.sbyml, their names in the text language's Pack/Bootup_<lang>.pack), and requests
// to put some of them in the pouch, which the game thread carries out through the game's own item get
// (tools/patches.py _item_tick).
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { ITEMCAT_WEAPON, ITEMCAT_BOW, ITEMCAT_SHIELD, ITEMCAT_ARROW, ITEMCAT_ARMOR, ITEMCAT_MATERIAL, ITEMCAT_COUNT };
// What a piece of armour does, or what a material adds to a dish (ActorInfo armorEffectEffectType /
// cureItemEffectType, in this order; "AAndB" is two)
enum { ITEMFX_NONE, ITEMFX_ATTACK, ITEMFX_DEFENSE, ITEMFX_CLIMB, ITEMFX_BEAM, ITEMFX_STEALTH, ITEMFX_GUARDIAN, ITEMFX_FLAME,
       ITEMFX_COLD, ITEMFX_SHOCK, ITEMFX_FREEZE, ITEMFX_HEAT, ITEMFX_LIGHTNING, ITEMFX_SAND, ITEMFX_SNOW, ITEMFX_SWIM,
       ITEMFX_STAMINA_MAX, ITEMFX_FIREPROOF, ITEMFX_STAMINA, ITEMFX_HEARTS_MAX, ITEMFX_SPEED, ITEMFX_COUNT };

typedef struct {
    const char *actor;  // "Weapon_Sword_001"
    const char *name;   // in the text language, UTF-8
    const char *pinyin; // the name's readings, syllables spaced ("kao lu yu"): what the search finds it by too
    int cat;            // ITEMCAT_*
    int value;          // what the item get is given for one: durability x 100 (weapons, bows, shields), else 1
    bool stack;         // one pouch entry holds many (arrows, materials)
    // what the pouch shows of it (ActorInfo; 0 where it has none)
    int attack;         // attackPower (weapons, bows, arrows)
    int durability;     // generalLife (weapons, bows, shields)
    int guard;          // weaponCommonGuardPower (shields)
    int defense;        // armorDefenceAddLevel (armour)
    int hearts;         // cureItemHitPointRecover, in quarter hearts (food and materials)
    int price;          // itemSellingPrice, in rupees
    unsigned char effect[2];  // ITEMFX_* (armour: what it does; materials: what they add to a dish)
} GameItem;

// The items that can be added, sorted by category and name (read on first use; 0 without game files or the game's
// item get in the game code)
int items_count(void);
const GameItem *items_at(int i);
// Does item i match a search text: its name, its actor, or the pinyin of its name ("luyu" finds 烤鲈鱼 - the menu's
// search is typed on a controller's ASCII keyboard, which has no IME, so a Chinese name needs the reading)
bool items_matches(int i, const char *text);
bool items_available(void);  // the game code has the item get (its sites were found)
// Puts `count` of item i in the pouch on the game thread's next frame in play (a stack takes them at once, anything
// else one entry each, at most ITEMS_MAX_EACH). false: the queue is full.
#define ITEMS_MAX_EACH 99
bool items_request(int i, int count);
unsigned items_request_seq(int i, int count);  // the same, returning the request's number (0: the queue is full)
// The item whose actor that is, even one the list leaves out for sharing its name (a Divine Helm's upgrades), and a
// request for it (0: no such item, or the queue is full)
const GameItem *items_by_actor(const char *actor);
unsigned items_request_actor(const char *actor, int count);
// One of that actor's item in a treasure chest put in front of Link, on the game thread's next frame in play (place: 0
// ahead, 1 and 2 to its sides, for several at once); its result's `added` is 1 once the chest was made. 0: the queue
// is full, or this game code can't make chests (items_chests_available)
unsigned items_request_chest(const char *actor, int place);
bool items_chests_available(void);
// How many requests were made so far, and the last one the game thread carried out: its number among them (0: none yet),
// and how many of it went in the pouch. item -1: dropped, the game rebooted before it could be carried out.
typedef struct { unsigned seq; int item, asked, added; } ItemResult;
unsigned items_requested(void);
ItemResult items_last_result(void);
bool items_result_of(unsigned seq, ItemResult *out);  // request seq's result, once carried out (among the latest 64)
// Item i's picture in the pouch as RGBA8 rows, w x h (free() it); NULL without one
unsigned char *items_icon(int i, int *w, int *h);

#ifdef __cplusplus
}
#endif
