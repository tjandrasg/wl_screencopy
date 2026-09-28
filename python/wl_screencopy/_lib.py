"""Low-level ctypes binding for ``libwl_screencopy``.

This module mirrors ``include/wl_screencopy.h``. It is deliberately thin:
structures, prototypes, and library discovery. Everything user friendly
lives in :mod:`wl_screencopy._session` and :mod:`wl_screencopy`.

If the import fails with "library not found", build it first::

    make            # writes python/wl_screencopy/_native/libwl_screencopy.so
"""

from __future__ import annotations

import ctypes
import ctypes.util
import os
import platform
from pathlib import Path
from typing import Optional

__all__ = [
    "native",
    "NativeError",
    "CFrame",
    "COutputInfo",
    "CGrabOptions",
    "CStats",
    "CSessionOptions",
    "STATUS",
    "PIXFMT",
    "BACKENDS",
]


class NativeError(RuntimeError):
    """Raised when the native library cannot be found or fails to load."""


# ---------------------------------------------------------------- discovery

def _library_filenames() -> list[str]:
    system = platform.system()
    if system == "Linux":
        return [
            "libwl_screencopy.so",
            "libwl_screencopy.so.0",
            "libwl_screencopy.so.0.1.0",
        ]
    if system == "Darwin":  # not a Wayland platform, but keep the loader sane
        return ["libwl_screencopy.dylib"]
    return ["wl_screencopy.dll"]


def _candidate_paths() -> list[Path]:
    pkg_dir = Path(__file__).resolve().parent
    names = _library_filenames()

    out: list[Path] = []

    # 1. Explicit override.
    env = os.environ.get("WL_SCREENCOPY_LIB")
    if env:
        p = Path(env).expanduser()
        out.append(p if p.is_absolute() else Path.cwd() / p)

    # 2. Shipped next to the package (built by `make native`).
    for name in names:
        out.append(pkg_dir / "_native" / name)

    # 3. Development tree: ../build/libwl_screencopy.so
    for parent in [pkg_dir.parent.parent, pkg_dir.parent]:
        for name in names:
            out.append(parent / "build" / name)

    # 4. Standard install locations.
    for prefix in ("/usr/local/lib", "/usr/lib", "/usr/lib64"):
        for name in names:
            out.append(Path(prefix) / name)

    # 5. Whatever the dynamic loader finds.
    found = ctypes.util.find_library("wl_screencopy")
    if found:
        out.append(Path(found))

    return out


def _load() -> ctypes.CDLL:
    tried: list[str] = []
    for path in _candidate_paths():
        try:
            if not path.exists():
                tried.append(f"{path} (missing)")
                continue
            dll = ctypes.CDLL(str(path))
        except OSError as exc:  # wrong arch, missing symbols, ...
            tried.append(f"{path} ({exc})")
            continue
        if hasattr(dll, "wsc_session_open"):
            return dll
        tried.append(f"{path} (no wsc_session_open symbol)")
    raise NativeError(
        "libwl_screencopy could not be loaded.\nTried:\n  "
        + "\n  ".join(tried)
        + "\n\nBuild it with `make` in the wl_screencopy source tree, or set "
        "WL_SCREENCOPY_LIB=/path/to/libwl_screencopy.so."
    )


_lib = _load()

# ---------------------------------------------------------------- constants

STATUS = {
    0: "ok",
    -1: "error",
    -2: "unsupported",
    -3: "invalid",
    -4: "no-memory",
    -5: "timeout",
    -6: "failed",
    -7: "disconnected",
    -8: "protocol",
    -9: "busy",
}

# enum wsc_pixfmt
PIXFMT = {
    "rgb24": 0,
    "rgb": 0,
    "rgba": 1,
    "bgr24": 2,
    "bgra": 3,
    "rgbx": 4,
    "bgrx": 5,
}
PIXFMT_NAME = {0: "rgb24", 1: "rgba", 2: "bgr24", 3: "bgra", 4: "rgbx", 5: "bgrx"}
PIXFMT_CHANNELS = {0: 3, 1: 4, 2: 3, 3: 4, 4: 4, 5: 4}
PIXFMT_NAMESPACES = {
    "rgb24": "rgb",
    "rgba": "rgba",
    "bgr24": "bgr",
    "bgra": "bgra",
    "rgbx": "rgbx",
    "bgrx": "bgrx",
}

BACKENDS = {
    1 << 0: "wlr-screencopy",
    1 << 1: "ext-image-copy-capture",
}


# ---------------------------------------------------------------- structs

class CSessionOptions(ctypes.Structure):
    _fields_ = [
        ("display_name", ctypes.c_char_p),
        ("buffer_count", ctypes.c_int32),
        ("cursor", ctypes.c_bool),
    ]


class COutputInfo(ctypes.Structure):
    _fields_ = [
        ("wl_name", ctypes.c_uint32),
        ("name", ctypes.c_char * 64),
        ("description", ctypes.c_char * 256),
        ("x", ctypes.c_int32),
        ("y", ctypes.c_int32),
        ("width", ctypes.c_int32),
        ("height", ctypes.c_int32),
        ("phys_width_mm", ctypes.c_int32),
        ("phys_height_mm", ctypes.c_int32),
        ("scale", ctypes.c_int32),
        ("effective_scale", ctypes.c_double),
        ("refresh", ctypes.c_int32),
        ("transform", ctypes.c_int32),
        ("preferred", ctypes.c_bool),
    ]


