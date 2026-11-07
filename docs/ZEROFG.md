# ZeroFG in Xendroid+

ZeroFG is an optional frame-generation engine. It has two choices: **Zero**
(RC7, quality) and **ReallyZero** (the dense flow, lower cost). It is **off by default**.
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
ZeroFG release v1.1.0 (`2b7711e`). Zero maps explicitly to `Mode::kRC7`, and ReallyZero to
`Mode::kDenseFlow`, the same engines as 1.1's `kZero`/`kReallyZero`.
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
- Engine snapshot comparison: every imported engine file matches the v1.1.0 release blob for blob.

These are build, static and CPU results. The initial Plus APK was installed on
2026-11-05. Its first game checks on stock and Turnip failed activation: device B
initialized, but handoff and real/synthetic output counts stayed at zero.
`IssueSwap` was missing the separate `RecordSourcePresent()` boundary used by
native Source qualification. The local correction restores that boundary and
its inflight scope without changing the engine or qualification thresholds.
A behavioral CPU test now checks actual Source/publication identities, cadence
intervals and single attribution of backpressure. Phone qualification of the
corrected APK is still required; these tests do not establish phone latency,
cadence or in-game hot-swap stability.

The full/panel HUD now reports the requested ZeroFG tier separately from output
authority, and labels the observed output submission rate. Authority and output
rate alone do not prove synthetic scanout. Legacy engine GPU timings are not
reused as ZeroFG timings. Native diagnostics retain the normal `xe.log` append
path and per-session archives.

The source snapshot is the same engine qualified on stock and Turnip in the
ARMSX2 integration. That evidence establishes the engine's picture and ordered
submission contract, not the runtime behavior of this new host port. In-game
switching, resize, pause/resume, external-display changes and unsupported-device
fallback need host-specific runtime checks. Do not infer input latency or a
fixed GPU-time saving from an OSD reading.

## Local presenter latency diagnostic (2026-11-06)

The local diagnostic carries each Real's Source ID and original IssueSwap
monotonic timestamp through the egress request and the display timing record.
Only a matching `actualPresentTimeGOOGLE` enters the Real issue-to-shown
measurement. Synthetic frames are counted separately and excluded from this
comparison; their B timestamp is not the synthetic picture's temporal origin.
The engine selection is captured from the bound session, not a later UI request.
No estimator, shader, pacing policy or product default is changed.

`ZeroFGLatencyBatch` logs exact microsecond samples in bounded, disjoint roughly
five-second batches for native Off and both ZeroFG modes. Each batch includes
its monotonic bounds, sample count and reliability counters. Missing identity,
invalid/future timestamps, failed timing queries and overflow cannot fabricate
a latency sample. Partial B batches at teardown are marked `complete=false`.
Native repaint presents without a Source tag are not new Real samples.

Compare Off, ReallyZero and Zero in the same running gameplay scene, with the
same driver, resolution and Edge setting. Exclude startup, emulator menu pauses
and the first 15 seconds after each mode transition; collect at least 30 seconds
of complete stable batches per leg and bracket both modes with Off. Pool the
raw samples for percentiles, never average per-window percentiles or sum stage
percentiles. The result is IssueSwap-to-display-timestamp latency, not controller
input-to-photon latency; display timing coverage and rejected batches must be
reported with it. Phone results for this diagnostic are recorded separately.
### Phone observation with cooler (2026-11-06)

Same Gears3 gameplay scene/fixed camera, Adreno840/PurpleVK (Turnip), SGSR1
Edge enabled, one APK, bracketed Off -> RZ -> Off -> Zero -> Off. After20seconds
stabilization perchange, eachleg contains35–40seconds ofcomplete reliable
batches.5,689Real timing samples were pooled exactly, excluding synthetics:

| Mode | Real samples | p50ms | p90ms | p99ms | Added p50 against adjacent Off |
|---|---:|---:|---:|---:|---:|
| Off (three legs pooled) |3440|26.824|32.116|34.579|baseline|
| ReallyZero |1050|35,01|76.860|92.589|46.475–46.608ms|
| Zero |1199|40,02|82.505|95.161|47.829–47.844ms|

The individual Off medians were26.943/26.810/26.795ms. These are Real IssueSwap
to driver-reported actual presentation times, not controller input-to-photon.
Only this scene/driver was measured. The observer did not change engine math,
shaders, pacing or defaults. The metadata mode is rebound on each Connect,
before starting its worker; the persistent Impl must not retain its first mode.

Matched Real+synthetic timing returns were close to60/s in both modes. Rolling
128-sample interval snapshots still showed occasional25–50ms tails; one additional
surface timing was overtaken during Zero while its Real IDs remained complete.
Do not claim perfect60Hz cadence or zero output loss. Battery was31.3–31.5C during
RZ and34.4–34.8C during Zero with the cooler; this does not prove a thermal cause
for earlier owner-reported stutters.

The older ready_wait_us diagnostic is late CPU observation/reclamation of capture
completion. Capture completion can already be consumed through a GPU semaphore
before the CPU observes it, so this counter may exceed issue-to-shown. It is not
an end-to-end latency measurement and must not be added to the stage percentiles.