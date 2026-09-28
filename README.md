# wl_screencopy

![100% AI-generated](https://img.shields.io/badge/code-100%25_AI--generated-orange)

> [!WARNING]
> **This is a 100% AI-generated repository.** Every file in it - the C core,
> the Python/ctypes bindings, the build files, the tests, the examples, the
> comments and this README - was written by an AI coding agent. No human
> wrote or reviewed it line by line.
>
> * The claims below are backed by automated tests that pass, but the code
>   has **not** had a human correctness or security review.
> * Treat it as unvetted code: read it before you build it, and do not depend
>   on it for anything you cannot afford to break.
> * Screen capture reads whatever is currently on your monitors, and this
>   project maps compositor shared-memory buffers directly into your process.
>   A bug here could leak pixels, stall or crash a Wayland session, or hang
>   on a compositor it mis-handles. Nothing here is affiliated with, audited
>   or endorsed by the Wayland, wlroots or KDE projects.
> * It was developed and tested against a single nested wlroots compositor.
>   Behaviour on your compositor is unverified unless someone reports it.
>
> Use at your own risk, as per the license. Human review, bug reports and
> pull requests are very welcome.

Wayland screen capture and recording over the **`wlr-screencopy-unstable-v1`**
protocol, exposed to Python as **NumPy RGB arrays**.

```python
import wl_screencopy as wcs

frame = wcs.capture()     # np.ndarray, uint8, (H, W, 3), top-down RGB
```

The heavy lifting is a small C library (`libwl_screencopy`) that speaks the
protocol and converts the compositor's shared-memory buffer into packed RGB;
the Python layer is `ctypes` + NumPy, so a capture goes straight into your
array with a single conversion pass and no temporary copies.

* **Real protocol, no shortcuts** - uses `zwlr_screencopy_manager_v1`
  (`capture_output` / `capture_output_region`, `copy` / `copy_with_damage`,
  `flags` / `damage` / `ready` timestamps), not a screenshot D-Bus portal.
* **NumPy-native** - `(H, W, 3)` uint8 RGB by default, plus `rgba`, `bgr24`
  (OpenCV), `bgra`, `rgbx`, `bgrx`. Pass `out=` to reuse one buffer forever.
* **Verified** - the integration tests compare captured pixels against a
  client that paints a formula-defined pattern: **100 % of pixels match
  exactly** (see `tests/`).
* **Records** - `Recorder` paces captures and pipes raw frames to ffmpeg
  (NVENC when present, libx264 otherwise), with a per-frame callback.

---

## Install

```sh
# Debian / Ubuntu
sudo apt install build-essential pkg-config libwayland-dev libwayland-bin
# Fedora
sudo dnf install gcc make pkgconf-pkg-config libwayland-devel libwayland

make            # builds libwl_screencopy.so into python/wl_screencopy/_native/
pip install -e .                        # or: pip install .
python3 -c "import wl_screencopy as w; print(w.backends())"
```

Requirements: Linux with a Wayland session, a C compiler, `wayland-scanner`
(Debian/Ubuntu: `libwayland-bin`; the headers come from `libwayland-dev`) and
NumPy. `ffmpeg` is only needed for MP4 output; `Pillow` only for
`save_image()` without ffmpeg.

Nothing is installed system-wide by `make`; the Python package finds the
shared object next to itself. `make install PREFIX=~/.local` (or the default
`/usr/local`) is available if you want the C library and header installed.

## Which compositors?

`wlr-screencopy-unstable-v1` is the wlroots protocol, so it works on
**Sway, Hyprland, river, labwc, Cage, Wayfire, Mudlark, FXWM** and other
wlroots-based compositors. Check your own in five seconds:

```sh
python3 -m wl_screencopy list
# compositor support: wlr-screencopy v3
#   [0]* DP-1  2560x1440@164.69Hz at (0,0)  ...
```

If it prints `compositor support: NONE` (e.g. GNOME, or KDE builds without
screencasting), the library still lists outputs and explains the situation
instead of pretending to work:

```
CompositorUnsupportedError: the compositor does not advertise
zwlr_screencopy_manager_v1, so wlr-screencopy capture is unavailable. ...
```

X11 sessions have no Wayland socket at all, so they are out of scope.

## Capturing

```python
import numpy as np
import wl_screencopy as wcs

# Whole preferred output -> (H, W, 3) uint8 RGB.
img = wcs.capture()

# Choose the monitor by index or connector name, optionally with the pointer.
img = wcs.capture("DP-1", cursor=True)

# A region, in *logical* pixels; the array comes back in device pixels.
img = wcs.capture(region=(100, 200, 800, 600))

# Metadata: presentation timestamp, damage rect, scale, output index.
arr, info = wcs.grab()
print(info.pts, info.damage, info.scale, info.output_index)

# Reuse one array in a loop: no allocation per frame.
buf = np.empty((1440, 2560, 3), dtype=np.uint8)
while True:
    wcs.capture(out=buf)

# Zero-copy view of the library buffer (valid until ~2 captures later).
view = wcs.capture(copy=False)
```

Coordinate and orientation guarantees:

* Rows are always **top-down**; the compositor's `y-invert` flag is applied
  for you (and reported as `info.y_inverted` / `info.compositor_flags`).
* Regions are logical pixels (what window coordinates use), like `grim`;
  the returned frame is `region * output.scale` in device pixels.
* `wcs.outputs()` reports position in the global layout, scale, refresh rate,
  physical size and the connector name.

## Recording

```python
import wl_screencopy as wcs

# Straight to a file.
with wcs.Recorder(fps=60, cursor=True) as rec:
    rec.record("clip.mp4", duration=20.0)
    print(rec.stats())        # "1200 frames in 20.01s = 59.9 fps ..."

# Frames are yours; encode is optional.
with wcs.Recorder(fps=30) as rec:
    for frame in rec.frames(max_frames=300):    # (H, W, 3) uint8 RGB
        model_input = preprocess(frame)

# Both: process each frame on its way into the video.
def overlay(frame, index):
    frame[0:8, 0:8] = (255, 0, 0)               # burn in a marker
with wcs.Recorder(fps=30) as rec:
    rec.record("clip.mp4", max_frames=600, on_frame=overlay)

# Background capture thread.
rec = wcs.Recorder(fps=30).start()
rec.start_thread(lambda frames: [process(f) for f in frames])
...
rec.stop(); rec.close()
```

Low-CPU mode: `Recorder(wait_for_damage=True)` uses
`copy_with_damage`, which parks until the screen actually changes. With
`repeat_frames=True` (the default) the previous frame is re-emitted so a
recording keeps correct timing while the desktop is idle.

### CLI

```sh
python -m wl_screencopy list [--json]
python -m wl_screencopy snapshot shot.png --info
python -m wl_screencopy record clip.mp4 --fps 60 --duration 20 --cursor
python -m wl_screencopy record - --duration 5 --out-dir frames/     # no encode, PNG samples
python -m wl_screencopy frames stack.npy --count 30                 # -> np.ndarray (30,H,W,3)
python -m wl_screencopy bench --seconds 5
```

All of them accept `--output`, `--region x,y,w,h`, `--cursor`, `--damage`,
`--pixel-format`, `--display`, `--timeout`, `--buffers`.

## Performance

Measured with `bench` at 1280x720 on an Intel iGPU, nested compositor, single
thread:

| metric | value |
| --- | --- |
| throughput | ~165 fps / ~455 MB/s (bounded by the output's refresh rate) |
| wait for compositor frame | ~8-11 ms (one output commit) |
| `wl_shm` -> RGB24 conversion | ~1.5 ms |
| allocations in steady state | none (rotating shm pool + `out=` buffers) |

Notes on keeping it fast:

* Capture is bounded by the compositor's commit rate; `fps` caps it from
  above. Requesting 240 fps on a 60 Hz output does not create frames.
* `buffers=N` rotates N shared-memory buffers so frame N+1 is copied by the
  compositor while you are still reading frame N.
* 32-bit sources into 32-bit destinations (`rgba`) use a byte-permutation fast
  path; `rgb24` uses the 32->24 path. Both are single-pass, cache friendly.
* A dmabuf backend would avoid the CPU copy entirely, but NumPy wants CPU
  pixels anyway - `wl-screencopy` hands back shm buffers on every wlroots
  compositor, which is exactly what this needs.

## API summary

| call | returns |
| --- | --- |
| `wcs.capture(output=None, region=None, *, cursor, wait_for_damage, timeout, pixel_format, out, copy)` | `np.ndarray (H,W,3or4) uint8` |
| `wcs.grab(...)`, `Session.grab(...)` | `(np.ndarray, FrameInfo)` |
| `wcs.probe(output, region)` | `FrameInfo` (geometry only, no pixels) |
| `wcs.outputs()`, `wcs.output_info(spec)` | `list[Output]`, `Output` |
| `wcs.backends()`, `wcs.supports_screencopy()` | `{'wlr-screencopy': 3}`, `bool` |
| `wcs.Session(display=None, buffers=2, cursor=False)` | session object |
| `wcs.Recorder(output, region, fps, cursor, wait_for_damage, pixel_format, buffers, timeout, repeat_frames, session)` | recorder |
| `rec.read(out=None)`, `rec.frames(out=None, max_frames=None)` | frame / iterator of frames |
| `rec.record(path, duration, max_frames, writer, codec, crf, preset, on_frame)` | `RecorderStats` |
| `wcs.save_image(arr, path)` | PNG/JPEG via Pillow, falling back to ffmpeg |
| `wcs.VideoWriter(path, w, h, fps, codec=...)` | raw-RGB-to-ffmpeg pipe |

Errors: `WlScreencopyError` base, with `CompositorUnsupportedError`,
`InvalidError` (also a `ValueError`), `FrameTimeoutError`,
`CaptureFailedError`, `DisconnectedError`, `ProtocolError`, `BusyError`,
`NoMemoryError`. Every message carries the library's full explanation,
including any text the compositor reported.

Thread safety: a Wayland connection belongs to one thread. `Session`
serialises its calls with a lock, so calling from several threads is safe but
captures queue behind each other. For pipeline stages, capture on one thread
and hand arrays to workers (`frame.copy()` first if you reuse `out=`).

## Using the C library directly

```c
#include <wl_screencopy.h>

wsc_status st;
wsc_session *s = wsc_session_open(&(wsc_session_options)WSC_SESSION_OPTIONS_INIT, &st);
wsc_frame f = WSC_FRAME_INIT;
st = wsc_capture_screen(s, &f);            /* f.data, f.width, f.height, f.stride */
```

`tools/wsc_dump.c` is a complete example (`info`, `grab`, `bench`) that also
writes PPM files, handy for debugging without Python.

## Repository layout

```
include/wl_screencopy.h    public C API
src/                       session/registry, capture state machine, conversion
proto/                     vendored protocol XMLs (see proto/PROVENANCE.md)
python/wl_screencopy/      ctypes binding, Session, Recorder, ffmpeg writer, CLI
tools/wsc_dump.c           C command line tool
tests/                     C unit tests + pytest integration tests (see tests/README.md)
examples/                  quickstart, recording, motion detection, multi-monitor
```

## Limitations / roadmap

* **One backend.** Only `wlr-screencopy-unstable-v1` is implemented.
  `ext-image-copy-capture-v1` (KWin 6.2+, GNOME 47+) is a different,
  session-based protocol; the discovery bitmask reserves a flag for it and
  adding it means one more `src/backend_*.c` plus a compositor to test against.
* **dmabuf is read but not used.** If a compositor offers only dmabuf, the
  library fails with an explicit message rather than silently misbehaving.
* **Rotation/transform** of outputs is passed through (`Output.transform`)
  but frames are not un-rotated; the compositor delivers what it composites.
* **Lossy video.** `Recorder` writes H.264 (yuv420p) by default, which is not
  pixel-exact - expected for a lossy codec, but worth knowing if you plan to
  diff frames. For bit-accurate archives use `--codec libx264rgb`, or skip
  encoding entirely (`record("-", on_frame=...)`, `frames` for a `.npy`
  stack, `--out-dir` for PNG samples).
* Encoded video timing assumes frames arrive at the target `fps`.

## License

MIT. Vendored protocol XMLs keep their upstream MIT copyright headers; see
`proto/PROVENANCE.md`.
