# ZeroFG source snapshot

This directory contains the ZeroFG engine of the MFG development branch `mfg/core`. It is not published yet. It sits on top of v1.1.0 and the
license commit `10e68b6`. ZeroFG is `Apache-2.0 OR GPL-2.0-or-later` since that commit; it is used here under Apache-2.0, `LICENSE`.

The branch adds:
- any phase between the Reals (x3 / x4), with the same-pair contract;
- Zero's screen by cells (`kHudCells`);
- Zero's light gain (`kLightGain`).

At the middle, with both switches off, it is bit-identical to v1.1.0. The engine sources, public headers, shader sources, shipped SPIR-V
headers, shader compiler and CMake file are that branch's, byte for byte. Until it is published this snapshot is for internal builds, and
the published v1.1.0 snapshot is `f9b0b5cef`.

No laboratory recordings, runtime binaries, device identifiers or experimental environment controls are included.

The host adapter in `integration/xenia/` is the thin adapter from the same
release's `host/xendroid/` directory. Xendroid's presenter owns recording order,
queue submission, synchronization, presentation and resource lifetimes.

Product mapping in this integration:

| User choice | Core mode |
|---|---|
| Off | Native Xendroid+ presentation |
| ReallyZero | `zerofg::Mode::kReallyZero` (the dense optical flow) |
| Zero | `zerofg::Mode::kZero` (RC7, the layered engine) |

The integration names them `Mode::kDenseFlow` and `Mode::kRC7`, the same engines
under their development names. One interpolator serves the selected mode, with
per-flight descriptors. Recordings are submitted serially in their recording
order; an abandoned recording cannot be followed by a retry using its retained
history. Mode changes drain the old session before recreating generation state.

The engine is Apache-2.0; see `LICENSE`. Xenia-derived host integration files
outside this directory retain their original notices.
