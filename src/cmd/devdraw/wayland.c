#include <u.h>
#include <libc.h>
#include <draw.h>
#include <memdraw.h>
#include <memlayer.h>
#include <keyboard.h>
#include <mouse.h>
#include <cursor.h>
#include <thread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "bigarrow.h"
#include "devdraw.h"
#include "wayland-clip.h"
#include "wayland-pointer-warp.h"
#include "wayland-xdg-decoration.h"
#include "wayland-xdg-shell.h"

// alt+click and ctl+click are mapped to mouse buttons
// to support single button mice.
#define ALT_BUTTON 1
#define CTL_BUTTON 2

struct WaylandBuffer {
	int w;
	int h;
	int size;
	char *data;
	struct wl_buffer* wl_buffer;
};
typedef struct WaylandBuffer WaylandBuffer;

struct WaylandClient {
	// The screen image written to by the client, and read by this driver.
	Memimage *memimage;

	// The current mouse coordinates and a bitmask of held buttons.
	int mouse_x;
	int mouse_y;
	int buttons;
	int surface_mouse_x;
	int surface_mouse_y;

	// Booleans indicating whether control or alt are currently held.
	int ctl;
	int alt;

	// State for key repeat for keyboard keys.
	int repeat_rune;
	int repeat_start_ms;

	// Key repeat configuration. Can be changed by
	// wl_surface_repeat_info events.
	int repeat_interval_ms;
	int repeat_delay_ms;

	uint32_t pointer_enter_serial;

	// The Wayland surface for this window
	// and its corresponding xdg objects.
	struct wl_surface *wl_surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *xdg_toplevel;

	// Initial configure call is complete
	int configured;

	// Current decoration mode; client-side means we draw simple borders.
	int decoration_mode;
	struct zxdg_toplevel_decoration_v1 *xdg_decoration;

	// Surface size in buffer pixels (may include CSD border).
	int surface_w;
	int surface_h;
	int buffer_scale;
	int csd_thickness;
	int content_offset_x;
	int content_offset_y;

	// These are called each frame while the key is pressed
	// or scrolling is active, to implement key repeat and
	// inertial scrolling.
	struct wl_callback *wl_key_repeat_callback;

	// The mouse pointer and the surface for the current cursor.
	struct wl_pointer *wl_pointer;
	struct wl_surface *wl_surface_cursor;

	// The keyboard and xkb state used
	// for mapping scan codes to key codes.
	struct wl_keyboard *wl_keyboard;
	struct xkb_context *xkb_context;
	struct xkb_keymap *xkb_keymap;
	struct xkb_state *xkb_state;
};
typedef struct WaylandClient WaylandClient;

static QLock wayland_lock;
// Wayland calls from rpc_* use wayland_lock; only gfx_main dispatches.

// Required globals wayland objects.
static struct wl_display *wl_display;
static struct wl_output *wl_output;
static struct wl_registry *wl_registry;
static struct wl_shm *wl_shm;
static struct wl_compositor *wl_compositor;
static struct xdg_wm_base *xdg_wm_base;
static struct wl_seat *wl_seat;
static struct wl_data_device_manager *wl_data_device_manager;
static struct wl_data_device *wl_data_device;

// Optional global wayland objects.
// Need to NULL check them before using.
static struct zxdg_decoration_manager_v1 *decoration_manager;
static struct wp_pointer_warp_v1 *pointer_warp;

// The wl output scale factor reported by wl_output.
// We only set it if we get th event before entering the graphics loop.
// Once we enter the loop, we never change it to avoid the need
// to reason about which scale a buffer was created with.
int wl_output_scale_factor = 1;
int entered_gfx_loop = 0;
// Buffer scale is frozen once gfx_main enters the render loop.

// The delay in ms which a key must be held to begin repeating..
int key_repeat_delay_ms = 500;
// The number of ms between repeats of a repeating key.
int key_repeat_ms = 100;

// A pool of xrgb888 buffers used for drawing to the screen.
// When drawing, we give ownership of the buffer's memory to the compositor.
// The compositor notifies us asynchronously when it is done reading the buffer.
// In the case that we need to draw (rpc_flush) before the buffer is ready,
// we will need to allocate a whole new buffer to draw to.
// We use this pool to avoid the need to setup a new shared memory buffer
// each time this happens.
#define N_XRGB8888_BUFFERS 3
struct WaylandBuffer *xrgb8888_buffers[N_XRGB8888_BUFFERS];

int wayland_debug = 0;

#define DEBUG(...)					\
do {								\
	if (wayland_debug) {			\
		fprint(2, __VA_ARGS__);	\
	}							\
} while(0)

static void fatal_wayland(const char *msg) {
	// Callers must not hold wayland_lock when calling fatal_wayland().
	wlclip_shutdown();
	if (wl_display != NULL) {
		wl_display_disconnect(wl_display);
	}
	sysfatal("%s", msg);
}

static void registry_global(void *data, struct wl_registry *wl_registry,
	uint32_t name, const char *interface, uint32_t version) {
	if (strcmp(interface, wl_output_interface.name) == 0) {
		wl_output = wl_registry_bind(wl_registry, name, &wl_output_interface, 2);

	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		wl_shm = wl_registry_bind(wl_registry, name, &wl_shm_interface, 1);

	} else if (strcmp(interface, wl_compositor_interface.name) == 0) {
		wl_compositor = wl_registry_bind(wl_registry, name,
			&wl_compositor_interface, 4);

	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		xdg_wm_base = wl_registry_bind(wl_registry, name,
			&xdg_wm_base_interface, 1);

	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		wl_seat = wl_registry_bind(wl_registry, name, &wl_seat_interface, 4);

	} else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) {
		wl_data_device_manager = wl_registry_bind(wl_registry, name, &wl_data_device_manager_interface, 2);

	} else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
		decoration_manager = wl_registry_bind(wl_registry, name,
			&zxdg_decoration_manager_v1_interface, 1);

	} else if (strcmp(interface, wp_pointer_warp_v1_interface.name) == 0) {
		pointer_warp = wl_registry_bind(wl_registry, name,
			&wp_pointer_warp_v1_interface, 1);
	}
}

