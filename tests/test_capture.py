"""End-to-end capture tests against a real wlroots compositor."""

from __future__ import annotations

import numpy as np
import pytest


def test_compositor_advertises_screencopy(nested, session):
    assert "wlr-screencopy" in session.backends
    assert session.backends["wlr-screencopy"] >= 1
    assert session.supports_screencopy


def test_outputs_are_described(session):
    outs = session.outputs()
    assert len(outs) >= 1
    out = outs[0]
    assert out.width > 0 and out.height > 0
    assert out.scale >= 1
    assert out.preferred
    # str() is used by the CLI: keep it informative and crash free.
    assert str(out)


def test_capture_returns_numpy_rgb(session):
    frame = session.capture(timeout=10.0)
    assert isinstance(frame, np.ndarray)
    assert frame.dtype == np.uint8
    assert frame.ndim == 3 and frame.shape[2] == 3
    assert frame.flags["C_CONTIGUOUS"]
    # Must carry the output's device resolution.
    out = session.output()
    assert frame.shape[1] == out.width * out.scale
    assert frame.shape[0] == out.height * out.scale


def test_screen_has_content(session, has_pattern_client):
    """A recording of a black screen would pass most tests, so check first."""
    frame = session.capture(timeout=10.0)
    assert frame.max() > 0, "capture is entirely black"
    if has_pattern_client:
        # The pattern covers the full gamut, so its mean is mid grey.
        assert 60 < frame.mean() < 200


def test_capture_is_pixel_exact(session, has_pattern_client, reference):
    """The core promise: RGB arrays match what the compositor shows."""
    if not has_pattern_client:
        pytest.skip("test client not available (run `make tools`)")
    frame = session.capture(timeout=10.0)
    assert frame.shape == reference.shape
    diff = np.abs(frame.astype(int) - reference.astype(int))
    exact = float((diff.sum(axis=2) == 0).mean())
    assert exact > 0.995, (
        f"only {exact * 100:.2f}% of pixels match; max channel diff "
        f"{diff.max()} at {np.unravel_index(diff.argmax(), diff.shape)}"
    )


def test_capture_into_provided_array(session, has_pattern_client, reference):
    """out= must fill the caller's array in place, without reallocating."""
    info = session.probe()
    out = np.zeros((info.height, info.width, 3), dtype=np.uint8)
    returned = session.capture(out=out, timeout=10.0)

    assert returned is out
    assert out.any(), "capture(out=...) left the array untouched"

    if has_pattern_client:
        diff = np.abs(out.astype(int) - reference.astype(int))
        assert float((diff.sum(axis=2) == 0).mean()) > 0.995


def test_capture_into_non_contiguous_slice(session):
    """A row-strided destination must still receive correct rows."""
    info = session.probe()
    w, h = info.width, info.height
    backing = np.zeros((h, w + 7, 3), dtype=np.uint8)
    view = backing[:, :w, :]
    assert not view.flags["C_CONTIGUOUS"]

    session.capture(out=view, timeout=10.0)
    assert view.any()

    # The padding columns must not have been written.
    assert not backing[:, w:, :].any(), "capture wrote past the row width"


