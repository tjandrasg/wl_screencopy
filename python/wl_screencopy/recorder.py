"""Recorder: a paced screen capture loop that yields NumPy frames or writes video."""

from __future__ import annotations

import os
import threading
import time
from dataclasses import dataclass
from typing import Callable, Iterator, Optional

import numpy as np

from .errors import FrameTimeoutError, WlScreencopyError
from .session import OutputSpec, Session
from .video import VideoWriter
from ._lib import PIXFMT, PIXFMT_CHANNELS

__all__ = ["Recorder", "RecorderStats"]


@dataclass
class RecorderStats:
    """Live bookkeeping for a running recorder."""

    frames: int = 0
    timeouts: int = 0
    repeated: int = 0
    bytes_captured: int = 0
    started_at: float = 0.0
    elapsed: float = 0.0
    fps_target: float = 0.0
    fps_actual: float = 0.0

    def __str__(self) -> str:
        return (f"{self.frames} frames in {self.elapsed:.2f}s "
                f"= {self.fps_actual:.1f} fps (target {self.fps_target:g}, "
                f"{self.timeouts} timeouts, {self.repeated} repeated)")


class Recorder:
    """Records a Wayland output as a stream of NumPy RGB frames.

    The recorder paces captures to ``fps``. Because ``zwlr_screencopy`` hands
    over one frame per compositor commit, capturing is naturally bounded by
    the monitor refresh rate; ``fps`` caps it from above and (with
    ``repeat_frames``) keeps the output timeline even when the screen is idle.

    Example - frames into your own code::

        with wl_screencopy.Recorder(fps=30) as rec:
            for frame in rec.frames():        # (H, W, 3) uint8 RGB
                detect(frame)
                if rec.stats().frames >= 300:
                    break

    Example - straight to a file::

        with wl_screencopy.Recorder(output=0, fps=60) as rec:
            rec.record("gameplay.mp4", duration=20.0)

    Reuse one array across frames with ``out=`` to avoid an allocation per
    frame; the recorder keeps it alive.
    """

    def __init__(
        self,
        *,
        output: OutputSpec = None,
        region: Optional[tuple] = None,
        fps: float = 30.0,
        cursor: bool = False,
        wait_for_damage: bool = False,
        pixel_format: str = "rgb24",
        buffers: int = 3,
        timeout: Optional[float] = 5.0,
        repeat_frames: bool = True,
        session: Optional[Session] = None,
        display: Optional[str] = None,
    ) -> None:
        if fps <= 0:
            raise ValueError(f"fps must be > 0, got {fps}")
        if str(pixel_format).lower() not in PIXFMT:
            raise ValueError(
                f"unknown pixel_format {pixel_format!r}; expected one of "
                f"{sorted(k for k in PIXFMT if len(k) > 3)}"
            )

        self._owns_session = session is None
        self._session = session or Session(display=display, buffers=buffers,
                                           cursor=cursor)
        self.output = output
        self.region = region
        self.fps = float(fps)
        self.cursor = cursor
        self.wait_for_damage = bool(wait_for_damage)
        self.pixel_format = str(pixel_format).lower()
        self.timeout = timeout
        self.repeat_frames = bool(repeat_frames)

        self._running = False
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._lock = threading.Lock()
        self._last: Optional[np.ndarray] = None
        self._next_due = 0.0
        self._stats = RecorderStats(fps_target=self.fps)
        self._channels = PIXFMT_CHANNELS[PIXFMT[self.pixel_format]]
        self._frame_info = None

    # ------------------------------------------------------------ lifecycle

    @property
    def session(self) -> Session:
        return self._session

    @property
    def frame_shape(self):
        """``(H, W, C)`` of frames this recorder produces (after start())."""
        if self._frame_info is None:
            self._frame_info = self._session.probe(self.output, self.region,
                                                   cursor=self.cursor)
        return (self._frame_info.height, self._frame_info.width, self._channels)

    def start(self) -> "Recorder":
        """Probe the geometry and start counting. Safe to call twice."""
        if self._running:
            return self
        self._frame_info = self._session.probe(self.output, self.region,
                                               cursor=self.cursor)
        if self._frame_info.width <= 0 or self._frame_info.height <= 0:
            raise WlScreencopyError(
                f"output produced a {self._frame_info.width}x"
                f"{self._frame_info.height} frame; nothing to record"
            )
        self._stop.clear()
        self._running = True
        self._next_due = time.monotonic()
        self._stats = RecorderStats(fps_target=self.fps,
                                    started_at=time.monotonic())
        return self

    def stop(self) -> None:
        self._stop.set()
        self._running = False
        if self._thread is not None:
            self._thread.join(timeout=10)
            self._thread = None

    def close(self) -> None:
        self.stop()
        if self._owns_session:
            self._session.close()

    def __enter__(self) -> "Recorder":
        return self.start()

    def __exit__(self, *exc) -> None:
        self.close()

    # ---------------------------------------------------------------- stats

    def stats(self) -> RecorderStats:
        with self._lock:
            s = self._stats
            elapsed = (time.monotonic() - s.started_at if s.started_at else 0.0)
            return RecorderStats(
                frames=s.frames,
                timeouts=s.timeouts,
                repeated=s.repeated,
                bytes_captured=s.bytes_captured,
                started_at=s.started_at,
                elapsed=elapsed,
                fps_target=self.fps,
                fps_actual=(s.frames / elapsed if elapsed > 0 else 0.0),
            )

    # -------------------------------------------------------------- capture

    def read(self, out: Optional[np.ndarray] = None,
             timeout: Optional[float] = None) -> np.ndarray:
        """Capture the next frame, honouring the fps cap.

        ``out`` (optional) is filled in place - use it in a loop to avoid a
        fresh allocation per frame. On a compositor that stopped committing
        (a blank screen), ``FrameTimeoutError`` propagates unless
        ``repeat_frames`` is set, in which case the previous frame is returned
        again so a recording keeps its timing.
        """
        if not self._running:
            self.start()

        frame_period = 1.0 / self.fps
        with self._lock:
            due = self._next_due
            now = time.monotonic()

        wait = due - now
        if wait > 0:
            time.sleep(min(wait, 0.5) if wait > 0.5 else wait)
        elif wait < -frame_period * 4:
            # We are way behind (consumer too slow): resync instead of
            # spinning to catch up.
            with self._lock:
                self._next_due = now

        cap_timeout = timeout if timeout is not None else self.timeout

        try:
            frame = self._session.capture(
                self.output, self.region, cursor=self.cursor,
                wait_for_damage=self.wait_for_damage, timeout=cap_timeout,
                pixel_format=self.pixel_format, out=out,
            )
            repeated = False
        except FrameTimeoutError:
            if not self.repeat_frames or self._last is None:
                raise
            prev = self._last
            if out is not None and prev.shape == out.shape:
                np.copyto(out, prev)
                frame = out
            else:
                frame = prev
            repeated = True

        with self._lock:
            self._next_due = max(self._next_due + frame_period, time.monotonic())
            self._stats.frames += 1
            if repeated:
                self._stats.repeated += 1
                self._stats.timeouts += 1
            else:
                self._last = np.array(frame, copy=True) if out is None else frame
                self._stats.bytes_captured += frame.nbytes

        return frame

    # ------------------------------------------------------------- streams

    def frames(self, out: Optional[np.ndarray] = None,
               max_frames: Optional[int] = None) -> Iterator[np.ndarray]:
        """Iterate frames until :meth:`stop`, ``max_frames``, or an error."""
        count = 0
        while not self._stop.is_set():
            frame = self.read(out=out)
            yield frame
            count += 1
            if max_frames is not None and count >= max_frames:
                return

    def record(
        self,
        path: Optional[str] = None,
        *,
        duration: Optional[float] = None,
        max_frames: Optional[int] = None,
        writer: Optional[VideoWriter] = None,
        codec: str = "auto",
        crf: int = 23,
        preset: str = "veryfast",
        on_frame: Optional[Callable[[np.ndarray, int], None]] = None,
        ffmpeg: Optional[str] = None,
    ) -> RecorderStats:
        """Record to a video file (default) and return final stats.

        ``path`` may be None when a pre-built ``writer`` is supplied. Pass
        ``writer=None`` plus a callback via ``on_frame`` to consume frames
        yourself instead of encoding.
        """
        if path is None and writer is None and on_frame is None:
            raise ValueError(
                "record() needs a path, a writer, or an on_frame callback "
                "(otherwise nothing would consume the captured frames)"
            )

        self.start()
        owns_writer = writer is None and path is not None
        if owns_writer:
            h, w, c = self.frame_shape
            if c != 3:
                raise ValueError(
                    "video writing expects 3-channel RGB frames; use "
                    "pixel_format='rgb24' (or pass your own writer)"
                )
            writer = VideoWriter(path, w, h, fps=self.fps, codec=codec,
                                 crf=crf, preset=preset, ffmpeg=ffmpeg)

        end_at = time.monotonic() + duration if duration else None
        index = 0
        try:
            for frame in self.frames():
                if writer is not None:
                    writer.write(frame)
                if on_frame is not None:
                    on_frame(frame, index)
                index += 1
                if max_frames is not None and index >= max_frames:
                    break
                if end_at is not None and time.monotonic() >= end_at:
                    break
        finally:
            if owns_writer and writer is not None:
                writer.close()
        return self.stats()

    # ---------------------------------------------------------- background

    def start_thread(
        self,
        target: Callable[[Iterator[np.ndarray]], None],
        **iter_kwargs,
    ) -> threading.Thread:
        """Run ``target(recorder.frames())`` on a background thread."""
        if self._thread is not None and self._thread.is_alive():
            raise WlScreencopyError("a recorder thread is already running")

        def run():
            try:
                target(self.frames(**iter_kwargs))
            except Exception:  # surfaced through the exception attribute
                self._thread_error = __import__("traceback").format_exc()

        self._thread_error = None
        self._thread = threading.Thread(target=run, name="wl-screencopy",
                                        daemon=True)
        self._thread.start()
        return self._thread

    @property
    def thread_error(self) -> Optional[str]:
        return getattr(self, "_thread_error", None)

    def __repr__(self) -> str:
        state = "running" if self._running else "idle"
        try:
            shape = "x".join(str(v) for v in self.frame_shape)
        except Exception:
            shape = "?"
        return (f"<Recorder {shape} {self.fps:g}fps output={self.output!r} "
                f"{state}>")
