/* SPDX-License-Identifier: MIT */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <strings.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>

#include "internal.h"

/* ------------------------------------------------------------- diagnostics */

/*
 * The compositor reports protocol errors through wl_log(). Capture the last
 * message so wsc_session_error_string() can explain what went wrong instead
 * of leaving a message on stderr that a library caller never sees.
 */
/*
 * libwayland's client log handler takes a va_list (wayland >= 1.23) and
 * cannot report the previous one back, so restoring means going back to the
 * default handler (NULL). A caller that installs its own handler after the
 * first session keeps it: wl_log_set_handler_client() is only called on the
 * 0->1 and 1->0 transitions.
 */
static _Thread_local char last_log[WSC_ERROR_STR_MAX];
static int log_handler_users;

static void wsc_log_handler(const char *fmt, va_list ap) {
	char buf[WSC_ERROR_STR_MAX];

	vsnprintf(buf, sizeof(buf), fmt, ap);
	snprintf(last_log, sizeof(last_log), "%s", buf);

	if (getenv("WSC_DEBUG")) {
		fprintf(stderr, "wl_screencopy: %s", buf);
	}
}

static void enable_log_capture(void) {
	if (log_handler_users++ == 0) {
		wl_log_set_handler_client(wsc_log_handler);
	}
}

static void disable_log_capture(void) {
	if (--log_handler_users <= 0) {
		log_handler_users = 0;
		wl_log_set_handler_client(NULL); /* back to the default */
	}
}

const char *wsc__last_log(void) { return last_log; }

void wsc__set_error(wsc_session *session, const char *fmt, ...) {
	char detail[WSC_ERROR_STR_MAX];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(detail, sizeof(detail), fmt, ap);
	va_end(ap);

	if (session) {
		snprintf(session->error, sizeof(session->error), "%s", detail);
	}
	if (getenv("WSC_DEBUG")) {
		fprintf(stderr, "wl_screencopy: %s", detail);
	}
}

void wsc__clear_error(wsc_session *session) {
	if (session) {
		session->error[0] = '\0';
	}
	last_log[0] = '\0';
}

const char *wsc_session_error_string(wsc_session *session) {
	static const char *fallback = "no error recorded";
	if (session && session->error[0] != '\0') {
		return session->error;
	}
	if (last_log[0] != '\0') {
		return last_log;
	}
	return fallback;
}

const char *wsc_status_str(wsc_status status) {
	switch (status) {
	case WSC_OK: return "ok";
	case WSC_ERROR: return "error";
	case WSC_ERROR_UNSUPPORTED: return "unsupported";
	case WSC_ERROR_INVALID: return "invalid argument";
	case WSC_ERROR_NO_MEMORY: return "out of memory";
	case WSC_ERROR_TIMEOUT: return "timeout";
	case WSC_ERROR_FAILED: return "capture failed";
	case WSC_ERROR_DISCONNECTED: return "disconnected";
	case WSC_ERROR_PROTOCOL: return "protocol error";
	case WSC_ERROR_BUSY: return "busy";
	default: return "unknown status";
	}
}

const char *wsc_version_string(void) {
	return "0.1.0 (wlr-screencopy-unstable-v1 v1-v3)";
}