static void registry_global_remove(void *data, struct wl_registry *wl_registry,
	uint32_t name) {}

static const struct wl_registry_listener wl_registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

void wl_output_geometry(void *data, struct wl_output *wl_output,
	int32_t x, int32_t y, int32_t physical_width, int32_t physical_height,
	int32_t subpixel, const char *make, const char *model, int32_t transform) {}

void wl_output_mode(void *data, struct wl_output *wl_output, uint32_t flags,
	int32_t width, int32_t height, int32_t refresh) {}

void wl_output_done(void *data, struct wl_output *wl_output) {}

void wl_output_scale(void *data, struct wl_output *wl_output, int32_t factor) {
	DEBUG("wl_output_scale(factor=%d)\n", factor);

	qlock(&wayland_lock);

	if (!entered_gfx_loop) {
		wl_output_scale_factor = factor;
	}

	qunlock(&wayland_lock);
}

static const struct wl_output_listener wl_output_listener = {
	.geometry = wl_output_geometry,
	.mode = wl_output_mode,
	.done = wl_output_done,
	.scale = wl_output_scale,
};

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial) {
	xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
	.ping = xdg_wm_base_ping,
};

void delete_buffer(WaylandBuffer *b) {
	munmap(b->data, b->size);
	wl_buffer_destroy(b->wl_buffer);
	free(b);
}

static void wl_buffer_release(void *data, struct wl_buffer *wl_buffer) {
	if (data == NULL) {
		wl_buffer_destroy(wl_buffer);
		return;
	}
	for (int i = 0; i < N_XRGB8888_BUFFERS; i++) {
		if (xrgb8888_buffers[i] == NULL) {
			xrgb8888_buffers[i] = (WaylandBuffer*) data;
			return;
		}
	}
	delete_buffer((WaylandBuffer*) data);
}

static const struct wl_buffer_listener wl_buffer_listener = {
	.release = wl_buffer_release,
};

#define CSD_BORDER_THICKNESS_SURF 4
#define CSD_MIN_CONTENT_W_SURF 64
#define CSD_MIN_CONTENT_H_SURF 48

static int csd_border_thickness(WaylandClient *wl) {
	int scale = wl->buffer_scale;
	if (scale <= 0) {
		DEBUG("csd_border_thickness: invalid scale %d\n", scale);
		return 0;
	}
	return CSD_BORDER_THICKNESS_SURF * scale;
}

static void set_buffer_scale(WaylandClient *wl, int scale) {
	wl->buffer_scale = scale;
	wl_surface_set_buffer_scale(wl->wl_surface, scale);
}

static void update_csd_metrics(WaylandClient *wl) {
	if (wl->decoration_mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE) {
		wl->csd_thickness = csd_border_thickness(wl);
	} else {
		wl->csd_thickness = 0;
	}
	wl->content_offset_x = wl->csd_thickness;
	wl->content_offset_y = wl->csd_thickness;
}

static int surface_to_client_xy(WaylandClient *wl, wl_fixed_t sx, wl_fixed_t sy,
	int *outx, int *outy, int *out_sx, int *out_sy) {
	// Expects buffer scale and CSD offsets to be stable during the render loop.
	int scale = wl->buffer_scale;
	if (scale <= 0) {
		return 0;
	}
	int x = (int)(wl_fixed_to_double(sx) * scale + 0.5);
	int y = (int)(wl_fixed_to_double(sy) * scale + 0.5);
	*out_sx = x;
	*out_sy = y;

	x -= wl->content_offset_x;
	y -= wl->content_offset_y;
	int w = Dx(wl->memimage->r);
	int h = Dy(wl->memimage->r);
	if (x < 0) {
		x = 0;
	} else if (w > 0 && x >= w) {
		x = w - 1;
	}
	if (y < 0) {
		y = 0;
	} else if (h > 0 && y >= h) {
		y = h - 1;
	}
	*outx = x;
	*outy = y;
	return 1;
}

static void set_csd_min_size(WaylandClient *wl) {
	int min_w = CSD_MIN_CONTENT_W_SURF;
	int min_h = CSD_MIN_CONTENT_H_SURF;
	if (wl->decoration_mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE) {
		min_w += 2 * CSD_BORDER_THICKNESS_SURF;
		min_h += 2 * CSD_BORDER_THICKNESS_SURF;
	}
	xdg_toplevel_set_min_size(wl->xdg_toplevel, min_w, min_h);
}

static void xdg_toplevel_decoration_configure(void *data,
	struct zxdg_toplevel_decoration_v1 *decoration, uint32_t mode) {
	Client* c = data;
	WaylandClient *wl = (WaylandClient*) c->view;
	qlock(&wayland_lock);
	wl->decoration_mode = mode;
	update_csd_metrics(wl);
	if (mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE) {
		set_csd_min_size(wl);
	} else if (mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE) {
		xdg_toplevel_set_min_size(wl->xdg_toplevel, 0, 0);
	}
	qunlock(&wayland_lock);
}

