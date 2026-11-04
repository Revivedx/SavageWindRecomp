#include <assert.h>
#include <stdio.h>
#include <string.h>
#define DISCORD_RPC_TEST
#include "../runtime/discord_rpc.c"

int main(void) {
    char j[1024];

    // Escaped text
    assert(discord_activity_json(j, sizeof j, 42, 1000, "Isla \"Eventide\"\\", "", 7) > 0);
    assert(strstr(j, "\"details\":\"Isla \\\"Eventide\\\"\\\\\""));
    assert(!strstr(j, "\"state\""));  // (Discord refuses an empty state)
    assert(strstr(j, "\"pid\":42"));
    assert(strstr(j, "\"start\":1000"));
    assert(strstr(j, "\"nonce\":\"7\""));
    assert(strstr(j, "\"cmd\":\"SET_ACTIVITY\""));

    // UTF-8 goes through as it is
    assert(discord_activity_json(j, sizeof j, 1, 1, "Aldea Kakariko", "Cañón", 1) > 0);
    assert(strstr(j, "\"state\":\"Cañón\""));

    // Too small a buffer fails rather than cuts
    assert(discord_activity_json(j, 16, 1, 1, "Aldea Kakariko", "Necluda", 1) < 0);

    // The frame: op and length, little endian, then the text
    uint8_t f[64];
    int n = discord_frame(f, sizeof f, 1, "{}");
    assert(n == 10);
    assert(f[0] == 1 && f[1] == 0 && f[4] == 2 && f[5] == 0);
    assert(f[8] == '{' && f[9] == '}');
    assert(discord_frame(f, 9, 1, "{}") < 0);

    // The id: digits only
    assert(discord_app_id_valid("1566877612024791185"));
    assert(!discord_app_id_valid(""));
    assert(!discord_app_id_valid("12ab"));

    puts("PASS: discord_rpc");
}