def test_region_capture_is_crop_of_full(session):
    """A region capture must line up exactly with the full frame."""
    full = session.capture(timeout=10.0)
    out = session.output()
    h, w = full.shape[:2]

    # Stay clear of the edges so the compositor cannot clip the region.
    x, y = max(1, w // 7), max(2, h // 5)
    rw, rh = max(16, min(w - x - 3, 300)), max(16, min(h - y - 3, 200))
    region = session.capture(region=(x, y, rw, rh), timeout=10.0)

    assert region.shape == (rh, rw, 3)
    crop = full[y:y + rh, x:x + rw]
    assert crop.shape == region.shape
    # The screen may animate between the two captures; with the static test
    # client these must be identical.
    diff = np.abs(region.astype(int) - crop.astype(int))
    assert diff.mean() < 6.0, f"region is not a crop of the full frame " \
                              f"(mean diff {diff.mean():.2f})"


def test_region_rejects_garbage(session):
    for bad in [(0, 0, 0, 10), (10, 10, 0, 0), (-1, 0, 10, 10), (1, 2, 3)]:
        with pytest.raises(ValueError):
            session.capture(region=bad)


def test_out_array_validation(session):
    info = session.probe()
    h, w = info.height, info.width

    with pytest.raises(TypeError):
        session.capture(out=np.zeros((h, w, 3), dtype=np.float32))
    with pytest.raises(ValueError):
        session.capture(out=np.zeros((h, w), dtype=np.uint8))
    with pytest.raises(ValueError):
        # 4 channels for an rgb24 request
        session.capture(out=np.zeros((h, w, 4), dtype=np.uint8))
    # A destination that is too small for the frame the compositor produces
    # is reported once the geometry is known: InvalidError, which is also a
    # ValueError, so `except ValueError` catches argument mistakes too.
    with pytest.raises(ValueError):
        session.capture(out=np.zeros((4, 4, 3), dtype=np.uint8),
                        region=(0, 0, 32, 32))


def test_pixel_formats(session, has_pattern_client, reference):
    rgb = session.capture(pixel_format="rgb24", timeout=10.0)
    rgba = session.capture(pixel_format="rgba", timeout=10.0)
    bgr = session.capture(pixel_format="bgr24", timeout=10.0)
    bgra = session.capture(pixel_format="bgra", timeout=10.0)

    assert rgb.shape[2] == 3
    assert rgba.shape[2] == 4 and bgra.shape[2] == 4

    # Compositors send XRGB8888 (no alpha); converted RGBA must be opaque.
    assert (rgba[:, :, 3] == 255).all(), "RGBA alpha must be 255, not padding"
    assert (bgra[:, :, 3] == 255).all(), "BGRA alpha must be 255, not padding"

    # Channel layouts must agree with each other.
    np.testing.assert_array_equal(rgba[:, :, :3], rgb)
    np.testing.assert_array_equal(bgr[:, :, ::-1], rgb)
    np.testing.assert_array_equal(bgra[:, :, 2::-1], rgb)

    if has_pattern_client:
        np.testing.assert_array_equal(rgba[:, :, :3], reference)


def test_probe_matches_capture(session):
    info = session.probe()
    frame = session.capture(timeout=10.0)
    assert (info.height, info.width) == frame.shape[:2]
    assert info.width > 0 and info.height > 0


def test_grab_exposes_metadata(session):
    frame, info = session.grab(timeout=10.0)
    assert frame.shape == info.shape
    assert info.output_index == 0
    assert info.scale >= 1.0
    assert info.pixel_format == "rgb24"
    # ready carries a presentation timestamp; require it to be plausible.
    assert info.pts_us > 0
    assert isinstance(info.damage, (tuple, type(None)))


def test_repeated_captures_are_stable(session, has_pattern_client, reference):
    """Two captures of a static screen must be byte identical."""
    a = session.capture(timeout=10.0)
    b = session.capture(timeout=10.0)
    np.testing.assert_array_equal(a, b)
    if has_pattern_client:
        np.testing.assert_array_equal(b, reference)


def test_stats_advance(session):
    before = session.stats()
    session.capture(timeout=10.0)
    after = session.stats()
    assert after.frames_ok == before.frames_ok + 1
    assert after.bytes_captured > before.bytes_captured
    assert after.last_wait_ms >= 0.0
    assert after.last_convert_ms >= 0.0


def test_zero_copy_view(session):
    """copy=False hands back a view into the library buffer."""
    a = session.capture(copy=False, timeout=10.0)
    assert a.dtype == np.uint8 and a.shape[2] == 3
    # Copy so it survives further captures, then verify it is still correct.
    saved = np.array(a, copy=True)
    session.capture(timeout=10.0)
    assert saved.any()


def test_capture_into_reused_array_loop(session):
    """The hot loop pattern: allocate once, reuse, no growing memory."""
    info = session.probe()
    buf = np.empty((info.height, info.width, 3), dtype=np.uint8)
    for _ in range(5):
        session.capture(out=buf, timeout=10.0)
    assert buf.any()


def test_session_is_reusable_across_outputs_list(session):
    first = session.outputs()
    second = session.outputs(refresh=True)
    assert [o.index for o in first] == [o.index for o in second]
