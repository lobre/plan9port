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
#include "wayland-xdg-shell.h"
#include "wayland-xdg-decoration.h"
#include "wayland-pointer-warp.h"

#define ALT_BUTTON 1
#define CTL_BUTTON 2

#define CSD_BORDER_THICKNESS 4
#define N_XRGB8888_BUFFERS 3

typedef struct WaylandBuffer WaylandBuffer;
typedef struct WaylandClient WaylandClient;
typedef struct WaylandCmd WaylandCmd;

static ClientImpl wayland_impl;

struct WaylandBuffer {
	int w;
	int h;
	int size;
	char *data;
	struct wl_buffer *wl_buffer;
};

struct WaylandClient {
	Client *client;
	Memimage *memimage;

	int mouse_x;
	int mouse_y;
	int buttons;
	int surface_mouse_x;
	int surface_mouse_y;
	int ctl;
	int alt;

	int repeat_rune;
	int repeat_start_ms;
	int repeat_interval_ms;
	int repeat_delay_ms;
	struct wl_callback *repeat_cb;

	uint32_t pointer_enter_serial;

	struct wl_surface *wl_surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *xdg_toplevel;
	struct zxdg_toplevel_decoration_v1 *xdg_decoration;

	struct wl_pointer *wl_pointer;
	struct wl_surface *wl_surface_cursor;
	struct wl_keyboard *wl_keyboard;
	struct wl_seat *wl_seat;
	struct xkb_context *xkb_context;
	struct xkb_keymap *xkb_keymap;
	struct xkb_state *xkb_state;

	int configured;
	int decoration_mode;
	int buffer_scale;
	int csd_thickness;
	int content_offset_x;
	int content_offset_y;
	int surface_w;
	int surface_h;

	int desired_w;
	int desired_h;
	WaylandCmd *attach_cmd;
};

enum CmdType {
	CMD_ATTACH,
	CMD_FLUSH,
	CMD_SETCURSOR,
	CMD_SETLABEL,
	CMD_SETMOUSE,
	CMD_GETSNARF,
	CMD_PUTSNARF,
	CMD_SHUTDOWN,
};

struct WaylandCmd {
	enum CmdType type;
	Client *client;
	Rectangle rect;
	Cursor *cursor;
	char *label;
	Point point;
	char *snarf_in;
	char *snarf_out;
	char *winsize;
	Memimage *img_out;

	int done;
	QLock lock;
	Rendez r;
	WaylandCmd *next;
};

static QLock queue_lock;
static WaylandCmd *queue_head;
static WaylandCmd *queue_tail;

static struct wl_display *wl_display;
static struct wl_registry *wl_registry;
static struct wl_output *wl_output;
static struct wl_shm *wl_shm;
static struct wl_compositor *wl_compositor;
static struct xdg_wm_base *xdg_wm_base;
static struct wl_seat *wl_seat;
static struct wl_data_device_manager *wl_data_device_manager;
static struct wl_data_device *wl_data_device;

static struct zxdg_decoration_manager_v1 *decoration_manager;
static struct wp_pointer_warp_v1 *pointer_warp;

static int wl_output_scale_factor = 1;
static int entered_gfx_loop;

static int wake_pipe[2] = { -1, -1 };

static WaylandBuffer *xrgb8888_buffers[N_XRGB8888_BUFFERS];

static int key_repeat_delay_ms = 500;
static int key_repeat_ms = 100;

static uint32_t last_input_serial;

static struct wl_data_offer *selection_offer;
static struct wl_data_source *snarf_source;
static int selection_owned;
static char *snarf_text;
static int offer_utf8;
static int offer_text;

static int clip_fd = -1;
static WaylandCmd *clip_cmd;
static char *clip_buf;
static int clip_len;
static int clip_cap;

static void cmd_done(WaylandCmd *cmd) {
	qlock(&cmd->lock);
	cmd->done = 1;
	rwakeup(&cmd->r);
	qunlock(&cmd->lock);
}

static void cmd_wait(WaylandCmd *cmd) {
	qlock(&cmd->lock);
	while (!cmd->done) {
		rsleep(&cmd->r);
	}
	qunlock(&cmd->lock);
}

static void queue_cmd(WaylandCmd *cmd) {
	cmd->r.l = &cmd->lock;
	qlock(&queue_lock);
	if (queue_tail != nil) {
		queue_tail->next = cmd;
	} else {
		queue_head = cmd;
	}
	queue_tail = cmd;
	cmd->next = nil;
	qunlock(&queue_lock);

	if (wake_pipe[1] >= 0) {
		char b = 1;
		if (write(wake_pipe[1], &b, 1) < 0) {
			/* ignore */
		}
	}
}

static WaylandCmd *dequeue_cmd(void) {
	WaylandCmd *cmd;
	qlock(&queue_lock);
	cmd = queue_head;
	if (cmd != nil) {
		queue_head = cmd->next;
		if (queue_head == nil)
			queue_tail = nil;
	}
	qunlock(&queue_lock);
	return cmd;
}

static void fatal_wayland(const char *msg) {
	if (wl_display != nil)
		wl_display_disconnect(wl_display);
	sysfatal("%s", msg);
}

static int buffer_scale(WaylandClient *wl) {
	return wl->buffer_scale > 0 ? wl->buffer_scale : 1;
}

static int csd_border_thickness(WaylandClient *wl) {
	return CSD_BORDER_THICKNESS * buffer_scale(wl);
}

