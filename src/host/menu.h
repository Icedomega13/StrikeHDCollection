/*
 * The HD pause menu: Resume, Save/Load State (with thumbnails), Settings
 * (display, picture, audio, system), Controls (full rebinding), Restart and
 * Quit. Opened with Esc or the gamepad's Back/View button. Built for
 * gamepads first; keyboard and mouse work too.
 */
#ifndef DS_MENU_H
#define DS_MENU_H
#include <SDL3/SDL.h>

#define MENU_SLOTS 9                 /* slot 0 = quick save (F5/F7), 1-8 */

typedef struct {
    const char *game_title;
    void (*save_slot)(int slot);
    int  (*load_slot)(int slot);                        /* 1 = loaded */
    /* Fill in what is known about a slot. Returns 1 if a state exists. The
     * texture (160x112) is owned by the host. */
    int  (*slot_info)(int slot, char *when, size_t n, SDL_Texture **thumb);
    void (*apply_settings)(void);                       /* g_set changed */
    void (*save_controls)(void);                        /* g_bind changed */
    void (*restart)(void);
    void (*switch_game)(void);                          /* back to the game selector */
    void (*quit)(void);
    /* Exclusive fullscreen modes for the window's display. */
    int  (*mode_count)(void);
    const char *(*mode_name)(int i);
    void (*open_settings_folder)(void);
    void (*make_hd_pack)(void);
    int  (*campaign_count)(void);                       /* campaigns of the running game */
    void (*restart_campaign)(void);                     /* restart at g_cheat.campaign */                         /* starter pack from the dump */
} menu_host;

void menu_init(const menu_host *h);
void menu_open(void);
void menu_open_launcher(void);     /* from the game picker: Settings, Controls, Back */
void menu_close(void);
int  menu_is_open(void);
void menu_event(const SDL_Event *e);
void menu_update(void);              /* once per displayed frame while open */
void menu_draw(int out_w, int out_h);

#endif
