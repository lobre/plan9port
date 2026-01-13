#include <u.h>
#include <libc.h>
#include <thread.h>
#include <fcntl.h>
#include <errno.h>
#include <wayland-client.h>

#include "wayland-clip.h"

typedef struct OfferInfo OfferInfo;
struct OfferInfo {
	char *mime;
	int mime_pref;
	OfferInfo *next;
	struct wl_data_offer *offer;
};

typedef struct ClipboardJob ClipboardJob;
struct ClipboardJob {
	int type;
	char *data_in;
	char *data_out;
	int done;
};

typedef struct ClipboardReadTask ClipboardReadTask;
struct ClipboardReadTask {
	int fd;
	ulong gen;
	ClipboardJob *job;
};

enum {
	ClipboardJobGet = 1,
	ClipboardJobPut,
};

static const char *clipboard_mimes[] = {
	"text/plain;charset=utf-8",
	"text/plain;charset=utf8",
	"UTF8_STRING",
	"text/plain",
	"TEXT",
	"STRING",
};

static QLock clip_lock;
static struct wl_display *clip_display;
static struct wl_data_device_manager *clip_manager;
static struct wl_data_device *clip_device;
static struct wl_data_offer *selection_offer;
static struct wl_data_source *snarf_source;
static int selection_owned;
static ulong selection_gen;
static uint32_t seat_serial;
static char *snarf;
static OfferInfo *offer_head;

static QLock clipboard_job_lock;
static Rendez clipboard_job_r;
static ClipboardJob *clipboard_job;
static int clipboard_wake_pipe[2] = { -1, -1 };
static const struct wl_data_source_listener wl_data_source_listener;
static int clip_thread_id;
static int shutdown_requested;
static int clip_inited;

static void wlclip_check_thread(void) {
	if (clip_thread_id != 0 && threadid() != clip_thread_id) {
		sysfatal("wlclip: called from wrong thread");
	}
}

static void wlclip_wake(void) {
	if (clipboard_wake_pipe[1] >= 0) {
		char b = 1;
		if (write(clipboard_wake_pipe[1], &b, sizeof(b)) < 0) {
			USED(b);
		}
	}
}

static void offer_list_add(OfferInfo *info) {
	info->next = offer_head;
	offer_head = info;
}

static void offer_list_remove(OfferInfo *info) {
	OfferInfo **pp = &offer_head;
	for (; *pp != NULL; pp = &(*pp)->next) {
		if (*pp == info) {
			*pp = info->next;
			info->next = NULL;
			return;
		}
	}
}

static void free_offer_info(OfferInfo *info) {
	if (info == NULL) {
		return;
	}
	if (info->offer != NULL) {
		wl_proxy_set_user_data((struct wl_proxy *) info->offer, NULL);
		wl_data_offer_destroy(info->offer);
		info->offer = NULL;
	}
	free(info->mime);
	free(info);
}

static void offers_free_all(void) {
	OfferInfo *info;
	qlock(&clip_lock);
	info = offer_head;
	offer_head = NULL;
	qunlock(&clip_lock);
	while (info != NULL) {
		OfferInfo *next = info->next;
		free_offer_info(info);
		info = next;
	}
}

static void offers_keep_only(struct wl_data_offer *keep) {
	OfferInfo *tofree = NULL;
	qlock(&clip_lock);
	OfferInfo *info = offer_head;
	while (info != NULL) {
		OfferInfo *next = info->next;
		if (info->offer != keep) {
			offer_list_remove(info);
			info->next = tofree;
			tofree = info;
		}
		info = next;
	}
	qunlock(&clip_lock);
	while (tofree != NULL) {
		OfferInfo *next = tofree->next;
		free_offer_info(tofree);
		tofree = next;
	}
}

static int mime_pref_index(const char *mime) {
	const char *p;

	if (mime == NULL) {
		return -1;
	}
	if (cistrncmp((char*)mime, "text/plain", 10) == 0) {
		p = mime + 10;
		while (*p == ';' || *p == ' ' || *p == '\t') {
			p++;
		}
		if (*p == '\0') {
			return 3;
		}
		for (; *p; p++) {
			while (*p == ';' || *p == ' ' || *p == '\t') {
				p++;
			}
			if (*p == '\0') {
				break;
			}
			if (cistrncmp((char*)p, "charset=", 8) == 0) {
				p += 8;
				if (cistrncmp((char*)p, "utf-8", 5) == 0) {
					return 0;
				}
				if (cistrncmp((char*)p, "utf8", 4) == 0) {
					return 1;
				}
			}
			while (*p && *p != ';') {
				p++;
			}
			if (*p == '\0') {
				break;
			}
		}
	}
	for (int i = 0; i < nelem(clipboard_mimes); i++) {
		if (cistrcmp((char*)mime, (char*)clipboard_mimes[i]) == 0) {
			return i;
		}
	}
	return -1;
}

