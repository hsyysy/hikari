#include <hikari/xdg_view.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/util/edges.h>

#include <hikari/configuration.h>
#include <hikari/geometry.h>
#include <hikari/log.h>
#include <hikari/mark.h>
#include <hikari/output.h>
#include <hikari/server.h>
#include <hikari/sheet.h>
#include <hikari/view.h>
#include <hikari/view_config.h>
#include <hikari/workspace.h>

static void
new_popup_handler(struct wl_listener *listener, void *data);

static void
apply_fullscreen(struct hikari_xdg_view *xdg_view, bool allow_migrate);

static void
fullscreen_request(struct hikari_xdg_view *xdg_view);

static void
request_fullscreen_handler(struct wl_listener *listener, void *data);

static void
set_title_handler(struct wl_listener *listener, void *data)
{
  (void)data;
  struct hikari_xdg_view *xdg_view =
      wl_container_of(listener, xdg_view, set_title);

  hikari_view_set_title(
      (struct hikari_view *)xdg_view, xdg_view->surface->toplevel->title);
}

static void
commit_handler(struct wl_listener *listener, void *data)
{
  (void)data;
  struct hikari_xdg_view *xdg_view =
      wl_container_of(listener, xdg_view, commit);

  struct hikari_view *view = (struct hikari_view *)xdg_view;
  struct wlr_xdg_surface *surface = xdg_view->surface;

  if (surface->initial_commit) {
    struct wlr_xdg_toplevel_decoration_v1 *dec;
    wl_list_for_each(dec,
        &hikari_server.xdg_decoration_manager->decorations, link) {
      if (dec->toplevel == surface->toplevel) {
        wlr_xdg_toplevel_decoration_v1_set_mode(
            dec, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        break;
      }
    }
    wlr_xdg_toplevel_set_size(surface->toplevel, 0, 0);
    wlr_xdg_surface_schedule_configure(surface);
    return;
  }

  if (!hikari_view_is_mapped(view)) {
    return;
  }

  view->surface_geometry_x = surface->geometry.x;
  view->surface_geometry_y = surface->geometry.y;

  uint32_t serial = surface->current.configure_serial;

  if (hikari_view_was_updated(view, serial)) {
    struct wlr_box new_geometry = surface->geometry;

    switch (view->pending_operation.type) {
      case HIKARI_OPERATION_TYPE_TILE:
      case HIKARI_OPERATION_TYPE_FULL_MAXIMIZE:
      case HIKARI_OPERATION_TYPE_VERTICAL_MAXIMIZE:
      case HIKARI_OPERATION_TYPE_HORIZONTAL_MAXIMIZE:
        wlr_xdg_toplevel_set_tiled(surface->toplevel,
            WLR_EDGE_LEFT | WLR_EDGE_RIGHT | WLR_EDGE_TOP | WLR_EDGE_BOTTOM);
        break;

      case HIKARI_OPERATION_TYPE_RESET:
      case HIKARI_OPERATION_TYPE_UNMAXIMIZE:
        wlr_xdg_toplevel_set_tiled(surface->toplevel, WLR_EDGE_NONE);
        break;

      case HIKARI_OPERATION_TYPE_RESIZE:
        break;
    }
    hikari_view_commit_pending_operation(view, &new_geometry);

    // the user took the maximization of a fullscreen view into his own hands,
    // hikari has nothing left to undo once the client leaves fullscreen
    if (xdg_view->fullscreen_maximized &&
        !hikari_view_is_fully_maximized(view)) {
      xdg_view->fullscreen_maximized = false;
    }

    // the view has settled, a fullscreen request that was waiting for it can be
    // applied now
    if (xdg_view->fullscreen_pending) {
      apply_fullscreen(xdg_view, true);
    }
  } else {
    struct wlr_box *geometry = hikari_view_geometry(view);
    struct hikari_output *output = view->output;
    bool visible = !hikari_view_is_hidden(view);

    struct wlr_box new_geometry = surface->geometry;

    if (new_geometry.width != geometry->width ||
        new_geometry.height != geometry->height) {
      if (visible) {
        hikari_view_damage_whole(view);
      }

      geometry->width = new_geometry.width;
      geometry->height = new_geometry.height;

      hikari_view_refresh_geometry(view, geometry);

      if (visible) {
        hikari_view_damage_whole(view);
      } else if (output->enabled) {
        hikari_output_schedule_frame(output);
      }
    } else if (output->enabled) {
      if (visible) {
        hikari_output_add_effective_surface_damage(
            output, surface->surface,
            geometry->x - view->surface_geometry_x,
            geometry->y - view->surface_geometry_y);
      } else {
        hikari_output_schedule_frame(output);
      }
    }
  }
}

static inline const char *
get_app_id(struct hikari_xdg_view *xdg_view)
{
  const char *app_id = xdg_view->surface->toplevel->app_id;

  return app_id == NULL ? "" : app_id;
}

static void
first_map(struct hikari_xdg_view *xdg_view, bool *focus)
{
  (void)focus;
  struct wlr_xdg_surface *xdg_surface = xdg_view->surface;
  assert(xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL);

  struct hikari_view *view = (struct hikari_view *)xdg_view;
  struct wlr_box *geometry = &xdg_view->view.geometry;

  *geometry = xdg_surface->geometry;
  view->surface_geometry_x = geometry->x;
  view->surface_geometry_y = geometry->y;
  geometry->x = 0;
  geometry->y = 0;
  hikari_view_refresh_geometry(view, geometry);

  const char *app_id = get_app_id(xdg_view);
  hikari_log_debug("first_map: app_id='%s' geometry=%dx%d+%d+%d",
      app_id, geometry->width, geometry->height, geometry->x, geometry->y);

  struct hikari_view_config *view_config =
      hikari_configuration_resolve_view_config(hikari_configuration, app_id);

  struct wlr_xdg_toplevel *xdg_toplevel = xdg_surface->toplevel;

  hikari_view_set_title(view, xdg_toplevel->title);
  hikari_view_configure(view, app_id, view_config);
  hikari_log_debug("first_map: configured, sheet=%p output=%p",
      (void *)view->sheet, (void *)view->output);
}

static struct wlr_surface *
surface_at(
    struct hikari_node *node, double ox, double oy, double *sx, double *sy)
{
  struct hikari_xdg_view *xdg_view = (struct hikari_xdg_view *)node;

  struct hikari_view *view = (struct hikari_view *)node;

  struct wlr_box *geometry = hikari_view_geometry(view);

  // wlr_xdg_surface_surface_at expects surface-local coordinates (relative to
  // the wlr_surface origin), not XDG geometry-relative coordinates. Account for
  // the XDG geometry offset so CSD shadows/decorations are handled correctly.
  double x = ox - geometry->x + view->surface_geometry_x;
  double y = oy - geometry->y + view->surface_geometry_y;

  return wlr_xdg_surface_surface_at(xdg_view->surface, x, y, sx, sy);
}

static void
map(struct hikari_view *view, bool focus)
{
  hikari_log_debug("xdg map: view=%p focus=%d", (void *)view, focus);

  struct hikari_xdg_view *xdg_view = (struct hikari_xdg_view *)view;
  struct wlr_xdg_surface *xdg_surface = xdg_view->surface;

  xdg_view->set_title.notify = set_title_handler;
  wl_signal_add(
      &xdg_view->surface->toplevel->events.set_title, &xdg_view->set_title);

  xdg_view->request_fullscreen.notify = request_fullscreen_handler;
  wl_signal_add(&xdg_surface->toplevel->events.request_fullscreen,
      &xdg_view->request_fullscreen);

  xdg_view->new_popup.notify = new_popup_handler;
  wl_signal_add(&xdg_surface->events.new_popup, &xdg_view->new_popup);

  hikari_view_map(view, xdg_surface->surface);
}

static void
map_handler(struct wl_listener *listener, void *data)
{
  (void)data;
  struct hikari_xdg_view *xdg_view = wl_container_of(listener, xdg_view, map);

  struct hikari_view *view = (struct hikari_view *)xdg_view;
  bool focus = false;

  hikari_log_debug("map_handler: view=%p unmanaged=%d sheet=%p",
      (void *)view, hikari_view_is_unmanaged(view), (void *)view->sheet);

  if (hikari_view_is_unmanaged(view)) {
    first_map(xdg_view, &focus);
  }

  map(view, focus);

  struct wlr_xdg_toplevel *toplevel = xdg_view->surface->toplevel;

  // wlroots suppresses `events.request_fullscreen` until the xdg surface is
  // initialized, a client that requests fullscreen before its initial commit
  // has to be acknowledged here
  if (toplevel->requested.fullscreen && !toplevel->current.fullscreen) {
    fullscreen_request(xdg_view);
  }
}

static void
unmap(struct hikari_view *view)
{
  hikari_log_trace("XDG UNMAP %p", view);

  struct hikari_xdg_view *xdg_view = (struct hikari_xdg_view *)view;

  assert(xdg_view->surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL);

  hikari_view_unmap(view);

  // the flags describe the fullscreen state of the surface that is going away,
  // a remap starts over with `map_handler` looking at the request again
  xdg_view->fullscreen_pending = false;
  xdg_view->fullscreen_maximized = false;

  wl_list_remove(&xdg_view->set_title.link);
  wl_list_remove(&xdg_view->request_fullscreen.link);
  wl_list_remove(&xdg_view->new_popup.link);
}

static void
unmap_handler(struct wl_listener *listener, void *data)
{
  (void)data;
  struct hikari_xdg_view *xdg_view = wl_container_of(listener, xdg_view, unmap);

  unmap((struct hikari_view *)xdg_view);
}

static void
activate(struct hikari_view *view, bool active)
{
  struct hikari_xdg_view *xdg_view = (struct hikari_xdg_view *)view;

  if (xdg_view->surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
    wlr_xdg_toplevel_set_activated(xdg_view->surface->toplevel, active);

    hikari_view_damage_whole(view);
  }
}

static uint32_t
resize(struct hikari_view *view, int width, int height)
{
  struct hikari_xdg_view *xdg_view = (struct hikari_xdg_view *)view;

  if (xdg_view->surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
    return wlr_xdg_toplevel_set_size(xdg_view->surface->toplevel, width, height);
  }

  return 0;
}

static void
quit(struct hikari_view *view)
{
  struct hikari_xdg_view *xdg_view = (struct hikari_xdg_view *)view;

  wlr_xdg_toplevel_send_close(xdg_view->surface->toplevel);
}

static void
destroy_handler(struct wl_listener *listener, void *data)
{
  (void)data;
  struct hikari_xdg_view *xdg_view =
      wl_container_of(listener, xdg_view, destroy);

  struct hikari_view *view = (struct hikari_view *)xdg_view;

  if (hikari_view_is_mapped(view)) {
    unmap(view);
  }

  wl_list_remove(&xdg_view->map.link);
  wl_list_remove(&xdg_view->unmap.link);
  wl_list_remove(&xdg_view->destroy.link);
  wl_list_remove(&xdg_view->commit.link);

  hikari_view_fini(view);
  hikari_free(xdg_view);
}

static void
focus(struct hikari_node *node)
{
  struct hikari_view *view = (struct hikari_view *)node;

  hikari_workspace_focus_view(view->sheet->workspace, view);
}

static void
for_each_surface(struct hikari_node *node,
    void (*func)(struct wlr_surface *, int, int, void *),
    void *data)
{
  struct hikari_xdg_view *xdg_view = (struct hikari_xdg_view *)node;

  wlr_xdg_surface_for_each_surface(xdg_view->surface, func, data);
}

static void
destroy_popup_handler(struct wl_listener *listener, void *data)
{
  (void)data;
  hikari_log_trace("DESTROY POPUP");
  struct hikari_xdg_popup *popup = wl_container_of(listener, popup, destroy);

  hikari_view_child_fini(&popup->view_child);

  wl_list_remove(&popup->destroy.link);
  wl_list_remove(&popup->unmap.link);
  wl_list_remove(&popup->map.link);
  wl_list_remove(&popup->commit.link);
  wl_list_remove(&popup->reposition.link);
  wl_list_remove(&popup->new_popup.link);

  hikari_free(popup);
}

static void
xdg_popup_create(struct wlr_xdg_popup *wlr_popup, struct hikari_view *parent);

static void
new_popup_popup_handler(struct wl_listener *listener, void *data)
{
  struct hikari_xdg_popup *xdg_popup =
      wl_container_of(listener, xdg_popup, new_popup);

  struct wlr_xdg_popup *wlr_popup = data;

  xdg_popup_create(wlr_popup, xdg_popup->view_child.parent);
}

static void
new_popup_handler(struct wl_listener *listener, void *data)
{
  struct hikari_xdg_view *xdg_view =
      wl_container_of(listener, xdg_view, new_popup);

  struct wlr_xdg_popup *wlr_popup = data;

  xdg_popup_create(wlr_popup, &xdg_view->view);
}

static void
popup_map(struct wl_listener *listener, void *data)
{
  (void)data;
  hikari_log_trace("POPUP MAP");

  struct hikari_xdg_popup *xdg_popup =
      wl_container_of(listener, xdg_popup, map);

  struct hikari_view *parent = xdg_popup->view_child.parent;

  hikari_view_damage_surface(parent, xdg_popup->view_child.surface, true);
}

static void
popup_unmap(struct wl_listener *listener, void *data)
{
  (void)data;
  hikari_log_trace("POPUP UNMAP");

  struct hikari_xdg_popup *xdg_popup =
      wl_container_of(listener, xdg_popup, unmap);

  struct hikari_view *parent = xdg_popup->view_child.parent;

  hikari_view_damage_surface(parent, xdg_popup->view_child.surface, true);
}

static void
popup_unconstrain(struct hikari_xdg_popup *popup)
{
  struct hikari_view *view = popup->view_child.parent;
  struct wlr_xdg_popup *wlr_popup = popup->popup;

  struct hikari_output *output = view->output;

  // a view that is not on an output yet leaves no box to constrain to, the
  // popup stays unconfigured until the client repositions it
  if (output == NULL) {
    return;
  }

  struct wlr_box *geometry = hikari_view_geometry(view);

  struct wlr_box output_toplevel_sx_box = {
    .x = -geometry->x,
    .y = -geometry->y,
    .width = output->geometry.width,
    .height = output->geometry.height,
  };

  wlr_xdg_popup_unconstrain_from_box(wlr_popup, &output_toplevel_sx_box);
}

static void
popup_commit(struct wl_listener *listener, void *data)
{
  (void)data;

  struct hikari_xdg_popup *popup = wl_container_of(listener, popup, commit);

  // wlroots only schedules a configure for an initialized surface, so the popup
  // is unconstrained on its first commit. the role commit that sets
  // `initial_commit` runs before this signal, so it is already true here
  if (popup->popup->base->initial_commit) {
    popup_unconstrain(popup);
  }
}

static void
popup_reposition(struct wl_listener *listener, void *data)
{
  (void)data;

  struct hikari_xdg_popup *popup =
      wl_container_of(listener, popup, reposition);

  popup_unconstrain(popup);
}

static void
xdg_popup_create(struct wlr_xdg_popup *wlr_popup, struct hikari_view *parent)
{
  struct hikari_xdg_popup *popup =
      hikari_malloc(sizeof(struct hikari_xdg_popup));

  hikari_log_trace("CREATE POPUP");

  popup->view_child.parent = parent;
  popup->popup = wlr_popup;

  wlr_popup->base->surface->data = parent;

  // the popup is destroyed before its xdg_surface, and destroy_xdg_popup()
  // asserts that no reposition listener is left behind, so the handler has to
  // run on the popup's own destroy signal
  popup->destroy.notify = destroy_popup_handler;
  wl_signal_add(&wlr_popup->events.destroy, &popup->destroy);

  popup->new_popup.notify = new_popup_popup_handler;
  wl_signal_add(&wlr_popup->base->events.new_popup, &popup->new_popup);

  popup->map.notify = popup_map;
  wl_signal_add(&wlr_popup->base->surface->events.map, &popup->map);

  popup->unmap.notify = popup_unmap;
  wl_signal_add(&wlr_popup->base->surface->events.unmap, &popup->unmap);

  popup->commit.notify = popup_commit;
  wl_signal_add(&wlr_popup->base->surface->events.commit, &popup->commit);

  popup->reposition.notify = popup_reposition;
  wl_signal_add(&wlr_popup->events.reposition, &popup->reposition);

  hikari_view_child_init(
      (struct hikari_view_child *)popup, parent, wlr_popup->base->surface);
}

static struct hikari_output *
resolve_fullscreen_output(struct wlr_xdg_toplevel *toplevel)
{
  struct wlr_output *wlr_output = toplevel->requested.fullscreen_output;

  if (wlr_output == NULL) {
    return NULL;
  }

  struct hikari_output *output = wlr_output->data;

  // a client can request fullscreen on any output it has a handle for, even on
  // one hikari does not render on
  if (output == NULL || !output->enabled) {
    return NULL;
  }

  return output;
}

// migrating a view is a user level operation, it shows and raises the view and
// `hikari_server_migrate_focus_view` even makes the workspace of the output the
// current one. none of that may be driven by a client while the view is forced
// visible by lock mode or while the user is in a mode that keeps state about
// the current workspace.
static bool
can_migrate_for_fullscreen(struct hikari_view *view)
{
  return hikari_server_in_normal_mode() && !hikari_view_is_forced(view);
}

// hikari arranges the geometry of its views on its own, so `wlr_xdg_toplevel`
// acknowledging a fullscreen request does not make a view fill an output by
// itself. the request is applied like `view-toggle-maximize-full` does, but
// without moving the cursor, on the output the client asked for. a view that is
// hidden or waits for a commit cannot be maximized, so the request stays
// pending until the view has settled or is shown again.
static void
apply_pending_fullscreen(struct hikari_xdg_view *xdg_view, bool allow_migrate)
{
  struct hikari_view *view = &xdg_view->view;

  if (hikari_view_is_hidden(view) || hikari_view_is_dirty(view)) {
    return;
  }

  struct wlr_xdg_toplevel *toplevel = xdg_view->surface->toplevel;

  if (!toplevel->requested.fullscreen) {
    xdg_view->fullscreen_pending = false;

    // only undo the maximize hikari applied for the fullscreen request, a view
    // the user has maximized himself stays maximized
    if (!xdg_view->fullscreen_maximized) {
      return;
    }

    xdg_view->fullscreen_maximized = false;

    hikari_view_set_full_maximized(view, false);

    return;
  }

  struct hikari_output *output = resolve_fullscreen_output(toplevel);

  if (allow_migrate && output != NULL && output != view->output &&
      can_migrate_for_fullscreen(view)) {
    double lx = output->geometry.x + output->geometry.width / 2.0;
    double ly = output->geometry.y + output->geometry.height / 2.0;

    if (hikari_view_has_focus(view)) {
      hikari_server_migrate_focus_view(output, lx, ly, false);
    } else {
      // a view that is the focus view of another workspace has to lose that
      // focus first, `hikari_view_migrate` does not take care of it
      hikari_view_clear_focus(view);

      hikari_view_migrate(view,
          output->workspace->sheet,
          (int)(lx - output->geometry.x),
          (int)(ly - output->geometry.y),
          false);
    }

    // migrating resets the view, so the maximize has to wait until the reset
    // has been committed
    if (hikari_view_is_dirty(view)) {
      return;
    }
  }

  xdg_view->fullscreen_pending = false;

  if (!hikari_view_is_fully_maximized(view)) {
    xdg_view->fullscreen_maximized = true;

    hikari_view_set_full_maximized(view, true);
  }
}

static void
apply_fullscreen(struct hikari_xdg_view *xdg_view, bool allow_migrate)
{
  // `hikari_view_migrate` shows the view, which calls back in through the
  // `shown` hook
  if (xdg_view->fullscreen_applying) {
    return;
  }

  xdg_view->fullscreen_applying = true;
  apply_pending_fullscreen(xdg_view, allow_migrate);
  xdg_view->fullscreen_applying = false;
}

static void
shown(struct hikari_view *view)
{
  struct hikari_xdg_view *xdg_view = (struct hikari_xdg_view *)view;

  // the view was not visible when the client requested fullscreen, nothing
  // else makes the request happen once the user brings the view up.
  //
  // the output the client asked for is ignored here. views are shown from
  // within iterations over sheets, groups and layouts, migrating one would
  // relink the very lists that are being walked. dragging a view the user just
  // brought up onto another output is not wanted either.
  if (xdg_view->fullscreen_pending) {
    apply_fullscreen(xdg_view, false);
  }
}

static void
fullscreen_request(struct hikari_xdg_view *xdg_view)
{
  struct wlr_xdg_toplevel *toplevel = xdg_view->surface->toplevel;

  wlr_xdg_toplevel_set_fullscreen(toplevel, toplevel->requested.fullscreen);

  xdg_view->fullscreen_pending = true;

  apply_fullscreen(xdg_view, true);
}

static void
request_fullscreen_handler(struct wl_listener *listener, void *data)
{
  (void)data;
  struct hikari_xdg_view *xdg_view =
      wl_container_of(listener, xdg_view, request_fullscreen);

  fullscreen_request(xdg_view);
}

static void
constraints(struct hikari_view *view,
    int *min_width,
    int *min_height,
    int *max_width,
    int *max_height)
{
  struct hikari_xdg_view *xdg_view = (struct hikari_xdg_view *)view;
  struct wlr_xdg_toplevel_state *state = &xdg_view->surface->toplevel->current;

  *min_width = state->min_width > 0 ? state->min_width : 0;
  *min_height = state->min_height > 0 ? state->min_height : 0;
  *max_width =
      state->max_width > 0 ? state->max_width : view->output->geometry.width;
  *max_height =
      state->max_height > 0 ? state->max_height : view->output->geometry.height;
}

void
hikari_xdg_view_init(struct hikari_xdg_view *xdg_view,
    struct wlr_xdg_surface *xdg_surface,
    struct hikari_workspace *workspace)
{
  assert(xdg_surface->toplevel != NULL);

  bool child = xdg_surface->toplevel->parent != NULL;

  hikari_view_init(&xdg_view->view, child, workspace);

  xdg_view->fullscreen_pending = false;
  xdg_view->fullscreen_maximized = false;
  xdg_view->fullscreen_applying = false;

  hikari_log_trace("NEW XDG %p", xdg_view);

  xdg_view->view.node.surface_at = surface_at;

  wlr_xdg_surface_ping(xdg_surface);

  xdg_view->surface = xdg_surface;
  xdg_view->surface->data = xdg_view;

  xdg_view->map.notify = map_handler;
  wl_signal_add(&xdg_surface->surface->events.map, &xdg_view->map);

  xdg_view->unmap.notify = unmap_handler;
  wl_signal_add(&xdg_surface->surface->events.unmap, &xdg_view->unmap);

  xdg_view->destroy.notify = destroy_handler;
  wl_signal_add(&xdg_surface->events.destroy, &xdg_view->destroy);

  xdg_view->commit.notify = commit_handler;
  wl_signal_add(&xdg_surface->surface->events.commit, &xdg_view->commit);

  assert(xdg_view->surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL);

  xdg_view->view.node.focus = focus;
  xdg_view->view.node.for_each_surface = for_each_surface;
  xdg_view->view.activate = activate;
  xdg_view->view.resize = resize;
  xdg_view->view.quit = quit;
  xdg_view->view.constraints = constraints;
  xdg_view->view.shown = shown;
#ifdef HAVE_XWAYLAND
  xdg_view->view.move = NULL;
  xdg_view->view.move_resize = NULL;
#endif
}
