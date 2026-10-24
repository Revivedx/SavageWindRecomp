// Extracted add-on RomFS images, kept separate from the base game's overlay.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Called before guest threads start; rebuild the catalog on every boot.
void dlc_init(const char *game_root, uint64_t program_id);
uint64_t dlc_base_id(void);
uint32_t dlc_count(void);
uint32_t dlc_list(uint32_t offset, uint32_t count, int32_t *indices);
// The catalog owns the descriptor until reset. Callers must not close it.
bool dlc_storage(uint64_t title_id, int *fd, uint64_t *size);
bool dlc_present(int32_t index);
