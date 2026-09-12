/* PRIME-C-CODE-BEGIN */
#include "prime.h"
#include "hp_gfx.h"
#include "hp_input.h"
#include "hp_fonts.h"
#include "hp_string.h"

/* ---- graphics library self-test panel ----
 * Draws every primitive of hp_gfx.c / hp_fonts.h onto the LCD and verifies
 * each one by reading pixels back with hp_get_pixel().  Results are shown
 * as green/red marks on screen ("T1..T15") and printed to the console
 * (final line "GFXTEST <pass>/<total>" for scripted checks).
 *
 * Covered: clear, pixel, fill_rect, rect, fill_circle, circle,
 * fill_triangle, triangle, hline, vline, line, text, text_bg,
 * proportional/monospace fonts, GROB (new/dims/select/blit/free).
 *
 * ENTER reruns the whole panel, q / ESC / ON exits.
 */

#define NT 15

static int g_pass;

/* record one test result: console tag + on-screen mark */
static void ok(int i, int cond, const char *name)
{
    int col = ((i - 1) % 6) * 52 + 4;
    int row = 166 + ((i - 1) / 6) * 13;
    char mark[2];
    g_pass += (cond != 0);
    prints("T");
    printd(i);
    prints(" ");
    prints(name);
    prints(cond ? " OK\n" : " FAIL\n");
    hp_fill_rect(col, row, 9, 9, cond ? HP_GREEN : HP_RED);
    mark[0] = "0123456789ABCDEF"[i & 15];
    mark[1] = 0;
    hp_text(col + 11, row, mark, HP_WHITE);
}

static unsigned px(int x, int y) { return (unsigned)hp_get_pixel(x, y); }