static void clipboard_job_complete(ClipboardJob *job) {
	qlock(&clipboard_job_lock);
	job->done = 1;
	rwakeup(&clipboard_job_r);
	qunlock(&clipboard_job_lock);
}

static void clipboard_job_start(ClipboardJob *job) {
	qlock(&clipboard_job_lock);
	while (clipboard_job != NULL) {
		rsleep(&clipboard_job_r);
	}
	clipboard_job = job;
	job->done = 0;
	qunlock(&clipboard_job_lock);

	wlclip_wake();
}

static void clipboard_job_wait(ClipboardJob *job) {
	qlock(&clipboard_job_lock);
	while (!job->done) {
		rsleep(&clipboard_job_r);
	}
	clipboard_job = NULL;
	rwakeup(&clipboard_job_r);
	qunlock(&clipboard_job_lock);
}

static void clipboard_readproc(void *v) {
	ClipboardReadTask *task = v;
	int fd = task->fd;
	int total = 0;
	char *new_snarf = NULL;
	int read_error = 0;
	for (;;) {
		char buf[128];
		int n = read(fd, &buf, sizeof(buf));
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n < 0) {
			read_error = 1;
			break;
		}
		if (n == 0) {
			break;
		}
		char *tmp = calloc(1, total + n + 1);
		if (new_snarf != NULL) {
			memcpy(tmp, new_snarf, total);
		}
		memcpy(tmp + total, buf, n);
		total += n;
		free(new_snarf);
		new_snarf = tmp;
	}
	close(fd);

	if (read_error) {
		free(new_snarf);
		task->job->data_out = NULL;
		clipboard_job_complete(task->job);
		free(task);
		threadexits(nil);
	}
	if (new_snarf == NULL) {
		new_snarf = smprint("");
	}

	char *out = NULL;
	qlock(&clip_lock);
	if (!selection_owned && task->gen == selection_gen) {
		free(snarf);
		snarf = new_snarf;
		new_snarf = NULL;
		if (snarf != NULL) {
			int n = strlen(snarf);
			out = calloc(1, n+1);
			strncpy(out, snarf, n);
		}
	}
	qunlock(&clip_lock);

	free(new_snarf);
	task->job->data_out = out;
	clipboard_job_complete(task->job);
	free(task);
	threadexits(nil);
}

static void clipboard_job_get(ClipboardJob *job) {
	struct wl_data_offer *offer = NULL;
	char *mime = NULL;
	ulong gen = 0;

	qlock(&clip_lock);
	if (selection_owned && snarf != NULL) {
		int n = strlen(snarf);
		job->data_out = calloc(1, n+1);
		strncpy(job->data_out, snarf, n);
		qunlock(&clip_lock);
		clipboard_job_complete(job);
		return;
	}
	qunlock(&clip_lock);

	qlock(&clip_lock);
	offer = selection_offer;
	gen = selection_gen;
	if (offer != NULL) {
		OfferInfo *info = wl_proxy_get_user_data((struct wl_proxy *) offer);
		if (info != NULL && info->mime != NULL) {
			mime = smprint("%s", info->mime);
		}
	}
	qunlock(&clip_lock);

	if (offer == NULL || mime == NULL) {
		free(mime);
		job->data_out = NULL;
		clipboard_job_complete(job);
		return;
	}

	int fds[2];
	if (pipe(fds) < 0) {
		free(mime);
		job->data_out = NULL;
		clipboard_job_complete(job);
		return;
	}

	qlock(&clip_lock);
	uint32_t serial = seat_serial;
	qunlock(&clip_lock);
	if (serial != 0) {
		wl_data_offer_accept(offer, serial, mime);
	}
	wl_data_offer_receive(offer, mime, fds[1]);
	close(fds[1]);
	wl_display_flush(clip_display);
	free(mime);

	ClipboardReadTask *task = calloc(1, sizeof(ClipboardReadTask));
	task->fd = fds[0];
	task->gen = gen;
	task->job = job;
	proccreate(clipboard_readproc, task, 0);
}