static const struct zxdg_toplevel_decoration_v1_listener xdg_toplevel_decoration_listener = {
	.configure = xdg_toplevel_decoration_configure,
};

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial) {
	DEBUG("xdg_surface_configure\n");
	const Client* c = data;
	WaylandClient *wl = (WaylandClient*) c->view;
	qlock(&wayland_lock);

	xdg_surface_ack_configure(wl->xdg_surface, serial);
	wl->configured = 1;

	qunlock(&wayland_lock);
	DEBUG("xdg_surface_configure: returned\n");
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

void xdg_toplevel_configure(void *data, struct xdg_toplevel *xdg_toplevel,
	int32_t width, int32_t height, struct wl_array *states) {
	DEBUG("xdg_toplevel_configure(width=%d, height=%d)\n", width, height);
	if (width == 0 || height == 0) {
		return;
	}
	Client* c = data;
	WaylandClient *wl = (WaylandClient*) c->view;
	qlock(&wayland_lock);

	int scale = wl->buffer_scale;
	if (scale <= 0) {
		qunlock(&wayland_lock);
		return;
	}
	width *= scale;
	height *= scale;
	int content_w = width;
	int content_h = height;
	int t = wl->csd_thickness;
	content_w = width - 2 * t;
	content_h = height - 2 * t;
	if (content_w < 1) {
		content_w = 1;
	}
	if (content_h < 1) {
		content_h = 1;
	}
	wl->surface_w = width;
	wl->surface_h = height;
	Rectangle r = Rect(0, 0, content_w, content_h);
	if (eqrect(r, wl->memimage->r)) {
		// The size didn't change, so nothing to do.
		qunlock(&wayland_lock);
		return;
	}

	// The size changed, so allocate a new Memimage and notify the client.
	wl->memimage = _allocmemimage(r, XRGB32);
	c->mouserect = r;

	qunlock(&wayland_lock);
	gfx_replacescreenimage(c, wl->memimage);
}

void xdg_toplevel_close(void *data, struct xdg_toplevel *xdg_toplevel) {
	DEBUG("xdg_toplevel_close\n");
	threadexitsall(nil);
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	.configure = xdg_toplevel_configure,
	.close = xdg_toplevel_close,
};

static const struct wl_callback_listener wl_callback_key_repeat_listener;

static void wl_callback_key_repeat(void *data, struct wl_callback *wl_callback, uint32_t time) {
	Client* c = data;
	WaylandClient *wl = (WaylandClient*) c->view;
	wl_callback_destroy(wl_callback);

	qlock(&wayland_lock);

	int repetitions = 0;
	int repeat_rune = wl->repeat_rune;

	if (wl->repeat_interval_ms == 0 || wl->repeat_rune == 0) {
		goto done;
	}

	int dt = time - wl->repeat_start_ms;

	// There is an initial delay for repetition to start, so
	// repeat_start_ms can be in the future.
	if (wl->repeat_start_ms < time && wl->repeat_interval_ms <= dt) {
		repetitions = dt / wl->repeat_interval_ms;

		// Incrementing this way, rather than setting start to now,
		// avoids losing fractional time to integer division.
		wl->repeat_start_ms += repetitions * wl->repeat_interval_ms;
	}

	wl_callback = wl_surface_frame(wl->wl_surface);
	wl_callback_add_listener(wl_callback, &wl_callback_key_repeat_listener, c);
	wl_surface_commit(wl->wl_surface);

done:
	qunlock(&wayland_lock);
	for(int i = 0; i < repetitions; i++) {
		gfx_keystroke(c, repeat_rune);
	}
}

static const struct wl_callback_listener wl_callback_key_repeat_listener = {
	.done = wl_callback_key_repeat,
};

void wl_pointer_enter(void *data,struct wl_pointer *wl_pointer, uint32_t serial,
	struct wl_surface *surface, wl_fixed_t surface_x, wl_fixed_t surface_y) {
	Client* c = data;
	WaylandClient *wl = (WaylandClient*) c->view;
	qlock(&wayland_lock);

	int x;
	int y;
	int sx_buf;
	int sy_buf;
	wl->pointer_enter_serial = serial;
	if (!surface_to_client_xy(wl, surface_x, surface_y, &x, &y, &sx_buf, &sy_buf)) {
		qunlock(&wayland_lock);
		return;
	}
	wl->surface_mouse_x = sx_buf;
	wl->surface_mouse_y = sy_buf;
	wl->mouse_x = x;
	wl->mouse_y = y;

	wl_pointer_set_cursor(wl->wl_pointer, serial, wl->wl_surface_cursor, 0, 0);

	qunlock(&wayland_lock);
	// We don't call gfx_mousetrack here, since we don't have the time.
}

void wl_pointer_leave(void *data, struct wl_pointer *wl_pointer,
	uint32_t serial, struct wl_surface *surface) {
	Client* c = data;
	WaylandClient *wl = (WaylandClient*) c->view;
	qlock(&wayland_lock);

	wl->buttons = 0;
	wl->pointer_enter_serial = 0;

	qunlock(&wayland_lock);
}

void wl_pointer_motion(void *data, struct wl_pointer *wl_pointer, uint32_t time,
	wl_fixed_t surface_x, wl_fixed_t surface_y){
	Client* c = data;
	WaylandClient *wl = (WaylandClient*) c->view;
	qlock(&wayland_lock);

	int x;
	int y;
	int sx_buf;
	int sy_buf;
	if (!surface_to_client_xy(wl, surface_x, surface_y, &x, &y, &sx_buf, &sy_buf)) {
		qunlock(&wayland_lock);
		return;
	}
	wl->surface_mouse_x = sx_buf;
	wl->surface_mouse_y = sy_buf;
	wl->mouse_x = x;
	wl->mouse_y = y;
	int mx = wl->mouse_x;
	int my = wl->mouse_y;
	int b = wl->buttons;

	qunlock(&wayland_lock);
	gfx_mousetrack(c, mx, my, b, (uint) time);
}

void wl_pointer_button(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
	uint32_t time, uint32_t button, uint32_t state) {
	DEBUG("wl_pointer_button(button=%d)\n", (int) button);
	wlclip_set_serial(serial);
	Client* c = data;
	WaylandClient *wl = (WaylandClient*) c->view;
	qlock(&wayland_lock);

	int mask = 0;
	switch (button) {
	case BTN_LEFT:
		mask = 1<<0;
		break;
	case BTN_MIDDLE:
	case BTN_SIDE:
	case BTN_EXTRA:
		mask = 1<<1;
		break;
	case BTN_RIGHT:
		mask = 1<<2;
		break;
	case BTN_4:
		mask = 1<<3;
		break;
	case BTN_5:
		mask = 1<<4;
		break;
	default:
		DEBUG("wl_pointer_button: unknown button: %d\n", button);
		qunlock(&wayland_lock);
		return;
	}
	int abort_compose = 0;
	if (button == BTN_LEFT) {
		if (wl->ctl) {
			mask = 1 << CTL_BUTTON;
		} else if (wl->alt) {
			abort_compose = 1;
			mask = 1 << ALT_BUTTON;
		}
	}
	DEBUG("wl_pointer_button: mask=%x\n", mask);

	int start_resize = 0;
	int start_move = 0;
	uint32_t edges = 0;
	if (state == WL_POINTER_BUTTON_STATE_PRESSED && button == BTN_LEFT &&
		wl->csd_thickness > 0 && !wl->ctl && !wl->alt) {
		int t = wl->csd_thickness;
		int w = wl->surface_w > 0 ? wl->surface_w : Dx(wl->memimage->r) + 2 * t;
		int h = wl->surface_h > 0 ? wl->surface_h : Dy(wl->memimage->r) + 2 * t;
		int sx = wl->surface_mouse_x;
		int sy = wl->surface_mouse_y;
		if (sx < t) {
			edges |= XDG_TOPLEVEL_RESIZE_EDGE_LEFT;
		} else if (sx >= w - t) {
			edges |= XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
		}
		if (sy < t) {
			edges |= XDG_TOPLEVEL_RESIZE_EDGE_TOP;
		} else if (sy >= h - t) {
			edges |= XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
		}
		if (edges == XDG_TOPLEVEL_RESIZE_EDGE_TOP &&
			sy < t && sx >= t && sx < w - t) {
			start_move = 1;
			edges = 0;
		} else if (edges != 0) {
			start_resize = 1;
		}
	}
	if (start_resize || start_move) {
		qunlock(&wayland_lock);
		if (start_resize) {
			xdg_toplevel_resize(wl->xdg_toplevel, wl_seat, serial, edges);
		} else {
			xdg_toplevel_move(wl->xdg_toplevel, wl_seat, serial);
		}
		return;
	}

	switch (state) {
	case WL_POINTER_BUTTON_STATE_PRESSED:
		wl->buttons |= mask;
		break;
	case WL_POINTER_BUTTON_STATE_RELEASED:
		wl->buttons &= ~mask;
		break;
	default:
		fprint(2, "Unknown button state: %d\n", state);
	}
	int x = wl->mouse_x;
	int y = wl->mouse_y;
	int b = wl->buttons;

	qunlock(&wayland_lock);
	if (abort_compose) {
		DEBUG("wl_pointer_button: gfx_abortcompose()\n");
		gfx_abortcompose(c);
	}
	DEBUG("wl_pointer_button: gfx_trackmouse(x=%d, y=%d, b=%d)\n", x, y, b);
	gfx_mousetrack(c, x, y, b, (uint) time);
}

void wl_pointer_axis(void *data, struct wl_pointer *wl_pointer, uint32_t time,
	uint32_t axis, wl_fixed_t value_fixed) {
	double value = wl_fixed_to_double(value_fixed);
	Client* c = data;
	WaylandClient *wl = (WaylandClient*) c->view;
	qlock(&wayland_lock);

	int x = wl->mouse_x;
	int y = wl->mouse_y;

	int b = 0;
	if (value < 0) {
		b |= 1 << 3;
	} else if (value > 0) {
		b |= 1 << 4;
	}
	b |= wl->buttons;

	qunlock(&wayland_lock);
	gfx_mousetrack(c, x, y, b, (uint) time);
}


static const struct wl_pointer_listener pointer_listener = {
	.enter = wl_pointer_enter,
	.leave = wl_pointer_leave,
	.motion = wl_pointer_motion,
	.button = wl_pointer_button,
	.axis = wl_pointer_axis,
};

void wl_keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard,
	uint32_t format, int32_t fd, uint32_t size) {
	DEBUG("wl_keyboard_keymap(format=%d, fd=%d, size=%d)\n", format, fd, size);
	char *keymap = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (keymap == MAP_FAILED) {
		DEBUG("wl_keyboard_keymap: %s", strerror(errno));
		return;
	}

	Client* c = data;
	WaylandClient *wl = (WaylandClient*) c->view;
	qlock(&wayland_lock);

	if (wl->xkb_keymap != NULL) {
		xkb_keymap_unref(wl->xkb_keymap);
	}
	wl->xkb_keymap = xkb_keymap_new_from_string(wl->xkb_context, keymap,
		XKB_KEYMAP_FORMAT_TEXT_V1,
		XKB_KEYMAP_COMPILE_NO_FLAGS);

	if (wl->xkb_state != NULL) {
		xkb_state_unref(wl->xkb_state);
	}
	wl->xkb_state = xkb_state_new(wl->xkb_keymap);

	qunlock(&wayland_lock);
    	munmap(keymap, size);
	close(fd);
}

