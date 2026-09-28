/* SPDX-License-Identifier: MIT */

#include <inttypes.h>

#include <wayland-client-protocol.h>

#include "convert.h"
#include "internal.h"

/* ------------------------------------------------------------------ meta */

int wsc_pixfmt_bpp(wsc_pixfmt fmt) {
	switch (fmt) {
	case WSC_PIX_RGB24:
	case WSC_PIX_BGR24:
		return 3;
	case WSC_PIX_RGBA:
	case WSC_PIX_BGRA:
	case WSC_PIX_RGBX:
	case WSC_PIX_BGRX:
		return 4;
	default:
		return -1;
	}
}

const char *wsc_pixfmt_str(wsc_pixfmt fmt) {
	switch (fmt) {
	case WSC_PIX_RGB24:
		return "rgb24";
	case WSC_PIX_RGBA:
		return "rgba";
	case WSC_PIX_BGR24:
		return "bgr24";
	case WSC_PIX_BGRA:
		return "bgra";
	case WSC_PIX_RGBX:
		return "rgbx";
	case WSC_PIX_BGRX:
		return "bgrx";
	default:
		return "unknown";
	}
}

/* ------------------------------------------------------- source layouts */

enum src_kind {
	KIND_UNSUPPORTED = 0,
	KIND_32, /* four bytes/pixel, channels at byte offsets */
	KIND_24, /* three bytes/pixel, channels at byte offsets */
	KIND_RGB565,
	KIND_BGR565,
	KIND_ARGB1555,
	KIND_XRGB1555,
	KIND_ARGB4444,
	KIND_XRGB4444,
	KIND_RGB10, /* 10-bit in a 32-bit word: xrgb2101010/argb2101010 */
	KIND_BGR10, /* xbgr2101010/abgr2101010 */
};

struct src_layout {
	enum src_kind kind;
	int bpp;
	int r, g, b, a; /* byte offsets, KIND_32 / KIND_24 only; -1 = absent */
	bool has_alpha; /* for the packed (non byte addressed) formats */
};

static bool src_layout_for(uint32_t shm_format, struct src_layout *out) {
	struct src_layout l = {0};

	switch (shm_format) {
	case WL_SHM_FORMAT_XRGB8888:
	case WL_SHM_FORMAT_ARGB8888:
		/* little endian: memory order is B, G, R, A */
		l.kind = KIND_32; l.bpp = 4; l.r = 2; l.g = 1; l.b = 0;
		l.a = shm_format == WL_SHM_FORMAT_ARGB8888 ? 3 : -1;
		break;
	case WL_SHM_FORMAT_XBGR8888:
	case WL_SHM_FORMAT_ABGR8888:
		/* little endian: memory order is R, G, B, A */
		l.kind = KIND_32; l.bpp = 4; l.r = 0; l.g = 1; l.b = 2;
		l.a = shm_format == WL_SHM_FORMAT_ABGR8888 ? 3 : -1;
		break;
	case WL_SHM_FORMAT_RGB888:
		l.kind = KIND_24; l.bpp = 3; l.r = 0; l.g = 1; l.b = 2; l.a = -1;
		break;
	case WL_SHM_FORMAT_BGR888:
		l.kind = KIND_24; l.bpp = 3; l.r = 2; l.g = 1; l.b = 0; l.a = -1;
		break;
	case WL_SHM_FORMAT_RGB565:
		l.kind = KIND_RGB565; l.bpp = 2; l.r = l.g = l.b = l.a = -1;
		break;
	case WL_SHM_FORMAT_BGR565:
		l.kind = KIND_BGR565; l.bpp = 2; l.r = l.g = l.b = l.a = -1;
		break;
	case WL_SHM_FORMAT_ARGB1555:
		l.kind = KIND_ARGB1555; l.bpp = 2; l.r = l.g = l.b = l.a = -1;
		break;
	case WL_SHM_FORMAT_XRGB1555:
		l.kind = KIND_XRGB1555; l.bpp = 2; l.r = l.g = l.b = l.a = -1;
		break;
	case WL_SHM_FORMAT_XRGB2101010:
		l.kind = KIND_RGB10; l.bpp = 4; l.r = l.g = l.b = l.a = -1;
		l.has_alpha = false;
		break;
	case WL_SHM_FORMAT_ARGB2101010:
		l.kind = KIND_RGB10; l.bpp = 4; l.r = l.g = l.b = l.a = -1;
		l.has_alpha = true;
		break;
	case WL_SHM_FORMAT_XBGR2101010:
		l.kind = KIND_BGR10; l.bpp = 4; l.r = l.g = l.b = l.a = -1;
		l.has_alpha = false;
		break;
	case WL_SHM_FORMAT_ABGR2101010:
		l.kind = KIND_BGR10; l.bpp = 4; l.r = l.g = l.b = l.a = -1;
		l.has_alpha = true;
		break;
	case WL_SHM_FORMAT_ARGB4444:
		l.kind = KIND_ARGB4444; l.bpp = 2; l.r = l.g = l.b = l.a = -1;
		break;
	case WL_SHM_FORMAT_XRGB4444:
		l.kind = KIND_XRGB4444; l.bpp = 2; l.r = l.g = l.b = l.a = -1;
		break;
	default:
		return false;
	}
	*out = l;
	return true;
}

