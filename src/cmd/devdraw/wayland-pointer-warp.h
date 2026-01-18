/* Minimal client header for wp_pointer_warp_v1 */

#ifndef WP_POINTER_WARP_V1_CLIENT_PROTOCOL_H
#define WP_POINTER_WARP_V1_CLIENT_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include "wayland-client.h"

#ifdef  __cplusplus
extern "C" {
#endif

struct wl_pointer;
struct wl_surface;
struct wp_pointer_warp_v1;

extern const struct wl_interface wp_pointer_warp_v1_interface;

static inline void
wp_pointer_warp_v1_set_user_data(struct wp_pointer_warp_v1 *wp_pointer_warp_v1, void *user_data)
{
	wl_proxy_set_user_data((struct wl_proxy *) wp_pointer_warp_v1, user_data);
}

static inline void *
wp_pointer_warp_v1_get_user_data(struct wp_pointer_warp_v1 *wp_pointer_warp_v1)
{
	return wl_proxy_get_user_data((struct wl_proxy *) wp_pointer_warp_v1);
}

static inline uint32_t
wp_pointer_warp_v1_get_version(struct wp_pointer_warp_v1 *wp_pointer_warp_v1)
{
	return wl_proxy_get_version((struct wl_proxy *) wp_pointer_warp_v1);
}

static inline void
wp_pointer_warp_v1_destroy(struct wp_pointer_warp_v1 *wp_pointer_warp_v1)
{
	wl_proxy_marshal_flags((struct wl_proxy *) wp_pointer_warp_v1,
		0, NULL, wl_proxy_get_version((struct wl_proxy *) wp_pointer_warp_v1),
		WL_MARSHAL_FLAG_DESTROY);
}

static inline void
wp_pointer_warp_v1_warp_pointer(struct wp_pointer_warp_v1 *wp_pointer_warp_v1,
	struct wl_surface *surface, struct wl_pointer *pointer,
	wl_fixed_t x, wl_fixed_t y, uint32_t serial)
{
	wl_proxy_marshal((struct wl_proxy *) wp_pointer_warp_v1,
		1, surface, pointer, x, y, serial);
}

#ifdef  __cplusplus
}
#endif

#endif
