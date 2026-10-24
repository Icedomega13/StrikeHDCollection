/*
 * Minimal immediate-mode drawing for the HD menus: crisp TrueType text
 * (Chakra Petch, baked per window size with stb_truetype), panels, and
 * simple shapes. Layout uses a virtual 1280x720 canvas scaled uniformly
 * and centred in the window, so menus look the same at any resolution.
 */
#ifndef DS_UI_H
#define DS_UI_H
#include <SDL3/SDL.h>

typedef enum { UI_SMALL, UI_BODY, UI_LARGE, UI_TITLE, UI_FONT_COUNT } ui_font;

#define UI_W 1280.0f
#define UI_H 720.0f

void  ui_init(SDL_Renderer *r);
void  ui_shutdown(void);
/* Call once per frame before drawing; output size in pixels. */
void  ui_begin(int out_w, int out_h);

/* Text: (x, y) is the top-left in virtual units. Returns the advance width. */
float ui_text(ui_font f, float x, float y, SDL_FColor c, const char *s);
float ui_text_w(ui_font f, const char *s);
float ui_line_h(ui_font f);
void  ui_text_center(ui_font f, float cx, float y, SDL_FColor c, const char *s);
void  ui_text_right(ui_font f, float rx, float y, SDL_FColor c, const char *s);

void  ui_rect(float x, float y, float w, float h, SDL_FColor c);
void  ui_frame(float x, float y, float w, float h, float t, SDL_FColor c);   /* outline, thickness t */
/* Panel with two cut corners (top-left, bottom-right), the menu's look. */
void  ui_panel(float x, float y, float w, float h, float cut, SDL_FColor fill, SDL_FColor edge);
void  ui_triangle(float x0, float y0, float x1, float y1, float x2, float y2, SDL_FColor c);
void  ui_image(SDL_Texture *t, float x, float y, float w, float h);
void  ui_dim(float alpha);                   /* darken the whole output */

/* Virtual <-> pixel conversion (for mouse input). */
void  ui_to_virtual(float px, float py, float *vx, float *vy);
void  ui_virtual_rect(float x, float y, float w, float h, SDL_FRect *out);   /* virtual units -> pixels */

static inline SDL_FColor ui_rgba(int r, int g, int b, int a)
{
    SDL_FColor c = { r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f };
    return c;
}

#endif
