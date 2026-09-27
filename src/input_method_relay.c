#include <hikari/input_method_relay.h>

#include <stdlib.h>

#include <wlr/types/wlr_input_method_v2.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_compositor.h>

#include <hikari/log.h>
#include <hikari/output.h>
#include <hikari/server.h>
#include <hikari/view.h>

#ifdef HAVE_LAYERSHELL
#include <hikari/layer_shell.h>
#endif

static struct hikari_text_input *
relay_find_focused_text_input(struct hikari_input_method_relay *relay)
{
  struct hikari_text_input *text_input;
  wl_list_for_each (text_input, &relay->text_inputs, link) {
    if (text_input->input->focused_surface != NULL &&
        text_input->input->current_enabled) {
      return text_input;
    }
  }
  return NULL;
}

static void
relay_send_im_state(
    struct hikari_input_method_relay *relay, struct wlr_text_input_v3 *input)
{
  struct wlr_input_method_v2 *im = relay->input_method;
  if (im == NULL) {
    return;
  }

  if (input->active_features & WLR_TEXT_INPUT_V3_FEATURE_SURROUNDING_TEXT) {
    wlr_input_method_v2_send_surrounding_text(im,
        input->current.surrounding.text, input->current.surrounding.cursor,
        input->current.surrounding.anchor);
  }
  wlr_input_method_v2_send_text_change_cause(
      im, input->current.text_change_cause);
  if (input->active_features & WLR_TEXT_INPUT_V3_FEATURE_CONTENT_TYPE) {
    wlr_input_method_v2_send_content_type(
        im, input->current.content_type.hint, input->current.content_type.purpose);
  }
  wlr_input_method_v2_send_done(im);
}

static void
damage_popup(struct hikari_input_popup *popup)
{
  if (!popup->mapped) {
    return;
  }

  /* Damage the popup's own output. Falling back to the workspace output keeps
   * the old behaviour while the owner is still unknown (popup->output is set
   * by popup_update_position()). */
  struct hikari_output *output = popup->output != NULL
      ? popup->output
      : hikari_server.workspace->output;
  if (output != NULL) {
    hikari_output_damage_whole(output);
  }
}

/* Find `needle` in a node's surface tree and report its offset relative to the
 * node's origin.
 *
 * wlr_surface_for_each_surface() invokes the iterator on the root surface
 * **itself** with (0, 0) (see surface_for_each_surface in wlroots'
 * types/wlr_compositor.c: it calls iterator(surface, x, y, ...) after walking
 * the below-subsurfaces), so the root matches too and there is no need for a
 * separate `view->surface == needle` check. */
struct surface_search {
  struct wlr_surface *needle;
  bool found;
  int sx;
  int sy;
};

static void
search_surface(struct wlr_surface *surface, int sx, int sy, void *data)
{
  struct surface_search *search = data;

  if (search->found || surface != search->needle) {
    return;
  }

  search->found = true;
  search->sx = sx;
  search->sy = sy;
}

static bool
node_surface_offset(
    struct hikari_node *node, struct wlr_surface *needle, int *sx, int *sy)
{
  struct surface_search search = { .needle = needle };

  hikari_node_for_each_surface(node, search_surface, &search);

  if (!search.found) {
    return false;
  }

  *sx = search.sx;
  *sy = search.sy;
  return true;
}

/* Compute the popup's anchor: the origin of the surface the text input is
 * focused on, in coordinates local to `*owner`. Returns false if it cannot be
 * found.
 *
 * `*owner` is the output the anchor belongs to. It matters because the anchor
 * is output-local: clamping the popup against a *different* output's size would
 * still let it get cut off on multi-monitor setups.
 *
 * This used to read `hikari_server.workspace->focus_view` directly, which was
 * wrong in three ways:
 *
 *   1. It is **NULL when the focus is on a layer surface** (launchers, panels),
 *      so the anchor collapsed to (0,0) and the popup landed in the output's
 *      top-left corner.
 *   2. It does not necessarily **own the focused surface** -- focus may have
 *      moved on before the text input's enter caught up, or the focused
 *      surface may belong to a different view.
 *   3. It **missed surface_geometry_x/y** -- render_view() in renderer.c
 *      subtracts them, this path did not, so the popup was offset from the
 *      cursor by the application's CSD margin.
 *
 * So: look up the real owner by surface, then subtract surface_geometry to
 * match what the renderer does. */
