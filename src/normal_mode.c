#include <hikari/normal_mode.h>

#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_input_method_v2.h>
#include <wlr/types/wlr_seat.h>

#include <hikari/action.h>
#include <hikari/binding.h>
#include <hikari/binding_group.h>
#include <hikari/configuration.h>
#include <hikari/indicator.h>
#include <hikari/indicator_frame.h>
#include <hikari/log.h>
#include <hikari/keyboard.h>
#include <hikari/renderer.h>
#include <hikari/server.h>
#include <hikari/view.h>

#ifndef NDEBUG
#include <hikari/layout.h>
#include <hikari/sheet.h>
#include <hikari/workspace.h>
#endif

struct cursor_down_state {
  double lx;
  double ly;
  double sx;
  double sy;
};

static struct cursor_down_state cursor_down_state;

static bool
handle_input(struct hikari_binding_group *map, uint32_t code)
{
  int nbindings = map->nbindings;
  struct hikari_binding *bindings = map->bindings;

  for (int i = 0; i < nbindings; i++) {
    struct hikari_binding *binding = &bindings[i];

    if (binding->keycode == code) {
      struct hikari_event_action *event_action;

      if (binding->action->end.action != NULL) {
        hikari_server.normal_mode.pending_action = &binding->action->end;
      }

      event_action = &binding->action->begin;
      if (event_action->action != NULL) {
        event_action->action(event_action->arg);
      }
      return true;
    }
  }

  return false;
}

static bool
handle_pending_action(void)
{
  struct hikari_event_action *pending_action =
      hikari_server.normal_mode.pending_action;

  if (pending_action != NULL) {
    pending_action->action(pending_action->arg);
    hikari_server.normal_mode.pending_action = NULL;
    return true;
  }

  return false;
}

#ifndef NDEBUG
static void
dump_debug(struct hikari_server *server)
{
  struct hikari_view *view;
  hikari_log_trace("---------------------------------------------------------------------");
  hikari_log_trace("VIEWS");
  hikari_log_trace("---------------------------------------------------------------------");
  wl_list_for_each (view, &hikari_server.visible_views, visible_server_views) {
    hikari_log_trace("%p ", view);
  }
  hikari_log_trace("---------------------------------------------------------------------");
  hikari_log_trace("GROUPS");
  hikari_log_trace("---------------------------------------------------------------------");
  struct hikari_group *group;
  wl_list_for_each (group, &hikari_server.groups, server_groups) {
    hikari_log_trace("%s ", group->name);

    wl_list_for_each (view, &group->visible_views, visible_group_views) {
      hikari_log_trace("%p ", view);
    }

    hikari_log_trace("/ ");

    wl_list_for_each (view, &group->views, group_views) {
      hikari_log_trace("%p ", view);
    }

    hikari_log_trace("");
  }
  hikari_log_trace("---------------------------------------------------------------------");
  struct hikari_output *output;
  wl_list_for_each (output, &hikari_server.outputs, server_outputs) {
    struct hikari_workspace *workspace = output->workspace;
    const char *output_name = workspace->output->wlr_output->name;
    hikari_log_trace("SHEETS %s (%p)", output_name, workspace->focus_view);
    hikari_log_trace("---------------------------------------------------------------------");
    struct hikari_sheet *sheets = workspace->sheets;
    struct hikari_sheet *sheet;
    for (int i = 0; i < 10; i++) {
      sheet = &sheets[i];
      if (!wl_list_empty(&sheet->views)) {
        hikari_log_trace("%d ", sheet->nr);
        wl_list_for_each (view, &sheet->views, sheet_views) {
          hikari_log_trace("%p ", view);
        }
        hikari_log_trace("");
      }
    }
    hikari_log_trace("---------------------------------------------------------------------");
    if (workspace->sheet->layout != NULL) {
      hikari_log_trace("---------------------------------------------------------------------");
      hikari_log_trace("LAYOUT %s", output_name);
      struct hikari_tile *tile;
      wl_list_for_each (tile, &workspace->sheet->layout->tiles, layout_tiles) {
        hikari_log_trace("%p ", tile->view);
      }
      hikari_log_trace("");
    }
  }
  hikari_log_trace("/////////////////////////////////////////////////////////////////////");
}
#endif

