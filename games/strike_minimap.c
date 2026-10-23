/*
 * Minimap data for the Strike engine. See strike_engine.h and
 * docs/11-minimap.md.
 *
 * The pause map lists the mission objectives followed by categories (enemy
 * types, MIAs, fuel, ammo, landing zones). Its icon builder marks:
 *   - for every available, unfinished objective n: the objects and units
 *     whose group byte is n (times a per-game factor), always;
 *   - for the selected category: every object or unit of that type.
 * Both unit lists are searched like the game does, with its liveness checks,
 * and like the game at most one icon per 512 x 512 px cell and 64 per entry.
 */
#include "strike_engine.h"
#include "hw/genesis.h"

static inline uint32_t mr8(uint32_t a)
{
    a &= 0xFFFFFF;
    if (a >= 0xFF0000) return g_ram[a & 0xFFFF];
    return a < g_rom_size ? g_rom[a] : 0;
}
static inline uint32_t mr16(uint32_t a) { return mr8(a) << 8 | mr8(a + 1); }
static inline uint32_t mptr(uint32_t a) { return (mr16(a) << 16 | mr16(a + 2)) & 0xFFFFFF; }
static inline int ms16(uint32_t a) { return (int16_t)mr16(a); }
static inline int ms8(uint32_t a) { return (int8_t)mr8(a); }
static inline int iabs(int v) { return v < 0 ? -v : v; }

typedef struct {
    const strike_mm_cfg *c;
    int group;          /* > 0: match the group byte; 0: match `type` */
    int type;
    int flags;          /* entry flags (+$A): bit 0 alive check, bit 1 extra filter */
    game_minimap *o;
    int obj, main, found;
    uint32_t cells[64];
    int ncells;
} search;

static void add(search *s, int x, int y)
{
    if (s->found >= 64) return;
    s->found++;
    uint32_t key = (uint32_t)((y >> 9) & 0xFFFF) << 16 | (uint32_t)((x >> 9) & 0xFFFF);
    for (int i = 0; i < s->ncells; i++) if (s->cells[i] == key) return;
    s->cells[s->ncells++] = key;
    game_minimap *o = s->o;
    if (o->n >= GAME_MM_MAX) return;
    game_mm_mark *m = &o->m[o->n++];
    m->x = (int16_t)x; m->y = (int16_t)y;
    m->obj = (uint8_t)s->obj; m->main = (uint8_t)s->main;
}

/* Desert: entries in `carried` that point at unit a2 hide it unless their
 * bit 0 is set ($0043BC). */
static int desert_carried_ok(const strike_mm_cfg *c, uint32_t a2)
{
    uint32_t a1 = mptr(c->carried);
    for (int guard = 0; a1 && guard < 256; guard++, a1 = mptr(a1)) {
        uint32_t a0 = mptr(a1 + 8);
        if (!a0) continue;
        if (mptr(a0 + 8) != a2) continue;
        return (mr8(a1 + 0xC) & 1) != 0;
    }
    return 1;
}

static void search_desert(search *s)
{
    const strike_mm_cfg *c = s->c;
    int n1 = ms16(c->list1_n), n2 = ms16(c->list2_n);
    for (int k = 0; k < n1 && k < 1024; k++) {            /* $0042E6 */
        uint32_t a = c->list1 + (uint32_t)k * 0x28;
        if (mr8(a + 0x19) & 2) continue;
        uint32_t a5 = mptr(a + 0x10);
        if (s->group) {
            int g = iabs(ms8(a5 + 0x16));
            if (!g || g != s->group || ms8(a5 + 0x17) <= 0) continue;
        } else if (ms16(mptr(a5 + 0x22) + 0x10) != s->type) continue;
        if (ms16(a + 0x1A) <= 0) continue;
        add(s, ms16(a5 + 0x12), ms16(a5 + 0x10));
    }
    for (int k = 0; k < n2 && k < 1024; k++) {            /* $004366 */
        uint32_t a = c->list2 - (uint32_t)k * 0x28;
        if ((s->flags & 1) && ms16(a + 0xC) <= 0) continue;
        uint32_t a5 = mptr(a);
        if (s->group) {
            int g = iabs(ms8(a5 + 0x16));
            if (!g || g != s->group || ms8(a5 + 0x17) <= 0) continue;
        } else if (ms16(mptr(a5) + 0xA) != s->type) continue;
        if ((s->flags & 2) && !desert_carried_ok(c, a)) continue;
        add(s, ms16(a5 + 0x10), ms16(a5 + 0xE));
    }
}

