/* Career statistics. See stats.h. */
#include "stats.h"
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#define MAX_IDS 8
static char ids[MAX_IDS][16];
static game_stats st[MAX_IDS];
static int nids;

game_stats *stats_for(const char *id)
{
    for (int i = 0; i < nids; i++) if (!strcmp(ids[i], id)) return &st[i];
    if (nids == MAX_IDS) return &st[MAX_IDS - 1];
    SDL_strlcpy(ids[nids], id, sizeof ids[nids]);
    memset(&st[nids], 0, sizeof st[nids]);
    return &st[nids++];
}

#define FIELDS(X) X(missions) X(best_campaign) X(objectives) X(damage) X(lives_lost) X(picked_up) X(delivered)

int stats_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    game_stats *s = NULL;
    while (fgets(line, sizeof line, f)) {
        char key[64], id[32];
        double v;
        if (sscanf(line, " [%31[^]]]", id) == 1) { s = stats_for(id); continue; }
        if (!s || sscanf(line, " %63[a-z_0-9] = %lf", key, &v) != 2) continue;
        if (!strcmp(key, "play_seconds")) s->play_seconds = v;
        else if (!strcmp(key, "shots_guns")) s->shots[0] = (int)v;
        else if (!strcmp(key, "shots_hydras")) s->shots[1] = (int)v;
        else if (!strcmp(key, "shots_hellfires")) s->shots[2] = (int)v;
#define X(n) else if (!strcmp(key, #n)) s->n = (int)v;
        FIELDS(X)
#undef X
    }
    fclose(f);
    return 1;
}

int stats_save(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "# Strike Collection HD career statistics\n");
    for (int i = 0; i < nids; i++) {
        const game_stats *s = &st[i];
        fprintf(f, "\n[%s]\nplay_seconds = %.0f\n", ids[i], s->play_seconds);
        fprintf(f, "shots_guns = %d\nshots_hydras = %d\nshots_hellfires = %d\n", s->shots[0], s->shots[1], s->shots[2]);
#define X(n) fprintf(f, #n " = %d\n", s->n);
        FIELDS(X)
#undef X
    }
    fclose(f);
    return 1;
}
