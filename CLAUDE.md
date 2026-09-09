# GrvEP — context for Claude

GrvEP is a portable groovebox/synthesizer firmware for an ESP32-S3: 30+ modes (drum
sequencers, subtractive/FM/wavetable/TB-303/granular/sampler synths, and a family of
generative modes — cellular automaton, boid swarm, theremin, ball physics...), SD-card
sample playback, OLED, a 4×8 key grid, joystick, 7 potentiometers, addressable LEDs.

The same application code (`src/`) runs on four targets. **Read this whole file before
touching build config or AMY's DSP code** — the AMY dual-copy setup below is the single
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

### Required one-time setup for the ESP32 target

`platformio.ini`'s `lib_deps` includes `${PROJECT_DIR}/../other_projects/amy` — a
**hard-coded relative path to a sibling checkout that does not exist in this repo**. The
ESP32 build will fail to find the AMY library unless that sibling clone exists:

```bash
# from the parent directory of this repo
mkdir -p other_projects
git clone https://github.com/shorepine/amy.git other_projects/amy
```

Nothing else in this repo depends on that sibling path — only the ESP32/PlatformIO build.
The desktop simulator, Android, and Web targets use the AMY copy vendored directly in
`lib/AMY Synthesizer/src/` (see below) and don't need it.

## AMY: two copies, kept in sync by hand

AMY (the synth engine, `amy.c`/`amy.h`/etc.) exists in **two places** in this project, and
they are **not** the same file kept in sync by tooling — any DSP-level change (bus effects,
oscillators, filters) has to be applied to both by hand:

1. **`lib/AMY Synthesizer/src/`** — vendored into this repo, used by the desktop simulator
   and Android builds. Audio buffers here are **interleaved** stereo (`L,R,L,R,...`).
2. **`../other_projects/amy/src/`** (the sibling clone above) — used by the ESP32 build via
   PlatformIO's `lib_deps`. Audio buffers here are **planar** (`[L,L,L...][R,R,R...]`).

The buffer layout difference is the single most common source of subtle bugs when porting a
DSP change between the two copies — a loop that does `for (i = ch; i < n; i += NCHANS)`
(interleaved-correct) is wrong on the planar copy, which needs
`for (i = 0; i < BLOCK_SIZE; i++)` over `buf + ch*BLOCK_SIZE`. When changing AMY DSP code,
check both loops' indexing explicitly, not just copy-paste the body.

After touching the ESP32-side copy, PlatformIO's cached lib copy can go stale — if a rebuild
doesn't pick up the change:
```bash
rm -rf ".pio/libdeps/esp32-s3-devkitc-1/AMY Synthesizer"
pio run -e esp32-s3-devkitc-1
```

## Architecture docs (`structure/`)

Deep-dive documentation lives in `structure/` — see `structure/README.md` for an index.
Start with `structure/SOFTWARE.md` (modes, AMY channel map, FX chain) and
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

## Conventions

- Commits are made only when explicitly requested — don't `git add`/`commit`/`push` as a
  side effect of finishing a task, even a large one. Report what changed and wait.
- Commit messages end with `Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>` (or
  the equivalent line for whichever Claude model made the change).
- No automated test suite — "done" means built cleanly for the target(s) touched *and*
  verified via the simulator (see Testing methodology above), not just "compiles."