static bool
popup_anchor(struct wlr_surface *focused_surface,
    struct hikari_output **owner, int *x, int *y)
{
  struct hikari_view *view;
  wl_list_for_each (view, &hikari_server.visible_views, visible_server_views) {
    int sx, sy;

    /* This guard is required: hikari_view_for_each_surface() contains
     * `assert(view->surface != NULL)`, and release builds (-DNDEBUG) compile
     * that assert away -- so hitting it means a NULL dereference and the whole
     * compositor goes down. "Not reachable today" is not worth betting the
     * compositor on. */
    if (view->surface == NULL) {
      continue;
    }

    if (!node_surface_offset((struct hikari_node *)view, focused_surface,
            &sx, &sy)) {
      continue;
    }

    /* Must stay consistent with render_view() in renderer.c: it also takes
     * hikari_view_geometry(view) and subtracts surface_geometry_x/y. */
    const struct wlr_box *geo = hikari_view_geometry(view);
    *owner = view->output;
    *x = geo->x - view->surface_geometry_x + sx;
    *y = geo->y - view->surface_geometry_y + sy;
    return true;
  }

#ifdef HAVE_LAYERSHELL
  /* Layer surfaces (launchers, panels) are not views; search them separately.
   * Guarded because hikari_output::layers only exists with layershell
   * enabled (include/hikari/output.h). */
  struct hikari_output *output;
  wl_list_for_each (output, &hikari_server.outputs, server_outputs) {
    for (int i = 0; i < 4; i++) {
      struct hikari_layer *layer;
      wl_list_for_each (layer, &output->layers[i], layer_surfaces) {
        int sx, sy;

        if (!layer->mapped ||
            !node_surface_offset(
                (struct hikari_node *)layer, focused_surface, &sx, &sy)) {
          continue;
        }

        /* A layer's geometry is already in output coordinates; the renderer
         * uses it as-is (renderer.c). */
        *owner = layer->output;
        *x = layer->geometry.x + sx;
        *y = layer->geometry.y + sy;
        return true;
      }
    }
  }
#endif

  return false;
}

static void
popup_update_position(struct hikari_input_popup *popup)
{
  struct hikari_text_input *text_input =
      relay_find_focused_text_input(popup->relay);

  if (text_input == NULL || text_input->input->focused_surface == NULL) {
    return;
  }

  struct wlr_text_input_v3 *ti = text_input->input;
  struct wlr_box cursor_rect = ti->current.cursor_rectangle;

  struct hikari_output *owner = NULL;
  int anchor_x = 0, anchor_y = 0;
  bool found =
      popup_anchor(ti->focused_surface, &owner, &anchor_x, &anchor_y);

  if (!found) {
    /* Fallback: revert to the old behaviour (the focus view's geometry).
     * A slightly off position beats throwing the popup somewhere unrelated
     * when the owner cannot be found. */
    struct hikari_view *focus_view = hikari_server.workspace->focus_view;
    if (focus_view != NULL) {
      const struct wlr_box *geo = hikari_view_geometry(focus_view);
      anchor_x = geo->x;
      anchor_y = geo->y;
      owner = focus_view->output;
    }
  }

  if (owner == NULL) {
    owner = hikari_server.workspace->output;
  }

  /* Remember the owner so rendering and damage can filter by output -- the
   * geometry below is output-local, so drawing it on another output would put
   * a copy of the popup on that screen too. */
  popup->output = owner;

  /* Only the IM client knows how many candidates it will show, so it decides
   * the popup size. We need it before we can tell whether it fits. */
  struct wlr_surface *surface = popup->popup->surface;
  popup->geometry.width = surface->current.width;
  popup->geometry.height = surface->current.height;

  /* Translate the cursor rectangle to **output** coordinates. View geometry,
   * cursor_rect and the popup geometry all live in the same output-local space
   * (renderer.c's render_surface uses geometry->x directly as an output
   * coordinate), so the screen bounds are simply (0, 0, width, height).
   *
   * The edge-flipping itself is in hikari_input_popup_place(), kept pure so its
   * edge cases can be tested without a compositor. */
  struct wlr_box cursor = {
    .x = anchor_x + cursor_rect.x,
    .y = anchor_y + cursor_rect.y,
    .width = cursor_rect.width,
    .height = cursor_rect.height,
  };

  if (owner != NULL) {
    hikari_input_popup_place(&cursor, popup->geometry.width,
        popup->geometry.height, owner->geometry.width, owner->geometry.height,
        &popup->geometry.x, &popup->geometry.y);
  } else {
    /* No output (should not happen): fall back to plain below-right. */
    popup->geometry.x = cursor.x;
    popup->geometry.y = cursor.y + cursor.height;
  }

  /* Send the cursor rectangle to the popup so fcitx5 can size itself */
  struct wlr_box sbox = {
    .x = cursor_rect.x,
    .y = cursor_rect.y,
    .width = cursor_rect.width,
    .height = cursor_rect.height,
  };
  wlr_input_popup_surface_v2_send_text_input_rectangle(popup->popup, &sbox);
}

