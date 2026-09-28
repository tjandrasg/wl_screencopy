#!/usr/bin/env bash
#
# tests/nest.sh - run a command inside a nested wlroots compositor that
# implements zwlr-screencopy-unstable-v1, so the recorder can be tested
# end to end without touching the user's real session.
#
#   tests/nest.sh build/wsc-dump info
#   tests/nest.sh -- python3 -c '...'
#
# Environment:
#   WSC_TEST_COMPOSITOR   compositor command to use (default: auto-detect)
#   WSC_TEST_CLIENT       client to start inside it (default: auto-detect)
#   WSC_KEEP_NESTED=1     leave the nested compositor running (debugging)
#
# On exit, if WSC_KEEP_NESTED=1 the socket path is printed to stderr.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"

parent_display="${WAYLAND_DISPLAY:-wayland-0}"
parent_rt="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
case "$parent_display" in
	/*) parent_socket="$parent_display" ;;
	*)  parent_socket="$parent_rt/$parent_display" ;;
esac

if [[ ! -S "$parent_socket" ]]; then
	echo "nest.sh: no parent Wayland display at $parent_socket" >&2
	echo "nest.sh (hint: this needs a running Wayland session)" >&2
	exit 77
fi

find_bin() {
	# $@ = candidate names, in preference order. Looks in PATH first, then in
	# .testbin/ (a locally provided test compositor, see tests/README.md) and
	# in unpacked .deb trees under .probe_debs.
	local name p dir
	for name in "$@"; do
		if p="$(command -v "$name" 2>/dev/null)"; then
			echo "$p"
			return 0
		fi
	done
	for dir in "$root/.testbin" "$root/.local/bin" "$root/.probe_debs"; do
		for name in "$@"; do
			[[ -x "$dir/$name" ]] && { echo "$dir/$name"; return 0; }
			p="$(find "$dir" -type f -name "$name" 2>/dev/null | head -1)" || true
			if [[ -n "$p" && -x "$p" ]]; then
				echo "$p"
				return 0
			fi
		done
	done
	return 1
}

compositor="${WSC_TEST_COMPOSITOR:-}"
if [[ -z "$compositor" ]]; then
	# cage is the smallest kiosk compositor; the others are fine too.
	# weston is deliberately last: only recent builds expose wlr-screencopy.
	compositor="$(find_bin cage labwc wayfire river sway weston)" || {
		echo "nest.sh: no wlroots compositor found (tried cage, labwc," >&2
		echo "         wayfire, river, sway, weston)." >&2
		echo "         Install one, set WSC_TEST_COMPOSITOR=/path/to/bin, or put" >&2
		echo "         a binary in $root/.testbin/. Without root:" >&2
		echo "           apt-get download cage && dpkg-deb -x cage*.deb \\" >&2
		echo "             && cp cage*/usr/bin/cage $root/.testbin/" >&2
		exit 77
	}
fi

rt="$(mktemp -d "${TMPDIR:-/tmp}/wsc-nested.XXXXXX")"
chmod 700 "$rt"

cleanup() {
	if [[ -n "${client_pid:-}" ]]; then
		kill "$client_pid" 2>/dev/null || true
	fi
	if [[ -n "${comp_pid:-}" ]]; then
		kill "$comp_pid" 2>/dev/null || true
		wait "$comp_pid" 2>/dev/null || true
	fi
	if [[ "${WSC_KEEP_NESTED:-0}" != "1" ]]; then
		rm -rf "$rt"
	fi
}
trap cleanup EXIT

echo "nest.sh: starting $compositor (nested inside $parent_socket)" >&2

# The nested backend connects to the parent through an absolute-path
# WAYLAND_DISPLAY, and publishes its own socket inside the private runtime dir.
WLR_BACKENDS="${WLR_BACKENDS:-wayland}" \
WLR_LIBINPUT_NO_DEVICES=1 \
WLR_RENDERER_ALLOW_SOFTWARE=1 \
CAGE_TERMINAL=true \
XDG_RUNTIME_DIR="$rt" \
WAYLAND_DISPLAY="$parent_socket" \
"$compositor" >"$rt/compositor.log" 2>&1 &
comp_pid=$!

# Wait for the compositor's socket to appear.
nested=""
for _ in $(seq 1 100); do
	if ! kill -0 "$comp_pid" 2>/dev/null; then
		echo "nest.sh: compositor exited immediately:" >&2
		tail -20 "$rt/compositor.log" >&2
		exit 1
	fi
	nested="$(find "$rt" -maxdepth 1 -type s 2>/dev/null | head -1)"
	[[ -n "$nested" ]] && break
	sleep 0.1
done
if [[ -z "$nested" ]]; then
	echo "nest.sh: timed out waiting for a nested Wayland socket" >&2
	tail -20 "$rt/compositor.log" >&2
	exit 1
fi
nested_name="$(basename "$nested")"

if [[ "${WSC_KEEP_NESTED:-0}" == "1" ]]; then
	echo "nest.sh: nested compositor pid $comp_pid, socket $nested" >&2
fi

# Optional: put something on screen so there is content to capture.
client="${WSC_TEST_CLIENT:-}"
if [[ -n "$client" && ! -x "$client" ]]; then
	echo "nest.sh: WSC_TEST_CLIENT=$client is not an executable" >&2
	exit 2
fi
if [[ -z "$client" ]]; then
	client="$(find_bin weston-image weston-smoke weston-simple-shm gtk4-demo || true)"
fi
if [[ -n "$client" && -n "${WSC_TEST_CLIENT_ARG:-}" ]]; then
	XDG_RUNTIME_DIR="$rt" WAYLAND_DISPLAY="$nested_name" \
		"$client" "$WSC_TEST_CLIENT_ARG" >"$rt/client.log" 2>&1 &
	client_pid=$!
elif [[ -n "$client" ]]; then
	XDG_RUNTIME_DIR="$rt" WAYLAND_DISPLAY="$nested_name" \
		"$client" >"$rt/client.log" 2>&1 &
	client_pid=$!
fi

# Give the client time to map and paint.
sleep 1

if [[ $# -gt 0 && "$1" == "--" ]]; then
	shift
fi

set +e
XDG_RUNTIME_DIR="$rt" WAYLAND_DISPLAY="$nested_name" "$@"
rc=$?
set -e

exit "$rc"
