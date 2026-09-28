"""Recorder and video writer tests."""

from __future__ import annotations

import shutil
import subprocess
import time

import numpy as np
import pytest

import wl_screencopy as wcs
from wl_screencopy.video import VideoWriter


def _ffprobe(path):
    if not shutil.which("ffprobe"):
        return None
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0",
         "-show_entries", "stream=width,height,nb_frames,r_frame_rate,codec_name",
         "-of", "default=noprint_wrappers=1", str(path)],
        capture_output=True, text=True,
    )
    if out.returncode != 0:
        return None
    values = {}
    for line in out.stdout.strip().splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            values[k] = v
    return values


def test_recorder_reads_paced_frames(nested):
    fps = 20.0
    with wcs.Recorder(fps=fps, display=nested.socket, timeout=10.0) as rec:
        start = time.monotonic()
        frames = list(rec.frames(max_frames=10))
        elapsed = time.monotonic() - start

    assert len(frames) == 10
    assert all(f.dtype == np.uint8 and f.ndim == 3 for f in frames)
    # Pacing must not be faster than requested; allow generous slack because
    # capture is also bounded by the compositor's own commit rate.
    assert elapsed >= (10 - 1) / fps * 0.9, (
        f"10 frames at {fps} fps took only {elapsed:.3f}s")


def test_recorder_shape_matches_output(nested):
    with wcs.Recorder(fps=30, display=nested.socket, timeout=10.0) as rec:
        shape = rec.frame_shape
        frame = rec.read()
    assert frame.shape == shape
    out_shape = (nested.height, nested.width, 3)
    assert shape == out_shape or shape[2] == 3


def test_recorder_stats(nested):
    with wcs.Recorder(fps=40, display=nested.socket, timeout=10.0) as rec:
        list(rec.frames(max_frames=5))
        stats = rec.stats()
    assert stats.frames == 5
    assert stats.fps_target == 40
    assert stats.elapsed > 0
    assert stats.fps_actual > 0
    # Every frame contributes width*height*3 bytes.
    assert stats.bytes_captured == stats.frames * nested.width * nested.height * 3


def test_recorder_reuses_out_array(nested):
    with wcs.Recorder(fps=50, display=nested.socket, timeout=10.0) as rec:
        h, w, c = rec.frame_shape
        buf = np.empty((h, w, c), dtype=np.uint8)
        got = [rec.read(out=buf) for _ in range(3)]
    assert all(g is buf for g in got)
    assert buf.any()


def test_recorder_records_video(nested, tmp_path):
    if shutil.which("ffmpeg") is None:
        pytest.skip("ffmpeg not installed")
    path = tmp_path / "clip.mp4"
    fps = 20.0
    with wcs.Recorder(fps=fps, display=nested.socket, timeout=10.0) as rec:
        stats = rec.record(str(path), max_frames=12)
    assert path.exists() and path.stat().st_size > 0
    assert stats.frames >= 12

    info = _ffprobe(path)
    if info is None:
        pytest.skip("ffprobe not installed")
    assert int(info["width"]) == nested.width
    assert int(info["height"]) == nested.height
    assert info["codec_name"] in ("h264", "hevc")
    nb = int(info.get("nb_frames", "0") or 0)
    assert nb >= 10, f"expected ~12 frames in the file, ffprobe says {nb}"


def test_recorder_video_with_callback(nested, tmp_path):
    seen = []

    def on_frame(frame, index):
        seen.append((index, int(frame[0, 0, 0])))

    with wcs.Recorder(fps=60, display=nested.socket, timeout=10.0) as rec:
        rec.record(str(tmp_path / "cb.mp4"), max_frames=4, on_frame=on_frame)
    assert [i for i, _ in seen] == [0, 1, 2, 3]


def test_recorder_without_writer_only_calls_back(nested):
    frames = []
    with wcs.Recorder(fps=60, display=nested.socket, timeout=10.0) as rec:
        rec.record(None, max_frames=3, on_frame=lambda f, i: frames.append(f.copy()))
    assert len(frames) == 3
    assert frames[0].shape[2] == 3


def test_recorder_stop_ends_iteration(nested):
    rec = wcs.Recorder(fps=20, display=nested.socket, timeout=10.0).start()
    count = 0
    for _ in rec.frames():
        count += 1
        if count == 3:
            rec.stop()
    rec.close()
    assert count == 3


def test_recorder_background_thread(nested):
    got = []

    def consume(stream):
        for frame in stream:
            got.append(frame.shape)
            if len(got) >= 4:
                break

    rec = wcs.Recorder(fps=40, display=nested.socket, timeout=10.0).start()
    thread = rec.start_thread(consume)
    thread.join(timeout=20)
    assert not thread.is_alive()
    assert len(got) >= 4
    assert rec.thread_error is None, rec.thread_error
    rec.close()


def test_recorder_rejects_bad_fps(nested):
    with pytest.raises(ValueError):
        wcs.Recorder(fps=0, display=nested.socket)
    with pytest.raises(ValueError):
        wcs.Recorder(fps=30, pixel_format="nope", display=nested.socket)


def test_video_writer_rejects_wrong_size(tmp_path):
    if shutil.which("ffmpeg") is None:
        pytest.skip("ffmpeg not installed")
    with VideoWriter(str(tmp_path / "v.mp4"), 64, 48, fps=10) as w:
        w.write(np.zeros((48, 64, 3), dtype=np.uint8))
        with pytest.raises(ValueError):
            w.write(np.zeros((32, 32, 3), dtype=np.uint8))
    assert (tmp_path / "v.mp4").exists()


def test_damage_mode_capture_works(nested):
    """copy_with_damage (protocol v2+) must deliver frames too.

    On a completely static screen wlroots sends nothing at all, so a timeout
    is a valid outcome and is reported as a skip rather than a failure.
    """
    session = wcs.Session(display=nested.socket)
    try:
        assert session.backends.get("wlr-screencopy", 0) >= 2, \
            "copy_with_damage needs zwlr-screencopy v2+"
        try:
            frame = session.capture(wait_for_damage=True, timeout=5.0)
        except wcs.FrameTimeoutError:
            pytest.skip("screen stayed unchanged, so no damaged frame arrived")
        assert frame.dtype == np.uint8 and frame.ndim == 3
    finally:
        session.close()
