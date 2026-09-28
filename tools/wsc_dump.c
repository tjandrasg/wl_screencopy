/* SPDX-License-Identifier: MIT
 *
 * wsc-dump - minimal C driver for libwl_screencopy.
 *
 *   wsc-dump info
 *   wsc-dump grab [--output N] [--region x,y,w,h] [--cursor] [--damage]
 *                 [--fmt rgb24|rgba|bgr24|bgra] [-o frame.ppm]
 *   wsc-dump bench [--output N] [--seconds S]
 *
 * Writes a PPM (P6) so the result can be eyeballed or diffed without any
 * image library.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "wl_screencopy.h"

static int write_ppm(const char *path, const wsc_frame *f) {
	FILE *fp = strcmp(path, "-") == 0 ? stdout : fopen(path, "wb");
	if (!fp) {
		fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
		return 1;
	}
	fprintf(fp, "P6\n%d %d\n255\n", f->width, f->height);
	for (int32_t y = 0; y < f->height; y++) {
		const uint8_t *row = f->data + (ptrdiff_t)y * f->stride;
		/* PPM is RGB; convert the other layouts on the fly. */
		for (int32_t x = 0; x < f->width; x++) {
			uint8_t rgb[3];
			switch (f->pixfmt) {
			case WSC_PIX_RGB24:
				rgb[0] = row[x * 3 + 0];
				rgb[1] = row[x * 3 + 1];
				rgb[2] = row[x * 3 + 2];
				break;
			case WSC_PIX_BGR24:
				rgb[0] = row[x * 3 + 2];
				rgb[1] = row[x * 3 + 1];
				rgb[2] = row[x * 3 + 0];
				break;
			case WSC_PIX_RGBA:
			case WSC_PIX_RGBX:
				rgb[0] = row[x * 4 + 0];
				rgb[1] = row[x * 4 + 1];
				rgb[2] = row[x * 4 + 2];
				break;
			case WSC_PIX_BGRA:
			case WSC_PIX_BGRX:
				rgb[0] = row[x * 4 + 2];
				rgb[1] = row[x * 4 + 1];
				rgb[2] = row[x * 4 + 0];
				break;
			default:
				rgb[0] = rgb[1] = rgb[2] = 0;
				break;
			}
			if (fwrite(rgb, 1, 3, fp) != 3) {
				fprintf(stderr, "short write\n");
				if (fp != stdout) fclose(fp);
				return 1;
			}
		}
	}
	if (fp != stdout) fclose(fp);
	return 0;
}

static void usage(void) {
	fprintf(stderr,
		"usage: wsc-dump info\n"
		"       wsc-dump grab [options] [-o out.ppm]\n"
		"       wsc-dump bench [--seconds N] [options]\n"
		"\noptions:\n"
		"  --display NAME   Wayland socket (default $WAYLAND_DISPLAY)\n"
		"  --output N       output index (default: first)\n"
		"  --region x,y,w,h logical region to capture\n"
		"  --cursor         ask the compositor to draw the pointer\n"
		"  --damage         use copy_with_damage\n"
		"  --fmt FMT        rgb24 (default), rgba, bgr24, bgra\n"
		"  --buffers N      shared memory buffers to rotate (default 2)\n"
		"  --timeout MS     per-frame deadline (default 2000)\n");
}

