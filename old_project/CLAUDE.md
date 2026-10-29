# GrvEP — context for Claude

GrvEP is a portable groovebox/synthesizer firmware for an ESP32-S3: 30+ modes (drum
sequencers, subtractive/FM/wavetable/TB-303/granular/sampler synths, and a family of
generative modes — cellular automaton, boid swarm, theremin, ball physics...), SD-card
sample playback, OLED, a 4×8 key grid, joystick, 7 potentiometers, addressable LEDs.

The same application code (`src/`) runs on four targets. **Read this whole file before
touching build config or AMY's DSP code** — the single AMY copy described below is the single
most likely thing to trip up a fresh session.

## The four build targets

| Target | Source | Build |
|---|---|---|
| **ESP32-S3** (real hardware) | `src/` via PlatformIO | `pio run -e esp32-s3-devkitc-1` |
| **Linux/Windows desktop simulator** | `src/` + `simulator/` via SDL2 | `cd simulator && ./build.sh` (Linux) or `./build_windows.sh` (cross-compile a Windows .exe) |
| **Android APK** | `src/` + the same SDL2 simulator, native C++ | `./build_native_apk.sh` (first run: `cd android-native && ./setup_android.sh`) |
| **Web (WASM/Emscripten)** | `src/` + `simulator/` via Emscripten | see `structure/WEB_SIMULATOR.md` |

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
once, in `lib/AMY Synthesizer/src/`.

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
  audio callback (`simulator/.../libminiaudio-audio.c`'s `data_callback`, right after
  `amy_simple_fill_buffer()`). **Always remove the temporary instrumentation before
  finishing** — grep the diff for stray `TEMP DBG`/debug defines before considering a fix
  done.
- **On real hardware** (board usually on `/dev/ttyACM1`): flash with
  `pio run -e esp32-s3-devkitc-1 -t upload --upload-port /dev/ttyACM1`. To read the serial
  log from an agent, **don't use `pio device monitor`** — it refuses to run without an
  interactive TTY and silently captures nothing. Read the port with pyserial instead
  (`~/.platformio/penv/bin/python`, open with `dtr=False, rts=False` so it doesn't reset the
  board), in the background, while the owner reproduces the issue. `handleButton()` already
  logs every button event (`BTN raw=… mapped=… DN/UP`); add `TEMP DBG` prints for state and
  remove them afterwards like any other instrumentation.
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
