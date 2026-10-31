// Readers for the archives inside the game's files (runtime/services/archive.c): Yaz0 compression and SARC. Used by
// fs.c (the HUD letters, the ResourceSizeTable) and the save converter (save_convert.c).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The data of a Yaz0 stream, malloc'd (free it), its size in *out_n; NULL if it isn't one or is damaged
uint8_t *yaz0_unpack(const uint8_t *d, size_t n, size_t *out_n);
// SARC (little-endian): the node of `name` (its index; start and end in the data, which begins at *data_off), or -1
int sarc_find(const uint8_t *d, size_t n, const char *name, uint32_t *data_off, uint32_t *start, uint32_t *end);
// How many nodes the SARC has, or -1 if it isn't a (little-endian) one
int sarc_count(const uint8_t *d, size_t n);
// Node i: its name without a leading '/' (NULL when the archive keeps only the name's hash) and its data inside d
bool sarc_node(const uint8_t *d, size_t n, int i, const char **name, const uint8_t **data, size_t *size);

#ifdef __cplusplus
}
#endif
