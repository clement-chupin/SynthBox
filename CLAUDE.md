# GrvEP-fusion — context for Claude

**This is a fork of GrvEP** (same portable groovebox/synthesizer firmware for an ESP32-S3:
30+ modes, SD-card sample playback, OLED, 4×8 key grid, joystick, 7 potentiometers,
addressable LEDs), forked specifically to merge in a friend's own graph-based DSP engine
(from `SevenistSynth_RitualMachine`, same hardware) underneath GrvEP's own UI/UX, replacing
AMY mode by mode. **The migration is in progress, not finished** — read
`~/.claude/plans/mutable-nibbling-melody.md` for the full phased plan and current status
before assuming either engine's role in a given mode. As of this writing: both AMY and the
new engine run side by side (see "Two synth engines" below); `audioNoteOn/Off/SetEnvelope/
SetVolume/SetShape/SetPitchBend/AllNotesOff` (the ~8 most-used `audio_engine.h` functions)
are wired to the new engine's one placeholder graph; everything else (~160 functions) is
still a no-op stub, filled in phase by phase.

The same application code (`src/`) runs on four targets. **Read this whole file before
touching build config, AMY's DSP code, or the vendored engine** — the vendoring/dual-engine
setup described below is the single most likely thing to trip up a fresh session.

## Two synth engines, on purpose, for now

- **AMY** (`lib/AMY Synthesizer/src/`) — the original engine, event-based, still fully
  initialized and running (`audioInit()` in `src/audio_engine.cpp` calls `amy_start()`
  exactly as GrvEP always did). `main.cpp`'s `loop()` still unconditionally calls
  `amy_update()` every tick, and ~9 raw `amy_add_event` call sites (drum-pad hi-hat chokes,
  mostly) still talk to it directly. **Do not remove AMY's init** even from a mode you're
  not touching — `loop()` depends on it being alive regardless (confirmed the hard way: a
  Phase-1 rewrite that skipped `amy_start()` crashed real hardware with a null-pointer
  `StoreProhibited` the instant `amy_render()` ran against never-allocated AMY state).
- **The vendored engine** (`engine/` — `SevenistSynth_RitualMachine`'s `framework/engine/`,
  copied verbatim) — graph-based (`GraphDesc`/`Engine::load()`), not event-based. One
  instance (`g_mainEngine` in `audio_engine.cpp`) with one fixed placeholder graph (saw →
  filter → env → vca) backs the facade's core functions today; more instances/graphs land
  per the plan's phases. Canonical minimal usage pattern:
  `SevenistSynth_RitualMachine/apps/template/src/synth.cpp`.
- **Both engines' audio is mixed, not routed exclusively** — `audioEngineRenderBlock()`
  (declared `extern "C"` in `audio_engine.h`/defined in `audio_engine.cpp`) is called and
  saturating-added into AMY's own output buffer at the exact point AMY itself finishes
  rendering a block, in **two separate places** (one per audio backend):
  - `lib/AMY Synthesizer/src/libminiaudio-audio.c`'s `data_callback()` — desktop sim,
    Android, and Web all share this file and this one edit.
  - `lib/AMY Synthesizer/src/i2s.c`'s `amy_render_audio()` — real ESP32 hardware's I2S
    path; this file isn't compiled for the other three targets at all, so it needed its
    own, separate mixing edit (confirmed via `nm`/the linker map, not assumed).
  **Any future change to how either engine produces audio must keep both mixing points in
  sync** — fixing one and not the other reproduces exactly the "no sound on hardware, fine
  in the simulator" bug this setup already hit once.

## Sample playback from SD (keys, preview, MODE_DJ) — the engine's own streaming sampler

The sample keys (MODE_SAMPLE/SS2), the browser preview and MODE_DJ's deck all play through
the vendored engine's SampleBank + Pads (same engine instance as the drums), see the
"SD streaming" section at the end of `src/audio_engine.cpp`:
- A WAV/MP3 is converted **once, on the board** into the engine's `.smp` format
  (`src/smp_import.*`), cached in `/grvsmp/cache/<hash>.smp` and reused while the source
  keeps its size. Upstream's ESP32 app has no such import (it expects `.smp` cooked on a PC);
  ours mirrors its simulator's convert-on-first-use. `.smp` is mono 16-bit: ~5.3 MB per minute.
