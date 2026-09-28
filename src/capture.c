/* SPDX-License-Identifier: MIT */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <wayland-client.h>

#include "internal.h"

/*
 * Client side of zwlr_screencopy_frame_v1.
 *
 * The per-frame exchange, from the protocol description:
 *
 *   capture_output[_region]  ->  [buffer] [linux_dmabuf] buffer_done
 *   copy[_with_damage]       ->  [flags] [damage...] ready | failed
 *
 * A frame object is single use: after ready or failed it is destroyed and the
 * next capture creates a fresh one, so this loop is re-entered for every
 * frame of a recording.
 */

static void capture_reset(struct wsc_capture *cap) {
	memset(cap, 0, sizeof(*cap));
}

/* Destroy the in-flight frame object, if any, and forget its state. */
void wsc__capture_abort(wsc_session *session) {
	struct wsc_capture *cap;

	if (!session) {
		return;
	}
	cap = &session->cap;
	if (cap->frame) {
		zwlr_screencopy_frame_v1_destroy(cap->frame);
	}
	capture_reset(cap);
}

/* ------------------------------------------------------------ events */

static void frame_handle_buffer(void *data,
		struct zwlr_screencopy_frame_v1 *frame, uint32_t format,
		uint32_t width, uint32_t height, uint32_t stride) {
	struct wsc_capture *cap = data;
	(void)frame;

	cap->buffer_seen = true;
	cap->shm_format = format;
	cap->width = (int32_t)width;
	cap->height = (int32_t)height;
	cap->stride = (int32_t)stride;
}

static void frame_handle_linux_dmabuf(void *data,
		struct zwlr_screencopy_frame_v1 *frame, uint32_t format,
		uint32_t width, uint32_t height) {
	struct wsc_capture *cap = data;
	(void)frame;

	cap->dmabuf_seen = true;
	cap->dmabuf_format = format;
	if (!cap->buffer_seen) {
		cap->width = (int32_t)width;
		cap->height = (int32_t)height;
	}
}

static void frame_handle_buffer_done(void *data,
		struct zwlr_screencopy_frame_v1 *frame) {
	struct wsc_capture *cap = data;
	(void)frame;

	cap->buffer_done = true;
}

static void frame_handle_flags(void *data,
		struct zwlr_screencopy_frame_v1 *frame, uint32_t flags) {
	struct wsc_capture *cap = data;
	(void)frame;

	cap->flags_seen = true;
	cap->flags = flags;
}

static void frame_handle_ready(void *data,
		struct zwlr_screencopy_frame_v1 *frame, uint32_t tv_sec_hi,
		uint32_t tv_sec_lo, uint32_t tv_nsec) {
	struct wsc_capture *cap = data;
	uint64_t sec;
	(void)frame;

	sec = ((uint64_t)tv_sec_hi << 32) | (uint64_t)tv_sec_lo;
	cap->pts_us = sec * 1000000ull + (uint64_t)tv_nsec / 1000ull;
	cap->ready = true;
}

static void frame_handle_failed(void *data,
		struct zwlr_screencopy_frame_v1 *frame) {
	struct wsc_capture *cap = data;
	(void)frame;

	cap->failed = true;
}

static void frame_handle_damage(void *data,
		struct zwlr_screencopy_frame_v1 *frame, uint32_t x, uint32_t y,
		uint32_t width, uint32_t height) {
	struct wsc_capture *cap = data;
	int32_t x2, y2;
	(void)frame;

	if (!cap->has_damage) {
		cap->has_damage = true;
		cap->damage_x = (int32_t)x;
		cap->damage_y = (int32_t)y;
		cap->damage_width = (int32_t)width;
		cap->damage_height = (int32_t)height;
		return;
	}

	x2 = cap->damage_x + cap->damage_width;
	y2 = cap->damage_y + cap->damage_height;
	if ((int32_t)x < cap->damage_x) cap->damage_x = (int32_t)x;
	if ((int32_t)y < cap->damage_y) cap->damage_y = (int32_t)y;
	if ((int32_t)(x + width) > x2) x2 = (int32_t)(x + width);
	if ((int32_t)(y + height) > y2) y2 = (int32_t)(y + height);
	cap->damage_width = x2 - cap->damage_x;
	cap->damage_height = y2 - cap->damage_y;
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
	.buffer = frame_handle_buffer,
	.flags = frame_handle_flags,
	.ready = frame_handle_ready,
	.failed = frame_handle_failed,
	.damage = frame_handle_damage,
	.linux_dmabuf = frame_handle_linux_dmabuf,
	.buffer_done = frame_handle_buffer_done,
};

