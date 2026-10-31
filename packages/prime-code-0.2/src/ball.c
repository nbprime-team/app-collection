/* PRIME-C-CODE-BEGIN */
#include "prime.h"
#include "hp_gfx.h"
#include "hp_input.h"

/* ---- ball demo ----
 * Double-buffered bouncing-ball animation with live keyboard/touch readout.
 * An orange ball bounces inside the screen; the last key code is shown in
 * the top-left corner (green = pressed, gray = released), touch points are
 * printed to the console.  q (0x51) / ESC (0x01) / ON (0x83) exit.
 * Source of truth for tests/harness.py's 'ball' case.
 */

static void show_hex(int x, int y, unsigned v, hp_color c)
{
    char buf[3];
    buf[0] = "0123456789ABCDEF"[(v >> 4) & 15];
    buf[1] = "0123456789ABCDEF"[v & 15];
    buf[2] = 0;
    hp_text(x, y, buf, c);
}

int main(void) {
    hp_event ev;
    hp_grob *bg;
    int x = 160, y = 120, vx = 3, vy = 2;
    int w, h, key = 0, keydown = 0, frames = 0, nkeys = 0, t;
    if (!hp_gfx_init()) { printf("GFX INIT FAIL\n", 0, 0, 0, 0); return 1; }
    if (!hp_input_install()) { printf("INPUT HOOK FAIL\n", 0, 0, 0, 0); return 1; }
    w = hp_gfx_w(); h = hp_gfx_h();
    bg = hp_grob_new(w, h);
    if (!bg) { printf("GROB FAIL\n", 0, 0, 0, 0); hp_input_remove(); return 1; }
    printf("BALL start %dx%d\n", w, h, 0, 0);
    printf("TW %d\n", hp_text_w("KEY:"), 0, 0, 0);   /* expect 24 */
    for (;;) {
        hp_grob_select(bg);
        hp_clear(HP_BLACK);
        hp_fill_circle(x, y, 8, HP_ORANGE);
        hp_text(4, 2, "KEY:", HP_WHITE);
        show_hex(4 + hp_text_w("KEY:"), 2, key, keydown ? HP_GREEN : HP_GRAY);
        hp_text(4, 26, "q/ESC/ON TO EXIT", HP_CYAN);
        hp_grob_select(NULL);
        hp_grob_blit(bg, 0, 0);
        t = hp_poll_event(&ev);
        if (t == HP_EV_KEY) {
            key = hp_event_key(&ev);
            keydown = hp_event_key_down(&ev);
            nkeys++;
            printf("KEY %02x %d\n", key, keydown, 0, 0);
            if (keydown && (key == 0x51 || key == 0x01 || key == 0x83)) {
                printf("KEY_TOTAL %d\n", nkeys, 0, 0, 0);
                hp_input_remove();
                printf("SLOT %08x\n", *(unsigned *)0x307fbfa0, 0, 0, 0);
                printf("BALL_DONE %d\n", frames, 0, 0, 0);
                hp_grob_free(bg);
                return 0;
            }
        } else if (t == HP_EV_TOUCH) {
            int tx = 0, ty = 0, n = hp_event_touch_count(&ev), k;
            for (k = 0; k < n && k < 4; k++) {
                if (hp_event_touch(&ev, k, &tx, &ty))
                    printf("TOUCH %d %d\n", tx, ty, 0, 0);
            }
        }
        x += vx; y += vy;
        if (x < 8 || x > w - 8) vx = -vx;
        if (y < 8 || y > h - 8) vy = -vy;
        frames++;
        __sleep(16);
    }
}
/* PRIME-C-CODE-END */