void wl_keyboard_enter(void *data, struct wl_keyboard *wl_keyboard,
	uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {
	DEBUG("wl_keyboard_enter\n");
	wlclip_set_serial(serial);
}

void wl_keyboard_leave(void *data, struct wl_keyboard *wl_keyboard,
	uint32_t serial, struct wl_surface *surface) {
	DEBUG("wl_keyboard_leave\n");
	Client* c = data;
	WaylandClient *wl = (WaylandClient*) c->view;
	qlock(&wayland_lock);

	wl->ctl = 0;
	wl->alt = 0;
	wl->repeat_rune = 0;

	qunlock(&wayland_lock);
	gfx_abortcompose(c);
}

void wl_keyboard_key(void *data, struct wl_keyboard *wl_keyboard,
	uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
	Client* c = data;
	WaylandClient *wl = (WaylandClient*) c->view;
	wlclip_set_serial(serial);
	qlock(&wayland_lock);

	wl->repeat_rune = 0;

	key += 8;	// Add 8 to translate Linux scan code to xkb code.
	uint32_t rune = xkb_state_key_get_utf32(wl->xkb_state, key);
	xkb_keysym_t keysym = xkb_state_key_get_one_sym(wl->xkb_state, key);

	if (wayland_debug) {
		char name[256];
		xkb_keysym_get_name(keysym, &name[0], 256);
		char *state_str = state == WL_KEYBOARD_KEY_STATE_PRESSED ? "down" : "up";
		DEBUG("wl_keyboard_key: keysym=%s, rune=0x%x, state=%s\n",
			name, rune, state_str);
	}

	switch (keysym) {
	case XKB_KEY_Return:
		rune = '\n';
		break;
	case XKB_KEY_Alt_L:
	case XKB_KEY_Alt_R:
		rune = Kalt;
		if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
			wl->alt = 1;
		} else {
			wl->alt = 0;
		}
		if (wl->buttons) {
			if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
				wl->buttons |= 1 << ALT_BUTTON;
			} else {
				wl->buttons &= ~(1 << ALT_BUTTON);
			}
			int x = wl->mouse_x;
			int y = wl->mouse_y;
			int b = wl->buttons;

			qunlock(&wayland_lock);
			gfx_mousetrack(c, x, y, b, (uint) time);
			return;
		}
		break;
	case XKB_KEY_Control_L:
	case XKB_KEY_Control_R:
		// For some reason, Kctl is not used;
		// it results in drawing a replacement character.
		// Common ctl combos still work.
		// For example ctl+w sends rune 0x17
		// which erases the previous word.
		rune = 0; // Kctl;
		if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
			wl->ctl = 1;
		} else {
			wl->ctl = 0;
		}
		if (wl->buttons) {
			if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
				wl->buttons |= 1 << CTL_BUTTON;
			} else {
				wl->buttons &= ~(1 << CTL_BUTTON);
			}
			int x = wl->mouse_x;
			int y = wl->mouse_y;
			int b = wl->buttons;

			qunlock(&wayland_lock);
			gfx_mousetrack(c, x, y, b, (uint) time);
			return;
		}
		break;
	case XKB_KEY_Delete:
		rune = Kdel;
		break;
	case XKB_KEY_Escape:
		rune = Kesc;
		break;
	case XKB_KEY_Home:
		rune = Khome;
		break;
	case XKB_KEY_End:
		rune = Kend;
		break;
	case XKB_KEY_Prior:
		rune = Kpgup;
		break;
	case XKB_KEY_Next:
		rune = Kpgdown;
		break;
	case XKB_KEY_Up:
		rune = Kup;
		break;
	case XKB_KEY_Down:
		rune = Kdown;
		break;
	case XKB_KEY_Left:
		rune = Kleft;
		break;
	case XKB_KEY_Right:
		rune = Kright;
		break;
	}

	if (wl->repeat_interval_ms && state == WL_KEYBOARD_KEY_STATE_PRESSED && rune != 0) {
		wl->repeat_rune = rune;
		wl->repeat_start_ms = time + wl->repeat_delay_ms;
		wl->wl_key_repeat_callback = wl_surface_frame(wl->wl_surface);
		wl_callback_add_listener(wl->wl_key_repeat_callback,
				&wl_callback_key_repeat_listener, c);
	}
	qunlock(&wayland_lock);
	if (state == WL_KEYBOARD_KEY_STATE_PRESSED && rune != 0) {
		gfx_keystroke(c, rune);
	}
}

