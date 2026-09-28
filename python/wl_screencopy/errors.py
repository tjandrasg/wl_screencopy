"""Exception types mapped from the C status codes."""

from __future__ import annotations

__all__ = [
    "WlScreencopyError",
    "CompositorUnsupportedError",
    "InvalidError",
    "NoMemoryError",
    "FrameTimeoutError",
    "CaptureFailedError",
    "DisconnectedError",
    "ProtocolError",
    "BusyError",
    "error_for_status",
]


class WlScreencopyError(RuntimeError):
    """Base class for every capture error.

    ``message`` contains the detailed explanation produced by the native
    library, which usually says exactly what the compositor did.
    """

    status = -1

    def __init__(self, message: str, *, status: int | None = None) -> None:
        super().__init__(message)
        if status is not None:
            self.status = status


class CompositorUnsupportedError(WlScreencopyError):
    """The compositor does not implement wlr-screencopy-unstable-v1."""

    status = -2


class InvalidError(WlScreencopyError, ValueError):
    """Bad arguments: unknown output, empty region, destination too small.

    Also derives from ``ValueError`` so the usual Python idiom
    (``except ValueError``) works for argument mistakes.
    """

    status = -3


class NoMemoryError(WlScreencopyError):
    """Shared memory or buffer allocation failed."""

    status = -4


class FrameTimeoutError(WlScreencopyError):
    """No frame arrived before the deadline."""

    status = -5


class CaptureFailedError(WlScreencopyError):
    """The compositor refused the frame (``zwlr_screencopy_frame.failed``)."""

    status = -6


class DisconnectedError(WlScreencopyError):
    """The Wayland connection is gone (compositor restart?)."""

    status = -7


class ProtocolError(WlScreencopyError):
    """The compositor reported a protocol error."""

    status = -8


class BusyError(WlScreencopyError):
    """Another capture is in flight on this session (not thread safe)."""

    status = -9


_BY_STATUS: dict[int, type[WlScreencopyError]] = {
    -1: WlScreencopyError,
    -2: CompositorUnsupportedError,
    -3: InvalidError,
    -4: NoMemoryError,
    -5: FrameTimeoutError,
    -6: CaptureFailedError,
    -7: DisconnectedError,
    -8: ProtocolError,
    -9: BusyError,
}


def error_for_status(status: int, message: str) -> WlScreencopyError:
    cls = _BY_STATUS.get(int(status), WlScreencopyError)
    return cls(message, status=int(status))
