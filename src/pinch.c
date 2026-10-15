#include <hikari/pinch.h>

#include <stdlib.h>
#include <time.h>

#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_gestures_v1.h>
#include <wlr/types/wlr_seat.h>

#include "pointer-gestures-unstable-v1-protocol.h"

struct recipient {
  struct wl_list link;
  struct wl_resource *resource;
  struct wl_listener destroy;
};

static void
recipient_destroy(struct wl_listener *listener, void *data)
{
  (void)data;
  struct recipient *recipient = wl_container_of(listener, recipient, destroy);
  wl_list_remove(&recipient->destroy.link);
  wl_list_remove(&recipient->link);
  free(recipient);
}

static void
end(struct hikari_pinch *pinch, uint32_t time_msec, bool cancelled)
{
  uint32_t serial = wlr_seat_client_next_serial(pinch->client);
  struct recipient *recipient, *tmp;
  wl_list_for_each_safe (recipient, tmp, &pinch->recipients, link) {
    zwp_pointer_gesture_pinch_v1_send_end(
        recipient->resource, serial, time_msec, cancelled);
    recipient_destroy(&recipient->destroy, NULL);
  }

  wl_list_remove(&pinch->surface_destroy.link);
  wl_list_remove(&pinch->client_destroy.link);
  wl_list_remove(&pinch->pointer_destroy.link);
  pinch->surface = NULL;
  pinch->client = NULL;
  pinch->pointer = NULL;
}

void
hikari_pinch_cancel(struct hikari_pinch *pinch)
{
  if (pinch->surface == NULL) {
    return;
  }

  uint32_t time_msec = pinch->time_msec;
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
    time_msec = (uint32_t)((uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000);
  }
  end(pinch, time_msec, true);
}

static void
focus_change(struct wl_listener *listener, void *data)
{
  struct hikari_pinch *pinch = wl_container_of(listener, pinch, focus_change);
  struct wlr_seat_pointer_focus_change_event *event = data;
  if (pinch->surface != NULL && event->new_surface != pinch->surface) {
    hikari_pinch_cancel(pinch);
  }
}

static void
surface_destroy(struct wl_listener *listener, void *data)
{
  (void)data;
  struct hikari_pinch *pinch = wl_container_of(listener, pinch, surface_destroy);
  hikari_pinch_cancel(pinch);
}

static void
client_destroy(struct wl_listener *listener, void *data)
{
  (void)data;
  struct hikari_pinch *pinch = wl_container_of(listener, pinch, client_destroy);
  hikari_pinch_cancel(pinch);
}

static void
pointer_destroy(struct wl_listener *listener, void *data)
{
  (void)data;
  struct hikari_pinch *pinch = wl_container_of(listener, pinch, pointer_destroy);
  hikari_pinch_cancel(pinch);
}

void
hikari_pinch_init(struct hikari_pinch *pinch,
    struct wlr_pointer_gestures_v1 *gestures,
    struct wlr_seat *seat)
{
  *pinch = (struct hikari_pinch){ .gestures = gestures, .seat = seat };
  wl_list_init(&pinch->recipients);
  pinch->focus_change.notify = focus_change;
  wl_signal_add(&seat->pointer_state.events.focus_change, &pinch->focus_change);
}

void
hikari_pinch_fini(struct hikari_pinch *pinch)
{
  hikari_pinch_cancel(pinch);
  wl_list_remove(&pinch->focus_change.link);
  wl_list_init(&pinch->focus_change.link);
}

void
hikari_pinch_begin(struct hikari_pinch *pinch,
    const struct wlr_pointer_pinch_begin_event *event)
{
  /* A second device must not start an overlapping gesture on this seat. */
  if (pinch->surface != NULL || event->pointer == NULL ||
      pinch->seat->pointer_state.focused_surface == NULL ||
      pinch->seat->pointer_state.focused_client == NULL) {
    return;
  }

  pinch->surface = pinch->seat->pointer_state.focused_surface;
  pinch->client = pinch->seat->pointer_state.focused_client;
  pinch->pointer = event->pointer;
  pinch->time_msec = event->time_msec;
  pinch->surface_destroy.notify = surface_destroy;
  wl_signal_add(&pinch->surface->events.destroy, &pinch->surface_destroy);
  pinch->client_destroy.notify = client_destroy;
  wl_signal_add(&pinch->client->events.destroy, &pinch->client_destroy);
  pinch->pointer_destroy.notify = pointer_destroy;
  wl_signal_add(&pinch->pointer->base.events.destroy, &pinch->pointer_destroy);

  uint32_t serial = wlr_seat_client_next_serial(pinch->client);
  struct wl_resource *resource;
  wl_resource_for_each(resource, &pinch->gestures->pinches) {
    /* wlroots 0.21 stores the owning seat as gesture resource user data;
     * NULL denotes an inert gesture created from a dead pointer resource. */
    if (wl_resource_get_user_data(resource) != pinch->seat ||
        wl_resource_get_client(resource) != pinch->client->client) {
      continue;
    }

    struct recipient *recipient = calloc(1, sizeof(*recipient));
    if (recipient == NULL) {
      wl_client_post_no_memory(pinch->client->client);
      hikari_pinch_cancel(pinch);
      return;
    }
    recipient->resource = resource;
    recipient->destroy.notify = recipient_destroy;
    wl_resource_add_destroy_listener(resource, &recipient->destroy);
    wl_list_insert(&pinch->recipients, &recipient->link);
    zwp_pointer_gesture_pinch_v1_send_begin(resource, serial,
        event->time_msec, pinch->surface->resource, event->fingers);
  }
}

void
hikari_pinch_update(struct hikari_pinch *pinch,
    const struct wlr_pointer_pinch_update_event *event)
{
  if (pinch->surface == NULL || pinch->pointer != event->pointer) {
    return;
  }
  if (pinch->surface != pinch->seat->pointer_state.focused_surface) {
    hikari_pinch_cancel(pinch);
    return;
  }

  pinch->time_msec = event->time_msec;
  struct recipient *recipient;
  wl_list_for_each (recipient, &pinch->recipients, link) {
    zwp_pointer_gesture_pinch_v1_send_update(recipient->resource,
        event->time_msec, wl_fixed_from_double(event->dx),
        wl_fixed_from_double(event->dy), wl_fixed_from_double(event->scale),
        wl_fixed_from_double(event->rotation));
  }
}

void
hikari_pinch_end(struct hikari_pinch *pinch,
    const struct wlr_pointer_pinch_end_event *event)
{
  if (pinch->surface == NULL || pinch->pointer != event->pointer) {
    return;
  }
  end(pinch, event->time_msec,
      event->cancelled || pinch->surface != pinch->seat->pointer_state.focused_surface);
}
