/* SPDX-License-Identifier: MIT
 *
 * Unit tests for the pixel conversion layer. These need no compositor:
 * they feed synthetic wl_shm buffers through wsc__convert() and check the
 * packed RGB(A) output byte by byte.
 *
 *   make unittest
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-client-protocol.h>

#include "convert.h"

static int failures;
static int checks;

#define CHECK(cond, ...) \
	do { \
		checks++; \
		if (!(cond)) { \
			failures++; \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
			fprintf(stderr, __VA_ARGS__); \
			fprintf(stderr, "\n"); \
		} \
	} while (0)

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

/* ------------------------------------------------------------------ 32bit */

static void test_xrgb_to_rgb24(void) {
	/* Two pixels: red then green, with a padded stride. */
	uint8_t src[16] = {0};
	uint8_t dst[6] = {0};
	uint32_t red = 0x00FF0000u;    /* XRGB8888: X,R,G,B in the integer */
	uint32_t green = 0x0000FF00u;

	put32(src + 0, red);
	put32(src + 4, green);

	wsc_status st = wsc__convert(src, 8, WL_SHM_FORMAT_XRGB8888, 2, 1, false,
		dst, 0, WSC_PIX_RGB24);
	CHECK(st == WSC_OK, "xrgb->rgb24 returned %d", st);
	CHECK(dst[0] == 255 && dst[1] == 0 && dst[2] == 0,
		"red pixel became %d,%d,%d", dst[0], dst[1], dst[2]);
	CHECK(dst[3] == 0 && dst[4] == 255 && dst[5] == 0,
		"green pixel became %d,%d,%d", dst[3], dst[4], dst[5]);
}

static void test_all_32bit_formats(void) {
	/* Reference colour: R=0x11 G=0x22 B=0x33 */
	struct { uint32_t fmt; const char *name; uint32_t pixel; } cases[] = {
		{ WL_SHM_FORMAT_XRGB8888, "xrgb8888", 0x00112233u },
		{ WL_SHM_FORMAT_ARGB8888, "argb8888", 0xFF112233u },
		{ WL_SHM_FORMAT_XBGR8888, "xbgr8888", 0x00332211u },
		{ WL_SHM_FORMAT_ABGR8888, "abgr8888", 0xFF332211u },
	};
	struct { wsc_pixfmt pf; const char *name; int r, g, b, a; } outs[] = {
		{ WSC_PIX_RGB24, "rgb24", 0, 1, 2, -1 },
		{ WSC_PIX_RGBA, "rgba", 0, 1, 2, 3 },
		{ WSC_PIX_BGR24, "bgr24", 2, 1, 0, -1 },
		{ WSC_PIX_BGRA, "bgra", 2, 1, 0, 3 },
		{ WSC_PIX_RGBX, "rgbx", 0, 1, 2, -1 },
		{ WSC_PIX_BGRX, "bgrx", 2, 1, 0, -1 },
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		for (size_t j = 0; j < sizeof(outs) / sizeof(outs[0]); j++) {
			uint8_t src[4];
			uint8_t dst[4] = { 0xAA, 0xAA, 0xAA, 0xAA };
			int bpp = wsc_pixfmt_bpp(outs[j].pf);

			put32(src, cases[i].pixel);
			wsc_status st = wsc__convert(src, 4, cases[i].fmt, 1, 1, false,
				dst, 0, outs[j].pf);
			CHECK(st == WSC_OK, "%s -> %s failed (%d)", cases[i].name,
				outs[j].name, st);
			CHECK(dst[outs[j].r] == 0x11 && dst[outs[j].g] == 0x22 &&
				dst[outs[j].b] == 0x33,
				"%s -> %s gave %02x %02x %02x %02x", cases[i].name,
				outs[j].name, dst[0], dst[1], dst[2], dst[3]);
			if (outs[j].a >= 0) {
				/* xrgb has no alpha: destinations must fill 255. */
				int expect_a = (cases[i].fmt == WL_SHM_FORMAT_XRGB8888 ||
					cases[i].fmt == WL_SHM_FORMAT_XBGR8888) ? 255 : 0xFF;
				CHECK(dst[outs[j].a] == (uint8_t)expect_a,
					"%s -> %s alpha %02x (expected %02x)", cases[i].name,
					outs[j].name, dst[outs[j].a], expect_a);
			}
			/* Nothing may be written past the pixel. */
			for (int k = bpp; k < 4; k++) {
				CHECK(dst[k] == 0xAA, "%s -> %s wrote past pixel at %d",
					cases[i].name, outs[j].name, k);
			}
		}
	}
}