/* ---------------------------------------------------------- shm buffers */

static struct wsc_buffer *ensure_pool(wsc_session *session, int32_t width,
		int32_t height, int32_t stride, uint32_t format,
		wsc_status *out_status) {
	struct wsc_shm *pool = session->pool;
	struct wsc_buffer *buf;

	if (pool && pool->width == width && pool->height == height &&
			pool->stride == stride && pool->format == format &&
			pool->count == session->opts.buffer_count) {
		goto out;
	}

	if (pool) {
		wsc_shm_destroy(pool);
		session->pool = NULL;
	}

	pool = wsc_shm_create(session->shm, width, height, stride, format,
		session->opts.buffer_count, out_status);
	if (!pool) {
		return NULL;
	}
	session->pool = pool;

out:
	buf = &pool->bufs[pool->next];
	pool->next = (pool->next + 1) % pool->count;
	if (out_status) {
		*out_status = WSC_OK;
	}
	return buf;
}

/* Rotating, library-owned destination buffers for wsc_grab(). */
static uint8_t *ensure_staging(wsc_session *session, int32_t width,
		int32_t height, int bpp, int slots) {
	size_t need = (size_t)width * (size_t)height * (size_t)bpp;

	if (session->staging_size < need || session->staging_slots < slots) {
		for (int i = 0; i < WSC_MAX_STAGING; i++) {
			free(session->staging[i]);
			session->staging[i] = NULL;
		}
		session->staging_size = need;
		session->staging_slots = slots;
		session->staging_next = 0;
		for (int i = 0; i < slots; i++) {
			session->staging[i] = calloc(1, need);
			if (!session->staging[i]) {
				return NULL;
			}
		}
	}

	session->staging_next = (session->staging_next + 1) % session->staging_slots;
	return session->staging[session->staging_next];
}

/* ------------------------------------------------------------- pumping */

static int clamp_timeout_ms(int32_t timeout_ms) {
	return timeout_ms > 0 ? timeout_ms : WSC_DEFAULT_TIMEOUT_MS;
}

/* One poll + dispatch, bounded by the absolute deadline (0 = no deadline). */
static wsc_status pump(wsc_session *session, uint64_t deadline_us) {
	int remaining = -1;

	if (deadline_us) {
		uint64_t now = wsc__now_us();
		if (now >= deadline_us) {
			return WSC_ERROR_TIMEOUT;
		}
		remaining = (int)((deadline_us - now + 999) / 1000ull);
	}
	return wsc_session_poll(session, remaining);
}

/* Pump events until `pred` holds, or report `timeout_msg` once the deadline
 * has passed. Other pump failures (e.g. a dead connection) propagate. */
#define PUMP_UNTIL(pred, timeout_msg) \
	do { \
		while (!(pred)) { \
			wsc_status _st = pump(session, deadline); \
			if (_st == WSC_ERROR_TIMEOUT) { \
				wsc__set_error(session, "%s", (timeout_msg)); \
				st = WSC_ERROR_TIMEOUT; \
				goto out; \
			} \
			if (_st != WSC_OK) { \
				st = _st; \
				goto out; \
			} \
		} \
	} while (0)

static wsc_status pick_output(wsc_session *session,
		const wsc_grab_options *opts, struct wsc_output **out_output,
		int *out_index) {
	struct wsc_output *o;
	int index = opts->output < 0 ? 0 : (int)opts->output;

	o = wsc__output_at(session, index);
	if (!o) {
		wsc__set_error(session,
			"cannot capture output %d: the compositor currently exposes %d "
			"output(s). Re-run output discovery after a hotplug, or pass "
			"output=-1 to use the first one.", index,
			wsc_output_count(session));
		return WSC_ERROR_INVALID;
	}
	if (o->width <= 0 || o->height <= 0) {
		wsc__set_error(session,
			"output %d reports no mode yet (disconnected or unconfigured)",
			index);
		return WSC_ERROR_FAILED;
	}
	*out_output = o;
	*out_index = index;
	return WSC_OK;
}

/*
 * Drive one capture up to (and optionally including) the pixel copy.
 *
 * On success the in-flight frame is left in session->cap with cap->buffer
 * pointing at raw compositor pixels when want_copy is true; the caller owns
 * it until it calls wsc__capture_abort().
 */
