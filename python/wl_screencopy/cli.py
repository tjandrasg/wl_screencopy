"""Command line interface: ``python -m wl_screencopy ...``."""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from typing import Optional

import numpy as np

from . import (
    __version__,
    capture,
    open_session,
    outputs,
    save_image,
)
from ._lib import native
from .errors import WlScreencopyError
from .recorder import Recorder
from .video import VideoWriter, VideoWriterError


def _parse_region(value: Optional[str]):
    if not value:
        return None
    try:
        parts = [int(p) for p in value.replace(" ", "").split(",")]
    except ValueError:
        raise SystemExit(f"--region must be x,y,w,h (got {value!r})")
    if len(parts) != 4:
        raise SystemExit(f"--region must have 4 values (got {value!r})")
    return tuple(parts)


def _add_common(p: argparse.ArgumentParser) -> None:
    p.add_argument("--display", help="Wayland socket name (default $WAYLAND_DISPLAY)")
    p.add_argument("-O", "--output", default=None,
                   help="output index or name, e.g. 0 or DP-1 (default: first)")
    p.add_argument("-r", "--region", default=None,
                   help="capture only x,y,w,h in logical pixels")
    p.add_argument("-c", "--cursor", action="store_true",
                   help="composite the mouse pointer into the capture")
    p.add_argument("-D", "--damage", action="store_true",
                   help="use copy_with_damage: wait for the screen to change")
    p.add_argument("-t", "--timeout", type=float, default=5.0,
                   help="seconds to wait for a frame (default 5)")
    p.add_argument("-b", "--buffers", type=int, default=3,
                   help="shared memory buffers to rotate (default 3)")
    p.add_argument("--pixel-format", default="rgb24",
                   choices=["rgb24", "rgba", "bgr24", "bgra", "rgbx", "bgrx"],
                   help="frame layout (default rgb24 -> ndarray (H,W,3))")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="wl_screencopy",
        description="Record a Wayland screen (wlr-screencopy) straight into "
                    "NumPy arrays.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="examples:\n"
               "  python -m wl_screencopy list\n"
               "  python -m wl_screencopy snapshot shot.png\n"
               "  python -m wl_screencopy record clip.mp4 --fps 60 --duration 20\n"
               "  python -m wl_screencopy record - --fps 30 --duration 5 "
               "--out-dir frames/\n"
               "  python -m wl_screencopy bench --seconds 5\n",
    )
    parser.add_argument("--version", action="version",
                        version=f"wl_screencopy {__version__}")
    sub = parser.add_subparsers(dest="command", required=True)

    p_list = sub.add_parser("list", help="show compositor support and outputs")
    p_list.add_argument("--json", action="store_true", help="machine readable")
    p_list.add_argument("--watch", type=float, default=0.0,
                        help="re-print every N seconds until Ctrl-C")
    _add_common(p_list)

    p_snap = sub.add_parser("snapshot", help="capture one frame")
    p_snap.add_argument("path", nargs="?", default="screenshot.png",
                        help="output file (.png/.jpg), or - to skip saving")
    p_snap.add_argument("--info", action="store_true",
                        help="print array shape/dtype and capture timing")
    _add_common(p_snap)

    p_rec = sub.add_parser("record", help="record a video (needs ffmpeg)")
    p_rec.add_argument("path", nargs="?", default="recording.mp4",
                       help="output file, e.g. out.mp4 (default recording.mp4)")
    p_rec.add_argument("--fps", type=float, default=30.0,
                       help="target frame rate (default 30)")
    p_rec.add_argument("--duration", type=float, default=None,
                       help="stop after N seconds (default: until Ctrl-C)")
    p_rec.add_argument("--frames", type=int, default=None,
                       help="stop after N frames")
    p_rec.add_argument("--codec", default="auto",
                       help="auto | libx264 | h264_nvenc | ... (default auto)")
    p_rec.add_argument("--crf", type=int, default=23,
                       help="quality (default 23; lower is better/bigger)")
    p_rec.add_argument("--preset", default="veryfast",
                       help="encoder preset (default veryfast)")
    p_rec.add_argument("--out-dir", default=None,
                       help="also dump each frame as PNG into this directory")
    p_rec.add_argument("--no-repeat", action="store_true",
                       help="with --damage, do not repeat frames while idle")
    _add_common(p_rec)

    p_frames = sub.add_parser(
        "frames", help="capture N frames and store them as a .npy array")
    p_frames.add_argument("path", nargs="?", default="frames.npy")
    p_frames.add_argument("--count", type=int, default=30)
    p_frames.add_argument("--fps", type=float, default=30.0)
    _add_common(p_frames)

    p_bench = sub.add_parser("bench", help="measure capture throughput")
    p_bench.add_argument("--seconds", type=float, default=5.0)
    p_bench.add_argument("--fps", type=float, default=1000.0,
                         help="cap the loop (default: uncapped)")
    _add_common(p_bench)

    return parser


