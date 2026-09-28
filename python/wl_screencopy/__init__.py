"""wl_screencopy - Wayland screen capture and recording, straight into NumPy.

Speaks the ``wlr-screencopy-unstable-v1`` protocol (the one wlroots-based
compositors implement: Sway, Hyprland, river, labwc, Cage, Wayfire, Mudlark,
FXWM, ...), copies frames into shared memory and hands them to you as packed
RGB arrays.

Quick start
-----------
::

    import wl_screencopy as wcs

    frame = wcs.capture()              # np.ndarray, uint8, (H, W, 3), RGB
    print(wcs.outputs())               # what can be captured

    with wcs.Recorder(fps=60) as rec:  # record 20 s of the primary output
        rec.record("clip.mp4", duration=20.0)

Iterating instead of encoding::

    with wcs.Recorder(fps=30, cursor=True) as rec:
        for frame in rec.frames(max_frames=300):
            ...

Notes
-----
* Frames are top-down; the compositor's ``y-invert`` flag is applied for you.
* Regions are given in *logical* pixels; the returned array is in *device*
  pixels (logical x scale), the same coordinate system grim uses.
* A Wayland connection must be driven by one thread; :class:`Session`
  serialises its methods with a lock, so multi-threaded use is safe but
  captures will queue behind each other.
"""

from __future__ import annotations

from typing import Optional, Tuple, Union

import numpy as np

from . import _lib
from .errors import (
    BusyError,
    CaptureFailedError,
    CompositorUnsupportedError,
    DisconnectedError,
    FrameTimeoutError,
    InvalidError,
    NoMemoryError,
    ProtocolError,
    WlScreencopyError,
)
from .recorder import Recorder, RecorderStats
from .session import OutputSpec, Session
from .types import FrameInfo, Output, Stats
from .video import VideoWriter, VideoWriterError

__version__ = "0.1.0"

__all__ = [
    # capture
    "capture", "grab", "probe",
    # discovery
    "outputs", "output_info", "backends", "supports_screencopy",
    # objects
    "Session", "Recorder", "RecorderStats", "Output", "FrameInfo", "Stats",
    "VideoWriter", "VideoWriterError",
    # errors
    "WlScreencopyError", "CompositorUnsupportedError", "InvalidError",
    "NoMemoryError", "FrameTimeoutError", "CaptureFailedError",
    "DisconnectedError", "ProtocolError", "BusyError",
    # helpers
    "save_image", "save_png", "open_session", "default_session",
    "native_version", "__version__",
]

# A process-wide session so `wcs.capture()` needs no setup.
_default_session: Optional[Session] = None
_default_lock = __import__("threading").Lock()


def open_session(
    display: Optional[str] = None,
    *,
    buffers: int = 2,
    cursor: bool = False,
) -> Session:
    """Open an explicit session (call :meth:`Session.close` when done)."""
    return Session(display=display, buffers=buffers, cursor=cursor)


def default_session(display: Optional[str] = None, *, reopen: bool = False) -> Session:
    """Return the shared session, creating it on first use.

    Pass ``reopen=True`` (e.g. after a compositor restart) to drop the cached
    connection and connect again.
    """
    global _default_session
    with _default_lock:
        if reopen or _default_session is None or _default_session.closed:
            if _default_session is not None:
                try:
                    _default_session.close()
                except Exception:
                    pass
            _default_session = Session(display=display)
        return _default_session


def close() -> None:
    """Close the shared session (mostly for tests and short-lived scripts)."""
    global _default_session
    with _default_lock:
        if _default_session is not None:
            try:
                _default_session.close()
            finally:
                _default_session = None


# ------------------------------------------------------------- capture API

