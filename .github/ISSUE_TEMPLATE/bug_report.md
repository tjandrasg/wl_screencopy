---
name: Bug report
about: A capture is wrong, a call raises, the build fails, or something hangs
title: "[bug] "
labels: bug
---

<!--
Reminder: this repository is 100% AI-generated and unreviewed, so "surprising
behaviour" is entirely possible and reports are genuinely useful. But the
maintainer cannot reproduce anything without knowing your compositor, so
please fill in the Environment table - it is the single most important part
of this report.
-->

## What happened

<!-- 1-3 sentences. What did you expect, and what did you get instead? -->



## Environment

This project talks to a compositor protocol that not all compositors
implement, so the exact stack matters more than usual.

| Field | Value |
| --- | --- |
| Compositor + version | <!-- sway 1.10 / hyprland 0.46 / labwc 0.8 / river 0.6 / kwin (Plasma 6.x) / weston 14 ... --> |
| Distribution | <!-- e.g. Ubuntu 24.04, Fedora 42, Arch (rolling) --> |
| GPU + driver | <!-- e.g. AMD Radeon + Mesa 25.0, NVIDIA 560.xx --> |
| Session | <!-- Wayland (required) - X11/XWayland will not work --> |
| wl_screencopy commit | <!-- git rev-parse --short HEAD --> |
| Native lib version | <!-- output of wcs.native_version() --> |
| Python / NumPy | <!-- python3 -V; python3 -c "import numpy; print(numpy.__version__)" --> |
| ffmpeg (only if recording) | <!-- ffmpeg -version | head -1 --> |

Paste the output of these three commands (they take a second and answer most
of the questions a maintainer would otherwise ask):

```sh
python3 -m wl_screencopy list
python3 -c "import wl_screencopy as w; print('backends:    ', w.backends()); print('native:      ', w.native_version()); print('screencopy:  ', w.supports_screencopy())"
wayland-info 2>/dev/null | grep -iE 'screencopy|image-copy|xdg_output' \
  || weston-info 2>/dev/null | grep -iE 'screencopy|image-copy|xdg_output' \
  || echo "no wayland-info/weston-info installed"
```

> If `list` prints `compositor support: NONE`, that is not a bug in this
> project: your compositor does not advertise `wlr-screencopy-unstable-v1`.
> KDE/Plasma and GNOME builds without a screencopy protocol are common cases -
> see README.md before filing.

## Steps to reproduce

```sh
# Minimal, copy-pasteable. Prefer a 5-line Python snippet over a big script.
```

1.
2.
3.

## Actual vs expected behaviour

<!-- For wrong pixels: describe the artefact (wrong colours, swapped channels,
     rotated/flipped image, stale frame, black frame, torn regions). A
     screenshot or a saved PNG is worth a thousand words. -->



## Logs

```
WAYLAND_DEBUG=1 python3 -m wl_screencopy list 2>&1 | head -200
```

<details>
<summary>Full log (click to expand)</summary>

```
paste here
```

</details>

**Before pasting a `WAYLAND_DEBUG=1` log, read it.** Protocol traces can
contain window titles, filenames and other clients' globals - strip anything
you do not want to publish.

## Does it work anywhere else?

<!-- e.g. "works on sway, fails on hyprland", or "fails on both". Capturing
     from a nested compositor (cage / weston / a sway session inside your
     session) is a quick way to tell "this project" from "my compositor". -->

- [ ] Tested on a second compositor
- [ ] Fails the same way there
- [ ] Passes there, so it looks compositor-specific

## Makefile / build issues only

```sh
make clean && make 2>&1 | tail -30
pkg-config --modversion wayland-client wayland-scanner
```
