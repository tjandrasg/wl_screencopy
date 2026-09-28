<!--
Thanks for taking the time.

This repository is 100% AI-generated and has not had a human correctness
review, so a human change - even one that is only "I ran the suite on hyprland
and here is the output" - is worth a lot here. CONTRIBUTING.md has the details;
the short version is: tell us what you ran it on, and add a test.

Delete this comment before submitting.
-->

## What does this change?

<!-- One or two sentences: what, and why. The diff already shows how. -->

Fixes #<!-- issue number, or write "no issue" -->

## Type of change

- [ ] bug fix
- [ ] documentation / test / build system only
- [ ] new pixel format or Python API
- [ ] C ABI (`include/wl_screencopy.h`, `src/abi.c`)
- [ ] protocol XML (`proto/*.xml`)
- [ ] new capture backend

## Environment tested on

The maintainer develops against a single nested wlroots compositor, so this
table is the only way to know what a change was actually proven against. Please
fill all four rows.

| | |
| --- | --- |
| Compositor + version | <!-- sway 1.10 / hyprland 0.46 / labwc 0.8 / cage ... --> |
| Distribution | |
| GPU + driver | |
| Commit tested | <!-- git rev-parse --short HEAD --> |

## Test results

```sh
make clean && make && make test
```

Paste the final summary line **including the skip count** - a green run where
everything skipped proves nothing:

```
```

- [ ] `make unittest` passes (conversion table; needs no compositor)
- [ ] `make test` passes with no unexplained skips
- [ ] build produced no new warnings under the `WARN` flags in the `Makefile`

## Evidence for correctness changes

<!-- Anything that moves pixels: say what was compared against what. e.g.
     "build/wsc-dump grab -o /tmp/f.ppm is byte-identical to tests/pattern.py
     for rgb24 and bgr24 at scale 2" or "region capture equals the equivalent
     crop of the full frame". "Tests pass" is not evidence of pixel correctness. -->

## Checklist

- [ ] bug fix comes with a regression test
- [ ] docs updated: `README.md`, docstrings, and `tests/README.md` if the
      harness changed
- [ ] if `include/wl_screencopy.h` or `src/abi.c` changed, `python/wl_screencopy/_lib.py`
      was updated **in this same commit** (structures are matched by layout)
- [ ] if `proto/*.xml` changed, `proto/PROVENANCE.md` records the upstream
      version/commit and what changed
- [ ] no build artefacts, `.so` files, captures, `.testbin/` or local notes are
      committed
- [ ] new C files start with `/* SPDX-License-Identifier: MIT */`
- [ ] new failure paths raise a specific class from `errors.py`, not a generic
      `RuntimeError`
- [ ] `git diff --check` is clean

## AI assistance

This project is explicit about being machine-written, and reviewers treat
machine-written contributions the same way. Just say which is the case:

- [ ] no AI tool was used
- [ ] an AI tool was used: <!-- which tool, and what part it wrote -->

## Notes for review

<!-- Known gaps, follow-up work, alternatives you rejected, anything that should
     be tested on a compositor you do not have. -->
