# Tests

Two layers:

| Test | Needs a compositor? | What it proves |
| --- | --- | --- |
| `test_convert.c` (`make unittest`) | no | the pixel conversion table: every supported `wl_shm` format into rgb24/rgba/bgr24/bgra/rgbx/bgrx, row stride, y-inversion, alpha fill, error cases |
| `test_capture.py`, `test_recorder.py` (`make test`) | yes, but it starts its own | the real protocol exchange, buffer management, region maths, NumPy shapes, pacing, MP4 output |

## Why the tests start a nested compositor

Screen capture can only be tested against a compositor that implements
`zwlr-screencopy-unstable-v1`. Rather than assuming your desktop is one (many
are not - e.g. KDE builds without screencasting, GNOME before 47, X11
sessions), the fixtures launch a *second*, tiny wlroots compositor nested
inside your current session and capture from that. Your desktop is untouched,
and the tests are deterministic.

`tests/testclient.c` paints a pattern whose formula is mirrored in
`tests/pattern.py`, so "the recorder returns exactly what is on screen" is
checked pixel by pixel rather than by "is it not black".

## Running

```sh
make            # builds the library, tools and the test client
make unittest   # pixel conversion unit tests, always run
make test       # unit + integration tests (skips with a message if no compositor)
```

Useful knobs:

```sh
WSC_TEST_COMPOSITOR=/usr/bin/labwc pytest -q   # use a specific compositor
WSC_NO_CLIENT=1 pytest -q                       # skip pixel-exactness tests
pytest -q -k pixel_exact                        # one test
./tests/nest.sh python3 -m wl_screencopy list   # ad-hoc command inside nested cage
./tests/nest.sh build/wsc-dump grab -o /tmp/f.ppm
```

## Getting a compositor without root

The suite looks for `cage`, `labwc`, `wayfire`, `river`, `sway`, `weston` in
`PATH`, then in `.testbin/`, then in unpacked `.deb` trees. You can unpack a
compositor into your home directory (no root needed) and drop the binary in
`.testbin/`:

```sh
mkdir -p .testbin
apt-get download cage                      # or: dnf download cage
dpkg-deb -x cage_*.deb /tmp/cage-x         # rpm2cpio + cpio for RPM
cp /tmp/cage-x/usr/bin/cage .testbin/
```

Weston is tried last on purpose: older builds (including Weston 14) do not
expose `wlr-screencopy` at all, and the fixture skips with a clear message in
that case.