class CGrabOptions(ctypes.Structure):
    _fields_ = [
        ("output", ctypes.c_int32),
        ("x", ctypes.c_int32),
        ("y", ctypes.c_int32),
        ("width", ctypes.c_int32),
        ("height", ctypes.c_int32),
        ("cursor", ctypes.c_bool),
        ("wait_for_damage", ctypes.c_bool),
        ("timeout_ms", ctypes.c_int32),
        ("pixfmt", ctypes.c_int32),
    ]


class CFrame(ctypes.Structure):
    _fields_ = [
        ("data", ctypes.POINTER(ctypes.c_uint8)),
        ("width", ctypes.c_int32),
        ("height", ctypes.c_int32),
        ("stride", ctypes.c_int32),
        ("pixfmt", ctypes.c_int32),
        ("bpp", ctypes.c_int32),
        ("output", ctypes.c_int32),
        ("offset_x", ctypes.c_int32),
        ("offset_y", ctypes.c_int32),
        ("scale", ctypes.c_double),
        ("compositor_flags", ctypes.c_uint32),
        ("y_inverted", ctypes.c_bool),
        ("pts_us", ctypes.c_uint64),
        ("has_damage", ctypes.c_bool),
        ("damage_x", ctypes.c_int32),
        ("damage_y", ctypes.c_int32),
        ("damage_width", ctypes.c_int32),
        ("damage_height", ctypes.c_int32),
        ("stale", ctypes.c_bool),
    ]


class CStats(ctypes.Structure):
    _fields_ = [
        ("frames_ok", ctypes.c_uint64),
        ("frames_failed", ctypes.c_uint64),
        ("frames_timeout", ctypes.c_uint64),
        ("bytes_captured", ctypes.c_uint64),
        ("last_wait_us", ctypes.c_uint64),
        ("last_convert_us", ctypes.c_uint64),
        ("max_wait_us", ctypes.c_uint64),
    ]


def _check_abi() -> None:
    """Fail loudly if the Python struct layout and the C ABI disagree."""
    checks = [
        ("wsc_abi_sizeof_output_info", COutputInfo),
        ("wsc_abi_sizeof_frame", CFrame),
        ("wsc_abi_sizeof_grab_options", CGrabOptions),
        ("wsc_abi_sizeof_stats", CStats),
        ("wsc_abi_sizeof_session_options", CSessionOptions),
    ]
    for fn_name, struct in checks:
        try:
            fn = getattr(_lib, fn_name)
        except AttributeError:
            continue  # older library without ABI helpers
        fn.restype = ctypes.c_size_t
        c_size = int(fn())
        py_size = ctypes.sizeof(struct)
        if c_size != py_size:
            raise NativeError(
                f"ABI mismatch: sizeof({struct.__name__}) is {c_size} in "
                f"libwl_screencopy but {py_size} in the Python bindings. "
                "Rebuild the library (make) and/or update wl_screencopy."
            )


_check_abi()

# ---------------------------------------------------------------- prototypes

_void_p = ctypes.c_void_p
_u32 = ctypes.c_uint32
_i32 = ctypes.c_int32

_fn = _lib


def _proto(name, restype, argtypes):
    fn = getattr(_fn, name)
    fn.restype = restype
    fn.argtypes = argtypes
    return fn


_proto("wsc_session_open", _void_p, [ctypes.POINTER(CSessionOptions),
                                     ctypes.POINTER(ctypes.c_int)])
_proto("wsc_session_close", None, [_void_p])
_proto("wsc_session_backends", _u32, [_void_p])
_proto("wsc_session_wlr_version", _u32, [_void_p])
_proto("wsc_session_fd", _i32, [_void_p])
_proto("wsc_session_poll", ctypes.c_int, [_void_p, _i32])
_proto("wsc_session_refresh", _i32, [_void_p])
_proto("wsc_output_count", _i32, [_void_p])
_proto("wsc_output_get", ctypes.c_int, [_void_p, _i32, ctypes.POINTER(COutputInfo)])
_proto("wsc_output_find", _i32, [_void_p, ctypes.c_char_p])
_proto("wsc_session_error_string", ctypes.c_char_p, [_void_p])
_proto("wsc_status_str", ctypes.c_char_p, [ctypes.c_int])
_proto("wsc_pixfmt_str", ctypes.c_char_p, [ctypes.c_int])
_proto("wsc_pixfmt_bpp", _i32, [ctypes.c_int])
_proto("wsc_version_string", ctypes.c_char_p, [])
_proto("wsc_session_get_stats", ctypes.c_int, [_void_p, ctypes.POINTER(CStats)])
_proto("wsc_grab", ctypes.c_int, [_void_p, ctypes.POINTER(CGrabOptions),
                                  ctypes.POINTER(CFrame)])
_proto("wsc_grab_into", ctypes.c_int,
       [_void_p, ctypes.POINTER(CGrabOptions), _void_p, _i32,
        ctypes.POINTER(CFrame)])
_proto("wsc_probe", ctypes.c_int, [_void_p, ctypes.POINTER(CGrabOptions),
                                   ctypes.POINTER(CFrame)])


class _Native:
    """Namespaced access to the loaded library (mainly for debugging)."""

    lib = _lib
    path: Optional[str] = None

    @staticmethod
    def version() -> str:
        raw = _lib.wsc_version_string()
        return raw.decode("utf-8", "replace") if raw else "unknown"

    @staticmethod
    def status_str(code: int) -> str:
        raw = _lib.wsc_status_str(int(code))
        if raw:
            return raw.decode("utf-8", "replace")
        return STATUS.get(int(code), f"status {code}")


native = _Native()