/* ------------------------------------------------------------------ rows */

static void test_y_invert_and_stride(void) {
	/* 2x2 image with 8-byte rows: top row red, bottom row blue. */
	uint8_t src[16];
	uint8_t dst[18]; /* two 6-byte rows plus slack to catch overwrites */

	memset(src, 0, sizeof(src));
	put32(src + 0, 0x00FF0000u);   /* x=0, y=0 red  */
	put32(src + 4, 0x00FF0000u);   /* x=1, y=0 red  */
	put32(src + 8, 0x000000FFu);   /* x=0, y=1 blue */
	put32(src + 12, 0x000000FFu);  /* x=1, y=1 blue */

	memset(dst, 0, sizeof(dst));
	wsc_status st = wsc__convert(src, 8, WL_SHM_FORMAT_XRGB8888, 2, 2, false,
		dst, 6, WSC_PIX_RGB24);
	CHECK(st == WSC_OK, "2x2 convert failed (%d)", st);
	CHECK(dst[0] == 255 && dst[1] == 0 && dst[2] == 0, "top row not red");
	CHECK(dst[6] == 0 && dst[7] == 0 && dst[8] == 255, "bottom row not blue");

	memset(dst, 0, sizeof(dst));
	st = wsc__convert(src, 8, WL_SHM_FORMAT_XRGB8888, 2, 2, true,
		dst, 6, WSC_PIX_RGB24);
	CHECK(st == WSC_OK, "y-invert convert failed (%d)", st);
	CHECK(dst[0] == 0 && dst[1] == 0 && dst[2] == 255,
		"with y_invert the first row must be the source bottom row (blue)");
	CHECK(dst[6] == 255 && dst[7] == 0 && dst[8] == 0,
		"with y_invert the last row must be red");
}

static void test_dst_stride_respected(void) {
	const int src_stride = 4 * 4;    /* four 32-bit pixels per row */
	const int dst_stride = 4 * 3 + 3; /* one 4 px row plus 3 pad bytes */
	uint8_t src[4 * 4 * 4];
	uint8_t dst[4 * dst_stride];

	for (int y = 0; y < 4; y++) {
		for (int x = 0; x < 4; x++) {
			put32(src + y * src_stride + x * 4, 0x00102030u);
		}
	}
	memset(dst, 0xEE, sizeof(dst));

	/* 4x4 image into a destination with 3 padding bytes per row. */
	wsc_status st = wsc__convert(src, src_stride, WL_SHM_FORMAT_XRGB8888, 4, 4,
		false, dst, dst_stride, WSC_PIX_RGB24);
	CHECK(st == WSC_OK, "strided convert failed (%d)", st);
	for (int y = 0; y < 4; y++) {
		const uint8_t *row = dst + y * dst_stride;
		/* XRGB8888 0x00102030 is R=0x10 G=0x20 B=0x30. */
		CHECK(row[0] == 0x10 && row[1] == 0x20 && row[2] == 0x30,
			"row %d first pixel wrong: %02x %02x %02x", y, row[0], row[1],
			row[2]);
		CHECK(row[12] == 0xEE && row[13] == 0xEE && row[14] == 0xEE,
			"row %d padding was written (%02x %02x %02x)", y, row[12],
			row[13], row[14]);
	}
}

/* --------------------------------------------------------- 24 and 16 bit */

