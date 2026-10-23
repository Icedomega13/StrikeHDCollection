/*
 * Career statistics per game, kept in stats.ini in the settings folder.
 * Counted by the host from the game's own status values (see main.c,
 * watch_frame), so they need no help from the game code.
 */
#ifndef DS_STATS_H
#define DS_STATS_H

typedef struct {
    double play_seconds;        /* time flying (the playfield on screen) */
    int missions;               /* campaigns started */
    int best_campaign;          /* highest campaign reached (1-based) */
    int objectives;             /* mission objectives completed */
    int shots[3];               /* guns, hydras, hellfires fired */
    int damage;                 /* armour lost */
    int lives_lost;
    int picked_up;              /* passengers / cargo winched aboard */
    int delivered;              /* passengers / cargo unloaded */
} game_stats;

int  stats_load(const char *path);
int  stats_save(const char *path);
game_stats *stats_for(const char *game_id);   /* never NULL for a known id */

#endif