int wsc__shm_format_bpp(uint32_t shm_format) {
	struct src_layout l = {0};
	return src_layout_for(shm_format, &l) ? l.bpp : -1;
}

const char *wsc__shm_format_str(uint32_t f) {
	switch (f) {
	case WL_SHM_FORMAT_ARGB8888: return "argb8888";
	case WL_SHM_FORMAT_XRGB8888: return "xrgb8888";
	case WL_SHM_FORMAT_ABGR8888: return "abgr8888";
	case WL_SHM_FORMAT_XBGR8888: return "xbgr8888";
	case WL_SHM_FORMAT_RGB888: return "rgb888";
	case WL_SHM_FORMAT_BGR888: return "bgr888";
	case WL_SHM_FORMAT_RGB565: return "rgb565";
	case WL_SHM_FORMAT_BGR565: return "bgr565";
	case WL_SHM_FORMAT_ARGB1555: return "argb1555";
	case WL_SHM_FORMAT_XRGB1555: return "xrgb1555";
	case WL_SHM_FORMAT_XRGB2101010: return "xrgb2101010";
	case WL_SHM_FORMAT_ARGB2101010: return "argb2101010";
	case WL_SHM_FORMAT_XBGR2101010: return "xbgr2101010";
	case WL_SHM_FORMAT_ABGR2101010: return "abgr2101010";
	case WL_SHM_FORMAT_ARGB4444: return "argb4444";
	case WL_SHM_FORMAT_XRGB4444: return "xrgb4444";
	default:
		break;
	}
	static _Thread_local char buf[32];
	snprintf(buf, sizeof(buf), "fourcc:%c%c%c%c", (int)(f & 0xff),
		(int)((f >> 8) & 0xff), (int)((f >> 16) & 0xff),
		(int)((f >> 24) & 0xff));
	return buf;
}

/* ---------------------------------------------------------- destinations */

/* Where each channel lands inside a destination pixel; -1 = not stored. */
static void dst_offsets(wsc_pixfmt fmt, int *r, int *g, int *b, int *a) {
	switch (fmt) {
	case WSC_PIX_RGB24: *r = 0; *g = 1; *b = 2; *a = -1; break;
	case WSC_PIX_BGR24: *r = 2; *g = 1; *b = 0; *a = -1; break;
	case WSC_PIX_RGBA:  *r = 0; *g = 1; *b = 2; *a = 3; break;
	/* BGRA / BGRX are named by memory order: byte 0 is blue. */
	case WSC_PIX_BGRA:  *r = 2; *g = 1; *b = 0; *a = 3; break;
	/* The padding byte of RGBX/BGRX is left untouched on purpose. */
	case WSC_PIX_RGBX:  *r = 0; *g = 1; *b = 2; *a = -1; break;
	case WSC_PIX_BGRX:  *r = 2; *g = 1; *b = 0; *a = -1; break;
	default:            *r = *g = *b = *a = -1; break;
	}
}

