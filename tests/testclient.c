/* SPDX-License-Identifier: MIT
 *
 * wsc-testclient - a minimal xdg-shell + shm client that paints a pattern
 * whose pixel values are reproducible from the formula below (see
 * tests/pattern.py). This is what the integration tests capture, so a
 * mismatch between what the compositor shows and what the recorder returns
 * is a real bug rather than a flake.
 *
 *   wsc-testclient [--width N] [--height N] [--animate] [--fps N] [--seconds N]
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

/* The single source of truth for the test pattern; mirrored in
 * tests/pattern.py. Keep both in sync. */
void wsc_test_pattern(uint32_t seq, int x, int y, uint8_t *rgb) {
	rgb[0] = (uint8_t)((x * 3 + y * 5 + seq * 11) & 0xff);
	rgb[1] = (uint8_t)((x ^ y ^ (int)(seq * 7)) & 0xff);
	rgb[2] = (uint8_t)((x * 7 + y * 2 + seq * 3) & 0xff);
}

struct window {
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct xdg_wm_base *wm_base;
	struct wl_shm *shm;
	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *xdg_toplevel;
	int width, height;
	struct wl_buffer *buffers[2];
	void *maps[2];
	int current;
	uint32_t seq;
	bool configured;
	bool running;
};

static int create_pool_fd(size_t size) {
	int fd = memfd_create("wsc-testclient", MFD_CLOEXEC);
	if (fd < 0) {
		return -1;
	}
	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static void fill(struct window *w, int idx) {
	uint8_t *map = w->maps[idx];
	int32_t *row = (int32_t *)map;

	for (int y = 0; y < w->height; y++) {
		for (int x = 0; x < w->width; x++) {
			uint8_t rgb[3];
			wsc_test_pattern(w->seq, x, y, rgb);
			/* wl_shm ARGB8888 is defined on the integer value, so on a
			 * little endian machine the bytes in memory are B,G,R,A. */
			row[y * w->width + x] = (int32_t)(0xff000000u |
				((uint32_t)rgb[0] << 16) | ((uint32_t)rgb[1] << 8) |
				rgb[2]);
		}
	}
}

static void handle_wm_base_ping(void *data,
		struct xdg_wm_base *wm_base, uint32_t serial) {
	(void)data;
	xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = handle_wm_base_ping,
};

static void handle_surface_enter(void *data, struct wl_surface *s,
		struct wl_output *o) { (void)data; (void)s; (void)o; }
static void handle_surface_leave(void *data, struct wl_surface *s,
		struct wl_output *o) { (void)data; (void)s; (void)o; }

static const struct wl_surface_listener surface_listener = {
	.enter = handle_surface_enter,
	.leave = handle_surface_leave,
};

static void handle_xdg_configure(void *data, struct xdg_surface *xs,
		uint32_t serial) {
	struct window *w = data;
	(void)xs;
	xdg_surface_ack_configure(xs, serial);
	w->configured = true;
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = handle_xdg_configure,
};

static void handle_toplevel_configure(void *data, struct xdg_toplevel *t,
		int32_t width, int32_t height, struct wl_array *states) {
	struct window *w = data;
	(void)t; (void)states;
	/* The pattern is defined in buffer coordinates, so the window keeps its
	 * own size and lets the compositor scale it. Recording tests rely on the
	 * client asking for the same size as the output. */
	(void)width; (void)height;
}

static void handle_toplevel_close(void *data, struct xdg_toplevel *t) {
	struct window *w = data;
	(void)t;
	w->configured = false;
	w->running = false;
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = handle_toplevel_configure,
	.close = handle_toplevel_close,
};

static void registry_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	struct window *w = data;

	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		w->compositor = wl_registry_bind(registry, name,
			&wl_compositor_interface, version < 4 ? version : 4);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		w->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface,
			version < 3 ? version : 2);
		xdg_wm_base_add_listener(w->wm_base, &wm_base_listener, w);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		w->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry,
		uint32_t name) { (void)data; (void)registry; (void)name; }

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static int make_buffer(struct window *w, int idx) {
	size_t size = (size_t)w->width * (size_t)w->height * 4u;
	struct wl_shm_pool *pool;
	int fd = create_pool_fd(size);

	if (fd < 0) {
		fprintf(stderr, "memfd_create failed: %s\n", strerror(errno));
		return -1;
	}
	w->maps[idx] = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (w->maps[idx] == MAP_FAILED) {
		close(fd);
		return -1;
	}
	pool = wl_shm_create_pool(w->shm, fd, (int32_t)size);
	close(fd);
	if (!pool) {
		return -1;
	}
	w->buffers[idx] = wl_shm_pool_create_buffer(pool, 0, w->width, w->height,
		w->width * 4, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	return w->buffers[idx] ? 0 : -1;
}

int main(int argc, char **argv) {
	struct window w = {0};
	int fps = 30, seconds = 0, frames = 0;
	bool animate = false;
	static struct option longopts[] = {
		{ "width", required_argument, 0, 'W' },
		{ "height", required_argument, 0, 'H' },
		{ "animate", no_argument, 0, 'a' },
		{ "fps", required_argument, 0, 'f' },
		{ "seconds", required_argument, 0, 's' },
		{ "frames", required_argument, 0, 'n' },
		{ 0, 0, 0, 0 },
	};
	int c;

	w.width = 1280;
	w.height = 720;

	while ((c = getopt_long(argc, argv, "W:H:af:s:n:", longopts, NULL)) != -1) {
		switch (c) {
		case 'W': w.width = atoi(optarg); break;
		case 'H': w.height = atoi(optarg); break;
		case 'a': animate = true; break;
		case 'f': fps = atoi(optarg); break;
		case 's': seconds = atoi(optarg); break;
		case 'n': frames = atoi(optarg); break;
		default: return 2;
		}
	}

	w.display = wl_display_connect(NULL);
	if (!w.display) {
		fprintf(stderr, "wsc-testclient: cannot connect to Wayland: %s\n",
			strerror(errno));
		return 1;
	}

	struct wl_registry *registry = wl_display_get_registry(w.display);
	wl_registry_add_listener(registry, &registry_listener, &w);
	wl_display_roundtrip(w.display);

	if (!w.compositor || !w.wm_base || !w.shm) {
		fprintf(stderr, "wsc-testclient: compositor lacks compositor/shm/"
			"xdg_wm_base (got %p %p %p)\n", (void *)w.compositor,
			(void *)w.wm_base, (void *)w.shm);
		return 1;
	}

	w.surface = wl_compositor_create_surface(w.compositor);
	wl_surface_add_listener(w.surface, &surface_listener, &w);
	w.xdg_surface = xdg_wm_base_get_xdg_surface(w.wm_base, w.surface);
	xdg_surface_add_listener(w.xdg_surface, &xdg_surface_listener, &w);
	w.xdg_toplevel = xdg_surface_get_toplevel(w.xdg_surface);
	xdg_toplevel_add_listener(w.xdg_toplevel, &toplevel_listener, &w);
	xdg_toplevel_set_app_id(w.xdg_toplevel, "org.wl_screencopy.testclient");
	xdg_toplevel_set_title(w.xdg_toplevel, "wl_screencopy test pattern");

	struct wl_region *region = wl_compositor_create_region(w.compositor);
	wl_region_add(region, 0, 0, w.width, w.height);
	xdg_surface_set_window_geometry(w.xdg_surface, 0, 0, w.width, w.height);
	wl_region_destroy(region);

	if (make_buffer(&w, 0) || make_buffer(&w, 1)) {
		return 1;
	}

	/* xdg-shell: the very first commit must not carry a buffer; it is what
	 * asks the compositor for an initial configure. Attaching a buffer too
	 * early is a protocol error ("xdg_surface has never been configured"). */
	wl_surface_commit(w.surface);

	while (!w.configured) {
		if (wl_display_dispatch(w.display) < 0) {
			return 1;
		}
	}

	fill(&w, 0);
	wl_surface_attach(w.surface, w.buffers[0], 0, 0);
	wl_surface_damage(w.surface, 0, 0, w.width, w.height);
	wl_surface_commit(w.surface);

	if (!animate) {
		/* Stay mapped with a static image until killed. */
		w.running = true;
		while (w.running) {
			if (wl_display_dispatch(w.display) <= 0) {
				break;
			}
		}
		return 0;
	}

	struct timespec start, now;
	clock_gettime(CLOCK_MONOTONIC, &start);
	long done = 0;
	for (;;) {
		long sleep_us = 1000000 / (fps > 0 ? fps : 30);
		struct timespec ts = { .tv_sec = sleep_us / 1000000,
			.tv_nsec = (sleep_us % 1000000) * 1000 };
		nanosleep(&ts, NULL);

		w.seq++;
		w.current ^= 1;
		fill(&w, w.current);
		wl_surface_attach(w.surface, w.buffers[w.current], 0, 0);
		wl_surface_damage(w.surface, 0, 0, w.width, w.height);
		wl_surface_commit(w.surface);
		wl_display_flush(w.display);
		done++;

		clock_gettime(CLOCK_MONOTONIC, &now);
		if (seconds && now.tv_sec - start.tv_sec >= seconds) {
			break;
		}
		if (frames && done >= frames) {
			break;
		}
		if (wl_display_prepare_read(w.display) == 0) {
			wl_display_cancel_read(w.display);
		}
		while (wl_display_prepare_read(w.display) != 0) {
			if (wl_display_dispatch_pending(w.display) < 0) {
				return 0;
			}
		}
		wl_display_cancel_read(w.display);
	}

	xdg_toplevel_destroy(w.xdg_toplevel);
	xdg_surface_destroy(w.xdg_surface);
	wl_surface_destroy(w.surface);
	wl_display_disconnect(w.display);
	return 0;
}
