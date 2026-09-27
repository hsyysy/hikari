#if !defined(HIKARI_INPUT_METHOD_RELAY_H)
#define HIKARI_INPUT_METHOD_RELAY_H

#include <wayland-server-core.h>
#include <wlr/util/box.h>

struct wlr_input_method_v2;
struct wlr_text_input_manager_v3;
struct wlr_input_method_manager_v2;
struct wlr_surface;
struct hikari_output;

struct hikari_text_input {
  struct wlr_text_input_v3 *input;
  struct hikari_input_method_relay *relay;
  struct wl_list link;

  struct wl_listener enable;
  struct wl_listener commit;
  struct wl_listener disable;
  struct wl_listener destroy;
};

struct hikari_input_popup {
  struct wlr_input_popup_surface_v2 *popup;
  struct hikari_input_method_relay *relay;
  struct wl_list link;

  struct wl_listener destroy;
  struct wl_listener surface_commit;
  struct wl_listener surface_map;
  struct wl_listener surface_unmap;

  bool mapped;
  struct wlr_box geometry;
  /* Output this popup belongs to, set from the anchor computed in
   * popup_update_position(). NULL until then, and the renderer/damage paths
   * fall back to old behaviour while it is NULL so an unowned popup cannot
   * silently stop being drawn.
   *
   * Needed because the popup geometry is output-local, while rendering runs
   * once per output: without this check the popup is drawn on *every* monitor
   * at the same local coordinates. */
  struct hikari_output *output;
};

struct hikari_input_method_relay {
  struct wl_list text_inputs;
  struct wl_list popups;
  struct wlr_input_method_v2 *input_method;

  struct wl_listener new_text_input;
  struct wl_listener new_input_method;
  struct wl_listener input_method_commit;
  struct wl_listener input_method_grab_keyboard;
  struct wl_listener input_method_new_popup;
  struct wl_listener input_method_destroy;
  struct wl_listener keyboard_focus_change;
};

/* Compute the popup's top-left corner, in **output-local** coordinates.
 *
 * `cursor` is the text cursor rectangle already translated to output
 * coordinates. `popup_*` is the popup size, which only the IM client knows
 * (it decides how many candidates to show). `screen_*` is the output size.
 *
 * The popup goes below-right of the cursor by default. If it does not fit
 * below it flips above; if it does not fit to the right it flips to the left
 * (right edges aligned).
 *
 * Two clamps follow, and they cover different cases:
 *   - `screen_* - popup_*` pulls the popup back inside when the flip could not
 *     help because the *cursor rectangle itself* extends past the screen edge.
 *   - the final "never negative" clamp catches what is left, i.e. a popup
 *     wider/taller than the screen. There it pins to 0, which keeps candidate
 *     "1." visible instead of showing the tail end of the list.
 *
 * Without this the popup gets cut off by the screen edge whenever the cursor
 * sits near the bottom or right edge, which makes it unreadable.
 *
 * Kept as a pure function so it can be tested **without a compositor**: the
 * edge cases (bottom-right corner, cursor past the edge, popup larger than the
 * screen) are hard to stage on a real desktop.
 * See tests/popup_placement.c in this repository. */
static inline void
hikari_input_popup_place(const struct wlr_box *cursor, int popup_width,
    int popup_height, int screen_width, int screen_height, int *x, int *y)
{
  *x = cursor->x;
  *y = cursor->y + cursor->height;

  if (*y + popup_height > screen_height) {
    *y = cursor->y - popup_height;
  }
  if (*x + popup_width > screen_width) {
    *x = cursor->x + cursor->width - popup_width;
  }

  if (*x + popup_width > screen_width) {
    *x = screen_width - popup_width;
  }
  if (*y + popup_height > screen_height) {
    *y = screen_height - popup_height;
  }
  if (*x < 0) {
    *x = 0;
  }
  if (*y < 0) {
    *y = 0;
  }
}

void
hikari_input_method_relay_init(struct hikari_input_method_relay *relay,
    struct wlr_text_input_manager_v3 *text_input_manager,
    struct wlr_input_method_manager_v2 *input_method_manager);

/* Drop every popup's reference to `output`, which is about to be freed.
 *
 * popup->output is a bare pointer into a heap-allocated hikari_output that the
 * output's destroy handler frees. A popup can stay mapped across an output
 * being unplugged, and nothing else recomputes its owner in that window, so
 * without this the renderer and damage paths would dereference freed memory.
 *
 * Clearing to NULL degrades to the "owner unknown" path, which draws on every
 * output and damages the workspace output -- the behaviour from before the
 * multi-output filtering, and safe. */
void
hikari_input_method_relay_output_destroyed(struct hikari_output *output);

#endif