static void test_24bit(void) {
	uint8_t src[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
	uint8_t dst[6] = {0};

	wsc_status st = wsc__convert(src, 6, WL_SHM_FORMAT_RGB888, 2, 1, false,
		dst, 0, WSC_PIX_RGB24);
	CHECK(st == WSC_OK, "rgb888 passthrough failed (%d)", st);
	CHECK(memcmp(dst, src, 6) == 0, "rgb888 changed bytes");

	memset(dst, 0, sizeof(dst));
	st = wsc__convert(src, 6, WL_SHM_FORMAT_BGR888, 2, 1, false, dst, 0,
		WSC_PIX_RGB24);
	CHECK(st == WSC_OK, "bgr888 convert failed (%d)", st);
	CHECK(dst[0] == 0x33 && dst[1] == 0x22 && dst[2] == 0x11,
		"bgr888 -> rgb24 gave %02x %02x %02x", dst[0], dst[1], dst[2]);
}

static void test_16bit(void) {
	uint8_t src[2];
	uint8_t dst[3];

	/* rgb565: R=11111 G=111111 B=11111 -> white */
	uint16_t white = 0xFFFF;
	memcpy(src, &white, 2);
	wsc_status st = wsc__convert(src, 2, WL_SHM_FORMAT_RGB565, 1, 1, false,
		dst, 0, WSC_PIX_RGB24);
	CHECK(st == WSC_OK, "rgb565 failed (%d)", st);
	CHECK(dst[0] == 255 && dst[1] == 255 && dst[2] == 255,
		"rgb565 white -> %d %d %d", dst[0], dst[1], dst[2]);

	/* pure red in rgb565 */
	uint16_t red = 0xF800;
	memcpy(src, &red, 2);
	st = wsc__convert(src, 2, WL_SHM_FORMAT_RGB565, 1, 1, false, dst, 0,
		WSC_PIX_RGB24);
	CHECK(st == WSC_OK, "rgb565 red failed (%d)", st);
	CHECK(dst[0] == 255 && dst[1] == 0 && dst[2] == 0,
		"rgb565 red -> %d %d %d", dst[0], dst[1], dst[2]);

	/* argb4444: A=F R=1 G=2 B=3 */
	uint16_t v = 0xF123;
	memcpy(src, &v, 2);
	uint8_t dst4[4] = {0};
	st = wsc__convert(src, 2, WL_SHM_FORMAT_ARGB4444, 1, 1, false, dst4, 0,
		WSC_PIX_RGBA);
	CHECK(st == WSC_OK, "argb4444 failed (%d)", st);
	CHECK(dst4[0] == 0x11 && dst4[1] == 0x22 && dst4[2] == 0x33 &&
		dst4[3] == 0xFF, "argb4444 -> %02x %02x %02x %02x", dst4[0],
		dst4[1], dst4[2], dst4[3]);
}

static void test_10bit(void) {
	/* xrgb2101010: 10-bit channels collapse to the top 8 bits. */
	uint8_t src[4];
	uint8_t dst[3];
	uint32_t full = 0x3FFFFFFFu; /* R=G=B=0x3FF */

	memcpy(src, &full, 4);
	wsc_status st = wsc__convert(src, 4, WL_SHM_FORMAT_XRGB2101010, 1, 1,
		false, dst, 0, WSC_PIX_RGB24);
	CHECK(st == WSC_OK, "xrgb2101010 failed (%d)", st);
	CHECK(dst[0] == 255 && dst[1] == 255 && dst[2] == 255,
		"10-bit white -> %d %d %d", dst[0], dst[1], dst[2]);
}

/* ------------------------------------------------------------- failures */

static void test_errors(void) {
	uint8_t src[4] = {0};
	uint8_t dst[4] = {0};

	wsc_status st = wsc__convert(src, 4, WL_SHM_FORMAT_AYUV, 1, 1, false, dst,
		0, WSC_PIX_RGB24);
	CHECK(st == WSC_ERROR_UNSUPPORTED, "AYUV should be unsupported, got %d",
		st);

	st = wsc__convert(src, 4, WL_SHM_FORMAT_XRGB8888, 0, 1, false, dst, 0,
		WSC_PIX_RGB24);
	CHECK(st == WSC_ERROR_INVALID, "zero width should be invalid, got %d", st);

	CHECK(wsc_pixfmt_bpp(WSC_PIX_RGB24) == 3, "rgb24 bpp");
	CHECK(wsc_pixfmt_bpp(WSC_PIX_RGBA) == 4, "rgba bpp");
	CHECK(wsc_pixfmt_bpp(WSC_PIX__COUNT) == -1, "bogus fmt bpp");
	CHECK(strcmp(wsc_pixfmt_str(WSC_PIX_RGB24), "rgb24") == 0, "rgb24 name");
	CHECK(wsc__shm_format_bpp(WL_SHM_FORMAT_XRGB8888) == 4, "xrgb bpp");
	CHECK(wsc__shm_format_bpp(WL_SHM_FORMAT_RGB565) == 2, "rgb565 bpp");
	CHECK(wsc__shm_format_bpp(WL_SHM_FORMAT_AYUV) == -1, "ayuv bpp");
}

int main(void) {
	test_xrgb_to_rgb24();
	test_all_32bit_formats();
	test_y_invert_and_stride();
	test_dst_stride_respected();
	test_24bit();
	test_16bit();
	test_10bit();
	test_errors();

	if (failures) {
		fprintf(stderr, "%d/%d checks FAILED\n", failures, checks);
		return 1;
	}
	printf("ok - %d checks passed (pixel conversion)\n", checks);
	return 0;
}