static void
handle_popup_surface_commit(struct wl_listener *listener, void *data)
{
  (void)data;
  struct hikari_input_popup *popup =
      wl_container_of(listener, popup, surface_commit);

  popup_update_position(popup);
  damage_popup(popup);
}

static void
handle_popup_surface_map(struct wl_listener *listener, void *data)
{
  (void)data;
  struct hikari_input_popup *popup =
      wl_container_of(listener, popup, surface_map);

  popup->mapped = true;
  popup_update_position(popup);
  damage_popup(popup);
}

static void
handle_popup_surface_unmap(struct wl_listener *listener, void *data)
{
  (void)data;
  struct hikari_input_popup *popup =
      wl_container_of(listener, popup, surface_unmap);

  damage_popup(popup);
  popup->mapped = false;
}

static void
handle_popup_destroy(struct wl_listener *listener, void *data)
{
  (void)data;
  struct hikari_input_popup *popup =
      wl_container_of(listener, popup, destroy);

  damage_popup(popup);

  wl_list_remove(&popup->destroy.link);
  wl_list_remove(&popup->surface_commit.link);
  wl_list_remove(&popup->surface_map.link);
  wl_list_remove(&popup->surface_unmap.link);
  wl_list_remove(&popup->link);
  free(popup);
}

static void
handle_im_new_popup(struct wl_listener *listener, void *data)
{
  struct hikari_input_method_relay *relay =
      wl_container_of(listener, relay, input_method_new_popup);
  struct wlr_input_popup_surface_v2 *wlr_popup = data;

  hikari_log_debug("new popup surface created");

  struct hikari_input_popup *popup = calloc(1, sizeof(*popup));
  if (popup == NULL) {
    return;
  }

  popup->popup = wlr_popup;
  popup->relay = relay;
  popup->mapped = false;

  popup->destroy.notify = handle_popup_destroy;
  wl_signal_add(&wlr_popup->events.destroy, &popup->destroy);

  popup->surface_commit.notify = handle_popup_surface_commit;
  wl_signal_add(&wlr_popup->surface->events.commit, &popup->surface_commit);

  popup->surface_map.notify = handle_popup_surface_map;
  wl_signal_add(&wlr_popup->surface->events.map, &popup->surface_map);

  popup->surface_unmap.notify = handle_popup_surface_unmap;
  wl_signal_add(&wlr_popup->surface->events.unmap, &popup->surface_unmap);

  wl_list_insert(&relay->popups, &popup->link);

  popup_update_position(popup);
}

static void
handle_text_input_enable(struct wl_listener *listener, void *data)
{
  (void)data;
  struct hikari_text_input *text_input =
      wl_container_of(listener, text_input, enable);
  struct hikari_input_method_relay *relay = text_input->relay;

  hikari_log_debug("text_input enable, im=%p", (void *)relay->input_method);

  if (relay->input_method == NULL) {
    return;
  }

  wlr_input_method_v2_send_activate(relay->input_method);
  relay_send_im_state(relay, text_input->input);
}

static void
handle_text_input_commit(struct wl_listener *listener, void *data)
{
  (void)data;
  struct hikari_text_input *text_input =
      wl_container_of(listener, text_input, commit);
  struct hikari_input_method_relay *relay = text_input->relay;

  if (!text_input->input->current_enabled) {
    return;
  }

  if (relay->input_method == NULL) {
    return;
  }

  relay_send_im_state(relay, text_input->input);

  /* The client may have just moved its cursor (set_cursor_rectangle). Popups
   * are otherwise only repositioned when the IM re-renders them, which happens
   * *before* the client's response to that render arrives -- so the popup would
   * keep the position derived from the previous preedit, a one-keystroke lag
   * that is plainly visible while typing.
   *
   * Measured on foot: after sending preedit N the rect used was always the one
   * belonging to preedit N-1, on all five keystrokes of "nihao".
   *
   * Only damage when the box actually moved: text-input commits are frequent,
   * and an unconditional damage here would force an extra redraw per commit.
   *
   * This reads `ti->current`, which by the time this handler runs already
   * holds this commit's values: text_input_commit() (types/wlr_text_input_v3.c)
   * does `current = pending` at :175 and only emits events.commit at :200,
   * after the assignment.
   *
   * Reading `pending` here would *happen* to give the same answer today --
   * commit does not clear `pending` (it is only reset by enable at :104 and
   * disable at :114), so right after the copy the two hold identical values.
   * Do not rely on that: `pending` means "uncommitted accumulation" and a
   * future refactor may well clear it. `current` is the value this commit
   * actually applied.
   *
   * (Also note the emit sits in the else branch: it fires only when the enabled
   * state did *not* change, which is exactly the case while typing.)
   *
   * Self-excitation note: popup_update_position() unconditionally sends
   * wlr_input_popup_surface_v2_send_text_input_rectangle() back to the IM, and
   * this handler is now another caller of it. An IM that redraws on every rect
   * it receives could in principle ping-pong via popup-surface commit ->
   * reposition -> send rect -> redraw -> ... No such IM is known here (our own
   * daemon only logs it), and the same entry point already existed via
   * handle_popup_surface_commit(), so this is a note rather than a defect --
   * but if IM-side CPU ever looks wrong, look here first. */
  struct hikari_input_popup *popup;
  wl_list_for_each (popup, &relay->popups, link) {
    if (!popup->mapped) {
      continue;
    }

    struct wlr_box before = popup->geometry;
    popup_update_position(popup);

    if (!wlr_box_equal(&before, &popup->geometry)) {
      damage_popup(popup);
    }
  }
}

