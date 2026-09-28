/* SPDX-License-Identifier: MIT
 *
 * wl_screencopy - screen capture for Wayland clients using the
 *                 wlr-screencopy-unstable-v1 protocol.
 *
 * The library is a small, dependency-light C core that copies compositor
 * output frames into shared memory and hands them out as packed RGB(A)
 * pixels.  It is designed to be driven from a single thread (a Wayland
 * connection is not thread safe) and to be wrapped by higher level
 * language bindings - notably Python + NumPy, see python/wl_screencopy.
 *
 * Copyright (c) 2026 wl_screencopy contributors.
 */

#ifndef WL_SCREENCOPY_H
#define WL_SCREENCOPY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wsc_session wsc_session;

#define WSC_VERSION_MAJOR 0
#define WSC_VERSION_MINOR 1
#define WSC_VERSION_PATCH 0

/* ------------------------------------------------------------------------- */
/* Status codes                                                              */
/* ------------------------------------------------------------------------- */

typedef enum wsc_status {
	WSC_OK = 0,
	/* Generic/unspecified failure. */
	WSC_ERROR = -1,
	/* The compositor does not implement the screencopy protocol. */
	WSC_ERROR_UNSUPPORTED = -2,
	/* Bad arguments (unknown output, empty region, ...). */
	WSC_ERROR_INVALID = -3,
	WSC_ERROR_NO_MEMORY = -4,
	/* No frame arrived within the deadline. */
	WSC_ERROR_TIMEOUT = -5,
	/* The compositor refused the frame (wl_frame.failed, output off, ...). */
	WSC_ERROR_FAILED = -6,
	/* The Wayland connection is gone (compositor exit / client killed). */
	WSC_ERROR_DISCONNECTED = -7,
	/* A protocol error was reported by the compositor. */
	WSC_ERROR_PROTOCOL = -8,
	/* The session is already busy with another operation. */
	WSC_ERROR_BUSY = -9,
} wsc_status;

/** Human readable status, e.g. "timeout". */
const char *wsc_status_str(wsc_status status);

/**
 * Detailed, human readable information about the most recent failure on
 * this session (including compositor-side protocol error text when the
 * compositor provides any). The string is owned by the session and stays
 * valid until the next failing call. Never NULL.
 */
const char *wsc_session_error_string(wsc_session *session);

/* ------------------------------------------------------------------------- */
/* Pixel formats                                                             */
/* ------------------------------------------------------------------------- */

/*
 * Formats are named by their byte order in memory, left to right, which is
 * what NumPy users expect from an (H, W, 3) / (H, W, 4) array.
 */
typedef enum wsc_pixfmt {
	WSC_PIX_RGB24 = 0, /* 3 bytes/pixel, packed  -> ndarray (H, W, 3) */
	WSC_PIX_RGBA = 1,  /* 4 bytes/pixel, packed  -> ndarray (H, W, 4) */
	WSC_PIX_BGR24 = 2, /* -> convenient for OpenCV */
	WSC_PIX_BGRA = 3,
	WSC_PIX_RGBX = 4, /* X = don't care */
	WSC_PIX_BGRX = 5,
	WSC_PIX__COUNT,
} wsc_pixfmt;

/** Bytes per pixel for a format, or -1 for an unknown format. */
int wsc_pixfmt_bpp(wsc_pixfmt fmt);

/** Short name, e.g. "rgb24". */
const char *wsc_pixfmt_str(wsc_pixfmt fmt);

/** Bit set of the capture backends a compositor supports. */
typedef enum wsc_backend_flag {
	WSC_BACKEND_NONE = 0,
	/** zwlr_screencopy_manager_v1 (wlroots: Sway, Hyprland, river, labwc, ...) */
	WSC_BACKEND_WLR_SCREENCOPY = 1u << 0,
	/**
	 * ext_image_copy_capture_manager_v1 (KWin 6.2+, GNOME 47+, wlroots 0.19+).
	 *
	 * Reserved: this release only implements the wlr backend, so a compositor
	 * that offers just this protocol is reported as unsupported. The flag is
	 * part of the ABI so discovery can grow without breaking callers.
	 */
	WSC_BACKEND_EXT_IMAGE_COPY = 1u << 1,
} wsc_backend_flag;

/* ------------------------------------------------------------------------- */
/* Output information                                                        */
/* ------------------------------------------------------------------------- */

#define WSC_OUTPUT_NAME_MAX 64
#define WSC_OUTPUT_DESC_MAX 256