static void update_csd_metrics(WaylandClient *wl) {
	if (wl->decoration_mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE)
		wl->csd_thickness = csd_border_thickness(wl);
	else
		wl->csd_thickness = 0;
	wl->content_offset_x = wl->csd_thickness;
	wl->content_offset_y = wl->csd_thickness;
}

static int surface_to_content_xy(WaylandClient *wl, wl_fixed_t sx, wl_fixed_t sy,
	int *outx, int *outy, int *out_sx, int *out_sy) {
	int scale = buffer_scale(wl);
	int x = (int)(wl_fixed_to_double(sx) * scale + 0.5);
	int y = (int)(wl_fixed_to_double(sy) * scale + 0.5);
	*out_sx = x;
	*out_sy = y;

	x -= wl->content_offset_x;
	y -= wl->content_offset_y;
	if (wl->memimage != nil) {
		int w = Dx(wl->memimage->r);
		int h = Dy(wl->memimage->r);
		if (x < 0)
			x = 0;
		else if (w > 0 && x >= w)
			x = w - 1;
		if (y < 0)
			y = 0;
		else if (h > 0 && y >= h)
			y = h - 1;
	}
	*outx = x;
	*outy = y;
	return 1;
}

static void delete_buffer(WaylandBuffer *b) {
	munmap(b->data, b->size);
	wl_buffer_destroy(b->wl_buffer);
	free(b);
}

static void wl_buffer_release(void *data, struct wl_buffer *wl_buffer) {
	if (data == nil) {
		wl_buffer_destroy(wl_buffer);
		return;
	}
	for (int i = 0; i < N_XRGB8888_BUFFERS; i++) {
		if (xrgb8888_buffers[i] == nil) {
			xrgb8888_buffers[i] = (WaylandBuffer*)data;
			return;
		}
	}
	delete_buffer((WaylandBuffer*)data);
}

static const struct wl_buffer_listener wl_buffer_listener = {
	.release = wl_buffer_release,
};

static WaylandBuffer *new_buffer(int w, int h, int format) {
	int stride = w * 4;
	int size = stride * h;
	char name[128];
	snprintf(name, sizeof(name), "/devdraw_wl_shm-%d-%lld", getpid(), (long long)nsec());
	int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd < 0)
		fatal_wayland("shm_open failed");
	shm_unlink(name);

	int ret;
	do {
		ret = ftruncate(fd, size);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0)
		fatal_wayland("ftruncate failed");

	char *data = mmap(nil, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED)
		fatal_wayland("mmap failed");

	WaylandBuffer *b = malloc(sizeof(WaylandBuffer));
	b->w = w;
	b->h = h;
	b->size = size;
	b->data = data;
	struct wl_shm_pool *p = wl_shm_create_pool(wl_shm, fd, size);
	b->wl_buffer = wl_shm_pool_create_buffer(p, 0, w, h, stride, format);
	wl_shm_pool_destroy(p);
	close(fd);
	return b;
}

static WaylandBuffer *get_xrgb8888_buffer(int w, int h) {
	for (int i = 0; i < N_XRGB8888_BUFFERS; i++) {
		if (xrgb8888_buffers[i] == nil)
			continue;
		if (xrgb8888_buffers[i]->w != w || xrgb8888_buffers[i]->h != h) {
			delete_buffer(xrgb8888_buffers[i]);
			xrgb8888_buffers[i] = nil;
			continue;
		}
		WaylandBuffer *b = xrgb8888_buffers[i];
		xrgb8888_buffers[i] = nil;
		return b;
	}
	WaylandBuffer *b = new_buffer(w, h, WL_SHM_FORMAT_XRGB8888);
	wl_buffer_add_listener(b->wl_buffer, &wl_buffer_listener, b);
	return b;
}

static void draw_csd_frame(WaylandBuffer *b, Memimage *img, int surface_w, int surface_h, int thickness) {
	uint32_t border = 0x0055AAAA;
	if (surface_w < thickness * 2 || surface_h < thickness * 2)
		return;
	int img_w = Dx(img->r);
	int content_w = img_w;
	int content_h = Dy(img->r);
	int max_w = surface_w - 2 * thickness;
	int max_h = surface_h - 2 * thickness;
	if (content_w > max_w)
		content_w = max_w;
	if (content_h > max_h)
		content_h = max_h;
	if (content_w <= 0 || content_h <= 0)
		return;

	uint32_t *pixels = (uint32_t*)b->data;
	int total = surface_w * surface_h;
	for (int i = 0; i < total; i++)
		pixels[i] = border;

	uint8_t *src = (uint8_t*)img->data->bdata;
	int src_stride = img_w * 4;
	for (int y = 0; y < content_h; y++) {
		uint8_t *dst = (uint8_t*)(pixels + (y + thickness) * surface_w + thickness);
		memcpy(dst, src + y * src_stride, content_w * 4);
	}
}