static void
handle_text_input_disable(struct wl_listener *listener, void *data)
{
  (void)data;
  struct hikari_text_input *text_input =
      wl_container_of(listener, text_input, disable);
  struct hikari_input_method_relay *relay = text_input->relay;

  if (relay->input_method == NULL) {
    return;
  }

  wlr_input_method_v2_send_deactivate(relay->input_method);
  wlr_input_method_v2_send_done(relay->input_method);
}

static void
handle_text_input_destroy(struct wl_listener *listener, void *data)
{
  (void)data;
  struct hikari_text_input *text_input =
      wl_container_of(listener, text_input, destroy);

  wl_list_remove(&text_input->enable.link);
  wl_list_remove(&text_input->commit.link);
  wl_list_remove(&text_input->disable.link);
  wl_list_remove(&text_input->destroy.link);
  wl_list_remove(&text_input->link);
  free(text_input);
}

static void
handle_new_text_input(struct wl_listener *listener, void *data)
{
  struct hikari_input_method_relay *relay =
      wl_container_of(listener, relay, new_text_input);
  struct wlr_text_input_v3 *wlr_text_input = data;

  hikari_log_debug("new text_input created by client");

  struct hikari_text_input *text_input = calloc(1, sizeof(*text_input));
  if (text_input == NULL) {
    return;
  }

  text_input->input = wlr_text_input;
  text_input->relay = relay;

  text_input->enable.notify = handle_text_input_enable;
  wl_signal_add(&wlr_text_input->events.enable, &text_input->enable);

  text_input->commit.notify = handle_text_input_commit;
  wl_signal_add(&wlr_text_input->events.commit, &text_input->commit);

  text_input->disable.notify = handle_text_input_disable;
  wl_signal_add(&wlr_text_input->events.disable, &text_input->disable);

  text_input->destroy.notify = handle_text_input_destroy;
  wl_signal_add(&wlr_text_input->events.destroy, &text_input->destroy);

  wl_list_insert(&relay->text_inputs, &text_input->link);
}

static void
handle_im_commit(struct wl_listener *listener, void *data)
{
  (void)data;
  struct hikari_input_method_relay *relay =
      wl_container_of(listener, relay, input_method_commit);
  struct wlr_input_method_v2 *im = relay->input_method;

  struct hikari_text_input *text_input = relay_find_focused_text_input(relay);
  if (text_input == NULL) {
    return;
  }

  struct wlr_text_input_v3 *input = text_input->input;

  if (im->current.preedit.text) {
    wlr_text_input_v3_send_preedit_string(input, im->current.preedit.text,
        im->current.preedit.cursor_begin, im->current.preedit.cursor_end);
  } else {
    wlr_text_input_v3_send_preedit_string(input, NULL, 0, 0);
  }

  if (im->current.commit_text) {
    wlr_text_input_v3_send_commit_string(input, im->current.commit_text);
  }

  if (im->current.delete.before_length || im->current.delete.after_length) {
    wlr_text_input_v3_send_delete_surrounding_text(input,
        im->current.delete.before_length, im->current.delete.after_length);
  }

  wlr_text_input_v3_send_done(input);
}