static wsc_status run_capture(wsc_session *session,
		const wsc_grab_options *opts, struct wsc_output *output,
		bool want_copy, wsc_status *out_status) {
	struct wsc_capture *cap = &session->cap;
	uint64_t deadline;
	wsc_status st = WSC_OK;

	capture_reset(cap);

	if (opts->x || opts->y || opts->width || opts->height) {
		if (opts->width <= 0 || opts->height <= 0 || opts->x < 0 ||
				opts->y < 0) {
			wsc__set_error(session,
				"invalid region %d,%d %dx%d: x/y must be >= 0 and width/"
				"height > 0 (or all four zero for the whole output)",
				opts->x, opts->y, opts->width, opts->height);
			*out_status = WSC_ERROR_INVALID;
			return WSC_ERROR_INVALID;
		}
		cap->frame = zwlr_screencopy_manager_v1_capture_output_region(
			session->screencopy, opts->cursor ? 1 : 0,
			output->wl_output, opts->x, opts->y, opts->width,
			opts->height);
	} else {
		cap->frame = zwlr_screencopy_manager_v1_capture_output(
			session->screencopy, opts->cursor ? 1 : 0,
			output->wl_output);
	}

	if (!cap->frame) {
		wsc__set_error(session, "could not create a screencopy frame: %s",
			strerror(errno));
		*out_status = WSC_ERROR_NO_MEMORY;
		return WSC_ERROR_NO_MEMORY;
	}
	zwlr_screencopy_frame_v1_add_listener(cap->frame, &frame_listener, cap);
	deadline = wsc__now_us() +
		(uint64_t)clamp_timeout_ms(opts->timeout_ms) * 1000ull;

	/* Stage 1: the compositor describes the buffers it can write. */
	PUMP_UNTIL(cap->buffer_seen || cap->failed,
		"the compositor never described a buffer for this frame; it may not "
		"implement zwlr_screencopy_frame_v1 correctly");
	if (cap->failed) {
		goto compositor_failed;
	}
	if (!cap->buffer_seen) {
		if (cap->dmabuf_seen) {
			wsc__set_error(session,
				"the compositor offered only linux-dmabuf buffers "
				"(fourcc %.4s) for this output; this build captures "
				"through shared memory, which every wlroots "
				"compositor also supports for the same output",
				(const char *)&cap->dmabuf_format);
		} else {
			wsc__set_error(session,
				"the compositor sent no buffer description for this "
				"frame (bound protocol v%u); it may be incompatible",
				session->wlr_version);
		}
		st = WSC_ERROR_UNSUPPORTED;
		goto out;
	}
	if (session->wlr_version >= 3) {
		PUMP_UNTIL(cap->buffer_done || cap->failed,
			"the compositor never sent buffer_done, which "
			"zwlr_screencopy_frame_v1 v3 requires");
		if (cap->failed) {
			goto compositor_failed;
		}
	}

	if (!want_copy) {
		/* The geometry is all a probe asks for. */
		*out_status = WSC_OK;
		return WSC_OK;
	}

	/* Stage 2: hand the compositor a matching shm buffer. */
	if (wsc__shm_format_bpp(cap->shm_format) < 0) {
		wsc__set_error(session,
			"compositor offered shm format %s, which this library does "
			"not know how to read", wsc__shm_format_str(cap->shm_format));
		st = WSC_ERROR_UNSUPPORTED;
		goto out;
	}
	{
		struct wsc_buffer *buf = ensure_pool(session, cap->width,
			cap->height, cap->stride, cap->shm_format, &st);
		if (!buf) {
			goto out;
		}
		cap->buffer = buf;

		if (opts->wait_for_damage && session->wlr_version >= 2) {
			zwlr_screencopy_frame_v1_copy_with_damage(cap->frame,
				buf->wl_buffer);
		} else {
			zwlr_screencopy_frame_v1_copy(cap->frame, buf->wl_buffer);
		}
		cap->copy_sent = true;
	}

	if (wl_display_flush(session->display) < 0 && errno != EAGAIN) {
		wsc__set_error(session, "wl_display_flush failed: %s",
			strerror(errno));
		st = WSC_ERROR_DISCONNECTED;
		goto out;
	}

	/* Stage 3: wait for ready/failed. */
	PUMP_UNTIL(cap->ready || cap->failed,
		"the compositor did not deliver a frame before the deadline");
	if (cap->failed) {
		goto compositor_failed;
	}
	if (!cap->ready) {
		wsc__set_error(session,
			"timed out after %d ms waiting for the compositor to deliver a "
			"frame of output \"%s\". The output may be powered off; with "
			"wait_for_damage the compositor only answers once the screen "
			"changes.", clamp_timeout_ms(opts->timeout_ms),
			output->xdg_name[0] ? output->xdg_name : "?");
		st = WSC_ERROR_TIMEOUT;
		goto out;
	}

	*out_status = WSC_OK;
	return WSC_OK;

compositor_failed:
	wsc__set_error(session,
		"the compositor refused this frame (zwlr_screencopy_frame.failed): "
		"the output was disabled or unplugged, the buffer was rejected, or "
		"the compositor could not read the screen. Details: %s",
		wsc__last_log()[0] ? wsc__last_log() : "(none reported)");
	st = WSC_ERROR_FAILED;
out:
	*out_status = st;
	return st;
}

