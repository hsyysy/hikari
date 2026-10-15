/* Real server-side Wayland resources and protocol logging, without a running
 * compositor. These checks exercise src/pinch.c, including destruction paths. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-client.h>

#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_pointer_gestures_v1.h>
#include <wlr/types/wlr_seat.h>

#include <hikari/pinch.h>
#include "pointer-gestures-unstable-v1-protocol.h"

#define CHECK(condition) do { \
  if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
    exit(EXIT_FAILURE); \
  } \
} while (0)

struct observed {
  struct wl_resource *resource;
  unsigned begin, update, end, cancelled;
  double scale;
};

struct fixture {
  struct wl_display *display;
  struct wl_protocol_logger *logger;
  struct wlr_pointer_gestures_v1 *gestures;
  struct wlr_seat seat;
  struct wlr_seat_client clients[2];
  struct wlr_surface surfaces[2];
  struct wlr_pointer pointers[2];
  int peers[2];
  bool pointer_finished[2];
  struct observed observed[4];
  struct hikari_pinch pinch;
};

static void
log_event(void *data, enum wl_protocol_logger_type type,
    const struct wl_protocol_logger_message *message)
{
  struct fixture *fixture = data;
  if (type != WL_PROTOCOL_LOGGER_EVENT) {
    return;
  }
  for (size_t i = 0; i < 4; i++) {
    struct observed *observed = &fixture->observed[i];
    if (observed->resource != message->resource) {
      continue;
    }
    if (strcmp(message->message->name, "begin") == 0) {
      observed->begin++;
    } else if (strcmp(message->message->name, "update") == 0) {
      observed->update++;
      observed->scale = wl_fixed_to_double(message->arguments[3].f);
    } else if (strcmp(message->message->name, "end") == 0) {
      observed->end++;
      observed->cancelled += message->arguments[2].i != 0;
    }
  }
}

static void
resource_destroy(struct wl_resource *resource)
{
  wl_list_remove(wl_resource_get_link(resource));
}

static void
gesture_destroy(struct wl_client *client, struct wl_resource *resource)
{
  (void)client;
  wl_resource_destroy(resource);
}

static const struct zwp_pointer_gesture_pinch_v1_interface implementation = {
  .destroy = gesture_destroy,
};

static void
subscribe(struct fixture *fixture, size_t slot, size_t client, bool inert)
{
  struct wl_resource *resource = wl_resource_create(
      fixture->clients[client].client, &zwp_pointer_gesture_pinch_v1_interface, 3, 0);
  CHECK(resource != NULL);
  /* Match wlroots 0.21's resource layout: owning seat in user data, resource
   * link in manager->pinches; a destroyed pointer produces an inert resource. */
  wl_resource_set_implementation(resource, &implementation,
      inert ? NULL : &fixture->seat, resource_destroy);
  wl_list_insert(&fixture->gestures->pinches, wl_resource_get_link(resource));
  fixture->observed[slot].resource = resource;
}

static void
focus(struct fixture *fixture, int client)
{
  struct wlr_surface *surface = client < 0 ? NULL : &fixture->surfaces[client];
  struct wlr_seat_pointer_focus_change_event event = {
    .seat = &fixture->seat,
    .old_surface = fixture->seat.pointer_state.focused_surface,
    .new_surface = surface,
  };
  /* wlroots emits focus_change AFTER replacing the seat's focus. */
  fixture->seat.pointer_state.focused_surface = surface;
  fixture->seat.pointer_state.focused_client = client < 0 ? NULL : &fixture->clients[client];
  wl_signal_emit_mutable(&fixture->seat.pointer_state.events.focus_change, &event);
}

