// Discord Rich Presence, through the local pipe of the Discord app (Windows; elsewhere nothing happens). Its own thread
// does all the talking: with Discord closed the game loses nothing, and it connects whenever Discord opens.
// Off by default: the switch is Options > Gameplay (savagewind.ini discord_rpc), and SAVAGEWIND_DISCORD=0|1 forces it
// either way. The Discord application comes from the environment's SAVAGEWIND_DISCORD_APP_ID (digits): without one the
// feature does nothing, as a build ships no application of its own until the project has one.
// SAVAGEWIND_DISCORD_LANG: en, es, USen, EUde...
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The two lines of the player's status (state may be empty). Never blocks; only a change goes to Discord.
void discord_rpc_set(const char *details, const char *state);

// (the messages, on their own for tests/test_discord_rpc.c)
bool discord_app_id_valid(const char *id);
int discord_activity_json(char *out, size_t size, long pid, uint64_t start, const char *details, const char *state, unsigned nonce);
int discord_frame(uint8_t *out, size_t size, uint32_t op, const char *json);