/* Fill a wsc_frame description for the caller. */
static void fill_frame(wsc_frame *out, wsc_session *session,
		const wsc_grab_options *opts, struct wsc_output *output,
		int output_index, uint8_t *data, int32_t stride, wsc_pixfmt pixfmt,
		int bpp) {
	struct wsc_capture *cap = &session->cap;
	double scale = output->scale > 0 ? (double)output->scale : 1.0;

	memset(out, 0, sizeof(*out));
	out->data = data;
	out->width = cap->width;
	out->height = cap->height;
	out->stride = stride;
	out->pixfmt = pixfmt;
	out->bpp = bpp;
	out->output = output_index;
	out->offset_x = (int32_t)(opts->x * scale);
	out->offset_y = (int32_t)(opts->y * scale);
	out->scale = scale;
	out->compositor_flags = cap->flags;
	out->y_inverted =
		(cap->flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT) != 0;
	out->pts_us = cap->pts_us;
	out->has_damage = cap->has_damage;
	out->damage_x = cap->damage_x;
	out->damage_y = cap->damage_y;
	out->damage_width = cap->damage_width;
	out->damage_height = cap->damage_height;
}

/* Core shared by wsc_grab()/wsc_grab_into(). dst == NULL means "allocate a
 * rotating library-owned buffer". */
static wsc_status grab(wsc_session *session, const wsc_grab_options *options,
		uint8_t *dst, int32_t dst_stride, wsc_frame *out_frame) {
	wsc_grab_options opts;
	struct wsc_output *output = NULL;
	struct wsc_capture *cap;
	wsc_status st = WSC_OK, cap_st = WSC_OK;
	int output_index = 0, bpp;
	uint64_t t0, t1, t2;
	bool borrowed = dst == NULL;

	if (!session) {
		return WSC_ERROR_INVALID;
	}
	opts = options ? *options : (wsc_grab_options)WSC_GRAB_OPTIONS_INIT;
	wsc__clear_error(session);

	if (!(session->backends & WSC_BACKEND_WLR_SCREENCOPY)) {
		wsc__set_error(session,
			"a capture was requested but the compositor does not "
			"advertise zwlr_screencopy_manager_v1");
		return WSC_ERROR_UNSUPPORTED;
	}
	if (session->cap.frame) {
		wsc__set_error(session,
			"a capture is already in flight on this session; a Wayland "
			"connection must be driven by one thread at a time");
		return WSC_ERROR_BUSY;
	}
	bpp = wsc_pixfmt_bpp(opts.pixfmt);
	if (bpp <= 0) {
		wsc__set_error(session, "invalid destination pixel format %d",
			(int)opts.pixfmt);
		return WSC_ERROR_INVALID;
	}

	st = pick_output(session, &opts, &output, &output_index);
	if (st != WSC_OK) {
		return st;
	}

	t0 = wsc__now_us();
	if (run_capture(session, &opts, output, true, &cap_st) != WSC_OK) {
		session->stats.frames_failed++;
		if (cap_st == WSC_ERROR_TIMEOUT) {
			session->stats.frames_timeout++;
		}
		wsc__capture_abort(session);
		return cap_st;
	}
	cap = &session->cap;
	t1 = wsc__now_us();

	if (borrowed) {
		dst = ensure_staging(session, cap->width, cap->height, bpp,
			session->opts.buffer_count);
		if (!dst) {
			wsc__set_error(session,
				"cannot allocate %d bytes for a %dx%d frame",
				(int)((size_t)cap->width * cap->height * bpp),
				cap->width, cap->height);
			wsc__capture_abort(session);
			return WSC_ERROR_NO_MEMORY;
		}
		dst_stride = 0; /* packed */
	}

	if (dst_stride == 0) {
		dst_stride = cap->width * bpp;
	} else if (dst_stride < cap->width * bpp) {
		wsc__set_error(session,
			"destination stride %d is too small for a %d px wide %s frame "
			"(needs at least %d bytes per row)", dst_stride, cap->width,
			wsc_pixfmt_str(opts.pixfmt), cap->width * bpp);
		wsc__capture_abort(session);
		return WSC_ERROR_INVALID;
	}

	st = wsc__convert(cap->buffer->data, cap->stride, cap->shm_format,
		cap->width, cap->height,
		(cap->flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT) != 0,
		dst, dst_stride, opts.pixfmt);
	if (st != WSC_OK) {
		wsc__set_error(session,
			"cannot convert the compositor buffer (%s) to %s",
			wsc__shm_format_str(cap->shm_format),
			wsc_pixfmt_str(opts.pixfmt));
		wsc__capture_abort(session);
		return st;
	}
	t2 = wsc__now_us();

	if (out_frame) {
		fill_frame(out_frame, session, &opts, output, output_index, dst,
			dst_stride, opts.pixfmt, bpp);
	}

	session->stats.frames_ok++;
	session->stats.bytes_captured +=
		(uint64_t)cap->width * (uint64_t)cap->height * (uint64_t)bpp;
	session->stats.last_wait_us = t1 - t0;
	session->stats.last_convert_us = t2 - t1;
	if (t1 - t0 > session->stats.max_wait_us) {
		session->stats.max_wait_us = t1 - t0;
	}
	session->frame_seq++;

	wsc__capture_abort(session);
	return WSC_OK;
}

