# ZeroFG source snapshot

This directory contains the approved ZeroFG engine at commit
`38dcc6f4a69fdf2e009a2908cd9ab6e2d3a57a64` from the author's ZeroFG source tree.
The engine sources, public headers, shader sources, shipped SPIR-V headers and
shader compiler are preserved from that snapshot. No laboratory recordings,
runtime binaries, device identifiers or experimental environment controls are
included. This vendored snapshot makes the host PR buildable without depending
on an unpublished remote core commit.

The host adapter in `integration/xenia/` is the thin adapter from that snapshot's
`host/xendroid/` directory. Xendroid's presenter owns recording order, queue
submission, synchronization, presentation and resource lifetimes.

Product mapping in this integration:

| User choice | Core mode |
|---|---|
| Off | Native Xendroid+ presentation |
| ReallyZero | `zerofg::Mode::kDenseFlow` (RC5+) |
| Zero | `zerofg::Mode::kRC7` |

The legacy `kZero` / `kReallyZero` core aliases refer to RC3 and are intentionally
not used by this integration. One interpolator serves the selected mode, with
per-flight descriptors. Recordings are submitted serially in their recording
order; an abandoned recording cannot be followed by a retry using its retained
history. Mode changes drain the old session before recreating generation state.

The engine is Apache-2.0; see `LICENSE`. Xenia-derived host integration files
outside this directory retain their original notices.
