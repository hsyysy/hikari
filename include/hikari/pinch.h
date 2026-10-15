#if !defined(HIKARI_PINCH_H)
#define HIKARI_PINCH_H

#include <stdint.h>
#include <wayland-server-core.h>

struct wlr_pointer;
struct wlr_pointer_gestures_v1;
struct wlr_pointer_pinch_begin_event;
struct wlr_pointer_pinch_update_event;
struct wlr_pointer_pinch_end_event;
struct wlr_seat;
struct wlr_seat_client;
struct wlr_surface;

/* One gesture per seat. Recipients are snapshotted at begin, not recomputed
 * from pointer focus for each update/end. */
struct hikari_pinch {
  struct wlr_pointer_gestures_v1 *gestures;
  struct wlr_seat *seat;
  struct wlr_seat_client *client;
  struct wlr_surface *surface;
  struct wlr_pointer *pointer;
  uint32_t time_msec;
  struct wl_list recipients;
  struct wl_listener focus_change;
  struct wl_listener surface_destroy;
  struct wl_listener client_destroy;
  struct wl_listener pointer_destroy;
};

void
hikari_pinch_init(struct hikari_pinch *pinch,
    struct wlr_pointer_gestures_v1 *gestures,
    struct wlr_seat *seat);

void
hikari_pinch_fini(struct hikari_pinch *pinch);

void
hikari_pinch_cancel(struct hikari_pinch *pinch);

void
hikari_pinch_begin(struct hikari_pinch *pinch,
    const struct wlr_pointer_pinch_begin_event *event);

void
hikari_pinch_update(struct hikari_pinch *pinch,
    const struct wlr_pointer_pinch_update_event *event);

void
hikari_pinch_end(struct hikari_pinch *pinch,
    const struct wlr_pointer_pinch_end_event *event);

#endif
