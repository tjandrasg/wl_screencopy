"""Video output: pipe raw RGB frames into ffmpeg.

Keeping encoding out of the capture library is deliberate - the C core only
moves pixels, and ffmpeg does what it is best at. NumPy users keep full
freedom to process every frame before it is written.
"""

from __future__ import annotations

import os
import shutil
import subprocess
from typing import Iterable, List, Optional, Sequence

import numpy as np

__all__ = ["VideoWriter", "find_ffmpeg", "available_codecs", "pick_codec"]


class VideoWriterError(RuntimeError):
    pass


def find_ffmpeg(explicit: Optional[str] = None) -> str:
    if explicit:
        return explicit
    env = os.environ.get("WL_SCREENCOPY_FFMPEG")
    if env:
        return env
    path = shutil.which("ffmpeg")
    if not path:
        raise VideoWriterError(
            "ffmpeg was not found on PATH. Install it (e.g. 'apt install "
            "ffmpeg') or record raw frames with "
            "wl_screencopy.record(..., writer=None) / save PNGs instead."
        )
    return path


def available_codecs(ffmpeg: Optional[str] = None) -> List[str]:
    try:
        exe = find_ffmpeg(ffmpeg)
        out = subprocess.run(
            [exe, "-hide_banner", "-encoders"],
            capture_output=True, text=True, timeout=20,
        ).stdout
    except Exception:
        return []
    codecs = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0].startswith("V") and "codec" in line:
            codecs.append(parts[1])
    return codecs


def _has_gpu_nvenc() -> bool:
    if not shutil.which("nvidia-smi"):
        return False
    try:
        return subprocess.run(["nvidia-smi", "-L"], capture_output=True,
                              timeout=10).returncode == 0
    except Exception:
        return False


def pick_codec(prefer: str = "auto", ffmpeg: Optional[str] = None,
               width: int = 0, height: int = 0) -> str:
    """Choose an H.264 encoder: NVENC when a GPU is usable, else libx264.

    NVENC refuses very small frames, so anything under 128 px on either axis
    goes to libx264 (this also keeps tiny unit-test fixtures working).
    """
    if prefer and prefer != "auto":
        return prefer
    codecs = set(available_codecs(ffmpeg))
    small = 0 < min(width, height) < 128
    if not small and _has_gpu_nvenc() and "h264_nvenc" in codecs:
        return "h264_nvenc"
    if "libx264" in codecs:
        return "libx264"
    if codecs:
        return sorted(codecs)[0]
    return "libx264"