static void search_jungle(search *s)
{
    const strike_mm_cfg *c = s->c;
    int n1 = ms16(c->list1_n), n2 = ms16(c->list2_n);
    for (int k = 0; k < n1 && k < 1024; k++) {            /* $039E12 */
        uint32_t a = c->list1 + (uint32_t)k * 0x1E;
        int f = (int)mr8(a + 0x19);
        if (f & 0x20) continue;
        if ((s->flags & 2) && (f & 0x68) != 0x48) continue;
        uint32_t a5 = mptr(a + 0xC);
        if (s->group) {
            int g = iabs(ms8(a5 + 0x18));
            if (!g || g != s->group) continue;
        } else if (ms16(mptr(a5 + 0x1A) + 0x10) != s->type) continue;
        add(s, ms16(a5 + 0x12), ms16(a5 + 0x10));
    }
    uint32_t a = c->list2;
    for (int k = 0; k < n2 && k < 1024; k++) {            /* $039EA0 */
        int f = (int)mr8(a + 8), big = (mr8(a + 9) & 0x80) != 0;
        uint32_t next = a - (big ? 0x2A : 0xE);
        if (f & 0x20) { a = next; continue; }
        if ((s->flags & 2) && (f & 0x68) != 0x48) { a = next; continue; }
        uint32_t a5 = mptr(a);
        if (s->group) {
            int g = iabs(ms8(a5 + 0x17));
            if (!g || g != s->group) { a = next; continue; }
        } else if (ms16(mptr(a5) + 0xA) != s->type) { a = next; continue; }
        int x = ms16(a5 + 0xE), y = ms16(a5 + 0xC);
        if (big) {
            x = ms16(a + 0xE); y = ms16(a + 0x12);
            uint32_t v = mptr(a + 4);
            if (v) { uint32_t p = mptr(v + 0x22); x = ms16(p); y = ms16(p + 4); }
        }
        add(s, x, y);
        a = next;
    }
}

static void run_search(search *s)
{
    s->found = 0;
    s->ncells = 0;
    if (s->c->kind == 0) search_desert(s);
    else search_jungle(s);
}

/* Objectives available now (bit n-1 = objective n). */
static uint32_t unlocked_mask(const strike_mm_cfg *c)
{
    if (!c->mask_from_tab) return mr16(c->unlocked) << 16 | mr16(c->unlocked + 2);
    uint32_t d1 = 0;                          /* $03A9C6 */
    for (int i = 0; i < 8; i++) if (ms16(c->done_tab + (uint32_t)i * 2) >= 0) d1 |= 1u << i;
    if (c->mask_from_tab == 2) {
        if ((int8_t)mr16(c->campaign) != 3) return 0xFFFFFFFFu;
    } else {
        uint32_t lvl = mptr(c->level_ptr);
        if (!(mr8(lvl + 0x22) & 2)) return 0xFFFFFFFFu;
    }
    for (int i = 0; i <= 7; i++) if (!(d1 >> i & 1)) { d1 |= 1u << i; break; }
    return d1;
}

/* Entry e of the list: 0 normal; 1 locked (not yet given: the pause map
 * shows "N. ?"); 2 location unknown (a "?" icon instead of the units). */
static int entry_state(const strike_mm_cfg *c, uint32_t e)
{
    int prereq = (int)mr8(e + 8), hid = ms8(e + 9);
    if (prereq && prereq <= 32 && !(unlocked_mask(c) >> (prereq - 1) & 1)) return 1;
    if (hid > 0 && ms16(c->done_tab + (uint32_t)(hid - 1) * 2) < 0) return 2;
    return 0;
}

static uint32_t table(const strike_mm_cfg *c, int *count)
{
    uint32_t tab = c->tab_rom ? 0 : mptr(c->obj_tab);   /* the RAM copy can be stale */
    if (c->tab_rom) {
        int camp = (int8_t)mr16(c->campaign);
        if (camp >= 0 && camp < 16) tab = mptr(c->tab_rom + (uint32_t)camp * 4);
    }
    int n = tab ? ms16(tab) : 0;
    if (tab < 0x200 || n <= 0 || n > 32) return 0;
    *count = n;
    return tab;
}