static void wayland_set_cursor(WaylandClient *wl, Cursor *cursor) {
	const uint32_t a = 0x00000000;
	const uint32_t fg = 0xFF000000;
	uint32_t data[8 * 32];
	int j = 0;
	for (int i = 0; i < 32; i++) {
		char c = cursor->set[i];
		data[j++] = (c >> 7) & 1 ? fg : a;
		data[j++] = (c >> 6) & 1 ? fg : a;
		data[j++] = (c >> 5) & 1 ? fg : a;
		data[j++] = (c >> 4) & 1 ? fg : a;
		data[j++] = (c >> 3) & 1 ? fg : a;
		data[j++] = (c >> 2) & 1 ? fg : a;
		data[j++] = (c >> 1) & 1 ? fg : a;
		data[j++] = (c >> 0) & 1 ? fg : a;
	}

	WaylandBuffer *b = new_buffer(16, 16, WL_SHM_FORMAT_ARGB8888);
	memcpy(b->data, (char*)&data[0], b->size);
	struct wl_buffer *buf = b->wl_buffer;
	munmap(b->data, b->size);
	wl_buffer_add_listener(buf, &wl_buffer_listener, nil);
	free(b);

	wl_surface_attach(wl->wl_surface_cursor, buf, 0, 0);
	wl_surface_damage_buffer(wl->wl_surface_cursor, 0, 0, 16, 16);
	wl_surface_commit(wl->wl_surface_cursor);
}

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial) {
	xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
	.ping = xdg_wm_base_ping,
};

static void wl_output_scale(void *data, struct wl_output *wl_output, int32_t factor) {
	if (!entered_gfx_loop)
		wl_output_scale_factor = factor;
}

static void wl_output_geometry(void *data, struct wl_output *wl_output,
	int32_t x, int32_t y, int32_t physical_width, int32_t physical_height,
	int32_t subpixel, const char *make, const char *model, int32_t transform) {
	USED(data);
	USED(wl_output);
	USED(x);
	USED(y);
	USED(physical_width);
	USED(physical_height);
	USED(subpixel);
	USED(make);
	USED(model);
	USED(transform);
}

static void wl_output_mode(void *data, struct wl_output *wl_output, uint32_t flags,
	int32_t width, int32_t height, int32_t refresh) {
	USED(data);
	USED(wl_output);
	USED(flags);
	USED(width);
	USED(height);
	USED(refresh);
}

static void wl_output_done(void *data, struct wl_output *wl_output) {
	USED(data);
	USED(wl_output);
}

static const struct wl_output_listener wl_output_listener = {
	.geometry = wl_output_geometry,
	.mode = wl_output_mode,
	.done = wl_output_done,
	.scale = wl_output_scale,
};

static void xdg_toplevel_decoration_configure(void *data,
	struct zxdg_toplevel_decoration_v1 *decoration, uint32_t mode) {
	WaylandClient *wl = data;
	wl->decoration_mode = mode;
	update_csd_metrics(wl);
}