class VideoWriter:
    """Writes ``(H, W, 3)`` uint8 frames to a video file through ffmpeg.

    ``with VideoWriter("out.mp4", 1920, 1080, fps=60) as w: w.write(frame)``

    Frames must all have the size given at construction time; if the output
    resolution changes mid-recording (hotplug, mode change) the writer raises
    and you should start a new file.
    """

    def __init__(
        self,
        path: str,
        width: int,
        height: int,
        fps: float = 30.0,
        *,
        codec: str = "auto",
        crf: int = 23,
        preset: str = "veryfast",
        pixel_format_in: str = "rgb24",
        container_pix_fmt: str = "yuv420p",
        extra_input_args: Optional[Sequence[str]] = None,
        extra_args: Optional[Sequence[str]] = None,
        ffmpeg: Optional[str] = None,
        overwrite: bool = True,
        quiet: bool = True,
    ) -> None:
        if width <= 0 or height <= 0:
            raise ValueError(f"bad video size {width}x{height}")
        self.path = os.path.abspath(path)
        self.width = int(width)
        self.height = int(height)
        self.fps = float(fps)
        self.pixel_format_in = pixel_format_in
        self.codec = pick_codec(codec, ffmpeg, width=width, height=height)

        exe = find_ffmpeg(ffmpeg)
        cmd: list[str] = [exe]
        if overwrite:
            cmd.append("-y")
        if quiet:
            cmd += ["-loglevel", "error"]
        cmd += [
            "-f", "rawvideo",
            "-pix_fmt", pixel_format_in,
            "-s", f"{self.width}x{self.height}",
            "-framerate", f"{self.fps:g}",
        ]
        cmd += list(extra_input_args or [])
        cmd += ["-i", "-"]
        cmd += ["-an", "-c:v", self.codec]
        if self.codec in ("libx264", "libx265"):
            cmd += ["-preset", preset, "-crf", str(crf)]
        elif self.codec.endswith("_nvenc"):
            # NVENC does not take -crf; -cq + -rc look closest to it.
            cmd += ["-preset", "p1", "-rc", "vbr", "-cq", str(crf)]
        if container_pix_fmt:
            cmd += ["-pix_fmt", container_pix_fmt]
        if self.path.endswith(".mp4") or self.path.endswith(".mov"):
            cmd += ["-movflags", "+faststart"]
        cmd += list(extra_args or [])
        cmd.append(self.path)

        self._cmd = cmd
        try:
            self._proc = subprocess.Popen(
                cmd,
                stdin=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
        except OSError as exc:
            raise VideoWriterError(f"cannot start ffmpeg: {exc}") from exc
        self._frames_written = 0
        self._closed = False

    # ------------------------------------------------------------- writing

    def write(self, frame: np.ndarray) -> None:
        """Append one frame. Must be uint8 with shape (height, width, 3)."""
        if self._closed or self._proc.poll() is not None:
            raise VideoWriterError(
                "the video writer is closed"
                + (f"; ffmpeg exited with {self._proc.returncode}"
                   if self._proc.poll() is not None else "")
            )
        arr = np.asarray(frame)
        if arr.dtype != np.uint8:
            raise ValueError(f"frame must be uint8, got {arr.dtype}")
        if arr.shape[:2] != (self.height, self.width):
            raise ValueError(
                f"frame is {arr.shape[1]}x{arr.shape[0]} but this writer was "
                f"created for {self.width}x{self.height}; the captured output "
                "resolution must not change during a recording"
            )
        if arr.ndim == 2:
            data = arr.tobytes()
        elif arr.shape[2] == 3:
            data = np.ascontiguousarray(arr).tobytes()
        elif arr.shape[2] == 4:
            # rgbx/rgba -> rgb for the encoder
            data = np.ascontiguousarray(arr[:, :, :3]).tobytes()
        else:
            raise ValueError(f"cannot write frame with shape {arr.shape}")

        stdin = self._proc.stdin
        assert stdin is not None
        try:
            stdin.write(data)
        except BrokenPipeError as exc:
            raise VideoWriterError(
                "ffmpeg stopped accepting frames: "
                + self._drain_stderr_tail()
            ) from exc
        self._frames_written += 1

    def writeseq(self, frames: Iterable[np.ndarray]) -> int:
        n = 0
        for frame in frames:
            self.write(frame)
            n += 1
        return n

    def _drain_stderr_tail(self) -> str:
        if self._proc.stderr is None:
            return "(no stderr)"
        try:
            data = self._proc.stderr.read() or b""
        except Exception:
            return "(stderr unavailable)"
        return data.decode("utf-8", "replace").strip()[-800:] or "(no message)"

    def close(self) -> None:
        """Flush and finish the file. Waits for ffmpeg to exit."""
        if self._closed:
            return
        self._closed = True
        proc = self._proc
        try:
            if proc.stdin is not None:
                proc.stdin.close()
        except Exception:
            pass
        try:
            ret = proc.wait(timeout=120)
        except subprocess.TimeoutExpired:  # pragma: no cover
            proc.kill()
            ret = proc.wait()
        if ret not in (0, None):
            tail = self._drain_stderr_tail()
            # SIGINT (2) is what Ctrl-C produces; not an error worth raising.
            if ret != -2:
                raise VideoWriterError(
                    f"ffmpeg exited with code {ret} while writing "
                    f"{self.path}: {tail}"
                )

    def abort(self) -> None:
        """Kill ffmpeg without waiting for a clean finish."""
        self._closed = True
        try:
            if self._proc.poll() is None:
                self._proc.kill()
            self._proc.wait(timeout=5)
        except Exception:
            pass

    @property
    def frames_written(self) -> int:
        return self._frames_written

    @property
    def command(self) -> list[str]:
        return list(self._cmd)

    def __enter__(self) -> "VideoWriter":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        if exc_type is None:
            self.close()
        else:
            self.abort()

    def __repr__(self) -> str:
        return (f"<VideoWriter {os.path.basename(self.path)} "
                f"{self.width}x{self.height}@{self.fps:g}fps codec={self.codec} "
                f"frames={self._frames_written}>")
