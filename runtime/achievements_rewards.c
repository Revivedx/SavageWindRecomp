// Achievement rewards: items an achievement gives, once for each save, through the game's own item get (runtime/items.c,
// as Cheats > Add items does). They are the game's own items that only amiibo figures drop (its
// Actor/Pack/Item_Amiibo_DropTable_NNN): no figure and nothing of a figure's data is involved.
//
// The save being played (ach_play_save) gets the reward of each achievement it has unlocked and hasn't been given,
// in play (not on the title screen): when it is unlocked, or when a save that has it already is loaded. What was given
// is kept in achievement-rewards.txt in the data root, one "<playthrough>\t<achievement id>" a line: the playthrough
// is the save's save_0 folder (ach_save_run), not its slot, or every autosave to another slot gave everything again.
// Lines from before, with a slot's game_data.sav, count for that slot's playthrough. An item the pouch has no
// room for goes in a golden chest in front of Link (items_request_chest), which the player opens or leaves; without
// the game code for chests, it is asked for again every RETRY_S seconds, with a notice the first time. The
// achievement's own notice shows its reward (runtime/ui/achievements_ui.mm); a reward for one unlocked earlier, or one
// in a chest, gets a notice of its own.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "achievements.h"
#include "framepace.h"
#include "items.h"
#include "rt.h"

#define REWARDS_FILE "achievement-rewards.txt"  // (the data root is the working directory)
#define MAX_PIECES 4
#define RETRY_S 20

typedef struct {
    const char *id;                    // the achievement (achievements_defs.c)
    const char *actors[MAX_PIECES];    // its items, one of each
} AchReward;

static const AchReward k_rewards[] = {
    {"runes", {"Armor_210_Head", "Armor_210_Upper", "Armor_210_Lower"}},        // Cap, Tunic, Trousers of Twilight
    {"map_towers", {"Armor_220_Head"}},                                        // Sheik's Mask
    // each Divine Beast's helm, the Champion's figure's own
    {"beast_ruta", {"Armor_181_Head"}},                                        // Vah Ruta Divine Helm
    {"beast_rudania", {"Armor_183_Head"}},                                     // Vah Rudania Divine Helm
    {"beast_naboris", {"Armor_184_Head"}},                                     // Vah Naboris Divine Helm
    {"beast_medoh", {"Armor_182_Head"}},                                       // Vah Medoh Divine Helm
    {"master_sword", {"Weapon_Sword_057"}},                                    // Goddess Sword
    {"old_memories", {"Armor_215_Head", "Armor_215_Upper", "Armor_215_Lower"}},  // Cap, Tunic, Trousers of the Sky
    {"hearts_20", {"Weapon_Sword_059"}},                                       // Sea-Breeze Boomerang
    {"shrine_60", {"Weapon_Lsword_057"}},                                      // Sword of the Six Sages
    {"eventide", {"Armor_205_Head", "Armor_205_Upper", "Armor_205_Lower"}},    // Cap, Tunic, Trousers of the Wind
    {"lynel_all", {"Weapon_Bow_072"}},                                         // Twilight Bow
    {"memories", {"Armor_200_Head", "Armor_200_Upper", "Armor_200_Lower"}},    // Cap, Tunic, Trousers of Time
    {"cook_100", {"Weapon_Lsword_059"}},                                       // Biggoron's Sword
    {"shrine_120", {"Armor_230_Head", "Armor_230_Upper", "Armor_230_Lower"}},  // Cap, Tunic, Trousers of the Hero
    {"dex_creatures", {"Weapon_Lsword_060"}},                                  // Fierce Deity Sword
    {"korok_900", {"Weapon_Sword_058"}},                                       // Sword (the 8-bit one)
    {"master_mode", {"Armor_225_Head", "Armor_225_Upper", "Armor_225_Lower"}},  // Fierce Deity Mask, Armor, Boots
};
#define NREWARDS ((int)(sizeof k_rewards / sizeof k_rewards[0]))

static const AchReward *reward_of(int index) {
    if (index < 0 || index >= g_ach_count) return NULL;
    for (int r = 0; r < NREWARDS; r++)
        if (!strcmp(k_rewards[r].id, g_ach_defs[index].id)) return &k_rewards[r];
    return NULL;
}

bool ach_has_reward(int index) { return reward_of(index) != NULL; }