def _cmd_list(args) -> int:
    session = open_session(args.display, buffers=args.buffers)
    try:
        back = session.backends
        outs = session.outputs(refresh=True)
        if args.json:
            payload = {
                "backends": back,
                "supports_screencopy": bool(back),
                "note": None if back else session.unsupported_reason,
                "outputs": [
                    {
                        "index": o.index,
                        "name": o.name,
                        "description": o.description,
                        "x": o.x, "y": o.y,
                        "width": o.width, "height": o.height,
                        "scale": o.scale,
                        "refresh_hz": o.refresh_hz,
                        "physical_mm": [o.physical_width_mm,
                                        o.physical_height_mm],
                        "diagonal_inches": round(o.diagonal_inches, 2),
                        "preferred": o.preferred,
                        "capture_size": None,
                    }
                    for o in outs
                ],
            }
            if back:
                for entry in payload["outputs"]:
                    try:
                        info = session.probe(entry["index"])
                        entry["capture_size"] = [info.width, info.height]
                    except WlScreencopyError as exc:
                        entry["probe_error"] = str(exc)
            print(json.dumps(payload, indent=2))
            return 0 if back else 1

        print(f"wl_screencopy {__version__} (native library: {native.version()})")
        print(f"compositor support: "
              f"{', '.join(f'{k} v{v}' for k, v in back.items()) or 'NONE'}")
        if not back:
            print(f"  note: {session.unsupported_reason}")
            print("  wlr-screencopy-unstable-v1 is provided by wlroots based "
                  "compositors (Sway, Hyprland, river, labwc, Cage, Wayfire).")
        print(f"outputs: {len(outs)}")
        for o in outs:
            print("  " + str(o))
            if back:
                try:
                    info = session.probe(o.index)
                    print(f"      -> capture would be {info.width}x{info.height}")
                except WlScreencopyError as exc:
                    print(f"      -> probe failed: {exc}")
        return 0 if back else 1
    finally:
        session.close()


def _cmd_snapshot(args) -> int:
    t0 = time.perf_counter()
    frame = capture(args.output, _parse_region(args.region), cursor=args.cursor,
                    wait_for_damage=args.damage, timeout=args.timeout,
                    pixel_format=args.pixel_format, display=args.display)
    dt = time.perf_counter() - t0

    if args.info or args.path == "-":
        print(f"array: shape={frame.shape} dtype={frame.dtype} "
              f"contiguous={frame.flags['C_CONTIGUOUS']} "
              f"mean={frame.reshape(-1, frame.shape[2]).mean(0).round(1).tolist()} "
              f"in {dt * 1e3:.1f} ms")
    if args.path != "-":
        save_image(frame, args.path)
        print(f"wrote {args.path} ({os.path.getsize(args.path)} bytes)")
    return 0