static void clipboard_job_put(ClipboardJob *job) {
	qlock(&clip_lock);
	int n = job->data_in != NULL ? strlen(job->data_in) : 0;
	free(snarf);
	snarf = calloc(1, n+1);
	if (job->data_in != NULL) {
		strncpy(snarf, job->data_in, n);
	}

	struct wl_data_source *source =
		wl_data_device_manager_create_data_source(clip_manager);
	wl_data_source_add_listener(source, &wl_data_source_listener, NULL);
	for (int i = 0; i < nelem(clipboard_mimes); i++) {
		wl_data_source_offer(source, clipboard_mimes[i]);
	}
	selection_owned = 1;
	snarf_source = source;
	wl_data_device_set_selection(clip_device, source, seat_serial);
	qunlock(&clip_lock);
	wl_display_flush(clip_display);
}

static void clipboard_job_pump(void) {
	ClipboardJob *job = NULL;
	qlock(&clipboard_job_lock);
	if (clipboard_job != NULL && !clipboard_job->done) {
		job = clipboard_job;
	}
	qunlock(&clipboard_job_lock);
	if (job == NULL) {
		return;
	}
	switch (job->type) {
	case ClipboardJobGet:
		clipboard_job_get(job);
		return;
	case ClipboardJobPut:
		clipboard_job_put(job);
		break;
	default:
		break;
	}
	clipboard_job_complete(job);
}

static void wl_data_offer_offer(void *data, struct wl_data_offer *offer, const char *mime_type) {
	if (mime_type == NULL) {
		return;
	}
	OfferInfo *info = data;
	if (info == NULL)
		return;
	int pref = mime_pref_index(mime_type);
	if (pref < 0) {
		return;
	}
	if (info->mime != NULL && info->mime_pref >= 0 && pref >= info->mime_pref) {
		return;
	}
	free(info->mime);
	info->mime = smprint("%s", mime_type);
	info->mime_pref = pref;
}

static const struct wl_data_offer_listener wl_data_offer_listener = {
	.offer = wl_data_offer_offer,
};

static void wl_data_device_listener_data_offer(void *data,
	struct wl_data_device *wl_data_device, struct wl_data_offer *id) {
	OfferInfo *info = calloc(1, sizeof(OfferInfo));
	info->offer = id;
	info->mime_pref = -1;
	qlock(&clip_lock);
	offer_list_add(info);
	qunlock(&clip_lock);
	wl_proxy_set_user_data((struct wl_proxy *) id, info);
	wl_data_offer_add_listener(id, &wl_data_offer_listener, info);
}

static void wl_data_device_listener_data_enter(void *data,
	struct wl_data_device *wl_data_device, uint32_t serial,
	struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y,
	struct wl_data_offer *id) {}

static void wl_data_device_listener_data_leave(void *data,
	struct wl_data_device *wl_data_device) {}

static void wl_data_device_listener_data_motion(void *data,
	struct wl_data_device *wl_data_device, uint32_t time,
	wl_fixed_t x, wl_fixed_t y) {}

static void wl_data_device_listener_data_drop(void *data,
	struct wl_data_device *wl_data_device) {}

static void wl_data_device_listener_selection(void *data,
	struct wl_data_device *wl_data_device, struct wl_data_offer *id) {
	if (id == NULL) {
		int owns;
		qlock(&clip_lock);
		selection_offer = NULL;
		selection_gen++;
		owns = selection_owned;
		qunlock(&clip_lock);
		offers_free_all();
		if (!owns) {
			qlock(&clip_lock);
			free(snarf);
			snarf = NULL;
			qunlock(&clip_lock);
		}
		return;
	}

	qlock(&clip_lock);
	selection_offer = id;
	selection_gen++;
	selection_owned = 0;
	snarf_source = NULL;
	qunlock(&clip_lock);
	offers_keep_only(id);
}

static const struct wl_data_device_listener wl_data_device_listener = {
	.data_offer = wl_data_device_listener_data_offer,
	.enter = wl_data_device_listener_data_enter,
	.leave = wl_data_device_listener_data_leave,
	.motion = wl_data_device_listener_data_motion,
	.drop = wl_data_device_listener_data_drop,
	.selection = wl_data_device_listener_selection,
};

