#include <stdint.h>
#include "prime_hook.h"        /* SDK：固件输入钩子（取事件必须用它，不能轮询） */
#include "unifont_draw.h"
#include "app_common.h"      /* 共享字体资源（GNU Unifont ASCII 子集） */

#define LCD_W 320
#define LCD_H 240
#define MAX_FRUITS 40
#define GAME_TOP 40
#define FLOOR_Y 232
#define SPAWN_Y 44
#define FRAME_MS 20
#define DROP_DELAY_FRAMES 20


extern void *prime_sys_get_lcd(void);
extern void prime_sys_sleep(uint32_t ms);


/* The firmware calls our hook with R0 = ui_event_prime_s*. */
static volatile int g_quit;
static volatile int g_touch_x;
static volatile int g_touch_y;
/* 0=none, 1=begin, 2=move, 3=end */
static volatile int g_touch_state;
static volatile uint32_t g_touch_serial;


struct Fruit {
    int active;
    int type;
    int x_q8;
    int y_q8;
    int vx_q8;
    int vy_q8;
};

static struct Fruit fruits[MAX_FRUITS];
/* 320*240*4 = 307200 bytes; renders off-screen then blits once per frame. */
static uint32_t framebuf[LCD_W * LCD_H] __attribute__((aligned(32)));
static uint32_t rng_state = 0x4D455247u;
static int score;
static int current_type;
static int current_x;
static int current_down;
static int game_over;
static int game_over_timer;
static int spawn_lock;

static const int radius_px[9] = {8,11,14,18,22,27,32,38,44};
static const int value_table[9] = {1,3,6,10,15,21,28,36,45};

static const uint32_t fruit_color[9] = {
    0xFFFF4F81u, 0xFFFF6B5Au, 0xFF7D45D8u,
    0xFFFFA62Bu, 0xFFE83B30u, 0xFFFFC845u,
    0xFF8CC63Eu, 0xFF9B5B2Cu, 0xFFFFD94Au
};
/* 输入钩子统一走 SDK：prime_hook_install()/prime_hook_remove()（见 prime_hook.h）。
 * 机制与 PureDOOM 的 install_input_hack() 同构：trampoline 直达本回调。 */
static void suika_event_hook(void *event);

/*
 * This is intentionally close to PureDOOM's my_get_event_hook().
 * The original hook:
 *   1. calls sys_get_event(event)
 *   2. checks event_type at +4
 *   3. for event_type 15, scans up to 8 records
 *   4. accepts records whose +4 field is zero
 *   5. uses action +0, x +6, y +8
 *   6. for key events (0x00100010), converts the key and enqueues it
 *
 * We only need the raw input, so the converted data is stored in volatile
 * globals and consumed by the game loop.
 */
__attribute__((noinline, used))
static void suika_event_hook(void *event)
{
    uint8_t *p = (uint8_t *)event;
    uint32_t event_type;
    int count;
    int i;

    if (!p)
        return;

    prime_sys_get_event(event);

    event_type = app_rd32(p + 4);

    /* Any key down/up event exits the game. */
    if (event_type == APP_EV_KEY) {
        int action = app_rd16(p + 28);
        if (action == (int)APP_KEY_DOWN || action == (int)APP_KEY_UP)
            g_quit = 1;
        return;
    }

    if (event_type != APP_EV_TICK)
        return;

    count = app_rd16(p + 24);
    if (count > 8)
        count = 8;
    if (count < 0)
        count = 0;

    for (i = 0; i < count; ++i) {
        uint8_t *m = p + 28 + i * 12;
        int action = app_rd16(m + 0);
        int valid_field = app_rd16(m + 4);
        int x = app_rd16(m + 6);
        int y = app_rd16(m + 8);

        /* Exactly the same validity test used by DOOM's hook. */
        if (valid_field != 0)
            continue;

        if (action != (int)APP_TOUCH_BEGIN &&
            action != (int)APP_TOUCH_MOVE &&
            action != (int)APP_TOUCH_END)
            continue;

        if (x < 0) x = 0;
        if (x >= LCD_W) x = LCD_W - 1;
        if (y < 0) y = 0;
        if (y >= LCD_H) y = LCD_H - 1;

        g_touch_x = x;
        g_touch_y = y;
        if (action == (int)APP_TOUCH_BEGIN)
            g_touch_state = 1;
        else if (action == (int)APP_TOUCH_MOVE)
            g_touch_state = 2;
        else
            g_touch_state = 3;
        ++g_touch_serial;
    }
}

