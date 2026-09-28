"""Output and frame value types, plus the C->NumPy bridge."""

from __future__ import annotations

import ctypes
from dataclasses import dataclass
from typing import Optional, Tuple

import numpy as np

from . import _lib

PIX_FMT_SHAPE = {
    "rgb24": 3,
    "rgba": 4,
    "bgr24": 3,
    "bgra": 4,
    "rgbx": 4,
    "bgrx": 4,
}


def _decode(raw: bytes | None) -> str:
    return raw.decode("utf-8", "replace") if raw else ""


@dataclass(frozen=True)
class Output:
    """A monitor as the compositor sees it."""

    index: int
    name: str
    description: str
    x: int
    y: int
    width: int
    height: int
    scale: int
    refresh_mhz: int
    physical_width_mm: int
    physical_height_mm: int
    transform: int
    preferred: bool
    wl_name: int = 0

    @classmethod
    def from_c(cls, info: _lib.COutputInfo, index: int) -> "Output":
        return cls(
            index=index,
            name=_decode(info.name),
            description=_decode(info.description).strip(),
            x=info.x,
            y=info.y,
            width=info.width,
            height=info.height,
            scale=info.scale,
            refresh_mhz=info.refresh,
            physical_width_mm=info.phys_width_mm,
            physical_height_mm=info.phys_height_mm,
            transform=info.transform,
            preferred=bool(info.preferred),
            wl_name=info.wl_name,
        )

    @property
    def refresh_hz(self) -> float:
        return self.refresh_mhz / 1000.0

    @property
    def logical_size(self) -> Tuple[int, int]:
        return (self.width, self.height)

    @property
    def diagonal_inches(self) -> float:
        if not self.physical_width_mm or not self.physical_height_mm:
            return 0.0
        import math

        return math.hypot(self.physical_width_mm, self.physical_height_mm) / 25.4

    def __str__(self) -> str:
        star = "*" if self.preferred else " "
        size = f"{self.width}x{self.height}"
        rate = f"@{self.refresh_hz:.2f}Hz" if self.refresh_mhz else ""
        scale = f" x{self.scale}" if self.scale != 1 else ""
        return (f"[{self.index}]{star} {self.name or '?':<10} {size}{rate}{scale}"
                f" at ({self.x},{self.y}) {self.description}".rstrip())


@dataclass(frozen=True)
class Stats:
    """Counters from the native library, handy for spotting a slow pipeline."""

    frames_ok: int = 0
    frames_failed: int = 0
    frames_timeout: int = 0
    bytes_captured: int = 0
    last_wait_ms: float = 0.0
    last_convert_ms: float = 0.0
    max_wait_ms: float = 0.0

    @classmethod
    def from_c(cls, s: _lib.CStats) -> "Stats":
        return cls(
            frames_ok=s.frames_ok,
            frames_failed=s.frames_failed,
            frames_timeout=s.frames_timeout,
            bytes_captured=s.bytes_captured,
            last_wait_ms=s.last_wait_us / 1e3,
            last_convert_ms=s.last_convert_us / 1e3,
            max_wait_ms=s.max_wait_us / 1e3,
        )


@dataclass
class FrameInfo:
    """Metadata of a captured frame (the pixels live in ``array``)."""

    width: int
    height: int
    output_index: int
    scale: float
    pixel_format: str
    pts_us: int
    y_inverted: bool
    compositor_flags: int
    damage: Optional[Tuple[int, int, int, int]]
    offset: Tuple[int, int] = (0, 0)

    @property
    def pts(self) -> float:
        """Presentation timestamp in seconds (compositor monotonic clock)."""
        return self.pts_us / 1e6

    @property
    def shape(self) -> Tuple[int, int, int]:
        return (self.height, self.width, PIX_FMT_SHAPE[self.pixel_format])


def _decode_cframe(cf: _lib.CFrame) -> FrameInfo:
    pixfmt = _lib.PIXFMT_NAME.get(int(cf.pixfmt), "rgb24")
    damage = None
    if cf.has_damage:
        damage = (cf.damage_x, cf.damage_y, cf.damage_width, cf.damage_height)
    return FrameInfo(
        width=int(cf.width),
        height=int(cf.height),
        output_index=int(cf.output),
        scale=float(cf.scale),
        pixel_format=pixfmt,
        pts_us=int(cf.pts_us),
        y_inverted=bool(cf.y_inverted),
        compositor_flags=int(cf.compositor_flags),
        damage=damage,
        offset=(int(cf.offset_x), int(cf.offset_y)),
    )


def view_from_cframe(cf: _lib.CFrame) -> np.ndarray:
    """Zero-copy NumPy view of a library-owned frame buffer.

    The memory belongs to the library and is recycled after a few further
    grabs, so use this only when you consume it immediately; otherwise pass
    ``copy=True`` to :func:`wl_screencopy.capture`.
    """
    pixfmt = _lib.PIXFMT_NAME.get(int(cf.pixfmt), "rgb24")
    channels = PIX_FMT_SHAPE[pixfmt]
    nbytes = int(cf.stride) * int(cf.height)
    buf = (ctypes.c_ubyte * nbytes).from_address(
        ctypes.cast(cf.data, ctypes.c_void_p).value
    )
    flat = np.frombuffer(buf, dtype=np.uint8)
    stride = int(cf.stride)
    width = int(cf.width)
    height = int(cf.height)
    row_bytes = width * channels
    if stride == row_bytes:
        return flat.reshape((height, width, channels))
    # Padded rows: expose a strided view without copying.
    return np.lib.stride_tricks.as_strided(
        flat,
        shape=(height, width, channels),
        strides=(stride, channels, 1),
        writeable=False,
    )