typedef struct wsc_output_info {
	/** Stable wl_output global name (used as identity across refreshes). */
	uint32_t wl_name;
	/** Connector name reported by the compositor, e.g. "DP-1", "WL-1".
	 *  Empty when the compositor does not implement xdg_output. */
	char name[WSC_OUTPUT_NAME_MAX];
	/** "Manufacturer Model" from wl_output, e.g. "Dell Inc. DELL U2720Q". */
	char description[WSC_OUTPUT_DESC_MAX];
	/** Position of the output in the global logical layout, in logical px. */
	int32_t x, y;
	/** Logical size, in logical px (i.e. what a window sees). */
	int32_t width, height;
	/** Physical size in millimetres (0 if unknown). */
	int32_t phys_width_mm, phys_height_mm;
	/** Buffer scale advertised by wl_output (>= 1). Note that
	 *  wp_fractional_scale_v1 describes per-*surface* scale and can only be
	 *  learned for a mapped surface, so it is deliberately not used here:
	 *  a screen recorder has no surface of its own. */
	int32_t scale;
	/** Scale used when converting logical -> device pixels. Currently the
	 *  same value as `scale`, kept separate so a HiDPI caller can rely on
	 *  one field only. */
	double effective_scale;
	/** Vertical refresh rate in milli-Hz (0 if unknown). */
	int32_t refresh;
	/** wl_output transform (0..7). */
	int32_t transform;
	/** True if this is the output a capture defaults to. */
	bool preferred;
} wsc_output_info;

/* ------------------------------------------------------------------------- */
/* Sessions                                                                  */
/* ------------------------------------------------------------------------- */

typedef struct wsc_session_options {
	/** Wayland socket name; NULL to use $WAYLAND_DISPLAY. */
	const char *display_name;
	/**
	 * Number of shared memory frame buffers to rotate between.  > 1 lets
	 * the caller keep reading frame N while frame N+1 is being copied by
	 * the compositor. Defaults to 2 (0 in a zeroed struct means default).
	 */
	int32_t buffer_count;
	/** Ask the compositor to overlay the hardware cursor by default. */
	bool cursor;
} wsc_session_options;

#define WSC_SESSION_OPTIONS_INIT { NULL, 0, false }

/**
 * Open a session. Returns NULL on failure; if out_status is non-NULL it
 * receives the failure reason and wsc_session_error_string() (on the
 * returned session, when there is one) explains why.
 *
 * A session with no usable capture backend is still returned successfully:
 * outputs can be listed, and wsc_grab() will fail with
 * WSC_ERROR_UNSUPPORTED plus an explanatory error string.
 */
wsc_session *wsc_session_open(const wsc_session_options *options,
	wsc_status *out_status);

void wsc_session_close(wsc_session *session);

/** Backends advertised by the compositor, see wsc_backend_flag. */
uint32_t wsc_session_backends(wsc_session *session);

/** Protocol version bound for the wlr screencopy manager (0 if absent). */
uint32_t wsc_session_wlr_version(wsc_session *session);

/** Wayland display fd, for integration with an external event loop. */
int wsc_session_fd(wsc_session *session);

/**
 * Handle pending Wayland events without blocking (timeout_ms < 0 waits).
 * Only needed by callers that drive the session from their own loop.
 */
wsc_status wsc_session_poll(wsc_session *session, int timeout_ms);

/** Number of outputs currently known, after a refresh. */
int wsc_output_count(wsc_session *session);

/**
 * Re-read the global list (outputs come and go at runtime). Returns the
 * new output count or -1 on error.
 */
int wsc_session_refresh(wsc_session *session);

/** Fill info for the output at index idx. */
wsc_status wsc_output_get(wsc_session *session, int idx,
	wsc_output_info *out_info);

/**
 * Look an output up by name. Accepts a decimal index ("0"), an exact
 * connector name ("DP-1", case insensitive) or a case-insensitive
 * substring of the description. Returns the index or -1.
 */
int wsc_output_find(wsc_session *session, const char *name);

/* ------------------------------------------------------------------------- */
/* Frames                                                                    */
/* ------------------------------------------------------------------------- */

typedef struct wsc_grab_options {
	/** Output index, or -1 for the preferred output. */
	int32_t output;
	/**
	 * Region to capture in output-local *logical* pixels. Set all four to
	 * 0 for the whole output. The compositor clips the region and returns
	 * a frame in *device* pixels, i.e. of size region*scale.
	 */
	int32_t x, y, width, height;
	/** Composite the hardware cursor into the frame (NULL opts: session default). */
	bool cursor;
	/**
	 * Only complete when the output content actually changed
	 * (zwlr_screencopy_frame_v1.copy_with_damage, protocol v2+). Saves a
	 * lot of CPU/GPU when recording a mostly idle desktop, but blocks
	 * until there is something new.
	 */
	bool wait_for_damage;
	/** Deadline in milliseconds, 0 for the default (2000 ms). */
	int32_t timeout_ms;
	/** Destination pixel format for wsc_grab_into(). */
	wsc_pixfmt pixfmt;
} wsc_grab_options;

