/* Minimal protocol implementation for wp_pointer_warp_v1 */

#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include "wayland-util.h"

extern const struct wl_interface wl_pointer_interface;
extern const struct wl_interface wl_surface_interface;

static const struct wl_interface *pointer_warp_v1_types[] = {
	NULL,
	&wl_surface_interface,
	&wl_pointer_interface,
};

static const struct wl_message wp_pointer_warp_v1_requests[] = {
	{ "destroy", "", pointer_warp_v1_types + 0 },
	{ "warp_pointer", "ooffu", pointer_warp_v1_types + 1 },
};

WL_EXPORT const struct wl_interface wp_pointer_warp_v1_interface = {
	"wp_pointer_warp_v1", 1,
	2, wp_pointer_warp_v1_requests,
	0, NULL,
};
