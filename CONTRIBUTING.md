# Contributing

Thanks for looking. Two things up front:

1. **This repository is 100% AI-generated and has not had a human correctness
   review** (see the warning at the top of `README.md`). Your review *is* the
   missing ingredient - a comment saying "I ran this on hyprland 0.46 and
   region capture is off by the scale factor" is worth more here than a large
   speculative refactor.
2. **There is no CI.** Nothing will test your branch for you: the maintainer's
   local `make test` run is the only gate, and it covers one nested wlroots
   compositor. Compositor diversity is the project's biggest gap.

The project does what you see in `README.md`: a C core that speaks
`wlr-screencopy-unstable-v1`, and a `ctypes` + NumPy layer on top. It has no
daemon, no config file, no plugin system, and adding any of those is a design
change worth discussing first.

## Licence

Everything here is MIT. **Contributions are offered under the same MIT licence**
(inbound = outbound); there is no CLA and no assignment of copyright. New C
files should start with `/* SPDX-License-Identifier: MIT */` to match the
existing sources.

If you lean on an AI tool to write your contribution, say so in the pull request
description. This project is transparent about being agent-written and it keeps
review honest to know which is which.

## Getting a working build

```sh
# Debian / Ubuntu
sudo apt install build-essential pkg-config libwayland-dev libwayland-bin
# Fedora
sudo dnf install gcc make pkgconf-pkg-config libwayland-devel libwayland

make                       # library + tools, copies the .so into the package
pip install -e .           # or: pip install .
python3 -m wl_screencopy list
```

`make` runs `wayland-scanner` over the vendored XML in `proto/`, so the build
needs `libwayland-bin` (Debian) / `libwayland-devel` (Fedora), not just
`libwayland-dev`. Python is >= 3.9 (`pyproject.toml`), NumPy is the only hard
dependency; Pillow (`pip install -e '.[images]'`) is only for `save_image()` and
ffmpeg only for `Recorder`.

## Running the tests

| Command | Needs a compositor? | Covers |
| --- | --- | --- |
| `make unittest` | no | pixel conversion: every `wl_shm` format into rgb24/rgba/bgr24/bgra/rgbx/bgrx, stride, y-inversion, alpha, error cases |
| `make test` | yes (it starts its own) | unit + integration: real protocol exchange, buffer pools, region maths, NumPy shapes, pacing, MP4 output |
| `make integration` | yes | integration only, assuming `make` already built everything |

The integration fixtures do **not** capture your desktop. They launch a second,
tiny wlroots compositor nested inside your session and capture that, so the
pixel-exactness assertions are deterministic and your screens are untouched.
`tests/testclient.c` paints a pattern whose formula is mirrored in
`tests/pattern.py`, which is why the suite can assert "exactly these pixels"
instead of "not black". `tests/README.md` explains the whole setup and is worth
reading before touching anything.

Tests **skip rather than fail** when no usable compositor or no parent Wayland
session exists - a green run with 30 skips proves nothing, so check the skip
count. Knobs:

```sh
WSC_TEST_COMPOSITOR=/usr/bin/labwc pytest -q   # pick the compositor
WSC_NO_CLIENT=1 pytest -q                       # skip pixel-exactness tests
WSC_KEEP_NESTED=1 tests/nest.sh build/wsc-dump info   # keep the nested compositor up
tests/nest.sh python3 -m wl_screencopy list     # any command inside the nested session
```

You do not need root for a test compositor:

```sh
mkdir -p .testbin
apt-get download cage && dpkg-deb -x cage_*.deb /tmp/cage-x
cp /tmp/cage-x/usr/bin/cage .testbin/
```

`.testbin/`, `build/` and capture artefacts are git-ignored - keep it that way;
do not commit a built `.so`, a PNG or a clip.

## Code style

No formatter or linter is configured, so match the file you are editing.

**C** (`src/`, `include/`, `tools/`, `tests/*.c`)

* C11, `-D_GNU_SOURCE`, indented with **tabs**, wrapped at ~90 columns.
* Keep the build warning-free under the `WARN` flags in the `Makefile`
  (`-Wall -Wextra -Wshadow -Wwrite-strings`, ...). New warnings are a review
  block.
