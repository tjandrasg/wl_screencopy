/* SPDX-License-Identifier: MIT */
#ifndef WSC_INTERNAL_H
#define WSC_INTERNAL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-client.h>
#include <wayland-util.h>

#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "wl_screencopy.h"

#include "convert.h"

#define WSC_DEFAULT_TIMEOUT_MS 2000
#define WSC_DEFAULT_BUFFERS 2
#define WSC_ERROR_STR_MAX 768
#define WSC_MAX_STAGING 8

/* ---------------------------------------------------------------- shm pool */

struct wsc_buffer {
	struct wl_buffer *wl_buffer;
	uint8_t *data;
};

struct wsc_shm {
	struct wl_shm_pool *pool;
	int fd;
	uint8_t *map;
	size_t size;
	/* Geometry the pool was created for. */
	int32_t width, height, stride;
	uint32_t format;
	int count;
	int next;
	struct wsc_buffer bufs[];
};

struct wsc_shm *wsc_shm_create(struct wl_shm *shm, int32_t width,
	int32_t height, int32_t stride, uint32_t format, int count,
	wsc_status *out_status);
void wsc_shm_destroy(struct wsc_shm *pool);

/* ---------------------------------------------------------------- outputs */

struct wsc_output {
	struct wl_list link;
	struct wl_output *wl_output;
	struct zxdg_output_v1 *xdg_output; /* may be NULL */
	uint32_t globals_name;
	bool done;

	/* wl_output.geometry */
	int32_t x, y;
	int32_t width, height; /* logical px, mode size / scale */
	int32_t phys_width_mm, phys_height_mm;
	int32_t refresh; /* mHz */
	int32_t transform;
	int32_t scale;
	char make[64], model[64];
	/* wl_output.name / wl_output.description (protocol v4+). */
	char out_name[64];
	char out_desc[256];

	/* zxdg_output_v1 */
	bool xdg_done;
	char xdg_name[64];
	char xdg_desc[256];
	int32_t lx, ly, lwidth, lheight;
	bool has_logical;
};

/* --------------------------------------------------------------- capture */

/* One in-flight zwlr_screencopy_frame_v1 exchange. */
struct wsc_capture {
	struct zwlr_screencopy_frame_v1 *frame;

	bool buffer_seen;
	bool buffer_done; /* v3 only; true when not applicable */
	bool dmabuf_seen;
	uint32_t shm_format;
	uint32_t dmabuf_format;
	int32_t width, height, stride;

	bool copy_sent;
	bool flags_seen;
	uint32_t flags;

	bool ready;
	bool failed;
	uint64_t pts_us;

	bool has_damage;
	int32_t damage_x, damage_y, damage_width, damage_height;

	struct wsc_buffer *buffer; /* shm buffer the copy targeted */
};

/* ---------------------------------------------------------------- session */

struct wsc_session {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_shm *shm;
	struct zwlr_screencopy_manager_v1 *screencopy;
	struct zxdg_output_manager_v1 *xdg_output_manager;
	uint32_t wlr_version;
	uint32_t backends; /* bitmask of wsc_backend_flag */

	struct wl_list outputs; /* wsc_output.link */

	wsc_session_options opts;

	char error[WSC_ERROR_STR_MAX];

	struct wsc_shm *pool;
	struct wsc_capture cap;

	/* Rotating, library-owned destination buffers for wsc_grab(). */
	uint8_t *staging[WSC_MAX_STAGING];
	size_t staging_size;
	size_t staging_stride;
	int staging_slots;
	int staging_next;

	wsc_stats stats;
	uint64_t frame_seq;
};

void wsc__set_error(wsc_session *session, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
void wsc__clear_error(wsc_session *session);

/* Last message routed through wl_log(), i.e. compositor protocol errors. */
const char *wsc__last_log(void);

/* Defined in capture.c, used by session.c on teardown. */
void wsc__capture_abort(struct wsc_session *session);

struct wsc_output *wsc__output_at(wsc_session *session, int idx);
struct wsc_output *wsc__output_by_name(wsc_session *session, uint32_t name);

/* Monotonic time in microseconds. */
uint64_t wsc__now_us(void);

#endif /* WSC_INTERNAL_H */