size_t ach_reward_text(int index, char *out, size_t n) {
    const AchReward *r = reward_of(index);
    if (n) out[0] = 0;
    if (!r || !n || !items_count()) return 0;
    size_t w = 0;
    for (int p = 0; p < MAX_PIECES && r->actors[p]; p++) {
        const GameItem *it = items_by_actor(r->actors[p]);
        if (!it) continue;
        int k = snprintf(out + w, n - w, "%s%s", w ? ", " : "", it->name);
        if (k < 0 || (size_t)k >= n - w) break;
        w += (size_t)k;
    }
    return w;
}

// ---- what was given, for which save (achievement-rewards.txt)
typedef struct { char key[512]; char id[48]; } Given;
static Given *g_given;
static int g_ngiven, g_capgiven;
static bool g_loaded;

static bool given(const char *key, const char *id) {
    for (int i = 0; i < g_ngiven; i++)
        if (!strcmp(g_given[i].id, id) && !strcmp(g_given[i].key, key)) return true;
    return false;
}

static void remember(const char *key, const char *id) {
    if (g_ngiven == g_capgiven) {
        int cap = g_capgiven ? g_capgiven * 2 : 32;
        Given *g = realloc(g_given, sizeof *g * cap);
        if (!g) return;
        g_given = g, g_capgiven = cap;
    }
    Given *g = &g_given[g_ngiven++];
    snprintf(g->key, sizeof g->key, "%s", key);
    snprintf(g->id, sizeof g->id, "%s", id);
    FILE *f = fopen(REWARDS_FILE, "ab");
    if (f) fprintf(f, "%s\t%s\n", key, id), fclose(f);
    else LOG("[achievements] can't write %s: the reward of %s may be given again\n", REWARDS_FILE, id);
}

static void load_given(void) {
    g_loaded = true;
    FILE *f = fopen(REWARDS_FILE, "rb");
    if (!f) return;
    char line[700];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *tab = strrchr(line, '\t');
        if (!tab || tab == line || !tab[1]) continue;
        *tab = 0;
        if (g_ngiven == g_capgiven) {
            int cap = g_capgiven ? g_capgiven * 2 : 32;
            Given *g = realloc(g_given, sizeof *g * cap);
            if (!g) break;
            g_given = g, g_capgiven = cap;
        }
        size_t ln = strlen(line), sfx = strlen("/game_data.sav");  // (a slot's line, from before: its playthrough)
        if (ln > sfx && !strcmp(line + ln - sfx, "/game_data.sav"))
            ach_save_run(line, g_given[g_ngiven].key, sizeof g_given[g_ngiven].key);
        else
            snprintf(g_given[g_ngiven].key, sizeof g_given[g_ngiven].key, "%s", line);
        snprintf(g_given[g_ngiven].id, sizeof g_given[g_ngiven].id, "%s", tab + 1);
        g_ngiven++;
    }
    fclose(f);
}

// ---- the rewards on their way, for the save being played (the item get runs on the game thread's next frame in play)
static struct {
    char key[512];                         // the save they are for (another save: all start over)
    unsigned seq[NREWARDS][MAX_PIECES];    // each piece's request (0: none asked now)
    bool got[NREWARDS][MAX_PIECES];        // each piece in the pouch already
    time_t retry_at[NREWARDS];             // pieces that found no room: asked again then (0: not waiting)
    bool told_waiting[NREWARDS];           // the "no room" notice was shown (once per reward and save played)
    bool in_chest[NREWARDS];               // the pieces with no room went into chests in front of Link
} cur;

// Asks for reward r's pieces not in the pouch yet; false if the queue was full (asked again on the next tick)
static bool ask(int r) {
    const AchReward *rw = &k_rewards[r];
    for (int p = 0; p < MAX_PIECES && rw->actors[p]; p++) {
        if (cur.got[r][p] || cur.seq[r][p]) continue;
        if (!items_by_actor(rw->actors[p])) {  // (not among this game's items: counted as given, so the rest still comes)
            LOG("[achievements] reward %s: no item %s in this game\n", rw->id, rw->actors[p]);
            cur.got[r][p] = true;
            continue;
        }
        if (!(cur.seq[r][p] = items_request_actor(rw->actors[p], 1))) return false;
    }
    return true;
}

// Reward r's pieces asked for: true once each has its result (*full: one found no room in the pouch)
static bool settled(int r, bool *missing, bool *full) {
    const AchReward *rw = &k_rewards[r];
    bool asked = false;
    *missing = *full = false;
    for (int p = 0; p < MAX_PIECES && rw->actors[p]; p++) {
        ItemResult res;
        if (cur.got[r][p]) continue;
        if (!cur.seq[r][p]) { *missing = true; continue; }
        asked = true;
        if (!items_result_of(cur.seq[r][p], &res)) return false;
        cur.seq[r][p] = 0;
        if (res.added > 0) cur.got[r][p] = true;
        else *missing = true, *full |= res.item >= 0;  // (no room in the pouch; item -1: dropped by a reboot)
    }
    return asked || *missing;
}

