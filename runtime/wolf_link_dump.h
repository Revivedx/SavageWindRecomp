#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
enum { WOLF_REGISTERED = 0x10, WOLF_HAS_AREA = 0x20 };
typedef enum {
    WOLF_DUMP_OK, WOLF_DUMP_MISSING, WOLF_DUMP_INVALID, WOLF_DUMP_NOT_WOLF,
    WOLF_KEYS_MISSING, WOLF_KEYS_INVALID, WOLF_AUTH_FAILED, WOLF_DUMP_MEMORY,
    WOLF_SAVE_INVALID
} WolfLinkStatus;
typedef struct {
    uint8_t uid[7], flags;
    uint16_t nickname[10];
    uint32_t app_id;
    uint8_t area[0xd8];
} WolfLinkDump;
WolfLinkStatus wolf_link_dump_read(const char *dump, const char *keys, WolfLinkDump *out);
WolfLinkStatus wolf_link_dump_load(WolfLinkDump *out);
bool wolf_link_dump_save(const WolfLinkDump *tag);
const char *wolf_link_dump_status_text(WolfLinkStatus status);
void wolf_link_dump_nickname(const WolfLinkDump *tag, char out[41]);
#ifdef __cplusplus
}
#endif
