"""Test fixtures: a nested wlroots compositor plus the known-pattern client.

The whole suite runs against a *second* compositor (cage by default) nested
inside whatever session is active, so it never disturbs the user's desktop and
works even on compositors that have no screencopy support at all.

Knobs::

    WSC_TEST_COMPOSITOR=/usr/bin/labwc pytest   # specific compositor
    WSC_NO_CLIENT=1 pytest                      # skip pixel-exactness tests

See tests/README.md for how to obtain a compositor without root.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, List, Optional

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))
sys.path.insert(0, str(ROOT / "tests"))

from pattern import pattern_frame  # noqa: E402

COMPOSITOR_CANDIDATES = ["cage", "labwc", "wayfire", "river", "sway", "weston"]


def _find_compositor() -> Optional[str]:
    env = os.environ.get("WSC_TEST_COMPOSITOR")
    if env:
        return env if Path(env).exists() and os.access(env, os.X_OK) else None
    for name in COMPOSITOR_CANDIDATES:
        found = shutil.which(name)
        if found:
            return found
    # Locally provided test compositor / unpacked .deb trees (no root needed).
    for directory in (".testbin", ".local/bin", ".probe_debs"):
        base = ROOT / directory
        if not base.exists():
            continue
        for name in COMPOSITOR_CANDIDATES:
            direct = base / name
            if direct.exists() and os.access(direct, os.X_OK):
                return str(direct)
            hits = sorted(base.rglob(name))
            if hits and os.access(hits[0], os.X_OK):
                return str(hits[0])
    return None


def _parent_socket() -> Optional[str]:
    display = os.environ.get("WAYLAND_DISPLAY", "wayland-0")
    if display.startswith("/"):
        return display if Path(display).exists() else None
    runtime = os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}")
    path = Path(runtime) / display
    return str(path) if path.exists() else None


def _wait_for_socket(runtime_dir: Path, timeout: float = 15.0) -> Optional[Path]:
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        sockets = [p for p in runtime_dir.iterdir()
                   if p.is_socket() and not p.name.endswith(".lock")]
        if sockets:
            return sorted(sockets)[0]
        time.sleep(0.05)
    return None


@dataclass
class Nested:
    """A running nested compositor plus a shutdown hook."""

    socket: str
    compositor: str
    width: int
    height: int
    log_dir: Path
    processes: List[subprocess.Popen] = field(default_factory=list)
    client: Optional[subprocess.Popen] = None

    def session_kwargs(self) -> dict:
        """Keyword arguments that point a Session at this compositor."""
        return {"display": self.socket}

    @property
    def client_alive(self) -> bool:
        return self.client is not None and self.client.poll() is None

    def shutdown(self) -> None:
        for proc in [self.client, *self.processes]:
            if proc is None or proc.poll() is not None:
                continue
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


def launch_nested(runtime: Path,
                  client_builder: Optional[Callable[[int, int], List[str]]],
                  ) -> Nested:
    """Start a nested compositor; optionally start a client once the output
    size is known (``client_builder(width, height)`` returns argv)."""
    parent = _parent_socket()
    compositor = _find_compositor()
    if not parent:
        pytest.skip("no parent Wayland session available to nest inside")
    if not compositor:
        pytest.skip(
            "no wlroots compositor found (tried "
            + ", ".join(COMPOSITOR_CANDIDATES)
            + "). Install one, set WSC_TEST_COMPOSITOR=/path/to/bin, or drop "
              "a binary in .testbin/ - see tests/README.md"
        )

    runtime.mkdir(parents=True, exist_ok=True)
    os.chmod(runtime, 0o700)

    env = dict(os.environ)
    env.update({
        # An absolute path connects to the *parent* compositor, while the
        # nested one publishes its own socket inside `runtime`.
        "WAYLAND_DISPLAY": parent,
        "XDG_RUNTIME_DIR": str(runtime),
        "WLR_BACKENDS": os.environ.get("WLR_BACKENDS", "wayland"),
        "WLR_LIBINPUT_NO_DEVICES": "1",
        "WLR_RENDERER_ALLOW_SOFTWARE": "1",
        "CAGE_TERMINAL": "true",
    })
    env.pop("WAYLAND_SOCKET", None)

    comp_log = open(runtime / "compositor.log", "wb")
    comp = subprocess.Popen([compositor], env=env, stdout=comp_log,
                            stderr=subprocess.STDOUT)
    info = Nested(socket="", compositor=compositor, width=0, height=0,
                  log_dir=runtime, processes=[comp])
    ok = False
    try:
        socket = _wait_for_socket(runtime)
        if socket is None:
            comp_log.close()
            tail = (runtime / "compositor.log").read_text(errors="replace")[-1500:]
            raise RuntimeError(
                f"nested compositor {compositor} never exposed a Wayland "
                f"socket:\n{tail}")
        info.socket = str(socket)

        # Ask the nested compositor what its output looks like so the client
        # can be created at exactly that size (1:1 pixel mapping).
        import wl_screencopy as wcs

        session = wcs.Session(display=info.socket)
        try:
            if not session.backends:
                pytest.skip(f"{compositor} does not implement "
                            "wlr-screencopy-unstable-v1")
            outs = session.outputs()
            if not outs:
                pytest.skip(f"{compositor} exposes no output to capture")
            info.width, info.height = outs[0].width, outs[0].height
        finally:
            session.close()

        if client_builder is not None:
            argv = client_builder(info.width, info.height)
            if argv:
                client_log = open(runtime / "client.log", "wb")
                info.client = subprocess.Popen(
                    argv,
                    env={**env, "WAYLAND_DISPLAY": info.socket,
                         "XDG_RUNTIME_DIR": str(runtime)},
                    stdout=client_log, stderr=subprocess.STDOUT,
                )
                time.sleep(1.2)  # let it map and paint
        ok = True
        return info
    finally:
        comp_log.close()
        if not ok:
            info.shutdown()


@pytest.fixture(scope="session")
def nested(tmp_path_factory) -> Nested:
    """Static test pattern, sized to the output; shared by the whole session."""
    client_bin = ROOT / "build" / "wsc-testclient"
    builder = None
    if os.environ.get("WSC_NO_CLIENT") != "1":
        if not client_bin.exists():
            print(f"note: {client_bin} missing, pixel-exactness tests will be "
                  f"skipped (run `make tools`)", file=sys.stderr)
        else:
            def builder(w: int, h: int) -> List[str]:
                return [str(client_bin), "--width", str(w), "--height", str(h)]

    return launch_nested(tmp_path_factory.mktemp("wsc-nested"), builder)


@pytest.fixture(scope="session")
def has_pattern_client(nested: Nested) -> bool:
    return nested.client_alive


@pytest.fixture(scope="module")
def nested_animated(tmp_path_factory) -> Nested:
    """A second compositor whose client repaints continuously.

    Used by the tests that must prove frames are actually new rather than a
    recycled buffer.
    """
    client_bin = ROOT / "build" / "wsc-testclient"
    if not client_bin.exists():
        pytest.skip("test client not built (run `make tools`)")

    def builder(w: int, h: int) -> List[str]:
        return [str(client_bin), "--width", str(w), "--height", str(h),
                "--animate", "--fps", "60"]

    return launch_nested(tmp_path_factory.mktemp("wsc-nested-anim"), builder)


@pytest.fixture
def session(nested: Nested):
    """A fresh Session bound to the nested compositor, closed after the test."""
    import wl_screencopy as wcs

    s = wcs.Session(display=nested.socket, buffers=2)
    yield s
    s.close()


@pytest.fixture(autouse=True)
def _no_leaked_default_session():
    """Keep the module-level default session from bleeding between tests."""
    import wl_screencopy as wcs

    yield
    wcs.close()


@pytest.fixture
def reference(nested: Nested) -> "object":
    """The RGB image the static test client paints (seq 0)."""
    return pattern_frame(nested.width, nested.height, seq=0)
