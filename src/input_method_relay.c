#include <hikari/input_method_relay.h>

#include <stdlib.h>

#include <wlr/types/wlr_input_method_v2.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_compositor.h>

#include <hikari/server.h>

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
handle_text_input_enable(struct wl_listener *listener, void *data)
{
  struct hikari_text_input *text_input =
      wl_container_of(listener, text_input, enable);
  struct hikari_input_method_relay *relay = text_input->relay;

  if (relay->input_method == NULL) {
    return;
  }

  wlr_input_method_v2_send_activate(relay->input_method);
  relay_send_im_state(relay, text_input->input);
}

static void
handle_text_input_commit(struct wl_listener *listener, void *data)
{
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
}

static void
handle_text_input_disable(struct wl_listener *listener, void *data)
{
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

  struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(hikari_server.seat);
  if (keyboard != NULL) {
    wlr_input_method_keyboard_grab_v2_set_keyboard(keyboard_grab, keyboard);
  }
}

static void
handle_im_destroy(struct wl_listener *listener, void *data)
{
  struct hikari_input_method_relay *relay =
      wl_container_of(listener, relay, input_method_destroy);

  wl_list_remove(&relay->input_method_commit.link);
  wl_list_remove(&relay->input_method_grab_keyboard.link);
  wl_list_remove(&relay->input_method_destroy.link);
  relay->input_method = NULL;
}

static void
handle_new_input_method(struct wl_listener *listener, void *data)
{
  struct hikari_input_method_relay *relay =
      wl_container_of(listener, relay, new_input_method);
  struct wlr_input_method_v2 *im = data;

  if (relay->input_method != NULL) {
    wlr_input_method_v2_send_unavailable(im);
    return;
  }

  relay->input_method = im;

  relay->input_method_commit.notify = handle_im_commit;
  wl_signal_add(&im->events.commit, &relay->input_method_commit);

  relay->input_method_grab_keyboard.notify = handle_im_grab_keyboard;
  wl_signal_add(&im->events.grab_keyboard, &relay->input_method_grab_keyboard);

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
hikari_input_method_relay_init(struct hikari_input_method_relay *relay,
    struct wlr_text_input_manager_v3 *text_input_manager,
    struct wlr_input_method_manager_v2 *input_method_manager)
{
  wl_list_init(&relay->text_inputs);
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