int strike_minimap(const strike_mm_cfg *c, game_minimap *o)
{
    o->n = 0;
    o->have_player = o->have_home = 0;
    uint32_t lvl = mptr(c->level_ptr);
    int count;
    uint32_t tab = table(c, &count);
    if (!lvl || !tab) return 0;
    o->level = lvl;
    o->count = count;
    o->sel = ms16(c->sel);
    if (o->sel < 0 || o->sel >= count) o->sel = 0;

    search s = { 0 };
    s.c = c;
    s.o = o;
    for (int i = 0; i < count; i++) {
        uint32_t e = tab + 2 + (uint32_t)i * 12;
        int st = entry_state(c, e);
        if (st) continue;
        s.obj = i;
        s.flags = (int)mr8(e + 10);
        s.main = mr8(e + 8) != 0;
        if (s.main) {                        /* mission objective: its group, always */
            s.group = (i + 1) * c->group_mul;
            run_search(&s);
        }
        if (i == o->sel) {                   /* selected entry: units of its type */
            s.group = 0;
            s.type = ms8(mptr(e));
            run_search(&s);
        }
    }

    uint32_t p = mptr(c->player_ptr);
    if (p >= 0xFF0000) {
        uint32_t pos = mptr(p + c->pos_off);
        if (pos >= 0xFF0000) { o->have_player = 1; o->px = ms16(pos); o->py = ms16(pos + 4); }
    }
    if (c->home_ptr && (ms16(c->home_flag1) || (c->home_flag2 && ms16(c->home_flag2)))) {
        uint32_t h = mptr(c->home_ptr);
        if (h >= 0xFF0000) { o->have_home = 1; o->hx = ms16(h + 4); o->hy = ms16(h); }
    }
    int miny = ms16(c->ground_rec + 0x30), minx = ms16(c->ground_rec + 0x32);
    int maxy = ms16(c->ground_rec + 0x34), maxx = ms16(c->ground_rec + 0x36);
    if (maxx < minx || maxy < miny) return 0;
    o->shift = 5;
    o->campaign = c->campaign ? (int8_t)mr16(c->campaign) + 1 : 0;
    if (c->shift_rom) {
        int camp = (int8_t)mr16(c->campaign), s = camp >= 0 && camp < 16 ? ms16(c->shift_rom + (uint32_t)camp * 2) : 5;
        if (s >= 2 && s <= 8) o->shift = s;
    }
    o->x0 = minx; o->y0 = miny;
    o->x1 = maxx + 320; o->y1 = maxy + 224;
    return 1;
}

void strike_minimap_select(const strike_mm_cfg *c, int index)
{
    int count;
    if (!table(c, &count)) return;
    if (index < 0 || index >= count) return;
    g_ram[c->sel & 0xFFFF] = (uint8_t)(index >> 8);
    g_ram[(c->sel + 1) & 0xFFFF] = (uint8_t)index;
}

int strike_minimap_name(const strike_mm_cfg *c, int index, char *buf, int n)
{
    int count;
    uint32_t tab = table(c, &count);
    if (!tab || index < 0 || index >= count || n < 8) { if (n) buf[0] = 0; return -1; }
    uint32_t e = tab + 2 + (uint32_t)index * 12;
    uint32_t s = mptr(e + 4);
    if (s && mr8(s) < 0x20) s++;             /* leading byte: centring */
    int st = entry_state(c, e), k = 0;
    if (st == 1) {                           /* "1.     ?" in the game */
        uint32_t d = mr8(s);
        buf[k++] = d >= '0' && d <= '9' ? (char)d : '?';
        buf[k++] = '.'; buf[k++] = ' '; buf[k++] = '?';
        buf[k] = 0;
        return 1;
    }
    for (int guard = 0; s && guard < 40 && k < n - 1; guard++) {
        uint32_t ch = mr8(s++);
        if (!ch || ch >= 0x80) break;
        if (ch < 0x20) continue;
        buf[k++] = (char)ch;
    }
    buf[k] = 0;
    return st;
}