- **One loader task** (`smpLoaderTask`) does all of the sampler's SD work: conversions and
  `SampleBank::pump()`. Keep `SampleBank::load()/unload()` on that task too (upstream calls them
  from its card task, never concurrently with `pump()`).
- Small `GrvEP-fusion` additions in `engine/modules/sampler_modules.*` (live tune/direction on a
  sounding pad, `PadBlob.reverse`) and `SampleBank::kMaxSamples` 32→64 — marked in the source.
- Measured on the board (2026-10-28): converting is ~2.4x real time for an MP3 (a 2:22 track
  takes ~60 s: 2/3 Helix decoding on core 0 next to the audio render, 1/3 SD writes), once per
  file; a cached file loads in ~1 s. SD writes fail at random a few times per long conversion and
  FatFs then fails every later call on that FILE — `flushBlock()` reopens the file to retry.
- Internal heap after boot is only ~11.6 KB (it was **2.6 KB** until three function-static
  6 KB `GraphDesc`s moved to one PSRAM scratch, `scratchGraph()`): every `fopen` needs a small
  internal-RAM lock and aborts when there is none. The Helix MP3 decoder's ~8.6 KB of small
  allocations are pushed to PSRAM (`heap_caps_malloc_extmem_enable()` around
  `MP3InitDecoder()`). The PSRAM itself is small (the pads heap only gets its 768 KB fallback,
  ~440 KB stay free).
- **The engine's command queue is single-producer and only `ENGINE_CMD_RING=32` deep; a full
  queue drops commands silently.** This facade is called from both `loop()` and main.cpp's
  `audioHandlerTask`, so every post to the Pads engine goes through `padsSetBlob()`/
  `padsSetParam()` (a mutex) — and never burst dozens of commands at once (e.g. only stop the
  pads that `pads_playing()` says are sounding).

## RAM is tight on real hardware — measure, don't guess

Vendoring the new engine's own static state (the `Engine` object itself, dominated by its
`ENGINE_CMD_RING` command queue, plus whatever placeholder heap `audioInit()` gives it) all
land in the ESP32-S3's ~320KB of internal DRAM, the same budget `main.cpp`'s own 18K lines
and AMY's own state already use most of. This has already broken two different things in
practice, both only on real hardware (the simulator has effectively unlimited RAM and won't
catch either):
- Too large a static buffer (256KB) overflowed DRAM outright at link time (a hard build
  failure, easy to catch).
- Too *small* a buffer (16KB, over-corrected from the above) let the firmware link and
  boot fine, but silently failed `Engine::load()` (`Err::NoMem`, not fatal, no crash) — and
  **separately**, starved a later `xTaskCreatePinnedToCore()` call in `main.cpp`'s `setup()`
  (whose return value is never checked) of enough free heap, which manifested as a frozen
  OLED with no other symptom at all. Two different failure modes from the same root cause,
  neither obvious from the symptom alone.
