# Vendored protocol XMLs

These files are copied verbatim from their upstream projects so the build has
no dependency on a particular distro layout (and so the wire format cannot
drift under us mid-build). Do not edit them by hand.

| File | Upstream | Version used | Notes |
| --- | --- | --- | --- |
| `wlr-screencopy-unstable-v1.xml` | wlroots (`protocol/`) | 0.20.2 | `zwlr_screencopy_manager_v1` v3. Copied from `wlroots_0.20.2.orig.tar.bz2` (Debian pool). Identical to the copy shipped by wlroots 0.17-0.20: the protocol has been frozen at v3 since 2022. |
| `xdg-output-unstable-v1.xml` | wayland-protocols (`unstable/xdg-output/`) | 1.42 (system copy) | Only used to turn `wl_output` into a friendly connector name ("DP-1") and logical geometry. |
| `xdg-shell.xml` | wayland-protocols (`stable/xdg-shell/`) | 1.42 (system copy) | Only used by `tests/testclient.c`, never linked into the capture library. |

Licenses: wlroots' protocol files are MIT, wayland-protocols XMLs are MIT
(see the `<copyright>` block inside each file).

## Refreshing them

```sh
# wlr-screencopy from the wlroots source tree
curl -LO https://deb.debian.org/debian/pool/main/w/wlroots/wlroots_0.20.2.orig.tar.bz2
tar xjf wlroots_0.20.2.orig.tar.bz2 wlroots-0.20.2/protocol/wlr-screencopy-unstable-v1.xml
cp wlroots-0.20.2/protocol/wlr-screencopy-unstable-v1.xml proto/

# xdg-shell / xdg-output from the installed system package
cp /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml proto/
cp /usr/share/wayland-protocols/unstable/xdg-output/xdg-output-unstable-v1.xml proto/
```

## The protocol in one paragraph

`zwlr_screencopy_manager_v1.capture_output{,_region}(overlay_cursor, output[,
x, y, w, h])` creates a single-use `zwlr_screencopy_frame_v1`. The compositor
answers with a `buffer` event (an `wl_shm` format, width, height, stride in
device pixels), optionally `linux_dmabuf`, then `buffer_done` (v3+). The
client creates a matching `wl_buffer` and sends `copy` (or `copy_with_damage`,
v2+). The compositor replies `flags` (bit 1 = y-inverted), zero or more
`damage` rectangles, and finally `ready` (with a presentation timestamp) or
`failed`. The frame object is then destroyed and the next frame starts from
scratch - which is why this library keeps its shared memory pool around and
rotates buffers instead of allocating per frame.