void wl_keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard,
	uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched,
	uint32_t mods_locked, uint32_t group) {
	DEBUG("wl_keyboard_modifiers\n");
	Client* c = data;
	WaylandClient *wl = (WaylandClient*) c->view;
	qlock(&wayland_lock);

	xkb_state_update_mask(wl->xkb_state, mods_depressed,
		mods_latched, mods_locked, 0, 0, group);

	qunlock(&wayland_lock);
}

void wl_keyboard_repeat_info(void *data, struct wl_keyboard *wl_keyboard,
	int32_t rate, int32_t delay) {
	DEBUG("wl_keyboard_repeat_info(rate=%d, delay=%d)\n",
		(int) rate, (int) delay);
	Client* c = data;
	WaylandClient *wl = (WaylandClient*) c->view;
	int interval = 0;

	// rate is in keystrokes per second. Capping to 1k simplifies
	// the code.
	rate = rate > 1000 ? 1000 : rate;
	if (rate > 0)
		interval = 1000 / rate;

	qlock(&wayland_lock);
	wl->repeat_interval_ms = interval;
	wl->repeat_delay_ms = delay;
	qunlock(&wayland_lock);
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = wl_keyboard_keymap,
	.enter = wl_keyboard_enter,
	.leave = wl_keyboard_leave,
	.key = wl_keyboard_key,
	.modifiers = wl_keyboard_modifiers,
	.repeat_info = wl_keyboard_repeat_info,
};