static int isqrt_u32(uint32_t n)
{
    uint32_t bit = 1u << 30;
    uint32_t res = 0;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= res + bit) {
            n -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return (int)res;
}

static void hline(uint32_t *fb, int x0, int x1, int y, uint32_t color)
{
    int x;
    if ((unsigned)y >= LCD_H) return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (x1 < 0 || x0 >= LCD_W) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= LCD_W) x1 = LCD_W - 1;
    for (x = x0; x <= x1; ++x)
        fb[y * LCD_W + x] = color;
}

static void fill_circle(uint32_t *fb, int cx, int cy, int radius, uint32_t color)
{
    int dy;
    int rr = radius * radius;
    for (dy = -radius; dy <= radius; ++dy) {
        int rem = rr - dy * dy;
        int dx = isqrt_u32((uint32_t)(rem > 0 ? rem : 0));
        hline(fb, cx - dx, cx + dx, cy + dy, color);
    }
}


static uint32_t random_u32(void)
{
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

static int next_type(void)
{
    uint32_t r = random_u32() % 100u;
    if (r < 28) return 0;
    if (r < 50) return 1;
    if (r < 68) return 2;
    if (r < 82) return 3;
    return 4;
}

static int fruit_radius(const struct Fruit *f) { return radius_px[f->type]; }

static int add_fruit(int type, int x, int y, int vx, int vy)
{
    int i;
    for (i = 0; i < MAX_FRUITS; ++i) {
        if (!fruits[i].active) {
            fruits[i].active = 1;
            fruits[i].type = type;
            fruits[i].x_q8 = x << 8;
            fruits[i].y_q8 = y << 8;
            fruits[i].vx_q8 = vx << 8;
            fruits[i].vy_q8 = vy << 8;
            return i;
        }
    }
    return -1;
}

static void remove_fruit(int i)
{
    if (i >= 0 && i < MAX_FRUITS)
        fruits[i].active = 0;
}

static void merge_pair(int a, int b)
{
    int type = fruits[a].type;
    int x = (fruits[a].x_q8 + fruits[b].x_q8) >> 1;
    int y = (fruits[a].y_q8 + fruits[b].y_q8) >> 1;
    int vx = (fruits[a].vx_q8 + fruits[b].vx_q8) >> 1;
    int vy = ((fruits[a].vy_q8 + fruits[b].vy_q8) >> 1) - 30;
    int i;

    remove_fruit(a);
    remove_fruit(b);
    score += value_table[type];

    if (type >= 8) {
        score += 100;
        return;
    }

    for (i = 0; i < MAX_FRUITS; ++i) {
        if (!fruits[i].active) {
            fruits[i].active = 1;
            fruits[i].type = type + 1;
            fruits[i].x_q8 = x;
            fruits[i].y_q8 = y;
            fruits[i].vx_q8 = vx;
            fruits[i].vy_q8 = vy;
            return;
        }
    }
}

static void resolve_physics(void)
{
    int i, j, pass;
    for (pass = 0; pass < 3; ++pass) {
        int changed = 0;

        for (i = 0; i < MAX_FRUITS; ++i) {
            struct Fruit *a = &fruits[i];
            if (!a->active) continue;

            a->vy_q8 += 44;
            if (a->vy_q8 > 1800) a->vy_q8 = 1800;
            a->x_q8 += a->vx_q8;
            a->y_q8 += a->vy_q8;

            {
                int r = fruit_radius(a);
                int minx = r << 8;
                int maxx = (LCD_W - r) << 8;
                int floor = (FLOOR_Y - r) << 8;
                if (a->x_q8 < minx) { a->x_q8 = minx; a->vx_q8 = -a->vx_q8 / 3; }
                if (a->x_q8 > maxx) { a->x_q8 = maxx; a->vx_q8 = -a->vx_q8 / 3; }
                if (a->y_q8 > floor) {
                    a->y_q8 = floor;
                    if (a->vy_q8 > 80) a->vy_q8 = -a->vy_q8 / 4;
                    else a->vy_q8 = 0;
                    a->vx_q8 = a->vx_q8 * 7 / 8;
                }
            }
        }

        for (i = 0; i < MAX_FRUITS; ++i) {
            struct Fruit *a = &fruits[i];
            if (!a->active) continue;
            for (j = i + 1; j < MAX_FRUITS; ++j) {
                struct Fruit *b = &fruits[j];
                int dx, dy, dist2, min_d, dist;
                if (!b->active) continue;
                dx = (b->x_q8 - a->x_q8) >> 8;
                dy = (b->y_q8 - a->y_q8) >> 8;
                min_d = fruit_radius(a) + fruit_radius(b);
                dist2 = dx * dx + dy * dy;
                if (dist2 > min_d * min_d) continue;

                if (a->type == b->type && a->type < 8) {
                    merge_pair(i, j);
                    changed = 1;
                    continue;
                }

                dist = isqrt_u32((uint32_t)(dist2 > 0 ? dist2 : 1));
                if (dist < 1) dist = 1;
                {
                    int overlap = min_d - dist;
                    int pushx = dx * overlap / dist;
                    int pushy = dy * overlap / dist;
                    a->x_q8 -= pushx * 128;
                    a->y_q8 -= pushy * 128;
                    b->x_q8 += pushx * 128;
                    b->y_q8 += pushy * 128;
                    if (dy > 0) {
                        if (b->vy_q8 > 0) b->vy_q8 /= 3;
                        if (a->vy_q8 > 0) a->vy_q8 /= 2;
                    }
                    if (dx != 0) {
                        a->vx_q8 += dx > 0 ? -12 : 12;
                        b->vx_q8 += dx > 0 ? 12 : -12;
                    }
                }
            }
        }

        if (!changed) break;
    }
}

static void check_game_over(void)
{
    int i;
    int danger = 0;
    for (i = 0; i < MAX_FRUITS; ++i) {
        if (!fruits[i].active) continue;
        if ((fruits[i].y_q8 >> 8) - fruit_radius(&fruits[i]) < GAME_TOP + 3 &&
            fruits[i].vy_q8 >= -10) {
            danger = 1;
            break;
        }
    }
    if (danger) {
        ++game_over_timer;
        if (game_over_timer > 70) game_over = 1;
    } else {
        game_over_timer = 0;
    }
}

static void spawn_current(void)
{
    current_type = next_type();
    current_x = LCD_W / 2;
    current_down = 1;
}

static void reset_game(void)
{
    int i;
    for (i = 0; i < MAX_FRUITS; ++i)
        fruits[i].active = 0;
    score = 0;
    game_over = 0;
    game_over_timer = 0;
    current_down = 0;
    spawn_lock = 0;
    rng_state ^= 0xA53C9E71u;
    spawn_current();
}

static void drop_current(void)
{
    if (!current_down || spawn_lock || game_over)
        return;
    if (add_fruit(current_type, current_x, SPAWN_Y, 0, 0) < 0) {
        game_over = 1;
        return;
    }
    current_down = 0;
    /* Prevent an immediate second drop from creating a new fruit at the edge. */
    spawn_lock = DROP_DELAY_FRAMES;
}

/*
 * Consume the most recent hook event. Events arrive asynchronously from the
 * firmware; no SVC is called here.
 */
static void consume_touch(void)
{
    int state = g_touch_state;
    int x = g_touch_x;
    (void)g_touch_y;

    if (!state)
        return;

    if (state == 1 || state == 2) {
        if (!game_over) {
            int r = radius_px[current_type];
            if (x < r) x = r;
            if (x > LCD_W - r) x = LCD_W - r;
            current_x = x;
            current_down = 1;
        }
    } else if (state == 3) {
        if (!game_over)
            drop_current();
    }

    g_touch_state = 0;
}

static void draw_hud(uint32_t *fb)
{
    char score_buf[12];
    char tmp[12];
    int n = 0;
    int t = 0;
    int v = score;
    int i;
    int sw;

    if (v == 0) score_buf[n++] = '0';
    else {
        while (v > 0 && t < 11) {
            tmp[t++] = (char)('0' + (v % 10));
            v /= 10;
        }
        for (i = t - 1; i >= 0; --i)
            score_buf[n++] = tmp[i];
    }
    score_buf[n] = 0;

    unifont_draw_text(fb, LCD_W, LCD_H, 8, 11, "SCORE", 2, 1, 0xFFFFFFFFu);
    sw = unifont_text_width(score_buf, 2, 1);
    (void)sw;
    unifont_draw_text(fb, LCD_W, LCD_H, 100, 11, score_buf, 2, 1, 0xFFFFD94Au);
    unifont_draw_text(fb, LCD_W, LCD_H, 245, 11, "NEXT", 1, 1, 0xFFB8C7D9u);
    hline(fb, 0, LCD_W - 1, GAME_TOP, 0xFF8C1D1Du);
}

static void draw_fruit(uint32_t *fb, int type, int x, int y)
{
    int r = radius_px[type];
    uint32_t dark = 0xFF202020u;
    fill_circle(fb, x, y, r + 1, dark);
    fill_circle(fb, x, y, r, fruit_color[type]);
    fill_circle(fb, x - r / 3, y - r / 3, r / 4 + 1, 0xFFFFFFFFu);
}

static void render(void)
{
    int i;

    app_clear_fb(framebuf, LCD_W * LCD_H, 0xFF15202Bu);
    draw_hud(framebuf);

    for (i = 0; i < MAX_FRUITS; ++i) {
        if (!fruits[i].active) continue;
        draw_fruit(framebuf,
                   fruits[i].type,
                   fruits[i].x_q8 >> 8,
                   fruits[i].y_q8 >> 8);
    }

    if (!game_over && current_down)
        draw_fruit(framebuf, current_type, current_x, SPAWN_Y);

    /* Keep the exit hint above fruit sprites so it remains readable. */
    unifont_draw_text(framebuf, LCD_W, LCD_H, 4, 222, "ANY KEY EXIT", 1, 0, 0xFFB8C7D9u);

    if (game_over) {
        unifont_draw_text(framebuf, LCD_W, LCD_H, 78, 96, "GAME OVER", 2, 1, 0xFFFFFFFFu);
        unifont_draw_text(framebuf, LCD_W, LCD_H, 60, 140, "TOUCH TO RESTART", 1, 0, 0xFFFFD94Au);
    }
}

static void restart_if_touched(void)
{
    if (g_touch_state == 3) {
        reset_game();
        g_touch_state = 0;
    }
}

__attribute__((section(".text.main"), noinline))
int main(void *config, void *reserved)
{
    uint32_t *lcd_fb;
    int frame;

    (void)config;
    (void)reserved;
    app_elf_requirements();

    lcd_fb = app_lcd_framebuffer();
    if (!lcd_fb)
        return 0;

    g_quit = 0;
    g_touch_state = 0;
    g_touch_serial = 0;
    reset_game();

    /* The input hook is the crucial DOOM-derived part. */
    prime_hook_install(suika_event_hook);

    for (frame = 0; ; ++frame) {
        if (g_quit)
            break;

        if (game_over)
            restart_if_touched();
        else
            consume_touch();

        if (!game_over) {
            resolve_physics();
            check_game_over();
            if (spawn_lock > 0)
                --spawn_lock;
            if (!current_down && spawn_lock == 0)
                spawn_current();
        }

        render();
        app_blit_fb(lcd_fb, framebuf, LCD_W * LCD_H);
        prime_sys_sleep(FRAME_MS);
    }

    prime_hook_remove();
    return 0;
}