static void
modifiers_handler(struct hikari_keyboard *keyboard)
{
  wlr_seat_set_keyboard(hikari_server.seat, keyboard->keyboard);
  struct hikari_view *focus_view = hikari_server.workspace->focus_view;

  if (hikari_server.keyboard_state.mod_released) {

    if (hikari_server_is_cycling() && focus_view != NULL) {
      hikari_view_raise(focus_view);
      hikari_view_center_cursor(focus_view);
      hikari_server_cursor_focus();
    }

    hikari_server_unset_cycling();
  }

  if (hikari_server.keyboard_state.mod_changed) {
    if (focus_view != NULL) {
      hikari_group_damage(focus_view->group);
      hikari_indicator_damage(&hikari_server.indicator, focus_view);
    }
#ifndef NDEBUG
    dump_debug(&hikari_server);
#endif
  }

  struct wlr_input_method_v2 *im =
      hikari_server.input_method_relay.input_method;
  if (im != NULL && im->keyboard_grab != NULL && !keyboard->is_virtual) {
    wlr_input_method_keyboard_grab_v2_set_keyboard(
        im->keyboard_grab, keyboard->keyboard);
    wlr_input_method_keyboard_grab_v2_send_modifiers(
        im->keyboard_grab, &keyboard->keyboard->modifiers);
  } else {
    wlr_seat_keyboard_notify_modifiers(
        hikari_server.seat, &keyboard->keyboard->modifiers);
  }
}

static void
cancel(void)
{}

static void
cursor_down_move(uint32_t time)
{
  struct wlr_seat *seat = hikari_server.seat;

  double x = hikari_server.cursor.wlr_cursor->x;
  double y = hikari_server.cursor.wlr_cursor->y;

  double moved_x = x - cursor_down_state.lx;
  double moved_y = y - cursor_down_state.ly;

  double sx = cursor_down_state.sx + moved_x;
  double sy = cursor_down_state.sy + moved_y;

  wlr_seat_pointer_notify_motion(seat, time, sx, sy);
}

static void
cursor_move(uint32_t time)
{
  assert(hikari_server_in_normal_mode());

  double sx, sy;
  struct wlr_seat *seat = hikari_server.seat;
  struct wlr_surface *surface;
  struct hikari_workspace *workspace;

  struct hikari_node *node =
      hikari_server_node_at(hikari_server.cursor.wlr_cursor->x,
          hikari_server.cursor.wlr_cursor->y,
          &surface,
          &workspace,
          &sx,
          &sy);

  if (node != NULL) {
    struct hikari_node *focus_node =
        (struct hikari_node *)hikari_server.workspace->focus_view;

    if (node != focus_node) {
      hikari_node_focus(node);
    }

    wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
    wlr_seat_pointer_notify_motion(seat, time, sx, sy);
  } else {
    if (hikari_server.workspace != workspace) {
      struct hikari_view *view = hikari_workspace_first_view(workspace);
      hikari_workspace_focus_view(workspace, view);
    }
    if (seat->pointer_state.focused_surface != NULL) {
      hikari_cursor_reset_image(&hikari_server.cursor);
    }
    wlr_seat_pointer_clear_focus(seat);
  }
}

