"""Session: a Wayland connection plus screencopy capture calls."""

from __future__ import annotations

import ctypes
import threading
from typing import List, Optional, Sequence, Tuple, Union

import numpy as np

from . import _lib
from .errors import (
    CompositorUnsupportedError,
    WlScreencopyError,
    error_for_status,
)
from .types import FrameInfo, Output, Stats, view_from_cframe, _decode

OutputSpec = Union[None, int, str, Output]


def _region_tuple(region) -> Tuple[int, int, int, int]:
    """Normalise a region argument to (x, y, width, height)."""
    if region is None:
        return (0, 0, 0, 0)
    if isinstance(region, np.ndarray):
        raise ValueError("region must be a (x, y, w, h) tuple, not an array")
    seq: Sequence[int] = region  # type: ignore[assignment]
    if len(seq) != 4:
        raise ValueError(f"region must have 4 values (x, y, w, h), got {tuple(seq)}")
    x, y, w, h = (int(v) for v in seq)
    if (w == 0) != (h == 0) or (w == 0 and (x or y)):
        raise ValueError(
            f"region {tuple(seq)} is not usable: pass None/(0,0,0,0) for the "
            "whole output, or a concrete x,y,w,h with w,h > 0"
        )
    if w < 0 or h < 0 or x < 0 or y < 0:
        raise ValueError(f"region coordinates must be >= 0, got {tuple(seq)}")
    return (x, y, w, h)