wsc_status wsc_grab_into(wsc_session *session, const wsc_grab_options *options,
		uint8_t *dst, int32_t dst_stride, wsc_frame *out_frame) {
	return grab(session, options, dst, dst_stride, out_frame);
}

wsc_status wsc_grab(wsc_session *session, const wsc_grab_options *options,
		wsc_frame *out_frame) {
	return grab(session, options, NULL, 0, out_frame);
}

wsc_status wsc_capture_screen(wsc_session *session, wsc_frame *out_frame) {
	wsc_grab_options opts = WSC_GRAB_OPTIONS_INIT;

	opts.pixfmt = WSC_PIX_RGB24;
	return wsc_grab(session, &opts, out_frame);
}

wsc_status wsc_probe(wsc_session *session, const wsc_grab_options *options,
		wsc_frame *out_frame) {
	wsc_grab_options opts;
	struct wsc_output *output = NULL;
	struct wsc_capture probe;
	wsc_status st = WSC_OK, cap_st = WSC_OK;
	int output_index = 0, bpp;

	if (!session || !out_frame) {
		return WSC_ERROR_INVALID;
	}
	opts = options ? *options : (wsc_grab_options)WSC_GRAB_OPTIONS_INIT;
	wsc__clear_error(session);

	if (!(session->backends & WSC_BACKEND_WLR_SCREENCOPY)) {
		wsc__set_error(session,
			"a probe was requested but the compositor does not advertise "
			"zwlr_screencopy_manager_v1");
		return WSC_ERROR_UNSUPPORTED;
	}
	if (session->cap.frame) {
		return WSC_ERROR_BUSY;
	}

	st = pick_output(session, &opts, &output, &output_index);
	if (st != WSC_OK) {
		return st;
	}

	st = run_capture(session, &opts, output, false, &cap_st);
	probe = session->cap;
	wsc__capture_abort(session);
	if (st != WSC_OK) {
		return cap_st;
	}

	bpp = wsc__shm_format_bpp(probe.shm_format);
	memset(out_frame, 0, sizeof(*out_frame));
	out_frame->data = NULL;
	out_frame->width = probe.width;
	out_frame->height = probe.height;
	out_frame->stride = probe.stride;
	out_frame->bpp = bpp > 0 ? bpp : 0;
	out_frame->output = output_index;
	out_frame->scale = output->scale > 0 ? (double)output->scale : 1.0;
	return WSC_OK;
}