/* n-bit channel expansions to 8 bits */
static inline uint8_t expand4(uint8_t v) { return (uint8_t)(v | (v << 4)); }
static inline uint8_t expand5(uint8_t v) { return (uint8_t)((v << 3) | (v >> 2)); }
static inline uint8_t expand6(uint8_t v) { return (uint8_t)((v << 2) | (v >> 4)); }

/* Read one source pixel into r/g/b/a (a = 255 when the source has none). */
static inline bool read_pixel(const uint8_t *p, const struct src_layout *sl,
		uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a) {
	uint16_t v;

	switch (sl->kind) {
	case KIND_32:
		*r = p[sl->r]; *g = p[sl->g]; *b = p[sl->b];
		*a = sl->a >= 0 ? p[sl->a] : 255;
		return true;
	case KIND_24:
		*r = p[sl->r]; *g = p[sl->g]; *b = p[sl->b]; *a = 255;
		return true;
	case KIND_RGB565:
		memcpy(&v, p, sizeof(v));
		*r = expand5((uint8_t)((v >> 11) & 0x1f));
		*g = expand6((uint8_t)((v >> 5) & 0x3f));
		*b = expand5((uint8_t)(v & 0x1f));
		*a = 255;
		return true;
	case KIND_BGR565:
		memcpy(&v, p, sizeof(v));
		*b = expand5((uint8_t)((v >> 11) & 0x1f));
		*g = expand6((uint8_t)((v >> 5) & 0x3f));
		*r = expand5((uint8_t)(v & 0x1f));
		*a = 255;
		return true;
	case KIND_XRGB1555:
		memcpy(&v, p, sizeof(v));
		*r = expand5((uint8_t)((v >> 10) & 0x1f));
		*g = expand5((uint8_t)((v >> 5) & 0x1f));
		*b = expand5((uint8_t)(v & 0x1f));
		*a = 255;
		return true;
	case KIND_RGB10:
	case KIND_BGR10: {
		/* little endian 10-bit fields, b/g/r (RGB10) or r/g/b (BGR10) */
		uint32_t w;
		memcpy(&w, p, sizeof(w));
		if (sl->kind == KIND_RGB10) {
			*r = (uint8_t)(((w >> 20) & 0x3ff) >> 2);
			*g = (uint8_t)(((w >> 10) & 0x3ff) >> 2);
			*b = (uint8_t)((w & 0x3ff) >> 2);
		} else {
			*r = (uint8_t)((w & 0x3ff) >> 2);
			*g = (uint8_t)(((w >> 10) & 0x3ff) >> 2);
			*b = (uint8_t)(((w >> 20) & 0x3ff) >> 2);
		}
		/* argb2101010 carries a 2 bit alpha in the top bits. */
		*a = sl->has_alpha ?
			(uint8_t)((((w >> 30) & 0x3u) * 255u + 1u) / 3u) : 255;
		return true;
	}
	case KIND_ARGB1555:
		memcpy(&v, p, sizeof(v));
		*a = (v & 0x8000) ? 255 : 0;
		*r = expand5((uint8_t)((v >> 10) & 0x1f));
		*g = expand5((uint8_t)((v >> 5) & 0x1f));
		*b = expand5((uint8_t)(v & 0x1f));
		return true;
	case KIND_ARGB4444:
		memcpy(&v, p, sizeof(v));
		*a = expand4((uint8_t)((v >> 12) & 0xf));
		*r = expand4((uint8_t)((v >> 8) & 0xf));
		*g = expand4((uint8_t)((v >> 4) & 0xf));
		*b = expand4((uint8_t)(v & 0xf));
		return true;
	case KIND_XRGB4444:
		memcpy(&v, p, sizeof(v));
		*r = expand4((uint8_t)((v >> 8) & 0xf));
		*g = expand4((uint8_t)((v >> 4) & 0xf));
		*b = expand4((uint8_t)(v & 0xf));
		*a = 255;
		return true;
	default:
		return false;
	}
}