class Session:
    """One Wayland connection.

    A Wayland connection must be driven by a single thread, so the methods
    here are serialised with a lock: calling them from several threads is
    safe but they will block each other (a capture holds the lock while it
    waits for the compositor).
    """

    def __init__(
        self,
        display: Optional[str] = None,
        *,
        buffers: int = 2,
        cursor: bool = False,
        auto_refresh: bool = True,
    ) -> None:
        self._lock = threading.RLock()
        self._closed = False
        self._outputs_cache: List[Output] = []
        self._auto_refresh = auto_refresh
        self._default_cursor = bool(cursor)

        opts = _lib.CSessionOptions()
        opts.display_name = display.encode("utf-8") if display else None
        opts.buffer_count = int(buffers)
        opts.cursor = bool(cursor)

        status = ctypes.c_int(0)
        handle = _lib._lib.wsc_session_open(ctypes.byref(opts), ctypes.byref(status))
        self._handle = handle or None
        if not handle:
            message = _decode(_lib._lib.wsc_session_error_string(None))
            code = int(status.value)
            if code == 0:
                code = -7
            raise error_for_status(code, message or "cannot open Wayland session")

        # A compositor without the protocol still yields a usable session for
        # listing outputs; capture() then raises CompositorUnsupportedError.
        if not self.backends:
            # Cache the explanation now, before any later call clears it.
            self._unsupported_reason = (
                _decode(_lib._lib.wsc_session_error_string(handle))
                or "compositor does not advertise wlr-screencopy-unstable-v1"
            )
        else:
            self._unsupported_reason = None
        self._outputs(refresh=True)

    # ------------------------------------------------------------- plumbing

    @property
    def handle(self):
        if not self._handle:
            raise WlScreencopyError("session is closed")
        return self._handle

    @property
    def closed(self) -> bool:
        return self._closed

    def _err(self) -> str:
        return _decode(_lib._lib.wsc_session_error_string(self._handle)) or "unknown error"

    def _check(self, status: int) -> None:
        if status >= 0:
            return
        raise error_for_status(status, self._err())

    def close(self) -> None:
        """Release all Wayland resources. Idempotent."""
        with self._lock:
            if self._closed or not self._handle:
                return
            _lib._lib.wsc_session_close(self._handle)
            self._handle = None
            self._closed = True
            self._outputs_cache = []

    def __enter__(self) -> "Session":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def __del__(self):  # pragma: no cover - best effort
        try:
            self.close()
        except Exception:
            pass

    def fileno(self) -> int:
        """Wayland socket fd (for ``select``/``asyncio`` integration)."""
        return int(_lib._lib.wsc_session_fd(self.handle))

    def poll(self, timeout_ms: int = 0) -> bool:
        """Pump Wayland events; returns True if events were handled."""
        return _lib._lib.wsc_session_poll(self.handle, int(timeout_ms)) == 0

    # ------------------------------------------------------------- discovery

    @property
    def backends(self) -> dict:
        """Mapping of backend name -> protocol version, e.g. ``{'wlr-screencopy': 3}``."""
        mask = int(_lib._lib.wsc_session_backends(self.handle))
        out: dict = {}
        if mask & 1:
            out["wlr-screencopy"] = int(_lib._lib.wsc_session_wlr_version(self.handle))
        if mask & 2:
            out["ext-image-copy-capture"] = 1
        return out

    @property
    def supports_screencopy(self) -> bool:
        return bool(self.backends)

    @property
    def unsupported_reason(self) -> Optional[str]:
        """Why capture is unavailable, or None when it is available."""
        return self._unsupported_reason

    def outputs(self, refresh: bool = False) -> List[Output]:
        """List the outputs (monitors). Cached; pass ``refresh=True`` to re-read."""
        with self._lock:
            if refresh or not self._outputs_cache:
                self._outputs(refresh=True)
            return list(self._outputs_cache)

    def _outputs(self, refresh: bool = False) -> List[Output]:
        with self._lock:
            if refresh:
                _lib._lib.wsc_session_refresh(self.handle)
            count = int(_lib._lib.wsc_output_count(self.handle))
            out: List[Output] = []
            info = _lib.COutputInfo()
            for i in range(max(count, 0)):
                if _lib._lib.wsc_output_get(self.handle, i, ctypes.byref(info)) == 0:
                    out.append(Output.from_c(info, i))
            self._outputs_cache = out
            return out

    def output(self, spec: OutputSpec = None) -> Output:
        """Resolve an output spec (None/first, index, or name) to an :class:`Output`."""
        idx = self.output_index(spec)
        outs = self.outputs()
        if not (0 <= idx < len(outs)):
            raise IndexError(f"output index {idx} out of range ({len(outs)} outputs)")
        return outs[idx]

    def output_index(self, spec: OutputSpec = None) -> int:
        """Resolve an output spec to an index. ``None`` means the preferred one."""
        if spec is None:
            outs = self.outputs()
            if not outs:
                raise WlScreencopyError(
                    "the compositor reports no outputs; is a monitor connected?"
                )
            return 0
        if isinstance(spec, Output):
            return spec.index
        if isinstance(spec, bool):
            raise TypeError("output must be an index, a name, or None")
        if isinstance(spec, int):
            outs = self.outputs()
            if not 0 <= spec < len(outs):
                raise IndexError(
                    f"no output index {spec}; the compositor exposes "
                    f"{len(outs)} output(s): {[o.name for o in outs]}"
                )
            return spec
        name = str(spec)
        with self._lock:
            idx = int(_lib._lib.wsc_output_find(self.handle, name.encode("utf-8")))
            if idx < 0:
                # Names change at runtime: refresh once before giving up.
                self._outputs(refresh=True)
                idx = int(_lib._lib.wsc_output_find(self.handle, name.encode("utf-8")))
            if idx < 0:
                outs = self.outputs()
                raise KeyError(
                    f"no output matching {name!r}; available: "
                    f"{[(o.index, o.name) for o in outs]}"
                )
            return idx

    # --------------------------------------------------------------- capture

    def _make_options(
        self,
        output: OutputSpec,
        region,
        cursor: Optional[bool],
        wait_for_damage: bool,
        timeout: Optional[float],
        pixel_format: str,
    ) -> _lib.CGrabOptions:
        if not self.backends:
            raise CompositorUnsupportedError(
                self._unsupported_reason
                or "compositor does not advertise wlr-screencopy-unstable-v1"
            )
        fmt_key = str(pixel_format).lower().replace("-", "")
        if fmt_key not in _lib.PIXFMT:
            raise ValueError(
                f"unknown pixel_format {pixel_format!r}; expected one of "
                f"{sorted(k for k in _lib.PIXFMT if len(k) > 3)}"
            )
        x, y, w, h = _region_tuple(region)
        opts = _lib.CGrabOptions()
        opts.output = self.output_index(output)
        opts.x, opts.y, opts.width, opts.height = x, y, w, h
        opts.cursor = bool(self._default_cursor if cursor is None else cursor)
        opts.wait_for_damage = bool(wait_for_damage)
        opts.timeout_ms = int(round((2.0 if timeout is None else timeout) * 1000))
        opts.pixfmt = _lib.PIXFMT[fmt_key]
        return opts

    def capture(
        self,
        output: OutputSpec = None,
        region=None,
        *,
        cursor: Optional[bool] = None,
        wait_for_damage: bool = False,
        timeout: Optional[float] = None,
        pixel_format: str = "rgb24",
        out: Optional[np.ndarray] = None,
        copy: bool = True,
    ) -> np.ndarray:
        """Capture one frame as a NumPy array.

        Parameters
        ----------
        output : None | int | str | Output
            ``None`` uses the preferred output; an ``int`` is an index and a
            ``str`` is matched against connector names / descriptions.
        region : (x, y, w, h) | None
            Logical pixels, relative to the top-left of that output. The
            returned frame is in device pixels, i.e. ``w * scale`` wide.
        cursor : bool | None
            Ask the compositor to composite the pointer. ``None`` keeps the
            session default (False).
        wait_for_damage : bool
            Block until the screen content changed (``copy_with_damage``).
            Cheaper than polling on an idle desktop.
        timeout : float | None
            Seconds to wait for the compositor; default 2.0.
        pixel_format : "rgb24" | "rgba" | "bgr24" | "bgra" | "rgbx" | "bgrx"
            ``"rgb24"`` (default) gives the ``(H, W, 3)`` RGB array.
        out : numpy.ndarray | None
            Fill this array instead of allocating one - the fast path when
            capturing in a loop. Must be uint8 with shape ``(H, W, C)`` and a
            matching channel count; it does not need to be contiguous.
        copy : bool
            With ``out=None``, whether to copy the library-owned buffer
            (default True). ``copy=False`` returns a view that stays valid
            only until the next one or two captures.

        Returns
        -------
        numpy.ndarray
            ``uint8`` array of shape ``(H, W, 3)`` (rgb24/bgr24) or
            ``(H, W, 4)`` (rgba/bgra/rgbx/bgrx), rows top-down.
        """
        opts = self._make_options(output, region, cursor, wait_for_damage,
                                  timeout, pixel_format)
        channels = _lib.PIXFMT_CHANNELS[opts.pixfmt]

        if out is not None:
            info = self.grab_into(out, opts)
            return out

        cframe = _lib.CFrame()
        with self._lock:
            status = int(_lib._lib.wsc_grab(self.handle, ctypes.byref(opts),
                                            ctypes.byref(cframe)))
            self._check(status)
            view = view_from_cframe(cframe)
            # The buffer belongs to the library and is recycled, so copy by
            # default; copy=False hands back the live view for hot loops.
            return np.array(view, copy=True) if copy else view

    def grab_into(self, out: np.ndarray, opts: _lib.CGrabOptions) -> FrameInfo:
        """Capture directly into a caller-owned array (single conversion pass)."""
        arr = np.asarray(out)
        if arr.dtype != np.uint8:
            raise TypeError(f"out must have dtype uint8, got {arr.dtype}")
        if arr.ndim != 3 or arr.shape[2] not in (3, 4):
            raise ValueError(
                f"out must be shaped (H, W, 3) or (H, W, 4), got {arr.shape}"
            )
        channels = _lib.PIXFMT_CHANNELS[opts.pixfmt]
        if arr.shape[2] != channels:
            raise ValueError(
                f"out has {arr.shape[2]} channels but the requested pixel "
                f"format '{_lib.PIXFMT_NAME[opts.pixfmt]}' produces {channels}"
            )
        if arr.strides[2] != 1:
            raise ValueError(
                "out must have contiguous channels (last stride must be 1); "
                "e.g. pass np.ascontiguousarray(your_view)"
            )
        row_stride = int(arr.strides[0])
        if row_stride < arr.shape[1] * channels:
            raise ValueError(
                f"out row stride is {row_stride}, which cannot hold "
                f"{arr.shape[1]} * {channels} bytes"
            )

        cframe = _lib.CFrame()
        with self._lock:
            status = int(_lib._lib.wsc_grab_into(
                self.handle, ctypes.byref(opts),
                ctypes.c_void_p(arr.ctypes.data), row_stride,
                ctypes.byref(cframe)))
            self._check(status)
        return _frame_info(cframe)

    def grab(
        self,
        output: OutputSpec = None,
        region=None,
        **kwargs,
    ) -> Tuple[np.ndarray, FrameInfo]:
        """Capture and also return the frame metadata (pts, damage, scale)."""
        opts = self._make_options(
            output, region, kwargs.pop("cursor", None),
            kwargs.pop("wait_for_damage", False), kwargs.pop("timeout", None),
            kwargs.pop("pixel_format", "rgb24"),
        )
        if kwargs:
            raise TypeError(f"unexpected keyword arguments: {sorted(kwargs)}")
        cframe = _lib.CFrame()
        with self._lock:
            status = int(_lib._lib.wsc_grab(self.handle, ctypes.byref(opts),
                                            ctypes.byref(cframe)))
            self._check(status)
            return view_from_cframe(cframe), _frame_info(cframe)

    def probe(self, output: OutputSpec = None, region=None, **kwargs) -> FrameInfo:
        """Return the geometry a capture would produce, without pixel transfer."""
        opts = self._make_options(output, region,
                                  kwargs.pop("cursor", None), False,
                                  kwargs.pop("timeout", None),
                                  kwargs.pop("pixel_format", "rgb24"))
        opts.wait_for_damage = False
        cframe = _lib.CFrame()
        with self._lock:
            status = int(_lib._lib.wsc_probe(self.handle, ctypes.byref(opts),
                                             ctypes.byref(cframe)))
            self._check(status)
        return _frame_info(cframe)

    # --------------------------------------------------------------- metrics

    def stats(self) -> Stats:
        cs = _lib.CStats()
        with self._lock:
            _lib._lib.wsc_session_get_stats(self.handle, ctypes.byref(cs))
        return Stats.from_c(cs)

    def __repr__(self) -> str:
        if self._closed:
            return "<wl_screencopy.Session closed>"
        return (f"<wl_screencopy.Session backends={self.backends} "
                f"outputs={[o.name for o in self.outputs()]}>")


def _frame_info(cf: _lib.CFrame) -> FrameInfo:
    from .types import _decode_cframe

    return _decode_cframe(cf)