* `wsc_` prefix for public symbols, `wsc__` for internal ones; internals go in
  `src/internal.h` or the `.c` file, never in `include/wl_screencopy.h`.
* Return `wsc_status` codes; do not add `abort()`/`assert()` on paths a
  compositor can reach. Set a useful message with the internal error setter so
  the Python layer can raise something specific.
* The capture path reuses pooled buffers (`src/capture.c`); avoid per-frame
  allocations and per-frame round-trips unless you can show the cost.

**Python** (`python/wl_screencopy/`)

* Python >= 3.9, 4-space indent, `from __future__ import annotations`, type
  hints on public functions, wrapped at ~88 columns.
* Public API stays in `__init__.py` / `__all__`; raise the specific classes from
  `errors.py` (`CompositorUnsupportedError`, `InvalidError`, `NoMemoryError`,
  `FrameTimeoutError`, `CaptureFailedError`, `DisconnectedError`,
  `ProtocolError`, `BusyError`) - never a bare `RuntimeError`, never a silent
  `None`.
* Frames are `(H, W, 3)` uint8 **top-down** RGB unless the caller explicitly
  asks for another format; regions are logical pixels, arrays come back in
  device pixels. Do not "fix" that asymmetry without an issue discussing it.

**The ABI boundary.** `include/wl_screencopy.h` + `src/abi.c` on one side and
`python/wl_screencopy/_lib.py` on the other. Change one and you must change the
other in the same commit, or Python reads out of bounds. Structures are matched
by layout, not by name - check field order and sizes.

**Protocol XML.** `proto/*.xml` files are vendored upstream copies recorded in
`proto/PROVENANCE.md`. Update them wholesale with the provenance note amended
(version, commit, what changed) rather than hand-editing interfaces.

## Making a pull request

* One change per PR. Describe **what** and **why**; the diff shows how.
* Required before you ask for review:
  ```sh
  make clean && make && make test
  git diff --check
  ```
  and report **which compositor, distro and GPU** you ran it on, plus whether
  anything skipped. "make test passed" without the compositor name is not
  enough information to judge a change on a protocol that every compositor
  implements slightly differently.
* Bug fixes come with a regression test. This project has already caught a
  channel-offset bug, alpha taken from a padding byte, and mishandled
  `wl_output` v4 events that way - those tests are why they stay fixed.
* Update docs in the same PR: `README.md`, docstrings, `tests/README.md` if the
  harness changed. A user-visible behaviour change with no doc update will be
  asked for one.
* If your change touches capture correctness, say what you verified about
  pixels, not just that tests ran: `build/wsc-dump grab -o /tmp/f.ppm` and
  `python3 -m wl_screencopy snapshot` are the quick ways to eyeball a frame.
* Rebase on `main` before review; force-push during review is fine.

## Where help is most useful

1. **Test reports from compositors nobody has tried** - sway, hyprland, labwc,
   river, wayfire, and multi-monitor / fractional-scaling setups. The suite was
   developed against one nested wlroots compositor; `make test` output from your
   machine is a real contribution even with no code.
2. **A second backend: `ext-image-copy-capture-v1`.** This is the protocol
   current KDE/Plasma builds expose, so it is the single largest gap - no KDE
   user can use this project today.
3. **Continuous integration.** There is none. A GitHub Actions workflow that
   builds under Debian and Fedora and runs `make unittest` (the conversion tests
   need no compositor) would catch most regressions with no special hardware.
4. **Damage handling and pacing.** `copy_with_damage` is implemented but the
   fast paths are not exercised much, and recorder timing assumes a fixed rate.
5. **Zero-copy / dmabuf** output, which the current shm-to-NumPy design
   deliberately does not do.
6. Packaging: a proper wheel build (the `.so` must ship inside the package and
   `setuptools` config is minimal), plus distro packages.

Open an issue first for anything that changes the public API, the ABI, or the
error model - those are cheap to discuss early and expensive to reverse later.