static void
handle_im_grab_keyboard(struct wl_listener *listener, void *data)
{
  struct hikari_input_method_relay *relay =
      wl_container_of(listener, relay, input_method_grab_keyboard);
  struct wlr_input_method_keyboard_grab_v2 *keyboard_grab = data;

  hikari_log_debug("keyboard grab requested");

  struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(hikari_server.seat);
  if (keyboard != NULL) {
    hikari_log_debug("setting keyboard on grab");
    wlr_input_method_keyboard_grab_v2_set_keyboard(keyboard_grab, keyboard);
  } else {
    hikari_log_warn("no keyboard available for grab");
  }
}

static void
handle_im_destroy(struct wl_listener *listener, void *data)
{
  (void)data;
  struct hikari_input_method_relay *relay =
      wl_container_of(listener, relay, input_method_destroy);

  wl_list_remove(&relay->input_method_commit.link);
  wl_list_remove(&relay->input_method_grab_keyboard.link);
  wl_list_remove(&relay->input_method_new_popup.link);
  wl_list_remove(&relay->input_method_destroy.link);
  relay->input_method = NULL;
}

static void
handle_new_input_method(struct wl_listener *listener, void *data)
{
  struct hikari_input_method_relay *relay =
      wl_container_of(listener, relay, new_input_method);
  struct wlr_input_method_v2 *im = data;

  hikari_log_debug("new input method connected");

  if (relay->input_method != NULL) {
    hikari_log_debug("already have an input method, rejecting");
    wlr_input_method_v2_send_unavailable(im);
    return;
  }

  relay->input_method = im;

  relay->input_method_commit.notify = handle_im_commit;
  wl_signal_add(&im->events.commit, &relay->input_method_commit);

  relay->input_method_grab_keyboard.notify = handle_im_grab_keyboard;
  wl_signal_add(&im->events.grab_keyboard, &relay->input_method_grab_keyboard);

  relay->input_method_new_popup.notify = handle_im_new_popup;
  wl_signal_add(&im->events.new_popup_surface, &relay->input_method_new_popup);

  relay->input_method_destroy.notify = handle_im_destroy;
  wl_signal_add(&im->events.destroy, &relay->input_method_destroy);

  struct hikari_text_input *text_input = relay_find_focused_text_input(relay);
  if (text_input != NULL) {
    wlr_input_method_v2_send_activate(im);
    relay_send_im_state(relay, text_input->input);
  }
}

static void
handle_keyboard_focus_change(struct wl_listener *listener, void *data)
{
  struct hikari_input_method_relay *relay =
      wl_container_of(listener, relay, keyboard_focus_change);
  struct wlr_seat_keyboard_focus_change_event *event = data;

  struct hikari_text_input *text_input;
  wl_list_for_each (text_input, &relay->text_inputs, link) {
    if (text_input->input->focused_surface != NULL &&
        text_input->input->focused_surface == event->old_surface) {
      if (text_input->input->current_enabled && relay->input_method != NULL) {
        wlr_input_method_v2_send_deactivate(relay->input_method);
        wlr_input_method_v2_send_done(relay->input_method);
      }
      wlr_text_input_v3_send_leave(text_input->input);
    }
  }

  if (event->new_surface == NULL) {
    return;
  }

  struct wl_client *focus_client =
      wl_resource_get_client(event->new_surface->resource);

  wl_list_for_each (text_input, &relay->text_inputs, link) {
    if (text_input->input->seat == hikari_server.seat &&
        wl_resource_get_client(text_input->input->resource) == focus_client) {
      wlr_text_input_v3_send_enter(text_input->input, event->new_surface);
    }
  }
}

void
hikari_input_method_relay_output_destroyed(struct hikari_output *output)
{
  struct hikari_input_popup *popup;
  wl_list_for_each (popup, &hikari_server.input_method_relay.popups, link) {
    if (popup->output == output) {
      popup->output = NULL;
    }
  }
}

void
hikari_input_method_relay_init(struct hikari_input_method_relay *relay,
    struct wlr_text_input_manager_v3 *text_input_manager,
    struct wlr_input_method_manager_v2 *input_method_manager)
{
  wl_list_init(&relay->text_inputs);
  wl_list_init(&relay->popups);
  relay->input_method = NULL;

  relay->new_text_input.notify = handle_new_text_input;
  wl_signal_add(
      &text_input_manager->events.new_text_input, &relay->new_text_input);

  relay->new_input_method.notify = handle_new_input_method;
  wl_signal_add(
      &input_method_manager->events.new_input_method, &relay->new_input_method);

  relay->keyboard_focus_change.notify = handle_keyboard_focus_change;
  wl_signal_add(&hikari_server.seat->keyboard_state.events.focus_change,
      &relay->keyboard_focus_change);
}
