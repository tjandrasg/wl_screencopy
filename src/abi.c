/* SPDX-License-Identifier: MIT
 *
 * ABI introspection helpers. Language bindings (the Python package) check
 * these against their own struct definitions at import time, so a mismatch
 * between include/wl_screencopy.h and the bindings fails loudly with a clear
 * message instead of corrupting memory.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stddef.h>

#include "wl_screencopy.h"

size_t wsc_abi_sizeof_output_info(void) { return sizeof(wsc_output_info); }
size_t wsc_abi_sizeof_frame(void) { return sizeof(wsc_frame); }
size_t wsc_abi_sizeof_grab_options(void) { return sizeof(wsc_grab_options); }
size_t wsc_abi_sizeof_stats(void) { return sizeof(wsc_stats); }
size_t wsc_abi_sizeof_session_options(void) { return sizeof(wsc_session_options); }

/* Offsets the bindings care about when mapping a frame onto an array. */
size_t wsc_abi_frame_offset_data(void) { return offsetof(wsc_frame, data); }
size_t wsc_abi_frame_offset_width(void) { return offsetof(wsc_frame, width); }
size_t wsc_abi_frame_offset_stride(void) { return offsetof(wsc_frame, stride); }
size_t wsc_abi_frame_offset_height(void) { return offsetof(wsc_frame, height); }
size_t wsc_abi_frame_offset_pixfmt(void) { return offsetof(wsc_frame, pixfmt); }
size_t wsc_abi_frame_offset_pts_us(void) { return offsetof(wsc_frame, pts_us); }