static const struct zxdg_toplevel_decoration_v1_listener xdg_toplevel_decoration_listener = {
	.configure = xdg_toplevel_decoration_configure,
};

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial) {
	WaylandClient *wl = data;
	xdg_surface_ack_configure(wl->xdg_surface, serial);
	wl->configured = 1;
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void xdg_toplevel_configure(void *data, struct xdg_toplevel *xdg_toplevel,
	int32_t width, int32_t height, struct wl_array *states) {
	WaylandClient *wl = data;
	int scale = buffer_scale(wl);
	int t = wl->csd_thickness;
	int surface_w_logical = width;
	int surface_h_logical = height;
	if (surface_w_logical == 0 || surface_h_logical == 0) {
		int border = (wl->csd_thickness > 0) ? 2 * CSD_BORDER_THICKNESS : 0;
		surface_w_logical = wl->desired_w + border;
		surface_h_logical = wl->desired_h + border;
	}
	int surface_w = surface_w_logical * scale;
	int surface_h = surface_h_logical * scale;
	int content_w = surface_w - 2 * t;
	int content_h = surface_h - 2 * t;
	if (content_w < 1)
		content_w = 1;
	if (content_h < 1)
		content_h = 1;

	wl->surface_w = surface_w;
	wl->surface_h = surface_h;
	Rectangle r = Rect(0, 0, content_w, content_h);
	if (wl->memimage != nil && eqrect(r, wl->memimage->r))
		return;

	wl->memimage = _allocmemimage(r, XRGB32);
	wl->client->mouserect = r;
	gfx_replacescreenimage(wl->client, wl->memimage);

	if (wl->attach_cmd != nil) {
		wl->attach_cmd->img_out = wl->memimage;
		cmd_done(wl->attach_cmd);
		wl->attach_cmd = nil;
	}
}

static void xdg_toplevel_close(void *data, struct xdg_toplevel *xdg_toplevel) {
	threadexitsall(nil);
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	.configure = xdg_toplevel_configure,
	.close = xdg_toplevel_close,
};

static const struct wl_callback_listener wl_key_repeat_listener;

static void wl_key_repeat(void *data, struct wl_callback *cb, uint32_t time) {
	WaylandClient *wl = data;
	wl_callback_destroy(cb);
	wl->repeat_cb = nil;

	if (wl->repeat_interval_ms == 0 || wl->repeat_rune == 0)
		return;

	int dt = time - wl->repeat_start_ms;
	int repetitions = 0;
	if (wl->repeat_start_ms < time && wl->repeat_interval_ms <= dt) {
		repetitions = dt / wl->repeat_interval_ms;
		wl->repeat_start_ms += repetitions * wl->repeat_interval_ms;
	}

	wl->repeat_cb = wl_surface_frame(wl->wl_surface);
	wl_callback_add_listener(wl->repeat_cb, &wl_key_repeat_listener, wl);
	wl_surface_commit(wl->wl_surface);

	for (int i = 0; i < repetitions; i++)
		gfx_keystroke(wl->client, wl->repeat_rune);
}

static const struct wl_callback_listener wl_key_repeat_listener = {
	.done = wl_key_repeat,
};

static void wl_pointer_enter(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
	struct wl_surface *surface, wl_fixed_t surface_x, wl_fixed_t surface_y) {
	WaylandClient *wl = data;
	int x, y, sx_buf, sy_buf;
	wl->pointer_enter_serial = serial;
	if (!surface_to_content_xy(wl, surface_x, surface_y, &x, &y, &sx_buf, &sy_buf))
		return;
	wl->surface_mouse_x = sx_buf;
	wl->surface_mouse_y = sy_buf;
	wl->mouse_x = x;
	wl->mouse_y = y;

	wl_pointer_set_cursor(wl->wl_pointer, serial, wl->wl_surface_cursor, 0, 0);
}

static void wl_pointer_leave(void *data, struct wl_pointer *wl_pointer,
	uint32_t serial, struct wl_surface *surface) {
	WaylandClient *wl = data;
	wl->buttons = 0;
	wl->pointer_enter_serial = 0;
}

static void wl_pointer_motion(void *data, struct wl_pointer *wl_pointer, uint32_t time,
	wl_fixed_t surface_x, wl_fixed_t surface_y) {
	WaylandClient *wl = data;
	int x, y, sx_buf, sy_buf;
	if (!surface_to_content_xy(wl, surface_x, surface_y, &x, &y, &sx_buf, &sy_buf))
		return;
	wl->surface_mouse_x = sx_buf;
	wl->surface_mouse_y = sy_buf;
	wl->mouse_x = x;
	wl->mouse_y = y;
	gfx_mousetrack(wl->client, x, y, wl->buttons, (uint)time);
}

static void wl_pointer_button(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
	uint32_t time, uint32_t button, uint32_t state) {
	WaylandClient *wl = data;
	last_input_serial = serial;
	int mask = 0;
	switch (button) {
	case BTN_LEFT:
		mask = 1 << 0;
		break;
	case BTN_MIDDLE:
	case BTN_SIDE:
	case BTN_EXTRA:
		mask = 1 << 1;
		break;
	case BTN_RIGHT:
		mask = 1 << 2;
		break;
	case BTN_4:
		mask = 1 << 3;
		break;
	case BTN_5:
		mask = 1 << 4;
		break;
	default:
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
		if (sx < t)
			edges |= XDG_TOPLEVEL_RESIZE_EDGE_LEFT;
		else if (sx >= w - t)
			edges |= XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
		if (sy < t)
			edges |= XDG_TOPLEVEL_RESIZE_EDGE_TOP;
		else if (sy >= h - t)
			edges |= XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
		if (edges == XDG_TOPLEVEL_RESIZE_EDGE_TOP &&
			sy < t && sx >= t && sx < w - t) {
			start_move = 1;
			edges = 0;
		} else if (edges != 0) {
			start_resize = 1;
		}
	}
	if (start_resize || start_move) {
		if (start_resize)
			xdg_toplevel_resize(wl->xdg_toplevel, wl->wl_seat, serial, edges);
		else
			xdg_toplevel_move(wl->xdg_toplevel, wl->wl_seat, serial);
		return;
	}

	if (state == WL_POINTER_BUTTON_STATE_PRESSED)
		wl->buttons |= mask;
	else if (state == WL_POINTER_BUTTON_STATE_RELEASED)
		wl->buttons &= ~mask;

	if (abort_compose)
		gfx_abortcompose(wl->client);
	gfx_mousetrack(wl->client, wl->mouse_x, wl->mouse_y, wl->buttons, (uint)time);
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = wl_pointer_enter,
	.leave = wl_pointer_leave,
	.motion = wl_pointer_motion,
	.button = wl_pointer_button,
};

static void wl_keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard,
	uint32_t format, int fd, uint32_t size) {
	WaylandClient *wl = data;
	char *map = mmap(nil, size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (map == MAP_FAILED)
		return;
	struct xkb_keymap *keymap = xkb_keymap_new_from_string(wl->xkb_context, map,
		XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map, size);
	close(fd);
	if (keymap == nil)
		return;
	if (wl->xkb_keymap != nil)
		xkb_keymap_unref(wl->xkb_keymap);
	wl->xkb_keymap = keymap;
	if (wl->xkb_state != nil)
		xkb_state_unref(wl->xkb_state);
	wl->xkb_state = xkb_state_new(keymap);
}

static void wl_keyboard_enter(void *data, struct wl_keyboard *wl_keyboard,
	uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {
	last_input_serial = serial;
}

static void wl_keyboard_leave(void *data, struct wl_keyboard *wl_keyboard,
	uint32_t serial, struct wl_surface *surface) {
	last_input_serial = serial;
}

static void wl_keyboard_key(void *data, struct wl_keyboard *wl_keyboard,
	uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
	WaylandClient *wl = data;
	last_input_serial = serial;
	if (wl->xkb_state == nil)
		return;

	if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
		wl->repeat_rune = 0;
		if (wl->repeat_cb != nil) {
			wl_callback_destroy(wl->repeat_cb);
			wl->repeat_cb = nil;
		}
	}
	key += 8;
	uint32_t rune = xkb_state_key_get_utf32(wl->xkb_state, key);
	xkb_keysym_t keysym = xkb_state_key_get_one_sym(wl->xkb_state, key);

	switch (keysym) {
	case XKB_KEY_Return: rune = '\n'; break;
	case XKB_KEY_Alt_L:
	case XKB_KEY_Alt_R:
		rune = Kalt;
		wl->alt = (state == WL_KEYBOARD_KEY_STATE_PRESSED);
		if (wl->buttons) {
			if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
				wl->buttons |= 1 << ALT_BUTTON;
			else
				wl->buttons &= ~(1 << ALT_BUTTON);
			gfx_mousetrack(wl->client, wl->mouse_x, wl->mouse_y, wl->buttons, (uint)time);
			return;
		}
		break;
	case XKB_KEY_Control_L:
	case XKB_KEY_Control_R:
		rune = 0;
		wl->ctl = (state == WL_KEYBOARD_KEY_STATE_PRESSED);
		if (wl->buttons) {
			if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
				wl->buttons |= 1 << CTL_BUTTON;
			else
				wl->buttons &= ~(1 << CTL_BUTTON);
			gfx_mousetrack(wl->client, wl->mouse_x, wl->mouse_y, wl->buttons, (uint)time);
			return;
		}
		break;
	case XKB_KEY_Delete: rune = Kdel; break;
	case XKB_KEY_Escape: rune = Kesc; break;
	case XKB_KEY_Home: rune = Khome; break;
	case XKB_KEY_End: rune = Kend; break;
	case XKB_KEY_Prior: rune = Kpgup; break;
	case XKB_KEY_Next: rune = Kpgdown; break;
	case XKB_KEY_Up: rune = Kup; break;
	case XKB_KEY_Down: rune = Kdown; break;
	case XKB_KEY_Left: rune = Kleft; break;
	case XKB_KEY_Right: rune = Kright; break;
	}

	if (wl->repeat_interval_ms && state == WL_KEYBOARD_KEY_STATE_PRESSED && rune != 0) {
		wl->repeat_rune = rune;
		wl->repeat_start_ms = time + wl->repeat_delay_ms;
		wl->repeat_cb = wl_surface_frame(wl->wl_surface);
		wl_callback_add_listener(wl->repeat_cb, &wl_key_repeat_listener, wl);
	}
	if (state == WL_KEYBOARD_KEY_STATE_PRESSED && rune != 0)
		gfx_keystroke(wl->client, rune);
}

static void wl_keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard,
	uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched,
	uint32_t mods_locked, uint32_t group) {
	WaylandClient *wl = data;
	last_input_serial = serial;
	if (wl->xkb_state != nil)
		xkb_state_update_mask(wl->xkb_state, mods_depressed,
			mods_latched, mods_locked, 0, 0, group);
}