/* ------------------------------------------------------------- convert */

wsc_status wsc__convert(const uint8_t *src, int32_t src_stride,
		uint32_t src_format, int32_t width, int32_t height, bool y_invert,
		uint8_t *dst, int32_t dst_stride, wsc_pixfmt dst_pixfmt) {
	struct src_layout sl;
	int dr, dg, db, da, dbpp;

	if (width <= 0 || height <= 0 || !src || !dst) {
		return WSC_ERROR_INVALID;
	}
	if (!src_layout_for(src_format, &sl)) {
		return WSC_ERROR_UNSUPPORTED;
	}
	dbpp = wsc_pixfmt_bpp(dst_pixfmt);
	if (dbpp <= 0) {
		return WSC_ERROR_UNSUPPORTED;
	}
	dst_offsets(dst_pixfmt, &dr, &dg, &db, &da);
	if (dst_stride == 0) {
		dst_stride = width * dbpp;
	}
	if (src_stride == 0) {
		src_stride = width * sl.bpp;
	}

	/*
	 * Fast path: 32-bit source into a 32-bit destination. Every wlroots
	 * compositor hands back XRGB8888 or ARGB8888, and RGBA/RGBX is a common
	 * destination, so this is worth writing as a pure byte permutation.
	 */
	if (sl.kind == KIND_32 && dbpp == 4) {
		for (int32_t y = 0; y < height; y++) {
			const int32_t sy = y_invert ? (height - 1 - y) : y;
			const uint8_t *s = src + (ptrdiff_t)sy * src_stride;
			uint8_t *d = dst + (ptrdiff_t)y * dst_stride;
			for (int32_t x = 0; x < width; x++) {
				uint8_t in[4], out[4] = { 0, 0, 0, 255 };
				memcpy(in, s + (size_t)x * 4, 4);
				out[dr] = in[sl.r];
				out[dg] = in[sl.g];
				out[db] = in[sl.b];
				if (da >= 0) {
					/* Sources without alpha (xrgb8888 - what compositors
					 * usually hand back) mean fully opaque, *not* whatever
					 * happens to sit in the padding byte. */
					out[da] = sl.a >= 0 ? in[sl.a] : 255;
				}
				memcpy(d + (size_t)x * 4, out, 4);
			}
		}
		return WSC_OK;
	}

	for (int32_t y = 0; y < height; y++) {
		const int32_t sy = y_invert ? (height - 1 - y) : y;
		const uint8_t *s = src + (ptrdiff_t)sy * src_stride;
		uint8_t *d = dst + (ptrdiff_t)y * dst_stride;

		/* Second fast path: 32-bit source, 24-bit destination. */
		if (sl.kind == KIND_32) {
			for (int32_t x = 0; x < width; x++) {
				const uint8_t *p = s + (size_t)x * 4;
				d[x * 3 + dr] = p[sl.r];
				d[x * 3 + dg] = p[sl.g];
				d[x * 3 + db] = p[sl.b];
			}
			continue;
		}

		for (int32_t x = 0; x < width; x++) {
			uint8_t r, g, b, a;
			if (!read_pixel(s + (size_t)x * sl.bpp, &sl, &r, &g, &b, &a)) {
				return WSC_ERROR_UNSUPPORTED;
			}
			if (dr >= 0) d[x * dbpp + dr] = r;
			if (dg >= 0) d[x * dbpp + dg] = g;
			if (db >= 0) d[x * dbpp + db] = b;
			if (da >= 0) d[x * dbpp + da] = a;
		}
	}
	return WSC_OK;
}