uint64_t wsc__now_us(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

/* ------------------------------------------------------------------ shm */

static int create_anonymous_fd(size_t size) {
	int fd;

	fd = memfd_create("wl-screencopy", MFD_CLOEXEC);
	if (fd >= 0) {
		if (ftruncate(fd, (off_t)size) == 0) {
			return fd;
		}
		close(fd);
		return -1;
	}

	/* Fallback for kernels/namespaces without memfd_create(2). */
	const char *dir = getenv("XDG_RUNTIME_DIR");
	char template[PATH_MAX];
	snprintf(template, sizeof(template), "%s/wl-screencopy-XXXXXX",
		(dir && *dir) ? dir : "/tmp");
	fd = mkstemp(template);
	if (fd < 0) {
		return -1;
	}
	unlink(template);
	if (ftruncate(fd, (off_t)size) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

struct wsc_shm *wsc_shm_create(struct wl_shm *shm, int32_t width,
		int32_t height, int32_t stride, uint32_t format, int count,
		wsc_status *out_status) {
	struct wsc_shm *pool;
	size_t single, size;
	wsc_status status = WSC_OK;
	int fd;
	void *map;

	if (count < 1) {
		count = 1;
	}
	single = (size_t)stride * (size_t)height;
	size = single * (size_t)count;
	if (single == 0 || size / single != (size_t)count) {
		status = WSC_ERROR_INVALID;
		goto err;
	}

	pool = calloc(1, sizeof(*pool) + (size_t)count * sizeof(pool->bufs[0]));
	if (!pool) {
		status = WSC_ERROR_NO_MEMORY;
		goto err;
	}
	pool->fd = -1;
	/* Set early so the error path below releases whatever was created. */
	pool->count = count;

	fd = create_anonymous_fd(size);
	if (fd < 0) {
		wsc__set_error(NULL, "cannot allocate %zu bytes of shared memory: %s",
			size, strerror(errno));
		status = WSC_ERROR_NO_MEMORY;
		goto err_pool;
	}
	pool->fd = fd;

	map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		wsc__set_error(NULL, "mmap(%zu) failed: %s", size, strerror(errno));
		status = WSC_ERROR_NO_MEMORY;
		goto err_pool;
	}
	pool->map = map;
	memset(map, 0, size);

	pool->pool = wl_shm_create_pool(shm, fd, (int32_t)size);
	if (!pool->pool) {
		wsc__set_error(NULL, "wl_shm.create_pool failed: %s",
			strerror(errno));
		status = WSC_ERROR_NO_MEMORY;
		goto err_pool;
	}

	for (int i = 0; i < count; i++) {
		pool->bufs[i].data = (uint8_t *)map + single * (size_t)i;
		pool->bufs[i].wl_buffer = wl_shm_pool_create_buffer(pool->pool,
			(int32_t)(single * (size_t)i), width, height, stride, format);
		if (!pool->bufs[i].wl_buffer) {
			wsc__set_error(NULL, "wl_shm_pool.create_buffer failed: %s",
				strerror(errno));
			status = WSC_ERROR_NO_MEMORY;
			goto err_pool;
		}
	}

	pool->size = size;
	pool->width = width;
	pool->height = height;
	pool->stride = stride;
	pool->format = format;
	pool->count = count;
	pool->next = 0;

	if (out_status) {
		*out_status = WSC_OK;
	}
	return pool;

err_pool:
	if (pool) {
		for (int i = 0; i < pool->count; i++) {
			if (pool->bufs[i].wl_buffer) {
				wl_buffer_destroy(pool->bufs[i].wl_buffer);
			}
		}
		if (pool->pool) {
			wl_shm_pool_destroy(pool->pool);
		}
		if (pool->map && pool->map != MAP_FAILED) {
			munmap(pool->map, pool->size);
		}
		if (pool->fd >= 0) {
			close(pool->fd);
		}
		free(pool);
	}
err:
	if (out_status) {
		*out_status = status;
	}
	return NULL;
}

void wsc_shm_destroy(struct wsc_shm *pool) {
	if (!pool) {
		return;
	}
	for (int i = 0; i < pool->count; i++) {
		if (pool->bufs[i].wl_buffer) {
			wl_buffer_destroy(pool->bufs[i].wl_buffer);
		}
	}
	if (pool->pool) {
		wl_shm_pool_destroy(pool->pool);
	}
	if (pool->map) {
		munmap(pool->map, pool->size);
	}
	if (pool->fd >= 0) {
		close(pool->fd);
	}
	free(pool);
}

/* --------------------------------------------------------------- outputs */

static void output_handle_geometry(void *data, struct wl_output *wl_output,
		int32_t x, int32_t y, int32_t phys_w, int32_t phys_h, int32_t subpix,
		const char *make, const char *model, int32_t transform) {
	struct wsc_output *o = data;
	(void)wl_output; (void)subpix;

	o->x = x;
	o->y = y;
	o->phys_width_mm = phys_w;
	o->phys_height_mm = phys_h;
	o->transform = transform;
	if (make) {
		snprintf(o->make, sizeof(o->make), "%s", make);
	}
	if (model) {
		snprintf(o->model, sizeof(o->model), "%s", model);
	}
}

static void output_handle_mode(void *data, struct wl_output *wl_output,
		uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
	struct wsc_output *o = data;
	(void)wl_output;

	/* Prefer the preferred mode; otherwise keep the last one seen. */
	if (flags & WL_OUTPUT_MODE_CURRENT) {
		o->width = width;
		o->height = height;
		o->refresh = refresh;
	} else if (o->width == 0 && o->height == 0) {
		o->width = width;
		o->height = height;
		o->refresh = refresh;
	}
}

static void output_handle_done(void *data, struct wl_output *wl_output) {
	struct wsc_output *o = data;
	(void)wl_output;
	o->done = true;
}

static void output_handle_scale(void *data, struct wl_output *wl_output,
		int32_t scale) {
	struct wsc_output *o = data;
	(void)wl_output;
	o->scale = scale > 0 ? scale : 1;
}

/* wl_output v4 additions. These carry the connector name ("DP-1") on
 * compositors without xdg_output; every opcode needs a handler, libwayland
 * aborts the client when a dispatched event has a NULL listener slot. */
static void output_handle_name(void *data, struct wl_output *wl_output,
		const char *name) {
	struct wsc_output *o = data;
	(void)wl_output;
	if (name) {
		snprintf(o->out_name, sizeof(o->out_name), "%s", name);
	}
}

static void output_handle_description(void *data, struct wl_output *wl_output,
		const char *description) {
	struct wsc_output *o = data;
	(void)wl_output;
	if (description) {
		snprintf(o->out_desc, sizeof(o->out_desc), "%s", description);
	}
}

static const struct wl_output_listener output_listener = {
	.geometry = output_handle_geometry,
	.mode = output_handle_mode,
	.done = output_handle_done,
	.scale = output_handle_scale,
	.name = output_handle_name,
	.description = output_handle_description,
};

static void xdg_output_handle_logical_position(void *data,
		struct zxdg_output_v1 *xdg_output, int32_t x, int32_t y) {
	struct wsc_output *o = data;
	(void)xdg_output;
	o->lx = x;
	o->ly = y;
	o->has_logical = true;
}

static void xdg_output_handle_logical_size(void *data,
		struct zxdg_output_v1 *xdg_output, int32_t width, int32_t height) {
	struct wsc_output *o = data;
	(void)xdg_output;
	o->lwidth = width;
	o->lheight = height;
	o->has_logical = true;
}

static void xdg_output_handle_name(void *data,
		struct zxdg_output_v1 *xdg_output, const char *name) {
	struct wsc_output *o = data;
	(void)xdg_output;
	if (name) {
		snprintf(o->xdg_name, sizeof(o->xdg_name), "%s", name);
	}
}

static void xdg_output_handle_description(void *data,
		struct zxdg_output_v1 *xdg_output, const char *desc) {
	struct wsc_output *o = data;
	(void)xdg_output;
	if (desc) {
		snprintf(o->xdg_desc, sizeof(o->xdg_desc), "%s", desc);
	}
}

static void xdg_output_handle_done(void *data,
		struct zxdg_output_v1 *xdg_output) {
	struct wsc_output *o = data;
	(void)xdg_output;
	o->xdg_done = true;
}

static const struct zxdg_output_v1_listener xdg_output_listener = {
	.logical_position = xdg_output_handle_logical_position,
	.logical_size = xdg_output_handle_logical_size,
	.done = xdg_output_handle_done,
	.name = xdg_output_handle_name,
	.description = xdg_output_handle_description,
};

static struct wsc_output *output_new(wsc_session *session, uint32_t name,
		uint32_t version) {
	struct wsc_output *o = calloc(1, sizeof(*o));
	if (!o) {
		return NULL;
	}
	o->globals_name = name;
	o->scale = 1;
	o->wl_output = wl_registry_bind(session->registry, name,
		&wl_output_interface, version < 4 ? version : 4);
	if (!o->wl_output) {
		free(o);
		return NULL;
	}
	wl_list_insert(session->outputs.prev, &o->link);
	wl_output_add_listener(o->wl_output, &output_listener, o);
	return o;
}

static void output_destroy(struct wsc_output *o) {
	if (!o) {
		return;
	}
	if (o->xdg_output) {
		zxdg_output_v1_destroy(o->xdg_output);
	}
	if (o->wl_output) {
		wl_output_destroy(o->wl_output);
	}
	wl_list_remove(&o->link);
	free(o);
}

struct wsc_output *wsc__output_at(wsc_session *session, int idx) {
	struct wsc_output *o;
	int i = 0;

	wl_list_for_each(o, &session->outputs, link) {
		if (i++ == idx) {
			return o;
		}
	}
	return NULL;
}

struct wsc_output *wsc__output_by_name(wsc_session *session, uint32_t name) {
	struct wsc_output *o;

	wl_list_for_each(o, &session->outputs, link) {
		if (o->globals_name == name) {
			return o;
		}
	}
	return NULL;
}

/* -------------------------------------------------------------- registry */

static void registry_handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	wsc_session *session = data;
	(void)registry;

	if (strcmp(interface, wl_shm_interface.name) == 0) {
		session->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0) {
		uint32_t bind_version = version < 3 ? version : 3;
		session->screencopy = wl_registry_bind(registry, name,
			&zwlr_screencopy_manager_v1_interface, bind_version);
		session->wlr_version = bind_version;
		session->backends |= WSC_BACKEND_WLR_SCREENCOPY;
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		output_new(session, name, version);
	} else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
		/* Bound lazily below; remember the manager. */
		if (!session->xdg_output_manager) {
			session->xdg_output_manager = wl_registry_bind(registry, name,
				&zxdg_output_manager_v1_interface, version < 3 ? version : 2);
		}
	}
}