static void wl_keyboard_repeat_info(void *data, struct wl_keyboard *wl_keyboard,
	int32_t rate, int32_t delay) {
	WaylandClient *wl = data;
	int interval = 0;
	rate = rate > 1000 ? 1000 : rate;
	if (rate > 0)
		interval = 1000 / rate;
	wl->repeat_interval_ms = interval;
	wl->repeat_delay_ms = delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = wl_keyboard_keymap,
	.enter = wl_keyboard_enter,
	.leave = wl_keyboard_leave,
	.key = wl_keyboard_key,
	.modifiers = wl_keyboard_modifiers,
	.repeat_info = wl_keyboard_repeat_info,
};

static void registry_global(void *data, struct wl_registry *registry,
	uint32_t name, const char *interface, uint32_t version) {
	if (strcmp(interface, wl_output_interface.name) == 0) {
		wl_output = wl_registry_bind(registry, name, &wl_output_interface, 2);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		wl_shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_compositor_interface.name) == 0) {
		wl_compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		xdg_wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		wl_seat = wl_registry_bind(registry, name, &wl_seat_interface, 4);
	} else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) {
		wl_data_device_manager = wl_registry_bind(registry, name,
			&wl_data_device_manager_interface, 2);
	} else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
		decoration_manager = wl_registry_bind(registry, name,
			&zxdg_decoration_manager_v1_interface, 1);
	} else if (strcmp(interface, wp_pointer_warp_v1_interface.name) == 0) {
		pointer_warp = wl_registry_bind(registry, name,
			&wp_pointer_warp_v1_interface, 1);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry,
	uint32_t name) {}

static const struct wl_registry_listener wl_registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void data_offer_offer(void *data, struct wl_data_offer *offer, const char *mime_type) {
	USED(data);
	USED(offer);
	if (cistrcmp((char*)mime_type, "text/plain;charset=utf-8") == 0)
		offer_utf8 = 1;
	if (cistrcmp((char*)mime_type, "text/plain") == 0)
		offer_text = 1;
}

static const struct wl_data_offer_listener data_offer_listener = {
	.offer = data_offer_offer,
};

static void data_device_data_offer(void *data, struct wl_data_device *device, struct wl_data_offer *id) {
	USED(device);
	if (selection_offer != nil && selection_offer != id) {
		wl_data_offer_destroy(selection_offer);
		selection_offer = nil;
	}
	selection_offer = id;
	offer_utf8 = 0;
	offer_text = 0;
	wl_data_offer_add_listener(id, &data_offer_listener, nil);
}

static void data_device_selection(void *data, struct wl_data_device *device, struct wl_data_offer *id) {
	USED(device);
	if (selection_offer != nil && selection_offer != id) {
		wl_data_offer_destroy(selection_offer);
		selection_offer = nil;
	}
	if (id == nil) {
		selection_offer = nil;
		selection_owned = 0;
		offer_utf8 = 0;
		offer_text = 0;
		return;
	}
	selection_offer = id;
	selection_owned = 0;
}