#define WSC_GRAB_OPTIONS_INIT { -1, 0, 0, 0, 0, false, false, 0, WSC_PIX_RGB24 }

typedef struct wsc_frame {
	/** Pixel data. Owned by the library for wsc_grab(), by the caller for
	 *  wsc_grab_into(); valid until the next grab on this session. */
	uint8_t *data;
	/** Frame size in device pixels. */
	int32_t width, height;
	/** Distance between the start of two rows, in bytes. */
	int32_t stride;
	wsc_pixfmt pixfmt;
	int32_t bpp;
	/** Index of the captured output, as in wsc_output_get(). */
	int32_t output;
	/** Top-left corner of the captured region, in device pixels. */
	int32_t offset_x, offset_y;
	/** Scale that was applied to the requested logical region. */
	double scale;
	/** Raw zwlr_screencopy_frame_v1.flags (bit 1 = contents y-inverted).
	 *  The library always hands out top-down images, so this is
	 *  informational: y_inverted is true if the compositor sent it. */
	uint32_t compositor_flags;
	bool y_inverted;
	/** Presentation timestamp of the captured frame, microseconds.
	 *  Origin is compositor defined (monotonic); 0 if not reported. */
	uint64_t pts_us;
	/** Union of the damage rectangles reported for this frame; only filled
	 *  when wsc_grab_options.wait_for_damage was set. */
	bool has_damage;
	int32_t damage_x, damage_y, damage_width, damage_height;
	/** True if the frame was dropped because the compositor reported the
	 *  output as disconnected/failed rather than delivering pixels. */
	bool stale;
} wsc_frame;

#define WSC_FRAME_INIT {0}

/**
 * Capture one frame into a caller-provided buffer - the fast path for
 * language bindings: a NumPy array's data pointer can be passed straight
 * through, so the compositor's pixels are converted into the array in a
 * single pass with no temporary.
 *
 * dst must hold at least dst_stride * height bytes. dst_stride may be 0 to
 * mean width * bpp. Rows are always written top-down regardless of what
 * the compositor sent.
 */
wsc_status wsc_grab_into(wsc_session *session, const wsc_grab_options *options,
	uint8_t *dst, int32_t dst_stride, wsc_frame *out_frame);

/**
 * Capture one frame into a library-owned buffer. The returned frame's
 * `data` pointer stays valid until the next grab on the same session
 * (buffer_count buffers are rotated, see wsc_session_options).
 */
wsc_status wsc_grab(wsc_session *session, const wsc_grab_options *options,
	wsc_frame *out_frame);

/** Convenience: capture the whole preferred output as RGB24. */
wsc_status wsc_capture_screen(wsc_session *session, wsc_frame *out_frame);

/**
 * Ask what size a capture would produce, without copying pixels.
 * Fills width/height/stride/bpp/scale/offset_* of out_frame.
 */
wsc_status wsc_probe(wsc_session *session, const wsc_grab_options *options,
	wsc_frame *out_frame);

/* ------------------------------------------------------------------------- */
/* Statistics (useful to spot a capture loop that cannot keep up)            */
/* ------------------------------------------------------------------------- */

typedef struct wsc_stats {
	uint64_t frames_ok;
	uint64_t frames_failed;
	uint64_t frames_timeout;
	uint64_t bytes_captured;
	/** Rolling timing of the last wsc_grab*() call, microseconds. */
	uint64_t last_wait_us;
	uint64_t last_convert_us;
	uint64_t max_wait_us;
} wsc_stats;

wsc_status wsc_session_get_stats(wsc_session *session, wsc_stats *out_stats);

/** Library version string, e.g. "0.1.0 (wlr-screencopy-unstable-v1 v3)". */
const char *wsc_version_string(void);

/* ------------------------------------------------------------------------- */
/* ABI introspection (used by language bindings to validate their layout)     */
/* ------------------------------------------------------------------------- */

size_t wsc_abi_sizeof_output_info(void);
size_t wsc_abi_sizeof_frame(void);
size_t wsc_abi_sizeof_grab_options(void);
size_t wsc_abi_sizeof_stats(void);
size_t wsc_abi_sizeof_session_options(void);

size_t wsc_abi_frame_offset_data(void);
size_t wsc_abi_frame_offset_width(void);
size_t wsc_abi_frame_offset_height(void);
size_t wsc_abi_frame_offset_stride(void);
size_t wsc_abi_frame_offset_pixfmt(void);
size_t wsc_abi_frame_offset_pts_us(void);

#ifdef __cplusplus
}
#endif

#endif /* WL_SCREENCOPY_H */
