# Security policy

**Read this first: this repository is 100% AI-generated and has not had a human
correctness review or a security review.** The claims below describe what the
code does today, as written - not what an audit would certify. If you find a
mistake in this document, that is itself a finding; please report it.

## 1. Threat model: what this software actually does

Be clear about the boundary before deciding what counts as a vulnerability.

* **Runs as you, with your privileges.** No setuid, no root, no daemon, no
  privilege escalation surface. `make install` is the only step that may need
  root, and only to write under `$(PREFIX)`.
* **No network exposure.** It makes no network connection and opens no
  listening socket. Its only IPC is the Wayland socket of your own session
  (`$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY`).
* **The security boundary is your Wayland session, not this library.**
  `wlr-screencopy-unstable-v1` has no per-client authorisation: any client able
  to connect to your compositor socket can bind the manager and read your whole
  screen. Any process already running as your user can do what this library
  does. **This project grants no new privilege**, and "a local process can
  capture my screen" is a property of the protocol and your session, not a bug
  here.
* **It writes whatever was on your screen to files you choose.** That includes
  passwords, 2FA codes, private chat, mail and documents. Output is plain PNG /
  PPM / `.npy` / MP4 - unencrypted, with no redaction, masking or
  content-filtering, and nothing about it is "deleted" in a forensically
  meaningful sense.
* **Frame data lives in shared memory buffers** created with
  `memfd_create(MFD_CLOEXEC)`; on kernels/namespaces without memfd it falls back
  to `mkstemp()` in `$XDG_RUNTIME_DIR` (or `/tmp` when unset), unlinked
  immediately. Screen pixels are in there for the lifetime of the buffer.
* **ffmpeg is spawned as a subprocess**, via `subprocess.Popen` with an argv
  list - never `shell=True`. The output path is passed through
  `os.path.abspath()` before it reaches the command line, so a filename cannot
  be smuggled in as an ffmpeg option. Note `overwrite=True` is the default
  (`-y`): existing files are replaced without asking. `extra_args` /
  `extra_input_args` are deliberately caller-controlled - you own what you pass.
* **The compositor is semi-trusted input.** Width, height, stride, pixel format
  and transform all come from it. There are basic checks (positive dimensions,
  `dst_stride >= width * bpp`, overflow-guarded size arithmetic when allocating
  pools), but **this parsing has not been fuzzed, audited or formally
  reasoned about**.

## 2. What is a vulnerability here

* Memory corruption in `src/` reachable from a compositor's advertised
  dimensions, stride, format, damage rectangles or file-descriptor events.
* A use-after-free, double-free, fd leak or unmapping bug around `wl_shm`
  pools (the lifetime code in `src/session.c` is the place to look first).
* Frame data reaching a reader who should not have it - e.g. a capture written
  into a world-readable directory, or pixels/paths appearing in debug output.
* Argument or path injection through the CLI or the `Recorder` /
  `VideoWriter` API, including symlink following when writing output.
* ABI mismatch between `include/wl_screencopy.h` / `src/abi.c` and the ctypes
  layer that can produce out-of-bounds reads in Python.

**Not vulnerabilities** (please read section 1 before reporting):

* Your compositor not implementing the protocol - you get
  `CompositorUnsupportedError`, by design. Same for X11/XWayland sessions.
* H.264/H.265 being lossy, or the recorded image differing from a PNG capture.
* A compositor, or another Wayland client, capturing your screen.
* Lack of encryption for recorded files.
* Performance problems, or hangs that need a hostile compositor plus a
  cooperative victim and cannot be triggered by normal use.

## 3. Reporting a vulnerability

**Please do not open a public issue, and do not include details in a normal
issue** - issues on this repository are readable by everyone, including the
`WAYLAND_DEBUG` traces people paste into them.

Use GitHub's private vulnerability reporting: **Security tab → Report a
vulnerability**
(<https://github.com/tjandrasg/wl_screencopy/security/advisories/new>). If that
form is unavailable, open an issue whose title is exactly `Security contact
request` and **leave the body completely empty**; the maintainer will get in
touch and you can share details privately.

If you can, include: the affected commit (`git rev-parse --short HEAD`),
compositor and version, distro, whether it is reachable from ordinary use or
needs a hostile compositor, and a reproducer or a patch. Patches move fastest
here.

What to expect: this is a one-person volunteer project built by an AI agent, so
an acknowledgement may take a week or two rather than a day. Please allow a
**90-day** private disclosure window before going public, and say so if you need
a different timeline - coordinated disclosure is fine. Fixing it may mean
re-verifying the C core, which is slower than patching reviewed code.

## 4. Using this project more safely

* Capture a dedicated workspace, not the screen where your password manager is
  open. Whatever is visible ends up in the file.
* Write captures somewhere not readable by others:
  `install -d -m 700 ~/captures` and record into it. Output files are created
  with your umask, and `/tmp` is not a safe place for them.
* Check `overwrite` before recording over files you care about - the default is
  to replace them.
* ffmpeg is a large, separately-maintained attack surface and is not vendored.
  Install it from your distro or another trusted source and keep it updated; the
  library only ever pipes raw frames to its stdin.
* `WSC_DEBUG=1` and `WAYLAND_DEBUG=1` produce verbose protocol traces that can
  contain window titles and other clients' metadata. Do not enable them in
  production, and read them before pasting them anywhere.
* If your requirement is "capture one window without ever exposing the rest of
  the screen", this is the wrong tool - the protocol cannot promise that.
* Vendored protocol definitions live in `proto/`; they are upstream copies
  documented in `proto/PROVENANCE.md`. Verify provenance and version when they
  change instead of pasting new XML in.

## 5. Supported versions

There are no tagged releases yet: `main` is the only supported branch, and the
current version is 0.1.0. There is no backport policy and no ABI stability
promise for `include/wl_screencopy.h` yet - pin a commit if you depend on it.
