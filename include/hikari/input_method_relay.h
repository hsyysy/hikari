#if !defined(HIKARI_INPUT_METHOD_RELAY_H)
#define HIKARI_INPUT_METHOD_RELAY_H

#include <wayland-server-core.h>

struct wlr_input_method_v2;
struct wlr_text_input_manager_v3;
struct wlr_input_method_manager_v2;
struct wlr_surface;

struct hikari_text_input {
  struct wlr_text_input_v3 *input;
  struct hikari_input_method_relay *relay;
  struct wl_list link;

  struct wl_listener enable;
  struct wl_listener commit;
  struct wl_listener disable;
  struct wl_listener destroy;
};

struct hikari_input_method_relay {
  struct wl_list text_inputs;
  struct wlr_input_method_v2 *input_method;

  struct wl_listener new_text_input;
  struct wl_listener new_input_method;
  struct wl_listener input_method_commit;
  struct wl_listener input_method_grab_keyboard;
  struct wl_listener input_method_destroy;
  struct wl_listener keyboard_focus_change;
};

void
hikari_input_method_relay_init(struct hikari_input_method_relay *relay,
    struct wlr_text_input_manager_v3 *text_input_manager,
    struct wlr_input_method_manager_v2 *input_method_manager);

#endif