def _cmd_record(args) -> int:
    if args.path == "-" and not args.out_dir:
        raise SystemExit("path '-' means 'do not encode'; combine it with "
                         "--out-dir to save frames as PNG")

    region = _parse_region(args.region)
    rec = Recorder(
        output=args.output, region=region, fps=args.fps, cursor=args.cursor,
        wait_for_damage=args.damage, pixel_format=args.pixel_format,
        buffers=args.buffers, timeout=args.timeout,
        repeat_frames=not args.no_repeat, display=args.display,
    )

    out_dir = args.out_dir
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)

    encode = args.path != "-"
    try:
        with rec:
            h, w, c = rec.frame_shape
            sys.stderr.write(f"recording {w}x{h} @ {args.fps:g} fps "
                             f"from output {args.output or 'default'} "
                             f"(Ctrl-C to stop)\n")
            writer = None
            if encode:
                writer = VideoWriter(args.path, w, h, fps=args.fps,
                                     codec=args.codec, crf=args.crf,
                                     preset=args.preset)
            try:
                frame_iter = rec.frames()
                for index, frame in enumerate(frame_iter):
                    if writer is not None:
                        writer.write(frame)
                    # PNG encoding is much slower than capturing, so dump a
                    # sample rather than every frame.
                    if out_dir and index % 30 == 0:
                        save_image(frame,
                                   os.path.join(out_dir, f"frame_{index:06d}.png"))
                    if index % 30 == 0:
                        s = rec.stats()
                        sys.stderr.write(
                            f"\r  {s.frames} frames, {s.fps_actual:.1f} fps, "
                            f"{s.repeated} repeated   ")
                        sys.stderr.flush()
                    if args.frames is not None and index + 1 >= args.frames:
                        break
                    if args.duration is not None and \
                            rec.stats().elapsed >= args.duration:
                        break
            finally:
                if writer is not None:
                    writer.close()
    except KeyboardInterrupt:
        sys.stderr.write("\ninterrupted, finalising file\n")
    finally:
        rec.close()

    stats = rec.stats()
    sys.stderr.write(f"done: {stats}\n")
    if encode:
        sys.stderr.write(f"saved {args.path}"
                         + (f" ({os.path.getsize(args.path)} bytes)\n"
                            if os.path.exists(args.path) else "\n"))
    return 0


def _cmd_frames(args) -> int:
    region = _parse_region(args.region)
    rec = Recorder(output=args.output, region=region, fps=args.fps,
                   cursor=args.cursor, wait_for_damage=args.damage,
                   pixel_format=args.pixel_format, buffers=args.buffers,
                   timeout=args.timeout, display=args.display)
    stack = []
    with rec:
        for frame in rec.frames(max_frames=args.count):
            stack.append(frame.copy())
    arr = np.stack(stack) if stack else np.empty((0,), dtype=np.uint8)
    np.save(args.path, arr)
    print(f"saved {arr.shape} {arr.dtype} to {args.path} "
          f"({os.path.getsize(args.path)} bytes)")
    print(f"  {rec.stats()}")
    rec.close()
    return 0


def _cmd_bench(args) -> int:
    rec = Recorder(output=args.output, region=_parse_region(args.region),
                   fps=args.fps, cursor=args.cursor,
                   wait_for_damage=args.damage,
                   pixel_format=args.pixel_format, buffers=args.buffers,
                   timeout=args.timeout, repeat_frames=False,
                   display=args.display)
    out = None
    n = 0
    t0 = time.perf_counter()
    try:
        with rec:
            h, w, c = rec.frame_shape
            out = np.empty((h, w, c), dtype=np.uint8)
            print(f"capturing {w}x{h} for {args.seconds:g}s "
                  f"(fps cap {args.fps:g}) ...")
            end = t0 + args.seconds
            while time.perf_counter() < end:
                rec.read(out=out)
                n += 1
    except KeyboardInterrupt:
        pass
    finally:
        rec.close()
    dt = time.perf_counter() - t0
    if n and dt > 0:
        mbps = n * out.nbytes / dt / 1e6
        print(f"{n} frames in {dt:.2f}s = {n / dt:.1f} fps ({mbps:.0f} MB/s)")
    else:
        print("no frames captured")
    return 0 if n else 1


def main(argv: Optional[list] = None) -> int:
    args = build_parser().parse_args(argv)
    handlers = {
        "list": _cmd_list,
        "snapshot": _cmd_snapshot,
        "record": _cmd_record,
        "frames": _cmd_frames,
        "bench": _cmd_bench,
    }
    try:
        return handlers[args.command](args)
    except (WlScreencopyError, VideoWriterError) as exc:
        sys.stderr.write(f"error: {exc}\n")
        return 2


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