static void registry_handle_global_remove(void *data,
		struct wl_registry *registry, uint32_t name) {
	wsc_session *session = data;
	struct wsc_output *o;
	(void)registry;

	o = wsc__output_by_name(session, name);
	if (o) {
		output_destroy(o);
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_handle_global,
	.global_remove = registry_handle_global_remove,
};

/* --------------------------------------------------------------- session */

wsc_session *wsc_session_open(const wsc_session_options *options,
		wsc_status *out_status) {
	wsc_session *session;
	wsc_status status = WSC_OK;

	session = calloc(1, sizeof(*session));
	if (!session) {
		status = WSC_ERROR_NO_MEMORY;
		goto err;
	}

	session->opts = options ? *options :
		(wsc_session_options)WSC_SESSION_OPTIONS_INIT;
	if (session->opts.buffer_count <= 0) {
		session->opts.buffer_count = WSC_DEFAULT_BUFFERS;
	}
	if (session->opts.buffer_count > WSC_MAX_STAGING) {
		session->opts.buffer_count = WSC_MAX_STAGING;
	}

	wl_list_init(&session->outputs);
	enable_log_capture();

	session->display = wl_display_connect(session->opts.display_name);
	if (!session->display) {
		const char *name = session->opts.display_name ?
			session->opts.display_name :
			(getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "wayland-0");
		wsc__set_error(session,
			"cannot connect to a Wayland compositor (socket \"%s\"): %s. "
			"Are you running inside a Wayland session? Note that X11/VNC "
			"sessions have no Wayland socket.",
			name, strerror(errno));
		status = WSC_ERROR_DISCONNECTED;
		goto err_session;
	}

	session->registry = wl_display_get_registry(session->display);
	if (!session->registry) {
		wsc__set_error(session, "wl_display.get_registry failed: %s",
			strerror(errno));
		status = WSC_ERROR_DISCONNECTED;
		goto err_session;
	}
	wl_registry_add_listener(session->registry, &registry_listener, session);

	if (wl_display_roundtrip(session->display) < 0) {
		wsc__set_error(session, "initial registry roundtrip failed: %s",
			strerror(errno));
		status = WSC_ERROR_DISCONNECTED;
		goto err_session;
	}
	/* Second roundtrip: xdg_output and wl_output send their state after the
	 * global is announced, and only in a later event batch. */
	if (wl_display_roundtrip(session->display) < 0) {
		wsc__set_error(session, "output description roundtrip failed: %s",
			strerror(errno));
		status = WSC_ERROR_DISCONNECTED;
		goto err_session;
	}

	/* Attach xdg_output to every output that predates the manager or missed
	 * its bind. */
	if (session->xdg_output_manager) {
		struct wsc_output *o;
		wl_list_for_each(o, &session->outputs, link) {
			if (!o->xdg_output) {
				o->xdg_output = zxdg_output_manager_v1_get_xdg_output(
					session->xdg_output_manager, o->wl_output);
				if (o->xdg_output) {
					zxdg_output_v1_add_listener(o->xdg_output,
						&xdg_output_listener, o);
				}
			}
		}
		wl_display_roundtrip(session->display);
	}

	if (!(session->backends & WSC_BACKEND_WLR_SCREENCOPY)) {
		wsc__set_error(session,
			"the compositor does not advertise zwlr_screencopy_manager_v1, "
			"so wlr-screencopy capture is unavailable. This protocol is "
			"implemented by wlroots-based compositors (Sway, Hyprland, "
			"river, labwc, Cage, Wayfire, ...). Outputs can still be "
			"listed. See README.md for compositors and alternatives.");
	}

	if (out_status) {
		*out_status = status;
	}
	return session;

err_session:
	wsc_session_close(session);
err:
	if (out_status) {
		*out_status = status;
	}
	return NULL;
}

void wsc_session_close(wsc_session *session) {
	if (!session) {
		return;
	}

	wsc__capture_abort(session);

	if (session->pool) {
		wsc_shm_destroy(session->pool);
		session->pool = NULL;
	}

	struct wsc_output *o, *tmp;
	wl_list_for_each_safe(o, tmp, &session->outputs, link) {
		output_destroy(o);
	}

	for (int i = 0; i < WSC_MAX_STAGING; i++) {
		free(session->staging[i]);
	}

	if (session->xdg_output_manager) {
		zxdg_output_manager_v1_destroy(session->xdg_output_manager);
	}
	if (session->screencopy) {
		zwlr_screencopy_manager_v1_destroy(session->screencopy);
	}
	if (session->shm) {
		wl_shm_destroy(session->shm);
	}
	if (session->registry) {
		wl_registry_destroy(session->registry);
	}
	if (session->display) {
		wl_display_disconnect(session->display);
	}
	if (log_handler_users > 0) {
		disable_log_capture();
	}
	free(session);
}

uint32_t wsc_session_backends(wsc_session *session) {
	return session ? session->backends : WSC_BACKEND_NONE;
}

uint32_t wsc_session_wlr_version(wsc_session *session) {
	return session ? session->wlr_version : 0;
}

int wsc_session_fd(wsc_session *session) {
	if (!session || !session->display) {
		return -1;
	}
	return wl_display_get_fd(session->display);
}

wsc_status wsc_session_poll(wsc_session *session, int timeout_ms) {
	struct pollfd pfd;
	int ret;

	if (!session || !session->display) {
		return WSC_ERROR_INVALID;
	}

	while (wl_display_prepare_read(session->display) != 0) {
		if (wl_display_get_error(session->display) != 0) {
			return WSC_ERROR_DISCONNECTED;
		}
		if (wl_display_dispatch_pending(session->display) < 0) {
			return WSC_ERROR_DISCONNECTED;
		}
	}

	if (wl_display_flush(session->display) < 0) {
		if (errno != EAGAIN) {
			wl_display_cancel_read(session->display);
			return WSC_ERROR_DISCONNECTED;
		}
	}

	if (timeout_ms != 0) {
		pfd.fd = wl_display_get_fd(session->display);
		pfd.events = POLLIN;
		pfd.revents = 0;
		do {
			ret = poll(&pfd, 1, timeout_ms < 0 ? -1 : timeout_ms);
		} while (ret == -1 && errno == EINTR);

		if (ret < 0) {
			wl_display_cancel_read(session->display);
			wsc__set_error(session, "poll() on the Wayland socket failed: %s",
				strerror(errno));
			return WSC_ERROR;
		}
		if (ret == 0) {
			/* Nothing to read before the deadline. */
			wl_display_cancel_read(session->display);
			return WSC_ERROR_TIMEOUT;
		}
	}

	if (wl_display_read_events(session->display) < 0) {
		return WSC_ERROR_DISCONNECTED;
	}
	if (wl_display_dispatch_pending(session->display) < 0) {
		return WSC_ERROR_DISCONNECTED;
	}
	return WSC_OK;
}

int wsc_output_count(wsc_session *session) {
	struct wsc_output *o;
	int n = 0;

	if (!session) {
		return -1;
	}
	wl_list_for_each(o, &session->outputs, link) {
		n++;
	}
	return n;
}

int wsc_session_refresh(wsc_session *session) {
	if (!session) {
		return -1;
	}
	if (wl_display_roundtrip(session->display) < 0) {
		wsc__set_error(session, "refresh roundtrip failed: %s",
			strerror(errno));
		return -1;
	}
	return wsc_output_count(session);
}

wsc_status wsc_output_get(wsc_session *session, int idx,
		wsc_output_info *out_info) {
	struct wsc_output *o;
	struct wsc_output *first;

	if (!session || !out_info) {
		return WSC_ERROR_INVALID;
	}
	o = wsc__output_at(session, idx);
	if (!o) {
		wsc__set_error(session, "no output at index %d (compositor exposes "
			"%d output(s))", idx, wsc_output_count(session));
		return WSC_ERROR_INVALID;
	}

	memset(out_info, 0, sizeof(*out_info));
	out_info->wl_name = o->globals_name;
	out_info->x = o->has_logical ? o->lx : o->x;
	out_info->y = o->has_logical ? o->ly : o->y;
	/* wl_output.geometry/mode are in logical pixels already; xdg_output
	 * reports the same logical size and is preferred when present. */
	out_info->width = o->has_logical && o->lwidth > 0 ? o->lwidth : o->width;
	out_info->height = o->has_logical && o->lheight > 0 ? o->lheight : o->height;
	out_info->phys_width_mm = o->phys_width_mm;
	out_info->phys_height_mm = o->phys_height_mm;
	out_info->refresh = o->refresh;
	out_info->transform = o->transform;
	out_info->scale = o->scale > 0 ? o->scale : 1;
	out_info->effective_scale = (double)out_info->scale;

	/* xdg_output is more descriptive, wl_output.name is more universal. */
	if (o->xdg_name[0]) {
		snprintf(out_info->name, sizeof(out_info->name), "%s", o->xdg_name);
	} else if (o->out_name[0]) {
		snprintf(out_info->name, sizeof(out_info->name), "%s", o->out_name);
	}
	if (o->xdg_desc[0]) {
		snprintf(out_info->description, sizeof(out_info->description), "%s",
			o->xdg_desc);
	} else if (o->out_desc[0]) {
		snprintf(out_info->description, sizeof(out_info->description), "%s",
			o->out_desc);
	} else if (o->make[0] || o->model[0]) {
		snprintf(out_info->description, sizeof(out_info->description),
			"%s %s", o->make, o->model);
	}

	first = wsc__output_at(session, 0);
	out_info->preferred = (first == o);

	return WSC_OK;
}

int wsc_output_find(wsc_session *session, const char *name) {
	int count, i;

	if (!session || !name) {
		return -1;
	}
	count = wsc_output_count(session);

	/* Plain index. */
	if (*name) {
		char *end = NULL;
		long v = strtol(name, &end, 10);
		if (end && *end == '\0' && v >= 0 && v < count) {
			return (int)v;
		}
	}

	for (i = 0; i < count; i++) {
		wsc_output_info info;
		if (wsc_output_get(session, i, &info) != WSC_OK) {
			continue;
		}
		if (info.name[0] && strcasecmp(info.name, name) == 0) {
			return i;
		}
	}
	for (i = 0; i < count; i++) {
		wsc_output_info info;
		if (wsc_output_get(session, i, &info) != WSC_OK) {
			continue;
		}
		if (info.description[0] &&
				strcasestr(info.description, name) != NULL) {
			return i;
		}
	}
	wsc__set_error(session, "no output matching \"%s\"", name);
	return -1;
}

wsc_status wsc_session_get_stats(wsc_session *session, wsc_stats *out_stats) {
	if (!session || !out_stats) {
		return WSC_ERROR_INVALID;
	}
	*out_stats = session->stats;
	return WSC_OK;
}