void	gfx_main(void) {
	DEBUG("gfx_main called\n");

	// Only gfx_main dispatches the Wayland queue; callbacks run on this thread.
	wl_display = wl_display_connect(NULL);
	wl_registry = wl_display_get_registry(wl_display);
	wl_registry_add_listener(wl_registry, &wl_registry_listener, NULL);
	wl_display_roundtrip(wl_display);

	// Ensure required globals were correctly bound.
	if (wl_display == NULL) {
		fatal_wayland("Unable to get Wayland display");
	}
	if (wl_registry == NULL) {
		fatal_wayland("Unable to get Wayland registry");
	}
	if (wl_output == NULL) {
		fatal_wayland("Unable to bind wl_output");
	}
	if (wl_shm == NULL) {
		fatal_wayland("Unable to bind wl_shm");
	}
	if (wl_compositor == NULL) {
		fatal_wayland("Unable to bind wl_compositor");
	}
	if (xdg_wm_base == NULL) {
		fatal_wayland("Unable to bind xdg_wm_base");
	}
	if (wl_seat == NULL) {
		fatal_wayland("Unable to bind wl_seat");
	}
	if (wl_data_device_manager == NULL) {
		fatal_wayland("Unable to bind wl_data_device_manager");
	}
	wl_output_add_listener(wl_output, &wl_output_listener, NULL);
	xdg_wm_base_add_listener(xdg_wm_base, &xdg_wm_base_listener, NULL);
	wl_data_device = wl_data_device_manager_get_data_device(
		wl_data_device_manager, wl_seat);
	// wlclip_init/pump/poll_fd/drain_wake/set_serial run on gfx_main.
	wlclip_init(wl_display, wl_data_device_manager, wl_data_device);
	wl_display_roundtrip(wl_display);

	entered_gfx_loop = 1;
	gfx_started();
	DEBUG("gfx_main: entering loop\n");
	int display_fd = wl_display_get_fd(wl_display);
	struct pollfd fds[2];
	for (;;) {
		wlclip_pump();
		while (wl_display_prepare_read(wl_display) != 0) {
			wl_display_dispatch_pending(wl_display);
		}
		wl_display_flush(wl_display);

		fds[0].fd = display_fd;
		fds[0].events = POLLIN;
		int nfds = 1;
		int clip_fd = wlclip_poll_fd();
		if (clip_fd >= 0) {
			fds[1].fd = clip_fd;
			fds[1].events = POLLIN;
			nfds = 2;
		}

		int ret = poll(fds, nfds, -1);
		if (ret < 0) {
			wl_display_cancel_read(wl_display);
			if (errno == EINTR) {
				continue;
			}
			fatal_wayland("poll failed");
		}

		int got_display = fds[0].revents & (POLLIN|POLLERR|POLLHUP);
		int got_wake = nfds > 1 && (fds[1].revents & (POLLIN|POLLERR|POLLHUP));
		if (got_wake) {
			wlclip_drain_wake();
		}

		if (fds[0].revents & POLLHUP) {
			wl_display_cancel_read(wl_display);
			fatal_wayland("Wayland compositor disconnected (POLLHUP)");
		}

		int pollerr = fds[0].revents & POLLERR;
		if (got_display) {
			if (wl_display_read_events(wl_display) < 0) {
				if (pollerr) {
					fatal_wayland("wl_display_read_events failed after POLLERR: %r");
				}
				fatal_wayland("wl_display_read_events: %r");
			}
		} else {
			wl_display_cancel_read(wl_display);
			if (pollerr) {
				fatal_wayland("Wayland display error (POLLERR)");
			}
		}
		wl_display_dispatch_pending(wl_display);
	}
}

static void rpc_resizeimg(Client*) {
	DEBUG("rpc_resizeimg\n");
}

static void rpc_resizewindow(Client*, Rectangle) {
	DEBUG("rpc_resizewindow\n");
}

static int next_shm = 0;

WaylandBuffer *new_buffer(int w, int h, int format) {
	int stride = w * 4;
	int size = stride * h;

	// Create an anonymous shared memory file.
	char name[128];
	snprintf(name, 128, "/acme_wl_shm-%d-%d", getpid(), next_shm++);
	int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd < 0) {
		fatal_wayland("shm_open failed");
	}
	shm_unlink(name);

	// Set the file's size.
	int ret;
	do {
		ret = ftruncate(fd, size);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0) {
		fatal_wayland("ftruncate failed");
	}

	char *d = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
	if (d == MAP_FAILED) {
		fatal_wayland("mmap failed");
	}

	WaylandBuffer *b = malloc(sizeof(WaylandBuffer));
	b->w = w;
	b->h = h;
	b->size = size;
	b->data = d;
	struct wl_shm_pool *p = wl_shm_create_pool(wl_shm, fd, size);
	b->wl_buffer = wl_shm_pool_create_buffer(p, 0, w, h, stride, format);
	wl_shm_pool_destroy(p);
	close(fd);
	return b;
}

void wayland_set_cursor(WaylandClient *wl, Cursor *cursor) {
	// Convert bitmap to ARGB.
	// Yes, this is super clunky. Sorry about that.
	const uint32_t a = 0x00000000;
	const uint32_t fg = 0xFF000000;
	uint32_t data[8*32];
	int j = 0;
	for (int i = 0; i < 32; i++) {
		char c = cursor->set[i];
		data[j++] = (c >>7) & 1 ? fg : a;
		data[j++] = (c >> 6) & 1 ? fg : a;
		data[j++] = (c >> 5) & 1 ? fg : a;
		data[j++] = (c >> 4) & 1 ? fg : a;
		data[j++] = (c >> 3) & 1 ? fg : a;
		data[j++] = (c >> 2) & 1 ? fg : a;
		data[j++] = (c >> 1) & 1 ? fg : a;
		data[j++] = (c >> 0) & 1 ? fg : a;
	}

	WaylandBuffer *b = new_buffer(16, 16, WL_SHM_FORMAT_ARGB8888);
	memcpy(b->data, (char*) &data[0], b->size);
	struct wl_buffer *buf = b->wl_buffer;

	// We don't want to bother saving this buffer in xrgb8888_buffers.
	// Unmap and use NULL for it's listener data.
	// This will cause it to be destroyed when it is released.
	munmap(b->data, b->size);
	wl_buffer_add_listener(buf, &wl_buffer_listener, NULL);
	free(b);

	wl_surface_attach(wl->wl_surface_cursor, buf, 0, 0);
	wl_surface_damage_buffer(wl->wl_surface_cursor, 0, 0, 16, 16);
	wl_surface_commit(wl->wl_surface_cursor);
}

