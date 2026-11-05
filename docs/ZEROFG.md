# ZeroFG in Xendroid+

ZeroFG is an optional frame-generation engine. It has two choices: **Zero**
(RC7, quality) and **ReallyZero** (RC5+, lower cost). It is **off by default**.
The pinned engine source and compiled shader headers are included in
`emulator-core/src/main/cpp/zerofg`; see its `SOURCE.md` and `LICENSE`.

## Settings and in-game switching

Select Off, ReallyZero or Zero under Frame Generation in Settings or the
in-game menu. The in-game choice uses the menu's existing Keep, Undo and Make
global behavior. Selecting another frame-generation engine turns ZeroFG off;
selecting ZeroFG stops the other engine. Only one engine owns output at a time.

The UI publishes a request. The renderer disconnects and drains the old session,
returns its surface authority, and creates fresh generation state for the new
choice. It does not destroy Vulkan resources from the Android UI callback.
The selected value is a request; the menu reports output authority separately,
so it does not describe an uncompleted transition as an active new engine.

## Integration boundaries

This ports the existing XenDroid independent-presenter integration, using the
approved core `38dcc6f`. Zero maps explicitly to `Mode::kRC7`, and ReallyZero to
`Mode::kDenseFlow`; the historical `kZero`/`kReallyZero` aliases are RC3 modes.
The engine records commands; the host owns queues, synchronization and pacing.
The approved presenter retains the normal integration's active defaults:
frequency/rate lock, presentation advance, reanchor cutover, phase recovery,
the Source/Generation/Post arbiter and ordered disconnect/drain. Some laboratory
switches were frozen on in the portable kit; their absent configuration names
do not mean those policies were removed. Dormant physical-operating-point probes
were not enabled by this port.
One interpolator serves each active mode, with separate bindings for generations
in flight. Submission follows recording order. Queue admission is acquired before
recording, so a retry cannot retain history from discarded GPU work.

ZeroFG requires Vulkan 1.3, enabled synchronization2/storage formats and the
Android image/fence handoff support. Optional Source capabilities are negotiated
when supported even if ZeroFG starts off, allowing later activation without
restarting a qualified Source device. Failure to qualify keeps native presentation.
The normal Plus path retains its own compatibility requirements.

The existing Plus image controls are shared by real and generated output.
The output-device pipelines include all 17 scaling/dither variants; live
scaling, CAS, FSR and dither overrides are read from the same menu state. The
original color-filter shader uses resources private to each output job.
SGSR1 Edge Direction (`sgsr_edge`) is ported from the existing XenDroid
implementation, with an optional dither variant. It appears in Settings and the
in-game Image menu. Existing scaling values keep their indices; Edge is appended
as value 6, and the default filter is unchanged.
Display refresh votes follow the active main-surface authority, including TV
output, and are released when that authority returns to the native presenter.

The guest vblank adaptation retains the Plus `VblankPacer`. While ZeroFG is live,
a pull may advance one grid event and consumes the following event to repay it;
it cannot advance a second event before repayment. With ZeroFG off, the native
grid and wait remain in use. The CPU falsifier covers stalls, clock changes and
on/off transitions; this is not a phone latency measurement.

## Build and validation

Use the normal commands and toolchain in `BUILD.md`. No separate engine download
or shader compiler is needed for the shipped ZeroFG shader headers. The host's
existing shader-generation toolchain still applies.

Validated on 2026-11-05:

- Android Debug APK built with JDK 21 / NDK 29.0.14206865.
- 29 JVM tests passed across the in-game menu, Keep/Undo, setting catalog and
  English/Portuguese string checks. The catalog expectations include the new
  essential/live ZeroFG control; existing controls remain explicitly checked.
- JNI declaration/registration check passed (97 native methods).
- The existing native vblank-pacer and frame-statistics tests passed.
- SGSR1 and SGSR1+dither compiled SPIR-V stayed byte-identical to the pre-Edge
  baseline (2,078 and 3,474 words respectively).
- Both Edge shaders passed `spirv-val --target-env vulkan1.0`.
- Engine snapshot comparison: 170 imported files match approved `38dcc6f`.

These are build, static and CPU results. The new Plus APK has not been installed
or qualified in a game; in particular, they do not establish phone latency,
cadence or in-game hot-swap stability.

The source snapshot is the same engine qualified on stock and Turnip in the
ARMSX2 integration. That evidence establishes the engine's picture and ordered
submission contract, not the runtime behavior of this new host port. In-game
switching, resize, pause/resume, external-display changes and unsupported-device
fallback need host-specific runtime checks. Do not infer input latency or a
fixed GPU-time saving from an OSD reading.
