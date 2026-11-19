# ZeroFG source snapshot

This directory contains the ZeroFG engine of the MFG development branch `mfg/core` (local, at `81309d6`). It is not published yet. It sits on top of v1.1.0 and the
license commit `10e68b6`. ZeroFG is `Apache-2.0 OR GPL-2.0-or-later` since that commit; it is used here under Apache-2.0, `LICENSE`.

The branch adds:
- any phase between the Reals (x3 / x4), with the same-pair contract;
- Zero's screen by cells (`kHudCells`);
- Zero's light gain (`kLightGain`);
- Zero's stop rule (`kStopStill`): when the camera stops, the fallback surface holds still instead of carrying the last search's
  motion; and a switch for the base's cleanup (`kBaseClean`, kept on);
- the render's two passes in one walk (the same picture), and `CreateInfo::zero_options`: `kZeroLight` (one background surface, one
  render pass, no guided filter) and `kZeroQuarterBase` (x4's quarter phases from the base's picture), both off by default in the engine.
- a wipe interpolates (`kCutLit`): a pair is a cut (B whole) only if the points lit in both pictures do not correlate either.
- fast motion is no cut (`kCutCost`): a cut also needs the motion search to have failed nearly everywhere.

At the middle, with the switches off (`kHudCells`, `kLightGain`, `kStopStill` 0; `kBaseClean` 1), it is bit-identical to v1.1.0, except on the pairs the old cut rule took for cuts and `kCutLit` or `kCutCost` do not. The engine sources, public headers, shader sources, shipped SPIR-V
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