int main(int argc, char **argv) {
	const char *display = NULL;
	const char *out_path = "frame.ppm";
	const char *fmt_str = "rgb24";
	const char *region = NULL;
	int output = -1;
	int buffers = 0;
	int timeout = 0;
	int seconds = 5;
	bool cursor = false, damage = false;
	static struct option longopts[] = {
		{ "display", required_argument, 0, 'd' },
		{ "output", required_argument, 0, 'O' },
		{ "region", required_argument, 0, 'r' },
		{ "cursor", no_argument, 0, 'c' },
		{ "damage", no_argument, 0, 'D' },
		{ "fmt", required_argument, 0, 'f' },
		{ "buffers", required_argument, 0, 'b' },
		{ "timeout", required_argument, 0, 't' },
		{ "seconds", required_argument, 0, 's' },
		{ "output-file", required_argument, 0, 'o' },
		{ "help", no_argument, 0, 'h' },
		{ 0, 0, 0, 0 },
	};
	int c;

	while ((c = getopt_long(argc, argv, "d:O:r:cdDf:b:t:s:o:h", longopts,
			NULL)) != -1) {
		switch (c) {
		case 'd': display = optarg; break;
		case 'O': output = atoi(optarg); break;
		case 'r': region = optarg; break;
		case 'c': cursor = true; break;
		case 'D': damage = true; break;
		case 'f': fmt_str = optarg; break;
		case 'b': buffers = atoi(optarg); break;
		case 't': timeout = atoi(optarg); break;
		case 's': seconds = atoi(optarg); break;
		case 'o': out_path = optarg; break;
		default: usage(); return 2;
		}
	}
	if (optind >= argc) {
		usage();
		return 2;
	}
	const char *cmd = argv[optind];

	wsc_pixfmt pixfmt = WSC_PIX_RGB24;
	if (!strcmp(fmt_str, "rgb24")) pixfmt = WSC_PIX_RGB24;
	else if (!strcmp(fmt_str, "rgba")) pixfmt = WSC_PIX_RGBA;
	else if (!strcmp(fmt_str, "bgr24")) pixfmt = WSC_PIX_BGR24;
	else if (!strcmp(fmt_str, "bgra")) pixfmt = WSC_PIX_BGRA;
	else if (!strcmp(fmt_str, "rgbx")) pixfmt = WSC_PIX_RGBX;
	else if (!strcmp(fmt_str, "bgrx")) pixfmt = WSC_PIX_BGRX;
	else {
		fprintf(stderr, "unknown format \"%s\"\n", fmt_str);
		return 2;
	}

	wsc_status st = WSC_OK;
	wsc_session_options sopt = WSC_SESSION_OPTIONS_INIT;
	sopt.display_name = display;
	sopt.buffer_count = buffers;
	sopt.cursor = cursor;

	wsc_session *s = wsc_session_open(&sopt, &st);
	if (!s) {
		fprintf(stderr, "cannot open session: %s\n",
			wsc_session_error_string(NULL));
		return 1;
	}

	if (!strcmp(cmd, "info")) {
		printf("libwl_screencopy %s\n", wsc_version_string());
		printf("backends:");
		if (wsc_session_backends(s) & WSC_BACKEND_WLR_SCREENCOPY) {
			printf(" wlr-screencopy (v%u)", wsc_session_wlr_version(s));
		}
		if (wsc_session_backends(s) & WSC_BACKEND_EXT_IMAGE_COPY) {
			printf(" ext-image-copy-capture");
		}
		if (wsc_session_backends(s) == WSC_BACKEND_NONE) {
			printf(" none");
		}
		printf("\n");
		printf("note: %s\n", wsc_session_error_string(s));

		int n = wsc_output_count(s);
		printf("outputs: %d\n", n);
		for (int i = 0; i < n; i++) {
			wsc_output_info info;
			if (wsc_output_get(s, i, &info) != WSC_OK) {
				continue;
			}
			printf("  [%d] %-12s %dx%d@%.2fHz scale=%d %dx%dmm"
				" at %+d,%+d%s  %s\n", i,
				info.name[0] ? info.name : "?", info.width, info.height,
				info.refresh / 1000.0, info.scale,
				info.phys_width_mm, info.phys_height_mm, info.x, info.y,
				info.preferred ? "*" : " ", info.description);
			wsc_grab_options go = WSC_GRAB_OPTIONS_INIT;
			wsc_frame probe = WSC_FRAME_INIT;
			go.output = i;
			if (wsc_probe(s, &go, &probe) == WSC_OK) {
				printf("      capture would be %dx%d (stride %d, %d bpp)\n",
					probe.width, probe.height, probe.stride, probe.bpp);
			}
		}
		wsc_session_close(s);
		return 0;
	}

	wsc_grab_options go = WSC_GRAB_OPTIONS_INIT;
	go.output = output;
	go.cursor = cursor;
	go.wait_for_damage = damage;
	go.timeout_ms = timeout;
	go.pixfmt = pixfmt;
	if (region) {
		if (sscanf(region, "%d,%d,%d,%d", &go.x, &go.y, &go.width,
				&go.height) != 4) {
			fprintf(stderr, "bad --region \"%s\", want x,y,w,h\n", region);
			wsc_session_close(s);
			return 2;
		}
	}

	if (!strcmp(cmd, "grab")) {
		wsc_frame f = WSC_FRAME_INIT;
		st = wsc_grab(s, &go, &f);
		if (st != WSC_OK) {
			fprintf(stderr, "grab failed (%s): %s\n", wsc_status_str(st),
				wsc_session_error_string(s));
			wsc_session_close(s);
			return 1;
		}
		wsc_stats stats = { 0 };
		wsc_session_get_stats(s, &stats);
		printf("frame %dx%d stride=%d fmt=%s output=%d scale=%.3f "
			"pts=%" PRIu64 "us y_invert=%d damage=%d,%d %dx%d\n",
			f.width, f.height, f.stride, wsc_pixfmt_str(f.pixfmt),
			f.output, f.scale, f.pts_us, (int)f.y_inverted,
			f.damage_x, f.damage_y, f.damage_width, f.damage_height);
		printf("wait=%" PRIu64 "us convert=%" PRIu64 "us total_ok=%" PRIu64 "\n",
			stats.last_wait_us, stats.last_convert_us, stats.frames_ok);
		if (strcmp(out_path, "-") != 0) {
			if (write_ppm(out_path, &f) != 0) {
				wsc_session_close(s);
				return 1;
			}
			printf("wrote %s\n", out_path);
		}
		wsc_session_close(s);
		return 0;
	}

	if (!strcmp(cmd, "bench")) {
		wsc_frame f = WSC_FRAME_INIT;
		struct timespec t0, t1;
		long frames = 0;
		double acc_wait = 0, acc_conv = 0;
		wsc_stats stats;

		clock_gettime(CLOCK_MONOTONIC, &t0);
		for (;;) {
			clock_gettime(CLOCK_MONOTONIC, &t1);
			double el = (t1.tv_sec - t0.tv_sec) +
				1e-9 * (t1.tv_nsec - t0.tv_nsec);
			if (el > seconds) {
				break;
			}
			st = wsc_grab(s, &go, &f);
			if (st != WSC_OK) {
				fprintf(stderr, "grab %ld failed (%s): %s\n", frames,
					wsc_status_str(st), wsc_session_error_string(s));
				break;
			}
			wsc_session_get_stats(s, &stats);
			acc_wait += (double)stats.last_wait_us / 1e6;
			acc_conv += (double)stats.last_convert_us / 1e6;
			frames++;
		}
		clock_gettime(CLOCK_MONOTONIC, &t1);
		double el = (t1.tv_sec - t0.tv_sec) +
			1e-9 * (t1.tv_nsec - t0.tv_nsec);
		printf("%ld frames in %.2fs = %.1f fps (avg wait %.2f ms, "
			"avg convert %.2f ms)\n", frames, el, frames / el,
			frames ? acc_wait / frames * 1e3 : 0.0,
			frames ? acc_conv / frames * 1e3 : 0.0);
		wsc_session_close(s);
		return frames > 0 ? 0 : 1;
	}

	usage();
	wsc_session_close(s);
	return 2;
}
