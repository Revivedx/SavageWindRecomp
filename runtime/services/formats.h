// Readers for the data files inside the game's archives (runtime/services/formats.c): BYML (the game's data) and MSBT
// (its text). Yaz0 and SARC: archive.h. Used by the item list (runtime/items.c) and Cheats > Collectible hints on the
// map (runtime/collectibles_table.c).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- BYML (little-endian "YB", version 2 and up): a node is an offset in the data; a value is a string's index
// (BYML_STR), a node (BYML_ARRAY, BYML_DICT) or the number itself (its bits for a float)
enum { BYML_STR = 0xa0, BYML_ARRAY = 0xc0, BYML_DICT = 0xc1, BYML_BOOL = 0xd0, BYML_INT = 0xd1, BYML_FLOAT = 0xd2,
       BYML_UINT = 0xd3 };
typedef struct {
    const uint8_t *d;
    size_t n;
    uint32_t keys, strs, root;  // the string tables and the root node
} Byml;

// False if d isn't a BYML file
bool byml_open(Byml *b, const uint8_t *d, size_t n);
// How many entries an array or a dictionary has
uint32_t byml_count(const Byml *b, uint32_t node);
// Entry i of an array or a dictionary: its type and value (and in a dictionary its key, when key isn't NULL)
bool byml_at(const Byml *b, uint32_t node, uint32_t i, uint8_t *type, uint32_t *value, const char **key);
// Entry `key` of the dictionary at `node`: its type and value; false if it has none
bool byml_get(const Byml *b, uint32_t node, const char *key, uint8_t *type, uint32_t *value);
// A string by its index (a BYML_STR value), NULL if there is none
const char *byml_str(const Byml *b, uint32_t i);
const char *byml_get_str(const Byml *b, uint32_t node, const char *key);
int byml_get_int(const Byml *b, uint32_t node, const char *key, int def);  // (an int or a uint)
bool byml_get_float(const Byml *b, uint32_t node, const char *key, float *out);
bool byml_at_float(const Byml *b, uint32_t node, uint32_t i, float *out);

// ---- MSBT (little-endian): each message, its label and its text as UTF-8 (control tags left out, a line break kept
// as '\n', cut off at 255 bytes), in the file's order of labels
void msbt_each(const uint8_t *d, size_t n, void (*fn)(void *ctx, const char *label, size_t label_len, const char *text),
               void *ctx);

#ifdef __cplusplus
}
#endif