static void rpc_setcursor(Client *c, Cursor *cursor, Cursor2*) {
	DEBUG("rpc_setcursor\n");
	WaylandClient *wl = (WaylandClient*) c->view;
	qlock(&wayland_lock);

	if (cursor == NULL) {
		cursor = &bigarrow;
	}
	wayland_set_cursor(wl, cursor);

	qunlock(&wayland_lock);
}

static void rpc_setlabel(Client *c, char *label) {
	WaylandClient *wl = (WaylandClient*) c->view;
	qlock(&wayland_lock);

	xdg_toplevel_set_title(wl->xdg_toplevel, label);

	qunlock(&wayland_lock);
}

static void rpc_setmouse(Client *c, Point p) {
	WaylandClient *wl = (WaylandClient*) c->view;
	qlock(&wayland_lock);

	struct wp_pointer_warp_v1 *warp = pointer_warp;
	struct wl_surface *surface = wl->wl_surface;
	struct wl_pointer *pointer = wl->wl_pointer;
	uint32_t serial = wl->pointer_enter_serial;
	double sx_surf = 0;
	double sy_surf = 0;
	int do_warp = 0;

	if (warp != NULL && pointer != NULL && serial != 0) {
		int sw_buf = wl->surface_w;
		int sh_buf = wl->surface_h;
		if (sw_buf <= 0 || sh_buf <= 0) {
			goto done;
		}
		// Internal coords are buffer pixels; warp expects surface-local coords.
		double buf_scale = (double) wl->buffer_scale;
		if (buf_scale <= 0) {
			goto done;
		}
		sx_surf = (double)(p.x + wl->content_offset_x) / buf_scale;
		sy_surf = (double)(p.y + wl->content_offset_y) / buf_scale;
		double max_x_surf = (double) sw_buf / buf_scale;
		double max_y_surf = (double) sh_buf / buf_scale;
		if (max_x_surf <= 0 || max_y_surf <= 0) {
			goto done;
		}
		double eps = 1.0 / 256.0;
		if (sx_surf < 0) {
			sx_surf = 0;
		} else if (sx_surf >= max_x_surf) {
			sx_surf = max_x_surf - eps;
		}
		if (sy_surf < 0) {
			sy_surf = 0;
		} else if (sy_surf >= max_y_surf) {
			sy_surf = max_y_surf - eps;
		}
		do_warp = 1;
	}
	// Warp not available or no valid enter serial.
done:
	qunlock(&wayland_lock);
	if (do_warp) {
		wp_pointer_warp_v1_warp_pointer(warp, surface, pointer,
			wl_fixed_from_double(sx_surf), wl_fixed_from_double(sy_surf), serial);
	}
}

static void rpc_topwin(Client*) {
	DEBUG("rpc_topwin\n");
}

static void rpc_bouncemouse(Client*, Mouse) {
	DEBUG("rpc_bouncemouse\n");
}

// Must be called with the lock held.
WaylandBuffer *get_xrgb8888_buffer(int w, int h) {
	for (int i = 0; i < N_XRGB8888_BUFFERS; i++) {
		if (xrgb8888_buffers[i] == NULL) {
			continue;
		}
		// Delete any cached buffers that are not the right size.
		if (xrgb8888_buffers[i]->w != w || xrgb8888_buffers[i]->h != h) {
			delete_buffer(xrgb8888_buffers[i]);
			xrgb8888_buffers[i] = NULL;
			continue;
		}
		WaylandBuffer *b = xrgb8888_buffers[i];
		xrgb8888_buffers[i] = NULL;
		return b;
	}
	WaylandBuffer *b = new_buffer(w, h, WL_SHM_FORMAT_XRGB8888);
	wl_buffer_add_listener(b->wl_buffer, &wl_buffer_listener, b);
	return b;
}

static void draw_csd_frame(WaylandBuffer *b, Memimage *img, int surface_w, int surface_h, int thickness) {
	uint32_t border = 0x0055AAAA; // rio activeborder color

	if (surface_w < thickness * 2 || surface_h < thickness * 2) {
		return;
	}
	int img_w = Dx(img->r);
	int content_w = img_w;
	int content_h = Dy(img->r);
	int max_w = surface_w - 2 * thickness;
	int max_h = surface_h - 2 * thickness;
	if (content_w > max_w) {
		content_w = max_w;
	}
	if (content_h > max_h) {
		content_h = max_h;
	}
	if (content_w <= 0 || content_h <= 0) {
		return;
	}

	uint32_t *pixels = (uint32_t*) b->data;
	int total = surface_w * surface_h;
	for (int i = 0; i < total; i++) {
		pixels[i] = border;
	}

	uint8_t *src = (uint8_t*) img->data->bdata;
	int src_stride = img_w * 4;
	for (int y = 0; y < content_h; y++) {
		uint8_t *dstrow = (uint8_t*) (pixels + (y + thickness) * surface_w + thickness);
		memcpy(dstrow, src + y * src_stride, content_w * 4);
	}
}

