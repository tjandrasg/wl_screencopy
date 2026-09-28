/* SPDX-License-Identifier: MIT */
#ifndef WSC_CONVERT_H
#define WSC_CONVERT_H

#include <stdbool.h>
#include <stdint.h>

#include "wl_screencopy.h"

/*
 * Pixel conversion between what a compositor hands back in a wl_shm buffer
 * (an wl_shm_format, almost always XRGB8888/ARGB8888) and the packed RGB(A)
 * layout that array consumers such as NumPy want.
 */

/* Bytes per pixel of a wl_shm format, or -1 if this library cannot read it. */
int wsc__shm_format_bpp(uint32_t shm_format);

/* Human readable name of a wl_shm format ("xrgb8888") or a fourcc. */
const char *wsc__shm_format_str(uint32_t shm_format);

/*
 * Convert a whole image.
 *
 * src/src_stride/src_format describe the compositor buffer; dst/dst_stride/
 * dst_pixfmt the destination. If y_invert is true the rows are flipped on
 * the way (compositor sent the image bottom-up).
 *
 * Returns WSC_OK, or WSC_ERROR_UNSUPPORTED for a format combination this
 * library does not implement. dst_stride == 0 means width * bpp(dst_pixfmt).
 */
wsc_status wsc__convert(const uint8_t *src, int32_t src_stride,
	uint32_t src_format, int32_t width, int32_t height, bool y_invert,
	uint8_t *dst, int32_t dst_stride, wsc_pixfmt dst_pixfmt);

#endif /* WSC_CONVERT_H */