void ach_rewards_tick(void) {
    static int ach_of[NREWARDS];
    static bool init;
    if (!init) {
        for (int r = 0; r < NREWARDS; r++) {
            ach_of[r] = -1;
            for (int i = 0; i < g_ach_count; i++)
                if (!strcmp(g_ach_defs[i].id, k_rewards[r].id)) ach_of[r] = i;
        }
        init = true;
    }
    if (!items_available() || !items_count()) return;
    if (!g_loaded) load_given();
    char slot[512], key[512];
    bool play = ach_play_save(slot, sizeof slot) && !framepace_title_ui();
    if (play) ach_save_run(slot, key, sizeof key);
    static int why_logged = -1;  // (the log says once what holds the rewards back, each time that changes)
    int why = !play ? (framepace_title_ui() ? 1 : 2) : 0;
    if (why != why_logged) {
        LOG("[achievements] rewards: %s\n", why == 1 ? "on the title screen" : why == 2 ? "no save in play yet" : "a save in play");
        why_logged = why;
    }
    if (!play) return;
    if (strcmp(key, cur.key)) {  // (another save: what was on its way to the last one is its own; this one starts over)
        memset(&cur, 0, sizeof cur);
        snprintf(cur.key, sizeof cur.key, "%s", key);
    }
    time_t now = time(NULL);
    for (int r = 0; r < NREWARDS; r++) {
        if (ach_of[r] < 0 || given(key, k_rewards[r].id) || !ach_met_in_play(ach_of[r])) continue;
        const AchReward *rw = &k_rewards[r];
        bool any_asked = false;
        for (int p = 0; p < MAX_PIECES && rw->actors[p]; p++) any_asked |= cur.seq[r][p] != 0;
        if (any_asked) {  // on its way: its results
            bool missing, full;
            if (!settled(r, &missing, &full)) continue;
            if (!missing) {
                remember(key, rw->id);
                LOG("[achievements] reward of %s given%s\n", rw->id, cur.in_chest[r] ? " (in a chest)" : "");
                if (cur.in_chest[r]) ach_toast_push(ACH_TOAST_CHEST + ach_of[r]);
                else if (!ach_announced(ach_of[r])) ach_toast_push(ACH_TOAST_REWARD + ach_of[r]);  // (its own notice didn't show it)
                continue;
            }
            // No room in the pouch: the pieces that didn't fit go in golden chests in front of Link, and the player
            // chooses whether to take them (the game's own chest: with the pouch full it offers to drop something)
            if (full && !cur.in_chest[r] && items_chests_available()) {
                cur.in_chest[r] = true;
                int place = 0;
                for (int p = 0; p < MAX_PIECES && rw->actors[p]; p++) {
                    if (!cur.got[r][p] && items_by_actor(rw->actors[p])) cur.seq[r][p] = items_request_chest(rw->actors[p], place++);
                }
                LOG("[achievements] reward of %s: no room in the pouch, %d in chests in front of Link\n", rw->id, place);
                continue;
            }
            if (full && !cur.told_waiting[r]) {
                LOG("[achievements] reward of %s: no room in the pouch, asked again every %d s\n", rw->id, RETRY_S);
                ach_toast_push(ACH_TOAST_WAITING + ach_of[r]), cur.told_waiting[r] = true;
            }
            cur.retry_at[r] = now + (full ? RETRY_S : 1);
            continue;
        }
        if (cur.retry_at[r] && now < cur.retry_at[r]) continue;
        cur.retry_at[r] = 0;
        bool first = true;
        for (int p = 0; p < MAX_PIECES && rw->actors[p]; p++) first &= !cur.got[r][p];
        if (!ask(r)) return;  // (the queue is full: next tick)
        bool all = true, asked = false;
        for (int p = 0; p < MAX_PIECES && rw->actors[p]; p++) all &= cur.got[r][p], asked |= cur.seq[r][p] != 0;
        if (all && !asked) {  // (nothing to ask for: none of its items is in this game)
            remember(key, rw->id);
            continue;
        }
        if (first && !cur.told_waiting[r]) LOG("[achievements] reward of %s on its way\n", rw->id);
    }
}