def capture(
    output: OutputSpec = None,
    region: Optional[Tuple[int, int, int, int]] = None,
    *,
    cursor: Optional[bool] = None,
    wait_for_damage: bool = False,
    timeout: Optional[float] = 2.0,
    pixel_format: str = "rgb24",
    out: Optional[np.ndarray] = None,
    copy: bool = True,
    display: Optional[str] = None,
) -> np.ndarray:
    """Grab one screen frame as a NumPy RGB array.

    Args:
        output: ``None`` (preferred output), an index, or a name such as
            ``"DP-1"`` / ``"eDP-1"`` (case-insensitive substring match on the
            description too).
        region: ``(x, y, w, h)`` in logical pixels within that output.
        cursor: composite the mouse pointer into the image.
        wait_for_damage: block until the screen actually changes
            (``copy_with_damage``); much cheaper when idle.
        timeout: seconds to wait for the compositor.
        pixel_format: ``"rgb24"`` -> ``(H, W, 3)``; also ``"rgba"``,
            ``"bgr24"`` (OpenCV), ``"bgra"``, ``"rgbx"``, ``"bgrx"``.
        out: an existing ``uint8`` array to fill, avoiding an allocation per
            frame. Shape must be ``(H, W, channels)``.
        copy: with ``out=None``, copy the library buffer (default True).
            ``copy=False`` returns a view valid only until the next capture.

    Returns:
        ``numpy.ndarray`` of dtype ``uint8``, shape ``(H, W, 3)`` for RGB24.

    Raises:
        CompositorUnsupportedError: no ``zwlr_screencopy_manager_v1`` here.
        FrameTimeoutError: the compositor did not deliver a frame.
        CaptureFailedError: the compositor refused the frame.
        DisconnectedError: the Wayland connection died.
    """
    session = default_session(display)
    return session.capture(
        output, region, cursor=cursor, wait_for_damage=wait_for_damage,
        timeout=timeout, pixel_format=pixel_format, out=out, copy=copy,
    )


def grab(
    output: OutputSpec = None,
    region: Optional[Tuple[int, int, int, int]] = None,
    **kwargs,
) -> Tuple[np.ndarray, FrameInfo]:
    """Like :func:`capture`, but also returns frame metadata (pts, damage)."""
    return default_session().grab(output, region, **kwargs)


def probe(output: OutputSpec = None, region=None, **kwargs) -> FrameInfo:
    """What size would a capture be? Does not transfer pixels."""
    return default_session().probe(output, region, **kwargs)


# ----------------------------------------------------------- discovery API

def outputs(refresh: bool = False) -> list:
    """List the outputs (monitors) the compositor exposes."""
    return default_session().outputs(refresh=refresh)


def output_info(spec: OutputSpec = None) -> Output:
    """Resolve an output spec to an :class:`Output`."""
    return default_session().output(spec)


def backends() -> dict:
    """Screencopy backends offered by the compositor, with protocol versions."""
    return default_session().backends


def supports_screencopy() -> bool:
    """True if this compositor can be recorded at all."""
    return default_session().supports_screencopy


def native_version() -> str:
    """Version string of the loaded native library."""
    return _lib.native.version()


# --------------------------------------------------------------- image I/O

def save_image(arr: np.ndarray, path: str, *, quality: int = 95) -> str:
    """Save an ``(H, W, 3)`` frame as PNG/JPEG.

    Uses Pillow when it is installed, otherwise falls back to ffmpeg. PNG is
    the safe default for screenshots.
    """
    arr = np.asarray(arr)
    if arr.ndim == 3 and arr.shape[2] == 4:
        arr = arr[:, :, :3]
    if arr.ndim != 3 or arr.shape[2] != 3:
        raise ValueError(f"expected (H, W, 3) uint8, got shape {arr.shape}")

    try:
        from PIL import Image  # type: ignore
    except ImportError:
        Image = None

    if Image is not None:
        Image.fromarray(arr, mode="RGB").save(path)
        return path

    # Fallback: ffmpeg can write single images as well.
    from .video import find_ffmpeg
    import subprocess

    h, w = arr.shape[:2]
    codec = "png" if path.lower().endswith(".png") else "mjpeg"
    exe = find_ffmpeg()
    cmd = [exe, "-y", "-loglevel", "error", "-f", "rawvideo",
           "-pix_fmt", "rgb24", "-s", f"{w}x{h}", "-i", "-",
           "-frames:v", "1", "-c:v", codec, path]
    proc = subprocess.run(cmd, input=np.ascontiguousarray(arr).tobytes(),
                          capture_output=True)
    if proc.returncode != 0:
        raise VideoWriterError(
            "cannot save image (install Pillow): "
            + proc.stderr.decode("utf-8", "replace")[-400:]
        )
    return path


#: alias, since screenshots are usually PNG
save_png = save_image
