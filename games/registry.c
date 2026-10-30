#include <string.h>
#include "game.h"
#include "util/sha1.h"

/* CMake defines DS_GAME_<ID> for each game it builds (the ones whose
 * generated/<id>/ sources exist). */
#if !DS_GAME_DESERT && !DS_GAME_JUNGLE && !DS_GAME_URBAN
#error "No game enabled: define DS_GAME_DESERT, DS_GAME_JUNGLE and/or DS_GAME_URBAN"
#endif

extern const game_module game_desert, game_jungle, game_urban;

const game_module *const g_games[] = {
#if DS_GAME_DESERT
    &game_desert,
#endif
#if DS_GAME_JUNGLE
    &game_jungle,
#endif
#if DS_GAME_URBAN
    &game_urban,
#endif
};
const int g_game_count = (int)(sizeof g_games / sizeof g_games[0]);

const game_module *game_find_sha1(const char *sha1)
{
    for (int i = 0; i < g_game_count; i++)
        if (!strcmp(g_games[i]->rom_sha1, sha1)) return g_games[i];
    return 0;
}

const game_module *game_find_id(const char *id)
{
    for (int i = 0; i < g_game_count; i++)
        if (!strcmp(g_games[i]->id, id)) return g_games[i];
    return 0;
}

const game_module *game_identify_rom(void)
{
    char h[41];
    sha1_hex(g_rom, g_rom_size, h);
    return game_find_sha1(h);
}

void game_boot(const game_module *g)
{
    cpu_lookup = g->lookup;
    gen_reset();
    g_ws_flight_frame = 0;
    cpu_reset();
}