static void rpc_flush(Client *c, Rectangle r) {
	WaylandClient *wl = (WaylandClient*) c->view;
	qlock(&wayland_lock);

	if (wl->configured) {
		int content_w = Dx(wl->memimage->r);
		int content_h = Dy(wl->memimage->r);
		int w = content_w;
		int h = content_h;
		int t = wl->csd_thickness;
		if (t > 0) {
			w = wl->surface_w > 0 ? wl->surface_w : content_w + 2 * t;
			h = wl->surface_h > 0 ? wl->surface_h : content_h + 2 * t;
		}
		WaylandBuffer *b = get_xrgb8888_buffer(w, h);
		if (t > 0) {
			draw_csd_frame(b, wl->memimage, w, h, t);
		} else {
			memcpy(b->data, (char*) wl->memimage->data->bdata, b->size);
		}
		wl_surface_attach(wl->wl_surface, b->wl_buffer, 0, 0);
		if (t > 0) {
			wl_surface_damage_buffer(wl->wl_surface, 0, 0, w, h);
		} else {
			wl_surface_damage_buffer(wl->wl_surface, r.min.x, r.min.y, Dx(r), Dy(r));
		}
		wl_surface_commit(wl->wl_surface);
		wl_display_flush(wl_display);
	}

	qunlock(&wayland_lock);
}

static ClientImpl wayland_impl = {
	rpc_resizeimg,
	rpc_resizewindow,
	rpc_setcursor,
	rpc_setlabel,
	rpc_setmouse,
	rpc_topwin,
	rpc_bouncemouse,
	rpc_flush
};

Memimage *rpc_attach(Client *c, char *label, char *winsize) {
	DEBUG("rpc_attach(%s)\n", label);

	qlock(&wayland_lock);

	WaylandClient *wl = calloc(1, sizeof(WaylandClient));
	c->impl = &wayland_impl;
	c->view = wl;

	wl->repeat_interval_ms = key_repeat_ms;
	wl->repeat_delay_ms = key_repeat_delay_ms;
	wl->wl_surface = wl_compositor_create_surface(wl_compositor);

	wl->xdg_surface = xdg_wm_base_get_xdg_surface(xdg_wm_base, wl->wl_surface);
	xdg_surface_add_listener(wl->xdg_surface, &xdg_surface_listener, c);

	wl->xdg_toplevel = xdg_surface_get_toplevel(wl->xdg_surface);
	xdg_toplevel_add_listener(wl->xdg_toplevel, &xdg_toplevel_listener, c);
	xdg_toplevel_set_title(wl->xdg_toplevel, label);

	wl->wl_pointer = wl_seat_get_pointer(wl_seat);
	wl_pointer_add_listener(wl->wl_pointer, &pointer_listener, c);
	wl->wl_surface_cursor = wl_compositor_create_surface(wl_compositor);
	wayland_set_cursor(wl, &bigarrow);

	wl->wl_keyboard = wl_seat_get_keyboard(wl_seat);
	wl_keyboard_add_listener(wl->wl_keyboard, &keyboard_listener, c);
	wl->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

	// If the xdg decorations extension is available,
	// enable server-side decorations.
	// Otherwise there will be no window decorations
	// (title, resize, buttons, etc.).
	if (decoration_manager != NULL) {
		struct zxdg_toplevel_decoration_v1 *d =
			zxdg_decoration_manager_v1_get_toplevel_decoration(
				decoration_manager, wl->xdg_toplevel);
		zxdg_toplevel_decoration_v1_add_listener(
			d, &xdg_toplevel_decoration_listener, c);
		zxdg_toplevel_decoration_v1_set_mode(d,
			ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
		wl->decoration_mode = ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
		wl->xdg_decoration = d;
	} else {
		wl->decoration_mode = ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
	}

	int scale = wl_output_scale_factor;
	set_buffer_scale(wl, scale);
	update_csd_metrics(wl);
	if (wl->decoration_mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE) {
		set_csd_min_size(wl);
	}
	int content_w = 640;
	int content_h = 480;
	if (winsize != NULL && winsize[0] != '\0') {
		Rectangle wr;
		int havemin;
		if (parsewinsize(winsize, &wr, &havemin) != 0) {
			USED(havemin);
			content_w = Dx(wr);
			content_h = Dy(wr);
			if (content_w < 1) {
				content_w = 1;
			}
			if (content_h < 1) {
				content_h = 1;
			}
			// Ignore origin; Wayland window placement is compositor-controlled.
		}
	}
	content_w *= scale;
	content_h *= scale;
	Rectangle r = Rect(0, 0, content_w, content_h);
	wl->memimage = _allocmemimage(r, XRGB32);
	c->mouserect = r;
	c->displaydpi = 110 * scale;
	int t = wl->csd_thickness;
	wl->surface_w = content_w + 2 * t;
	wl->surface_h = content_h + 2 * t;
	wl_surface_commit(wl->wl_surface);
	wl_display_flush(wl_display);

	qunlock(&wayland_lock);
	return wl->memimage;
}

char *rpc_getsnarf(void) {
	DEBUG("rpc_getsnarf\n");
	return wlclip_getsnarf();
}

void rpc_putsnarf(char *snarf_in) {
	DEBUG("rpc_putsnarf\n");
	wlclip_putsnarf(snarf_in);
}

void	rpc_shutdown(void) {
	DEBUG("rpc_shutdown\n");
	wlclip_shutdown();
}

void rpc_gfxdrawlock(void) {
	qlock(&wayland_lock);
}

void rpc_gfxdrawunlock(void) {
	qunlock(&wayland_lock);
}

int cloadmemimage(Memimage *i, Rectangle r, uchar *data, int ndata) {
	return _cloadmemimage(i, r, data, ndata);
}

int loadmemimage(Memimage *i, Rectangle r, uchar *data, int ndata) {
	return _loadmemimage(i, r, data, ndata);
}

int unloadmemimage(Memimage *i, Rectangle r, uchar *data, int ndata) {
	return _unloadmemimage(i, r, data, ndata);
}