static void
init(struct fixture *fixture)
{
  *fixture = (struct fixture){0};
  fixture->display = wl_display_create();
  CHECK(fixture->display != NULL);
  fixture->gestures = wlr_pointer_gestures_v1_create(fixture->display);
  CHECK(fixture->gestures != NULL);
  wl_signal_init(&fixture->seat.pointer_state.events.focus_change);
  for (size_t i = 0; i < 2; i++) {
    int sockets[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    fixture->peers[i] = sockets[1];
    struct wlr_seat_client *client = &fixture->clients[i];
    client->client = wl_client_create(fixture->display, sockets[0]);
    CHECK(client->client != NULL);
    client->seat = &fixture->seat;
    wl_signal_init(&client->events.destroy);
    struct wlr_surface *surface = &fixture->surfaces[i];
    surface->resource = wl_resource_create(client->client, &wl_surface_interface, 1, 0);
    CHECK(surface->resource != NULL);
    wl_signal_init(&surface->events.destroy);
    wlr_pointer_init(&fixture->pointers[i], NULL, "pinch-test");
    subscribe(fixture, i, i, false);
  }
  fixture->logger = wl_display_add_protocol_logger(fixture->display, log_event, fixture);
  CHECK(fixture->logger != NULL);
  hikari_pinch_init(&fixture->pinch, fixture->gestures, &fixture->seat);
  focus(fixture, 0);
}

static void
fini(struct fixture *fixture)
{
  hikari_pinch_fini(&fixture->pinch);
  for (size_t i = 0; i < 2; i++) {
    if (!fixture->pointer_finished[i]) {
      wlr_pointer_finish(&fixture->pointers[i]);
    }
    if (fixture->clients[i].client != NULL) {
      wl_client_destroy(fixture->clients[i].client);
    }
    close(fixture->peers[i]);
  }
  wl_protocol_logger_destroy(fixture->logger);
  wl_display_destroy(fixture->display);
}

static void
begin(struct fixture *fixture, size_t pointer)
{
  struct wlr_pointer_pinch_begin_event event = {
    .pointer = &fixture->pointers[pointer], .time_msec = 1, .fingers = 2,
  };
  hikari_pinch_begin(&fixture->pinch, &event);
}

static void
update(struct fixture *fixture, size_t pointer)
{
  struct wlr_pointer_pinch_update_event event = {
    .pointer = &fixture->pointers[pointer], .time_msec = 2, .scale = 1.5,
  };
  hikari_pinch_update(&fixture->pinch, &event);
}

static void
end(struct fixture *fixture, size_t pointer)
{
  struct wlr_pointer_pinch_end_event event = {
    .pointer = &fixture->pointers[pointer], .time_msec = 3,
  };
  hikari_pinch_end(&fixture->pinch, &event);
}

/* Validate the resource contract against the installed wlroots library too,
 * rather than relying only on resources constructed by the fixture above. */
static void
test_wlroots_subscription(void)
{
  struct fixture f = {0};
  f.display = wl_display_create();
  CHECK(f.display != NULL);
  struct wlr_seat *seat = wlr_seat_create(f.display, "pinch-test");
  CHECK(seat != NULL);
  wlr_seat_set_capabilities(seat, WL_SEAT_CAPABILITY_POINTER);
  f.gestures = wlr_pointer_gestures_v1_create(f.display);
  CHECK(f.gestures != NULL);
  int sockets[2];
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  struct wl_client *server_client = wl_client_create(f.display, sockets[0]);
  CHECK(server_client != NULL);
  struct wl_display *client_display = wl_display_connect_to_fd(sockets[1]);
  CHECK(client_display != NULL);
  struct wl_registry *registry = wl_display_get_registry(client_display);
  struct wl_seat *client_seat = wl_registry_bind(registry,
      wl_global_get_name(seat->global, server_client), &wl_seat_interface, 1);
  struct wl_pointer *pointer = wl_seat_get_pointer(client_seat);
  struct wl_proxy *manager = wl_registry_bind(registry,
      wl_global_get_name(f.gestures->global, server_client), &zwp_pointer_gestures_v1_interface, 3);
  /* get_pinch_gesture's constructor: create a real resource through wlroots. */
  struct wl_proxy *gesture = wl_proxy_marshal_flags(manager, 1,
      &zwp_pointer_gesture_pinch_v1_interface, 3, 0, NULL, pointer);
  CHECK(gesture != NULL);
  CHECK(wl_display_flush(client_display) >= 0);
  CHECK(wl_event_loop_dispatch(wl_display_get_event_loop(f.display), 0) >= 0);
  struct wl_resource *resource = wl_client_get_object(server_client, wl_proxy_get_id(gesture));
  CHECK(resource != NULL && wl_resource_get_user_data(resource) == seat);
  CHECK(!wl_list_empty(&f.gestures->pinches));
  f.observed[0].resource = resource;
  f.logger = wl_display_add_protocol_logger(f.display, log_event, &f);
  CHECK(f.logger != NULL);
  f.surfaces[0].resource = wl_resource_create(server_client, &wl_surface_interface, 1, 0);
  CHECK(f.surfaces[0].resource != NULL);
  wl_signal_init(&f.surfaces[0].events.destroy);
  wlr_pointer_init(&f.pointers[0], NULL, "pinch-test");
  hikari_pinch_init(&f.pinch, f.gestures, seat);
  seat->pointer_state.focused_surface = &f.surfaces[0];
  seat->pointer_state.focused_client = wlr_seat_client_for_wl_client(seat, server_client);
  CHECK(seat->pointer_state.focused_client != NULL);
  begin(&f, 0); update(&f, 0); end(&f, 0);
  CHECK(f.observed[0].begin == 1 && f.observed[0].update == 1 && f.observed[0].end == 1);
  hikari_pinch_fini(&f.pinch);
  wlr_pointer_finish(&f.pointers[0]);
  seat->pointer_state.focused_surface = NULL;
  seat->pointer_state.focused_client = NULL;
  wl_proxy_destroy(gesture);
  wl_proxy_destroy(manager);
  wl_pointer_destroy(pointer);
  wl_seat_destroy(client_seat);
  wl_registry_destroy(registry);
  wl_display_disconnect(client_display);
  wl_client_destroy(server_client);
  wl_protocol_logger_destroy(f.logger);
  wlr_seat_destroy(seat);
  wl_display_destroy(f.display);
  puts("  ok real installed wlroots subscription receives a complete pinch sequence");
}

int
main(void)
{
  struct fixture f;
  init(&f);
  begin(&f, 0); update(&f, 0); end(&f, 0);
  CHECK(f.observed[0].begin == 1 && f.observed[0].update == 1 && f.observed[0].end == 1);
  CHECK(f.observed[0].scale == 1.5 && f.observed[0].cancelled == 0);
  CHECK(f.observed[1].begin == 0 && f.observed[1].update == 0 && f.observed[1].end == 0);
  fini(&f);
  puts("  ok normal pinch remains on its initial recipient");

  init(&f);
  begin(&f, 0); focus(&f, 1); update(&f, 0); end(&f, 0);
  CHECK(f.observed[0].begin == 1 && f.observed[0].end == 1 && f.observed[0].cancelled == 1);
  CHECK(f.observed[1].begin == 0 && f.observed[1].update == 0 && f.observed[1].end == 0);
  focus(&f, 0); update(&f, 0); end(&f, 0);
  CHECK(f.observed[0].update == 0 && f.observed[0].end == 1);
  focus(&f, 1); begin(&f, 0); update(&f, 0); end(&f, 0);
  CHECK(f.observed[1].begin == 1 && f.observed[1].update == 1 && f.observed[1].end == 1);
  fini(&f);
  puts("  ok focus switch cancels A; B gets no orphan update/end; fresh gesture works");

  init(&f);
  begin(&f, 0); hikari_pinch_cancel(&f.pinch); focus(&f, -1);
  hikari_pinch_fini(&f.pinch); update(&f, 0); end(&f, 0);
  CHECK(f.observed[0].end == 1 && f.observed[0].cancelled == 1);
  hikari_pinch_init(&f.pinch, f.gestures, &f.seat);
  focus(&f, 0); begin(&f, 0); end(&f, 0);
  CHECK(f.observed[0].begin == 2 && f.observed[0].end == 2);
  fini(&f);
  puts("  ok lock cancellation and deactivate/reactivate deliver exactly one end per begin");

  init(&f);
  begin(&f, 0); hikari_pinch_fini(&f.pinch); update(&f, 0); end(&f, 0);
  CHECK(f.observed[0].end == 1 && f.observed[0].cancelled == 1);
  fini(&f);
  puts("  ok deactivation alone cancels an active pinch");

  init(&f);
  begin(&f, 0); begin(&f, 1); update(&f, 1); end(&f, 1);
  CHECK(f.observed[0].begin == 1 && f.observed[0].update == 0 && f.observed[0].end == 0);
  update(&f, 0); end(&f, 0);
  CHECK(f.observed[0].update == 1 && f.observed[0].end == 1);
  fini(&f);
  puts("  ok another pointer cannot interleave its gesture into this seat's active pinch");

  init(&f);
  begin(&f, 0); subscribe(&f, 2, 0, false); subscribe(&f, 3, 0, true);
  update(&f, 0); end(&f, 0);
  CHECK(f.observed[2].begin == 0 && f.observed[2].update == 0 && f.observed[2].end == 0);
  begin(&f, 0); update(&f, 0); end(&f, 0);
  CHECK(f.observed[2].begin == 1 && f.observed[2].update == 1 && f.observed[2].end == 1);
  CHECK(f.observed[3].begin == 0 && f.observed[3].update == 0 && f.observed[3].end == 0);
  fini(&f);
  puts("  ok late subscribers wait for the next begin; inert gestures receive nothing");

  init(&f);
  begin(&f, 0); wl_resource_destroy(f.observed[0].resource); update(&f, 0); end(&f, 0);
  CHECK(f.observed[0].update == 0 && f.observed[0].end == 0);
  fini(&f);
  puts("  ok destroying a gesture resource during pinch removes its recipient safely");

  init(&f);
  begin(&f, 0); wl_signal_emit_mutable(&f.surfaces[0].events.destroy, NULL);
  update(&f, 0); end(&f, 0);
  CHECK(f.observed[0].end == 1 && f.observed[0].cancelled == 1);
  fini(&f);
  puts("  ok surface destruction cancels and detaches lifetime listeners");

  init(&f);
  begin(&f, 0); wl_signal_emit_mutable(&f.clients[0].events.destroy, NULL);
  wl_client_destroy(f.clients[0].client); f.clients[0].client = NULL;
  update(&f, 0); end(&f, 0);
  CHECK(f.observed[0].end == 1 && f.observed[0].cancelled == 1);
  fini(&f);
  puts("  ok seat client destruction leaves no dangling target");

  init(&f);
  begin(&f, 0); wlr_pointer_finish(&f.pointers[0]); f.pointer_finished[0] = true;
  CHECK(f.observed[0].end == 1 && f.observed[0].cancelled == 1);
  begin(&f, 1); end(&f, 1);
  CHECK(f.observed[0].begin == 2 && f.observed[0].end == 2);
  fini(&f);
  puts("  ok pointer removal cancels; another pointer can start a new gesture");

  init(&f);
  focus(&f, -1); begin(&f, 0); focus(&f, 0); update(&f, 0); end(&f, 0);
  CHECK(f.observed[0].begin == 0 && f.observed[0].update == 0 && f.observed[0].end == 0);
  fini(&f);
  puts("  ok gestures starting without focus cannot inject orphan updates after focus returns");
  test_wlroots_subscription();
  puts("pinch: ALL PASS (12 cases)");
  return 0;
}
