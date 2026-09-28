---
name: Feature request
about: A new capability, backend, format or API convenience
title: "[feat] "
labels: enhancement
---

## Problem this solves

<!-- What can you not do today? A concrete code sketch of how you would like
     to call it is more useful than a general description. -->

```python
import wl_screencopy as wcs
# how you would like the API to look
```

## Preferred behaviour

<!-- Return types, error behaviour, defaults. Be specific: "raises
     CompositorUnsupportedError with X" is actionable, "fails nicely" is not. -->

## Compositor scope

This project currently implements one backend, `wlr-screencopy-unstable-v1`.
If your request involves another capture protocol, say which:

- [ ] `wlr-screencopy-unstable-v1` (current)
- [ ] `ext-image-copy-capture-v1` (KDE Plasma / PipeWire-oriented; on the roadmap)
- [ ] `wp-linux-dmabuf-unstable-v1` (zero-copy / GPU buffers)
- [ ] pipewire / desktop portal (different architecture entirely)
- [ ] Other:

<!-- Which compositors must it work on? A feature that only works on one
     compositor should say so, and should fail loudly elsewhere - silent
     capability degradation is a bug in this project's design. -->

## Alternatives considered

<!-- Another library, a workaround, a compositor setting. -->

## Will you test it?

The maintainer develops against a single nested wlroots compositor, so a
volunteer who can test on sway / hyprland / labwc / river is usually the
difference between this shipping and not.

- [ ] I can test on: <!-- compositor + distro -->