static const struct wl_data_device_listener data_device_listener = {
	.data_offer = data_device_data_offer,
	.selection = data_device_selection,
};

static void data_source_send(void *data, struct wl_data_source *source,
	const char *mime_type, int fd) {
	USED(source);
	USED(mime_type);
	if (snarf_text != nil) {
		int n = strlen(snarf_text);
		write(fd, snarf_text, n);
	}
	close(fd);
}

static void data_source_cancelled(void *data, struct wl_data_source *source) {
	if (source == snarf_source)
		snarf_source = nil;
	selection_owned = 0;
	wl_data_source_destroy(source);
}

static const struct wl_data_source_listener data_source_listener = {
	.send = data_source_send,
	.cancelled = data_source_cancelled,
};

static void start_clip_read(WaylandCmd *cmd) {
	if (clip_cmd != nil) {
		if (snarf_text != nil)
			cmd->snarf_out = smprint("%s", snarf_text);
		else
			cmd->snarf_out = smprint("");
		cmd_done(cmd);
		return;
	}
	if (selection_offer == nil) {
		cmd->snarf_out = smprint("");
		cmd_done(cmd);
		return;
	}
	const char *mime = offer_utf8 ? "text/plain;charset=utf-8" : "text/plain";
	if (!offer_utf8 && !offer_text) {
		cmd->snarf_out = smprint("");
		cmd_done(cmd);
		return;
	}
	wl_data_offer_accept(selection_offer, last_input_serial, mime);
	int fds[2];
	if (pipe(fds) < 0) {
		cmd->snarf_out = smprint("");
		cmd_done(cmd);
		return;
	}
	clip_fd = fds[0];
	fcntl(clip_fd, F_SETFL, O_NONBLOCK);
	clip_cmd = cmd;
	clip_len = 0;
	clip_cap = 0;
	free(clip_buf);
	clip_buf = nil;
	wl_data_offer_receive(selection_offer, mime, fds[1]);
	close(fds[1]);
	wl_display_flush(wl_display);
}

static void process_clip_read(void) {
	if (clip_fd < 0 || clip_cmd == nil)
		return;
	char buf[4096];
	for (;;) {
		int n = read(clip_fd, buf, sizeof(buf));
		if (n == 0) {
			close(clip_fd);
			clip_fd = -1;
			if (clip_buf == nil)
				clip_buf = smprint("");
			clip_cmd->snarf_out = clip_buf;
			clip_buf = nil;
			cmd_done(clip_cmd);
			clip_cmd = nil;
			return;
		}
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			close(clip_fd);
			clip_fd = -1;
			clip_cmd->snarf_out = smprint("");
			cmd_done(clip_cmd);
			clip_cmd = nil;
			return;
		}
		if (clip_len + n + 1 > clip_cap) {
			int newcap = clip_cap ? clip_cap * 2 : 4096;
			while (newcap < clip_len + n + 1)
				newcap *= 2;
			clip_buf = realloc(clip_buf, newcap);
			clip_cap = newcap;
		}
		memcpy(clip_buf + clip_len, buf, n);
		clip_len += n;
		clip_buf[clip_len] = 0;
	}
}