static inline void
start_cursor_down_handling(struct wlr_pointer_button_event *event)
{
  double lx = hikari_server.cursor.wlr_cursor->x;
  double ly = hikari_server.cursor.wlr_cursor->y;

  double sx;
  double sy;
  struct hikari_workspace *workspace;
  struct wlr_surface *surface;

  struct hikari_node *node =
      hikari_server_node_at(lx, ly, &surface, &workspace, &sx, &sy);

  if (node != NULL) {
    hikari_server.normal_mode.mode.cursor_move = cursor_down_move;
    cursor_down_state.lx = lx;
    cursor_down_state.ly = ly;
    cursor_down_state.sx = sx;
    cursor_down_state.sy = sy;
  }

  wlr_seat_pointer_notify_button(
      hikari_server.seat, event->time_msec, event->button, event->state);
}

static inline void
stop_cursor_down_handling(struct wlr_pointer_button_event *event)
{
  hikari_server.normal_mode.mode.cursor_move = cursor_move;

  wlr_seat_pointer_notify_button(
      hikari_server.seat, event->time_msec, event->button, event->state);

  hikari_server_cursor_focus();
}

static inline bool
is_cursor_down(void)
{
  return hikari_server.normal_mode.mode.cursor_move == cursor_down_move;
}

static void
button_handler(
    struct hikari_cursor *cursor, struct wlr_pointer_button_event *event)
{
  if (handle_pending_action()) {
    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED && is_cursor_down()) {
      stop_cursor_down_handling(event);
    }
    return;
  }

  if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
    uint32_t modifiers = hikari_server.keyboard_state.modifiers;
    struct hikari_binding_group *map = &cursor->bindings[modifiers];

    if (!handle_input(map, event->button)) {
      start_cursor_down_handling(event);
    }
  } else {
    if (is_cursor_down()) {
      stop_cursor_down_handling(event);
    } else {
      wlr_seat_pointer_notify_button(
          hikari_server.seat, event->time_msec, event->button, event->state);
    }
  }
}

static void
key_handler(
    struct hikari_keyboard *keyboard, struct wlr_keyboard_key_event *event)
{
  if (handle_pending_action()) {
    return;
  }

  if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
    uint32_t modifiers = hikari_server.keyboard_state.modifiers;
    struct hikari_binding_group *bindings = &keyboard->bindings[modifiers];

    if (handle_input(bindings, event->keycode)) {
      hikari_log_debug("consumed: keycode=%u modifiers=0x%x", event->keycode, modifiers);
      return;
    }
  }

  wlr_seat_set_keyboard(hikari_server.seat, keyboard->keyboard);

  struct wlr_input_method_v2 *im =
      hikari_server.input_method_relay.input_method;
  if (im != NULL && im->keyboard_grab != NULL && !keyboard->is_virtual) {
    hikari_log_debug("forwarding to IM grab: keycode=%u state=%u", event->keycode, event->state);
    wlr_input_method_keyboard_grab_v2_send_key(
        im->keyboard_grab, event->time_msec, event->keycode, event->state);
  } else {
    hikari_log_debug("forwarding to seat: keycode=%u state=%u im=%p grab=%p virt=%d", event->keycode, event->state, (void *)im, im ? (void *)im->keyboard_grab : NULL, keyboard->is_virtual);
    wlr_seat_keyboard_notify_key(
        hikari_server.seat, event->time_msec, event->keycode, event->state);
  }
}

void
hikari_normal_mode_init(struct hikari_normal_mode *normal_mode)
{
  normal_mode->mode.key_handler = key_handler;
  normal_mode->mode.button_handler = button_handler;
  normal_mode->mode.modifiers_handler = modifiers_handler;
  normal_mode->mode.render = hikari_renderer_normal_mode;
  normal_mode->mode.cancel = cancel;
  normal_mode->mode.cursor_move = cursor_move;
  normal_mode->pending_action = NULL;
}

void
hikari_normal_mode_enter(void)
{
  struct hikari_server *server = &hikari_server;

  assert(server->workspace != NULL);

  hikari_server.normal_mode.mode.cursor_move = cursor_move;

  server->mode->cancel();
  server->mode = (struct hikari_mode *)&server->normal_mode;
}
