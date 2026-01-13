#ifndef WAYLAND_CLIP_H
#define WAYLAND_CLIP_H

#include <wayland-client.h>

void wlclip_init(struct wl_display *display,
	struct wl_data_device_manager *manager,
	struct wl_data_device *device);
void wlclip_shutdown(void);
// wlclip_init/wlclip_pump/wlclip_poll_fd/wlclip_drain_wake/wlclip_set_serial
// must be called on the Wayland dispatch thread (gfx_main).
void wlclip_set_serial(uint32_t serial);
void wlclip_pump(void);
int wlclip_poll_fd(void);
void wlclip_drain_wake(void);
char *wlclip_getsnarf(void);
void wlclip_putsnarf(char *snarf_in);

#endif