static void run_all(void)
{
    hp_grob *g;
    int n, x, y, cnt, tw, th;

    g_pass = 0;
    hp_grob_select(NULL);

    /* T1 clear: both corners must be black afterwards */
    hp_clear(HP_BLACK);
    hp_text(4, 2, "PRIMETCC GFX TEST", HP_WHITE);
    ok(1, px(0, 0) == HP_BLACK && px(319, 239) == HP_BLACK, "CLEAR");

    /* T2 pixel: set + readback, neighbour untouched */
    hp_pixel(310, 5, HP_WHITE);
    ok(2, px(310, 5) == HP_WHITE && px(309, 5) == HP_BLACK, "PIXEL");

    /* T3 fill_rect: interior filled, outside corner untouched */
    hp_fill_rect(20, 16, 60, 30, HP_BLUE);
    ok(3, px(50, 31) == HP_BLUE && px(19, 15) == HP_BLACK &&
          px(80, 46) == HP_BLACK, "FILLRECT");

    /* T4 rect outline: edges drawn, interior NOT filled */
    hp_rect(100, 16, 60, 30, HP_GREEN);
    ok(4, px(130, 16) == HP_GREEN && px(100, 45) == HP_GREEN &&
          px(130, 31) == HP_BLACK, "RECT");

    /* T5 fill_circle: centre filled, just outside radius empty */
    hp_fill_circle(210, 31, 14, HP_MAGENTA);
    ok(5, px(210, 31) == HP_MAGENTA && px(226, 31) != HP_MAGENTA &&
          px(210, 14) != HP_MAGENTA, "FILLCIR");

    /* T6 circle outline: right vertex drawn, centre empty */
    hp_circle(290, 31, 14, HP_CYAN);
    ok(6, px(304, 31) == HP_CYAN && px(276, 31) == HP_CYAN &&
          px(290, 31) == HP_BLACK, "CIRCLE");

    /* T7 fill_triangle: interior orange, outside below base empty */
    hp_fill_triangle(20, 90, 100, 90, 60, 56, HP_ORANGE);
    ok(7, px(60, 78) == HP_ORANGE && px(60, 91) == HP_BLACK &&
          px(19, 90) == HP_BLACK, "FILLTRI");

    /* T8 triangle outline: base midpoint + apex vertex on the edge */
    hp_triangle(112, 90, 152, 90, 132, 56, HP_YELLOW);
    ok(8, px(132, 90) == HP_YELLOW && px(132, 56) == HP_YELLOW &&
          px(132, 73) == HP_BLACK, "TRI");

    /* T9 hline: inside drawn, one past both ends empty */
    hp_hline(160, 73, 80, HP_CYAN);
    ok(9, px(199, 73) == HP_CYAN && px(159, 73) != HP_CYAN &&
          px(240, 73) != HP_CYAN, "HLINE");

    /* T10 vline: same for vertical spans */
    hp_vline(250, 56, 34, HP_RED);
    ok(10, px(250, 72) == HP_RED && px(250, 55) != HP_RED &&
           px(250, 91) != HP_RED, "VLINE");

    /* T11 line: Bresenham endpoints land exactly */
    hp_line(266, 88, 310, 56, HP_WHITE);
    ok(11, px(266, 88) == HP_WHITE && px(310, 56) == HP_WHITE, "LINE");

    /* T12 text: at least one lit pixel in the glyph box */
    hp_text(20, 108, "HELLO GFX", HP_WHITE);
    cnt = 0;
    for (y = 108; y < 116; y++)
        for (x = 20; x < 20 + 9 * 6; x++)
            if (px(x, y) == HP_WHITE) cnt++;
    ok(12, cnt > 8, "TEXT");

    /* T13 text_bg: background cell fully painted (gap column check) */
    hp_text_bg(110, 108, "TEXT BG", HP_BLACK, HP_YELLOW);
    ok(13, px(110 + 2 * 6 + 5, 111) == HP_YELLOW &&
           px(110 + hp_text_w("TEXT BG") - 1, 115) == HP_YELLOW, "TEXTBG");

    /* T14 fonts: mono 16px glyphs must light up the box */
    hp_font_draw(hp_font_mono(16), 20, 126, "FONT 16", HP_GREEN);
    cnt = 0;
    for (y = 124; y < 150; y++)
        for (x = 18; x < 100; x++) {
            unsigned v = px(x, y);
            int mx = (int)((v >> 16) & 255);
            if ((int)((v >> 8) & 255) > mx) mx = (v >> 8) & 255;
            if ((int)(v & 255) > mx) mx = v & 255;
            if (mx > 64) cnt++;
        }
    ok(14, cnt > 40, "FONT");

    /* T15 GROB: dims via accessors, draw off-screen, blit, target size */
    g = hp_grob_new(80, 40);
    tw = hp_target_w();
    th = hp_target_h();
    n = 0;
    if (g && hp_grob_w(g) == 80 && hp_grob_h(g) == 40) {
        hp_grob_select(g);
        n = (hp_target_w() == 80 && hp_target_h() == 40);
        hp_clear(HP_GRAY);
        hp_fill_rect(10, 10, 20, 15, HP_RED);
        hp_rect(0, 0, hp_grob_w(g), hp_grob_h(g), HP_WHITE);
        hp_grob_select(NULL);
        hp_grob_blit(g, 225, 116);
    }
    if (g)
        hp_grob_free(g);
    ok(15, n && tw == 320 && th == 240 &&
           px(240, 131) == HP_RED &&            /* blitted red block   */
           px(228, 119) == HP_GRAY &&           /* grob background     */
           px(225, 116) == HP_WHITE &&          /* blitted border      */
           hp_target_w() == 320 && hp_target_h() == 240, "GROB");

    /* summary + help */
    prints("----------------\n");
    printf("GFXTEST %d/%d\n", g_pass, NT, 0, 0);
    if (g_pass == NT) {
        hp_text(4, 204, "PASS 15/15", HP_GREEN);
    } else {
        char b[24];
        char *p = b;
        const char *s = "FAIL ";
        while (*s) *p++ = *s++;
        itoa(g_pass, p, 10);
        while (*p) p++;
        s = "/15";
        while (*s) *p++ = *s++;
        *p = 0;
        hp_text(4, 204, b, HP_RED);
    }
    hp_text(4, 222, "ENTER RERUN  Q/ESC/ON EXIT", HP_GRAY);
}

int main(void)
{
    hp_event ev;
    if (!hp_gfx_init()) { printf("GFX INIT FAIL\n", 0, 0, 0, 0); return 1; }
    if (!hp_input_install()) { printf("INPUT HOOK FAIL\n", 0, 0, 0, 0); return 1; }
    prints("PRIMETCC GFX LIB TEST\n");
    run_all();
    for (;;) {
        int t = hp_poll_event(&ev);
        if (t == HP_EV_KEY) {
            int k = hp_event_key(&ev);
            if (hp_event_key_down(&ev)) {
                if (k == HP_KEY_ENTER) {
                    prints("RERUN\n");
                    run_all();
                } else if (k == HP_KEY_Q || k == HP_KEY_ESC ||
                           k == HP_KEY_ON) {
                    hp_input_remove();
                    printf("GFXTEST_EXIT\n", 0, 0, 0, 0);
                    return 0;
                }
            }
        }
        __sleep(16);
    }
}
/* PRIME-C-CODE-END */