static void handle_cmd(WaylandCmd *cmd) {
	WaylandClient *wl;
	switch (cmd->type) {
	case CMD_ATTACH:
		wl = calloc(1, sizeof(WaylandClient));
		wl->client = cmd->client;
		wl->repeat_interval_ms = key_repeat_ms;
		wl->repeat_delay_ms = key_repeat_delay_ms;
		wl->buffer_scale = wl_output_scale_factor;
		wl->desired_w = 640;
		wl->desired_h = 480;
		if (cmd->winsize != nil && cmd->winsize[0] != 0) {
			Rectangle wr;
			int havemin;
			if (parsewinsize(cmd->winsize, &wr, &havemin) != 0) {
				wl->desired_w = Dx(wr);
				wl->desired_h = Dy(wr);
			}
		}
		wl->wl_surface = wl_compositor_create_surface(wl_compositor);
		wl->xdg_surface = xdg_wm_base_get_xdg_surface(xdg_wm_base, wl->wl_surface);
		xdg_surface_add_listener(wl->xdg_surface, &xdg_surface_listener, wl);
		wl->xdg_toplevel = xdg_surface_get_toplevel(wl->xdg_surface);
		xdg_toplevel_add_listener(wl->xdg_toplevel, &xdg_toplevel_listener, wl);
		if (cmd->label != nil)
			xdg_toplevel_set_title(wl->xdg_toplevel, cmd->label);

		wl->wl_pointer = wl_seat_get_pointer(wl_seat);
		wl_pointer_add_listener(wl->wl_pointer, &pointer_listener, wl);
		wl->wl_seat = wl_seat;
		wl->wl_surface_cursor = wl_compositor_create_surface(wl_compositor);
		wayland_set_cursor(wl, &bigarrow);

		wl->wl_keyboard = wl_seat_get_keyboard(wl_seat);
		wl_keyboard_add_listener(wl->wl_keyboard, &keyboard_listener, wl);
		wl->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

		if (decoration_manager != nil) {
			struct zxdg_toplevel_decoration_v1 *d =
				zxdg_decoration_manager_v1_get_toplevel_decoration(
					decoration_manager, wl->xdg_toplevel);
			zxdg_toplevel_decoration_v1_add_listener(d, &xdg_toplevel_decoration_listener, wl);
			zxdg_toplevel_decoration_v1_set_mode(d,
				ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
			wl->decoration_mode = ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
			wl->xdg_decoration = d;
		} else {
			wl->decoration_mode = ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
		}
		update_csd_metrics(wl);

		cmd->client->view = wl;
		cmd->client->displaydpi = 110 * wl->buffer_scale;
		wl->attach_cmd = cmd;
		wl_surface_set_buffer_scale(wl->wl_surface, wl->buffer_scale);
		wl_surface_commit(wl->wl_surface);
		wl_display_flush(wl_display);
		return;
	case CMD_FLUSH:
		wl = (WaylandClient*)cmd->client->view;
		if (wl == nil || !wl->configured)
			break;
		int content_w = Dx(wl->memimage->r);
		int content_h = Dy(wl->memimage->r);
		int t = wl->csd_thickness;
		int w = content_w + 2 * t;
		int h = content_h + 2 * t;
		WaylandBuffer *b = get_xrgb8888_buffer(w, h);
		if (t > 0)
			draw_csd_frame(b, wl->memimage, w, h, t);
		else
			memcpy(b->data, (char*)wl->memimage->data->bdata, b->size);
		wl_surface_attach(wl->wl_surface, b->wl_buffer, 0, 0);
		if (t > 0)
			wl_surface_damage_buffer(wl->wl_surface, 0, 0, w, h);
		else
			wl_surface_damage_buffer(wl->wl_surface, cmd->rect.min.x, cmd->rect.min.y,
				Dx(cmd->rect), Dy(cmd->rect));
		wl_surface_commit(wl->wl_surface);
		wl_display_flush(wl_display);
		break;
	case CMD_SETCURSOR:
		wl = (WaylandClient*)cmd->client->view;
		if (wl == nil)
			break;
		if (cmd->cursor == nil)
			cmd->cursor = &bigarrow;
		wayland_set_cursor(wl, cmd->cursor);
		break;
	case CMD_SETLABEL:
		wl = (WaylandClient*)cmd->client->view;
		if (wl == nil || cmd->label == nil)
			break;
		xdg_toplevel_set_title(wl->xdg_toplevel, cmd->label);
		break;
	case CMD_SETMOUSE:
		wl = (WaylandClient*)cmd->client->view;
		if (wl == nil)
			break;
		if (pointer_warp != nil && wl->wl_pointer != nil && wl->pointer_enter_serial != 0) {
			int sw_buf = wl->surface_w;
			int sh_buf = wl->surface_h;
			double scale = buffer_scale(wl);
			double sx = (double)(cmd->point.x + wl->content_offset_x) / scale;
			double sy = (double)(cmd->point.y + wl->content_offset_y) / scale;
			double max_x = (double)sw_buf / scale;
			double max_y = (double)sh_buf / scale;
			double eps = 1.0 / 256.0;
			if (sx < 0) sx = 0;
			else if (sx >= max_x) sx = max_x - eps;
			if (sy < 0) sy = 0;
			else if (sy >= max_y) sy = max_y - eps;
			wp_pointer_warp_v1_warp_pointer(pointer_warp, wl->wl_surface, wl->wl_pointer,
				wl_fixed_from_double(sx), wl_fixed_from_double(sy), wl->pointer_enter_serial);
		}
		break;
	case CMD_GETSNARF:
		if (selection_owned && snarf_text != nil) {
			cmd->snarf_out = smprint("%s", snarf_text);
			break;
		}
		start_clip_read(cmd);
		return;
	case CMD_PUTSNARF:
		free(snarf_text);
		snarf_text = nil;
		if (cmd->snarf_in != nil)
			snarf_text = smprint("%s", cmd->snarf_in);
		if (wl_data_device_manager != nil && wl_data_device != nil) {
			struct wl_data_source *source =
				wl_data_device_manager_create_data_source(wl_data_device_manager);
			wl_data_source_add_listener(source, &data_source_listener, nil);
			wl_data_source_offer(source, "text/plain;charset=utf-8");
			wl_data_device_set_selection(wl_data_device, source, last_input_serial);
			wl_display_flush(wl_display);
			snarf_source = source;
			selection_owned = 1;
		}
		break;
	case CMD_SHUTDOWN:
		threadexitsall(nil);
		break;
	}
	cmd_done(cmd);
}

static void process_queue(void) {
	for (;;) {
		WaylandCmd *cmd = dequeue_cmd();
		if (cmd == nil)
			return;
		handle_cmd(cmd);
	}
}

void gfx_main(void) {
	wl_display = wl_display_connect(nil);
	if (wl_display == nil)
		fatal_wayland("Unable to connect to Wayland display");
	wl_registry = wl_display_get_registry(wl_display);
	wl_registry_add_listener(wl_registry, &wl_registry_listener, nil);
	wl_display_roundtrip(wl_display);

	if (wl_output == nil || wl_shm == nil || wl_compositor == nil ||
		xdg_wm_base == nil || wl_seat == nil || wl_data_device_manager == nil)
		fatal_wayland("Missing required Wayland globals");

	wl_output_add_listener(wl_output, &wl_output_listener, nil);
	xdg_wm_base_add_listener(xdg_wm_base, &xdg_wm_base_listener, nil);
	wl_data_device = wl_data_device_manager_get_data_device(wl_data_device_manager, wl_seat);
	wl_data_device_add_listener(wl_data_device, &data_device_listener, nil);
	wl_display_roundtrip(wl_display);

	if (pipe(wake_pipe) < 0)
		fatal_wayland("Failed to create wake pipe");
	fcntl(wake_pipe[0], F_SETFL, O_NONBLOCK);
	fcntl(wake_pipe[1], F_SETFL, O_NONBLOCK);

	entered_gfx_loop = 1;
	gfx_started();

	int display_fd = wl_display_get_fd(wl_display);
	struct pollfd fds[3];
	for (;;) {
		while (wl_display_prepare_read(wl_display) != 0)
			wl_display_dispatch_pending(wl_display);
		wl_display_flush(wl_display);

		fds[0].fd = display_fd;
		fds[0].events = POLLIN;
		int nfds = 1;
		if (clip_fd >= 0) {
			fds[nfds].fd = clip_fd;
			fds[nfds].events = POLLIN;
			nfds++;
		}
		fds[nfds].fd = wake_pipe[0];
		fds[nfds].events = POLLIN;
		nfds++;

		int ret = poll(fds, nfds, -1);
		if (ret < 0) {
			wl_display_cancel_read(wl_display);
			if (errno == EINTR)
				continue;
			fatal_wayland("poll failed");
		}

		int got_display = fds[0].revents & (POLLIN | POLLERR | POLLHUP);
		if (got_display) {
			if (wl_display_read_events(wl_display) < 0)
				fatal_wayland("wl_display_read_events failed");
		} else {
			wl_display_cancel_read(wl_display);
		}
		wl_display_dispatch_pending(wl_display);

		for (int i = 1; i < nfds; i++) {
			if (!(fds[i].revents & POLLIN))
				continue;
			if (fds[i].fd == wake_pipe[0]) {
				char buf[64];
				while (read(wake_pipe[0], buf, sizeof(buf)) > 0)
					;
				process_queue();
			} else if (fds[i].fd == clip_fd) {
				process_clip_read();
			}
		}
	}
}

static WaylandCmd *new_cmd(enum CmdType type, Client *c) {
	WaylandCmd *cmd = calloc(1, sizeof(WaylandCmd));
	cmd->type = type;
	cmd->client = c;
	return cmd;
}

Memimage *rpc_attach(Client *c, char *label, char *winsize) {
	c->impl = &wayland_impl;
	WaylandCmd *cmd = new_cmd(CMD_ATTACH, c);
	cmd->label = label;
	cmd->winsize = winsize;
	queue_cmd(cmd);
	cmd_wait(cmd);
	Memimage *img = cmd->img_out;
	free(cmd);
	return img;
}

static void rpc_flush(Client *c, Rectangle r) {
	WaylandCmd *cmd = new_cmd(CMD_FLUSH, c);
	cmd->rect = r;
	queue_cmd(cmd);
	cmd_wait(cmd);
	free(cmd);
}

static void rpc_setcursor(Client *c, Cursor *cursor, Cursor2 *c2) {
	USED(c2);
	WaylandCmd *cmd = new_cmd(CMD_SETCURSOR, c);
	cmd->cursor = cursor;
	queue_cmd(cmd);
	cmd_wait(cmd);
	free(cmd);
}

static void rpc_setlabel(Client *c, char *label) {
	WaylandCmd *cmd = new_cmd(CMD_SETLABEL, c);
	cmd->label = label;
	queue_cmd(cmd);
	cmd_wait(cmd);
	free(cmd);
}

static void rpc_setmouse(Client *c, Point p) {
	WaylandCmd *cmd = new_cmd(CMD_SETMOUSE, c);
	cmd->point = p;
	queue_cmd(cmd);
	cmd_wait(cmd);
	free(cmd);
}

static void rpc_topwin(Client *c) {
	USED(c);
}

static void rpc_bouncemouse(Client *c, Mouse m) {
	USED(c);
	USED(m);
}

static void rpc_resizeimg(Client *c) {
	USED(c);
}

static void rpc_resizewindow(Client *c, Rectangle r) {
	USED(c);
	USED(r);
}

char *rpc_getsnarf(void) {
	WaylandCmd *cmd = new_cmd(CMD_GETSNARF, nil);
	queue_cmd(cmd);
	cmd_wait(cmd);
	char *out = cmd->snarf_out;
	free(cmd);
	return out;
}

void rpc_putsnarf(char *snarf_in) {
	WaylandCmd *cmd = new_cmd(CMD_PUTSNARF, nil);
	cmd->snarf_in = snarf_in;
	queue_cmd(cmd);
	cmd_wait(cmd);
	free(cmd);
}

void rpc_shutdown(void) {
	WaylandCmd *cmd = new_cmd(CMD_SHUTDOWN, nil);
	queue_cmd(cmd);
	cmd_wait(cmd);
	free(cmd);
}

void rpc_gfxdrawlock(void) {
}

void rpc_gfxdrawunlock(void) {
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

int cloadmemimage(Memimage *i, Rectangle r, uchar *data, int ndata) {
	return _cloadmemimage(i, r, data, ndata);
}

int loadmemimage(Memimage *i, Rectangle r, uchar *data, int ndata) {
	return _loadmemimage(i, r, data, ndata);
}

int unloadmemimage(Memimage *i, Rectangle r, uchar *data, int ndata) {
	return _unloadmemimage(i, r, data, ndata);
}