- Current working values (verified via the linker map, `pio run`'s own RAM%, and an
  `ESP.getFreeHeap()` probe around the task-creation call — see git history on
  `src/audio_engine.cpp`/`platformio.ini` for the exact investigation): `fastMem[48*1024]`
  in `audioInit()`, `-DENGINE_CMD_RING=32` in `platformio.ini`. Before changing either,
  rebuild for ESP32 and check `pio run`'s own `RAM: [...] X% (used N bytes from 327680
  bytes)` line, not just "it compiles" — then flash and check `ESP.getFreeHeap()` actually
  stays healthy after `setup()`'s task creation, not just that the build succeeded.

## The four build targets

| Target | Source | Build |
|---|---|---|
| **ESP32-S3** (real hardware) | `src/` + `engine/` via PlatformIO | `pio run -e esp32-s3-devkitc-1` |
| **Linux/Windows desktop simulator** | `src/` + `engine/` + `simulator/` via SDL2 | `cd simulator && ./build.sh` (Linux) or `./build_windows.sh` (cross-compile a Windows .exe) |
| **Android APK** | `src/` + `engine/` + the same SDL2 simulator, native C++ | `./build_native_apk.sh` (first run: `cd android-native && ./setup_android.sh`) |
| **Web (WASM/Emscripten)** | `src/` + `engine/` + `web/` via Emscripten | `cd web && ./build.sh`, see `structure/WEB_SIMULATOR.md` |

`platformio.ini`'s `src_dir` is the repo root (not the default `src`) specifically so its
`build_src_filter` can pull in both `src/` and `engine/` as one firmware image — mirrors
`SevenistSynth_RitualMachine/apps/ritual_machine/platformio.ini`'s own `src_dir = ../..`
for the identical split. `simulator/CMakeLists.txt` and `web/CMakeLists.txt` each have
their own `ENGINE_SOURCES` glob doing the same job for their targets.

Full commands (flashing, monitor, `--install`, release builds, etc.) are in the root
`README.md`.

## AMY: one copy, shared by every target

AMY (the synth engine, `amy.c`/`amy.h`/etc.) lives in exactly one place:
**`lib/AMY Synthesizer/src/`**, vendored in this repo. The ESP32 build picks it up
automatically because PlatformIO scans `lib/` (there is deliberately no AMY entry in
`platformio.ini`'s `lib_deps`), and the simulator/Android/Web builds compile the same
files via `simulator/CMakeLists.txt` (`AMY_SRC`) and `simulator/hal/AMY-Arduino.h`. No
sibling checkout is needed anymore. (It used to be two hand-synced copies — this repo's
plus `../other_projects/amy` — which had drifted apart, e.g. the ESP32 one lacked the
bus1->bus0 merge, so bus-0 FX never reached the TB-303 there.) Any DSP change is made
once, in `lib/AMY Synthesizer/src/`. **This fork also touches two files in this vendored
copy** (`libminiaudio-audio.c`, `i2s.c`) to mix in the new engine's output — see "Two synth
engines" above; keep that in mind before assuming this directory is untouched/pristine AMY.

The **new vendored engine** lives at `engine/` (repo root, a sibling of `src/`, copied
verbatim from `SevenistSynth_RitualMachine/framework/engine/` — see "Two synth engines"
above). Unlike AMY, it has no ESP32/ARM-specific build quirks beyond one optional assembly
file (`engine/dsp/fft_s3.S`); it's portable C++17 and compiles the same way on all four
targets without a HAL shim layer of its own (Phase 1's scope deliberately stopped at the
audio layer — GrvEP's own input/display/LED drivers are untouched).

Buffer layout note: the per-bus `fbl[]` buffers are **planar** (`[L,L,L...][R,R,R...]`,
channel `c` at `buf + c*AMY_BLOCK_SIZE`) in this copy, despite a stale "interleaved"
comment on `mix_with_pan()`. Index them planar-style in any new bus DSP. `SAMPLE` is
fixed-point (s8.23) on every target incl. the simulator — use `S2F()`/`F2S()`.

If PlatformIO ever seems to build a stale AMY after an edit:
```bash
rm -rf .pio/build/esp32-s3-devkitc-1
pio run -e esp32-s3-devkitc-1     # first run after clearing can hit a transient SCons error; just re-run
```

## Architecture docs (`structure/`)

Deep-dive documentation lives in `structure/` — see `structure/README.md` for an index.
Taking over the project? Read `structure/HANDOFF.md` first (owner's working agreements,
current state, pitfalls, pending work). Then `structure/SOFTWARE.md` (modes, AMY channel map, FX chain) and
`structure/TEMPLATES.md` (the shared UI conventions every mode follows — read this before
adding a new mode, most of the wiring is copy-a-template-and-fill-in-the-blanks).

`docs/GUIDE_UTILISATEUR.md` is the end-user manual (French), not developer documentation.

## Testing methodology

The project has no automated test suite; verification is done by actually building and
running the simulator.

- **Interactive GUI automation (`xdotool` driving the SDL window) has proven unreliable**
  in sandboxed/headless environments partway through past sessions — inputs stop reaching
  the app despite correct window IDs/focus. This is an X11/WM-level issue, not a code
  regression; don't spend long debugging it.
- **The reliable fallback is headless boot-time injection**: temporarily edit `setup()` to
  force a mode/state and fire the sequence of actions you want to test, verify via
  `Serial.printf`/`fprintf(stderr, ...)` output (redirect stdout/stderr to a file, `grep`
  it) and, for anything audio-observable, an RMS/peak tap inserted temporarily into the
  audio callback — `lib/AMY Synthesizer/src/libminiaudio-audio.c`'s `data_callback`
  (desktop/Android/Web) right after `amy_simple_fill_buffer()`, **and separately**
  `lib/AMY Synthesizer/src/i2s.c`'s `amy_render_audio()` for real ESP32 hardware (not
  compiled for the other three targets — a tap in one says nothing about the other). On
  real hardware without a serial monitor attached to a TTY, read `/dev/ttyACM*`/
  `/dev/ttyUSB*` directly with `pyserial` instead of `pio device monitor` (which refuses
  piped/non-interactive stdin) — toggle RTS low→high→low first to force a clean reset so
  the baud-rate framing lines up with the bootloader's own banner. **Always remove the
  temporary instrumentation before finishing** — grep the diff for stray `TEMP DBG`/debug
  defines before considering a fix done.
- `setup()` runs to completion before `loop()` ever starts — a test that blocks with
  `delay()` inside `setup()` will never see `loop()`-driven ticks (10ms modulation engines,
  sequencer clocks, etc.) run at all. Either drive those ticks manually in a loop inside
  `setup()` (e.g. call the tick function directly, `delay(10)` between calls) or restructure
  the test to not need `loop()`'s natural cadence.
- The two extra simulator windows (spectrogram + real-time EQ curve, see README) are the
  fastest way to visually sanity-check a filter/EQ/FX change, especially with the NOISE
  category instruments (WHT/PINK/BRWN) whose full-band spectrum makes a filter's frequency
  response immediately visible.
- Kill any simulator process you started for a test before finishing your turn —
  `pgrep -af grvep_sim` / `kill`. Background instances left running (especially ones still
  holding a note or looping FX) both hint at test state you haven't cleaned up and can make
  a *later* test's audio device contend with a zombie instance and hang.

## Recurring bug classes worth knowing about

These have each shown up more than once across different modes — worth checking for by
default when touching related code:

- **A full "reconfigure this channel" `amy_event`** (setting `num_voices`/`oscs_per_voice`/
  envelope/etc., as sent by `patches_load_patch()`/`reset_osc()`-style calls) **silently
  resets or kills an already-sounding voice** on that channel/oscillator. Anything that can
  fire mid-note (changing a wavetable, retriggering an already-playing sample voice, etc.)
  needs a lighter "touch only this one field" event instead of the full reconfigure. When a
  filter/param FX doesn't seem to "stick" on notes triggered *after* the FX was turned on
  (as opposed to notes already playing when it was toggled), check whether the mode's own
  note-on path is silently reverting state that the FX set.
- **Shared FX (like the FILT filter) only reach the channels/oscillator ranges someone
  explicitly wired them to.** AMY's dynamic voice allocator doesn't know about
  dedicated/fixed oscillator ranges (STONE, MODULAR's MOD3 channels, GRANULAR2's pool,
  etc.), so a new dedicated range silently doesn't get the shared FX unless an
  `audioApplyFilterToX()`-style helper is added and wired into both
  `audioSetAllFilters()`/`audioSetAllFiltersT()`. This has been the root cause of "the FX
  overlay works but nothing audibly changes" bugs more than once.
- **Two positional arrays are indexed by `AppMode`, not by name**: `kIsSeq[MODE_COUNT]` and
  `kVizMode[MODE_COUNT]` in `main.cpp`. Adding a new `AppMode` enum value requires adding an
  entry to *both*, in the same relative position — C++ aggregate init silently zero-pads a
  short `bool` array (a harmless-looking but real bug: a missing entry defaults to `false`),
  while a short `const char*` array crashes on the missing entry (`printf("%s", nullptr)`).
  Grep for `kIsSeq\[` / `kVizMode\[` to find both when adding a mode.
- Before adding a new FX or mode, check whether an existing generic mechanism already
  covers what's needed rather than adding a special case — e.g. the generic modulation-slot
  engine (`ModSlot`/`gModSlots[]`) can automate any `fxList[]` param already; a bespoke
  fixed-destination FX (the project used to have a dedicated filter-cutoff-only "LFO" FX
  slot) is redundant once that exists.
- **Exact-equality thresholds and flat clamps near a parameter's own range boundary are
  fragile, in two different ways.** (1) A hard `value >= 18000.0f`-style bypass check can
  flicker unpredictably even at a fixed knob position, because an exponential pot-to-value
  mapping (`mn*powf(mx/mn, potValue)`) doesn't reliably land exactly on the literal even at
  `potValue==1.0` — plain float rounding in `powf` is enough to land a few units on either
  side. (2) A flat clamp like `fminf(value, ceiling)` applied above some threshold can turn
  a large fraction of a knob's travel into a dead zone with zero perceptible change (e.g.
  capping FILT's cutoff at a patch's native ~994Hz made ~half the pot's range — everything
  from 994Hz to 18000Hz — sound identical). The fix in both cases: don't gate on
  "value crossed a threshold" — either add real margin/hysteresis, or better, replace the
  clamp with a continuous remap so the *whole* physical range of the control stays
  meaningful (see `remapCutoffResToNative()` in `audio_engine.cpp`). The same "half the
  control does nothing" symptom can also come from splitting one physical pot into two
  logically-independent halves (e.g. a BPM-sync toggle whose "off" half didn't drive the
  displayed rate at all) — reads to a user as "the pot got stuck," worth checking for
  whenever a single control's range is conceptually divided into sub-modes.
- **GIF playback (MEDIA/`MODE_VID`) is simulator/Android-only.** `imgDecodeGifFrames()` (in
  `simulator/hal/jpegdec.h`) uses stb_image's already-vendored GIF decoder
  (`stbi_load_gif_from_memory`); the ESP32-side `include/jpegdec.h` copy is a stub that logs
  and points at `tools/to_bvid.py` — no on-device LZW/multi-frame GIF decoder has been
  written (a much bigger undertaking than the existing hand-rolled streaming PNG decoder in
  that same file, and not something to attempt untested against real hardware).

## Conventions

- Commits are made only when explicitly requested — don't `git add`/`commit`/`push` as a
  side effect of finishing a task, even a large one. Report what changed and wait.
- Commit messages end with `Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>` (or
  the equivalent line for whichever Claude model made the change).
- No automated test suite — "done" means built cleanly for the target(s) touched *and*
  verified via the simulator (see Testing methodology above), not just "compiles."

## graphify

This project has a knowledge graph at graphify-out/ with god nodes, community structure, and cross-file relationships.

Rules:
- For codebase questions, first run `graphify query "<question>"` when graphify-out/graph.json exists. Use `graphify path "<A>" "<B>"` for relationships and `graphify explain "<concept>"` for focused concepts. These return a scoped subgraph, usually much smaller than GRAPH_REPORT.md or raw grep output.
- If graphify-out/wiki/index.md exists, use it for broad navigation instead of raw source browsing.
- Read graphify-out/GRAPH_REPORT.md only for broad architecture review or when query/path/explain do not surface enough context.
- After modifying code, run `graphify update .` to keep the graph current (AST-only, no API cost).