static void wl_data_source_target(void *data,
	struct wl_data_source *wl_data_source,
	const char *mime_type) {}

static void wl_data_source_send(void *data,
	struct wl_data_source *wl_data_source,
	const char *mime_type, int32_t fd) {
	if (mime_pref_index(mime_type) < 0) {
		close(fd);
		return;
	}

	qlock(&clip_lock);

	int total = 0;
	if (snarf != NULL) {
		total = strlen(snarf);
	}
	char *p = snarf;
	while (total > 0) {
		int n = write(fd, p, total);
		if (n < 0 && errno == EAGAIN) {
			continue;
		}
		if (n < 0) {
			sysfatal("Write error");
		}
		p += n;
		total -= n;
	}

	qunlock(&clip_lock);
	close(fd);
}

static void wl_data_source_cancelled(void *data, struct wl_data_source *wl_data_source) {
	qlock(&clip_lock);
	if (wl_data_source == snarf_source) {
		snarf_source = NULL;
		selection_owned = 0;
	}
	qunlock(&clip_lock);
	wl_data_source_destroy(wl_data_source);
}

static const struct wl_data_source_listener wl_data_source_listener = {
	.target = wl_data_source_target,
	.send = wl_data_source_send,
	.cancelled = wl_data_source_cancelled,
};

void wlclip_init(struct wl_display *display,
	struct wl_data_device_manager *manager,
	struct wl_data_device *device) {
	clip_thread_id = threadid();
	clip_inited = 1;
	clip_display = display;
	clip_manager = manager;
	clip_device = device;
	clipboard_job_r.l = &clipboard_job_lock;

	if (clip_device != NULL) {
		wl_data_device_add_listener(clip_device, &wl_data_device_listener, NULL);
	}

	if (pipe(clipboard_wake_pipe) < 0) {
		sysfatal("Failed to create clipboard wake pipe");
	}
	fcntl(clipboard_wake_pipe[0], F_SETFL, O_NONBLOCK);
	fcntl(clipboard_wake_pipe[1], F_SETFL, O_NONBLOCK);
}

void wlclip_shutdown(void) {
	if (!clip_inited) {
		return;
	}
	if (clip_thread_id != 0 && threadid() != clip_thread_id) {
		shutdown_requested = 1;
		wlclip_wake();
		return;
	}
	offers_free_all();
	clip_inited = 0;
	clip_thread_id = 0;
}

void wlclip_set_serial(uint32_t serial) {
	if (!clip_inited) {
		return;
	}
	wlclip_check_thread();
	qlock(&clip_lock);
	seat_serial = serial;
	qunlock(&clip_lock);
}

void wlclip_pump(void) {
	if (!clip_inited) {
		return;
	}
	wlclip_check_thread();
	if (shutdown_requested) {
		shutdown_requested = 0;
		offers_free_all();
		clip_inited = 0;
		clip_thread_id = 0;
		return;
	}
	clipboard_job_pump();
}

int wlclip_poll_fd(void) {
	if (!clip_inited) {
		return -1;
	}
	wlclip_check_thread();
	return clipboard_wake_pipe[0];
}

void wlclip_drain_wake(void) {
	if (!clip_inited) {
		return;
	}
	wlclip_check_thread();
	char buf[64];
	if (clipboard_wake_pipe[0] < 0) {
		return;
	}
	for (;;) {
		int n = read(clipboard_wake_pipe[0], buf, sizeof(buf));
		if (n <= 0) {
			break;
		}
	}
}

char *wlclip_getsnarf(void) {
	ClipboardJob *job = calloc(1, sizeof(ClipboardJob));
	job->type = ClipboardJobGet;
	clipboard_job_start(job);
	clipboard_job_wait(job);
	char *out = job->data_out;
	free(job);
	return out;
}

void wlclip_putsnarf(char *snarf_in) {
	ClipboardJob *job = calloc(1, sizeof(ClipboardJob));
	job->type = ClipboardJobPut;
	if (snarf_in != NULL) {
		job->data_in = smprint("%s", snarf_in);
	} else {
		job->data_in = smprint("");
	}
	clipboard_job_start(job);
	clipboard_job_wait(job);
	free(job->data_in);
	free(job);
}
