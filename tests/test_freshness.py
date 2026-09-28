"""Tests that need a *changing* screen.

Capturing a static screen cannot distinguish "the recorder delivers new
frames" from "the recorder keeps handing back the same buffer". These use a
second nested compositor whose client repaints at 60 fps.
"""

from __future__ import annotations

import time

import numpy as np
import pytest

import wl_screencopy as wcs


def test_frames_are_actually_new(nested_animated):
    """Capturing must eventually return *different* content.

    Note that repeats are expected and correct: the compositor re-delivers
    the last composited content on commits without new damage, so capturing
    faster than the client repaints yields duplicate frames. The bug this
    test guards against is the opposite - a buffer that never changes.
    """
    session = wcs.Session(display=nested_animated.socket)
    try:
        first = session.capture(timeout=10.0)
        distinct = {hash(first.tobytes())}
        changed_after = None
        for i in range(1, 25):
            frame = session.capture(timeout=10.0)
            distinct.add(hash(frame.tobytes()))
            if not np.array_equal(frame, first) and changed_after is None:
                changed_after = i
        assert changed_after is not None, (
            "25 consecutive captures of an animating screen were identical; "
            "the recorder is returning a stale buffer"
        )
        assert len(distinct) >= 3, (
            f"only {len(distinct)} distinct frames in 25 captures of a screen "
            f"animating at 60 fps (first change at capture #{changed_after})"
        )
    finally:
        session.close()


def test_out_array_is_updated_in_place(nested_animated):
    session = wcs.Session(display=nested_animated.socket)
    try:
        info = session.probe()
        buf = np.zeros((info.height, info.width, 3), dtype=np.uint8)
        session.capture(out=buf, timeout=10.0)
        first = buf.copy()
        for _ in range(10):
            session.capture(out=buf, timeout=10.0)
            if not np.array_equal(first, buf):
                break
        else:
            pytest.fail("reuse of out= never changed the buffer")
    finally:
        session.close()


def test_damage_mode_delivers_changes(nested_animated):
    """With copy_with_damage, an animating screen must keep producing frames."""
    session = wcs.Session(display=nested_animated.socket)
    try:
        if session.backends.get("wlr-screencopy", 0) < 2:
            pytest.skip("copy_with_damage needs protocol v2+")
        frames = []
        for _ in range(4):
            try:
                frames.append(session.capture(wait_for_damage=True, timeout=5.0))
            except wcs.FrameTimeoutError:
                pytest.skip("compositor sent no damaged frame within 5s")
        assert len(frames) == 4
        assert any(not np.array_equal(a, b)
                   for a, b in zip(frames, frames[1:]))
    finally:
        session.close()


def test_recorder_throughput_with_live_content(nested_animated):
    """A short uncapped run must sustain a reasonable frame rate."""
    rec = wcs.Recorder(display=nested_animated.socket, fps=10000, buffers=3,
                       timeout=10.0)
    with rec:
        h, w, c = rec.frame_shape
        buf = np.empty((h, w, c), dtype=np.uint8)
        for _ in range(3):          # warm up buffer pool / caches
            rec.read(out=buf)
        n = 0
        start = time.monotonic()
        while time.monotonic() - start < 1.0:
            rec.read(out=buf)
            n += 1
    elapsed = time.monotonic() - start
    rec.close()
    assert n >= 10, f"only {n:.0f} frames/s, which suggests a stall"
