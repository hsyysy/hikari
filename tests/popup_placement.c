/* Boundary tests for popup edge-flipping.
 *
 * Tests the *real* function from the hikari sources
 * (hikari_input_popup_place in include/hikari/input_method_relay.h), not a copy,
 * so this doubles as a regression test.
 *
 * Why it exists: the placement logic has four branches (flip down->up,
 * flip right->left, and two clamps) and those cases are hard to stage on a real
 * desktop -- getting the terminal cursor to the right edge requires typing a
 * long line, and a popup larger than the screen is not reproducible at all.
 *
 *   make test
 *
 * or by hand (pkg-config rather than a hardcoded include path, so this keeps
 * working across wlroots versions and distributions):
 *
 *   cc -I. -Iinclude -DWLR_USE_UNSTABLE=1 $(pkg-config --cflags wlroots-0.21) \
 *      -o /tmp/t-popup tests/popup_placement.c && /tmp/t-popup
 */
#include <stdio.h>

#include <hikari/input_method_relay.h>

static int failures = 0;

static void
check(const char *what, int got, int want)
{
  if (got != want) {
    printf("  FAIL %s: got %d, want %d\n", what, got, want);
    failures++;
  } else {
    printf("  ok   %s = %d\n", what, got);
  }
}

/* Screen 1920x1080 and popup 460x56 are both measured values, taken from a
 * live session (the popup size comes from the IME client, so it varies with the
 * number of candidates; 460x56 is what a 9-candidate row produces). The exact
 * numbers only matter for arithmetic that is already covered by the checks
 * below -- any screen/popup pair exercises the same branches. */
#define SW 1920
#define SH 1080
#define PW 460
#define PH 56

static void
place(int cx, int cy, int cw, int ch, int *x, int *y)
{
  struct wlr_box cursor = { .x = cx, .y = cy, .width = cw, .height = ch };

  hikari_input_popup_place(&cursor, PW, PH, SW, SH, x, y);
}

int
main(void)
{
  int x, y;

  printf("-- 1: middle of the screen, room everywhere -> below-right, no flip\n");
  place(800, 500, 1, 32, &x, &y);
  check("x", x, 800);
  check("y", y, 532);

  printf("-- 2: cursor at the bottom edge -> flip above\n");
  /* Cursor bottom at 1040, only 40px left below < 56 -> must flip. */
  place(800, 1008, 1, 32, &x, &y);
  check("x", x, 800);
  check("y", y, 1008 - PH); /* 952 */

  printf("-- 3: cursor at the right edge -> flip left (right edges aligned)\n");
  /* Cursor x=1900, only 20px left to the right < 460 -> align right edges:
   * 1900 + 1 - 460 = 1441 */
  place(1900, 500, 1, 32, &x, &y);
  check("x", x, 1441);
  check("y", y, 532);

  printf("-- 4: bottom-right corner -> flip both ways\n");
  place(1900, 1008, 1, 32, &x, &y);
  check("x", x, 1441);
  check("y", y, 952);

  printf("-- 5: exactly fits -> no flip (must not flip early)\n");
  /* Cursor bottom at 1024, exactly 56px left -> fits, must not flip. */
  place(800, 992, 1, 32, &x, &y);
  check("y", y, 1024);
  /* Cursor right at 1460, exactly 460px left -> fits, must not flip. */
  place(1460, 500, 1, 32, &x, &y);
  check("x", x, 1460);

  printf("-- 6: popup larger than the screen -> clamped, never negative\n");
  {
    /* Result is 0, not (screen - popup): the flip already moved x off the left
     * edge and the final "never negative" clamp pins it to 0. That is the
     * wanted behaviour -- showing the left half keeps candidate "1." visible. */
    struct wlr_box cursor = { .x = 100, .y = 100, .width = 1, .height = 32 };

    hikari_input_popup_place(&cursor, SW + 200, SH + 200, SW, SH, &x, &y);
    check("x", x, 0);
    check("y", y, 0);
  }

  printf("-- 7: cursor at the top-left corner -> coordinates stay non-negative\n");
  place(0, 0, 1, 32, &x, &y);
  check("x", x, 0);
  check("y", y, 32);

  printf("-- 8: cursor rect itself extends past the right edge -> clamped inside\n");
  {
    /* Cursor at x=1900 with width 100, so its right edge is at 2000 > 1920.
     * Flipping left gives 1900 + 100 - 460 = 1540, which is still 2000 > 1920,
     * so the explicit clamp pulls it back to 1920 - 460 = 1460.
     * This is the only case that reaches the `screen_width - popup_width`
     * branch -- a popup larger than the screen does not (it is caught by the
     * final "never negative" clamp instead, see case 6). */
    place(1900, 500, 100, 32, &x, &y);
    check("x", x, 1460);
    check("y", y, 532);
  }

  printf("\n%s\n", failures == 0 ? "ALL PASS" : "FAILURES");
  return failures != 0;
}
