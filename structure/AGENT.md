# AGENT.md — orientation for an AI coding agent

Entry point for any AI agent (Claude Code, Codex, or otherwise) picking up work in GrvEP.
It doesn't duplicate content — it tells you which file answers which question, in roughly
the order you'll need them. For the same index framed for a human reader, see
[README.md](README.md).

## 1. Read this first, always

**`/CLAUDE.md`** (repo root) — the short version: what GrvEP is, the four build targets and
their build commands, and the single most likely thing to trip up a fresh session (AMY's
one shared copy, `lib/AMY Synthesizer/src/`, vendored once and compiled by every target —
there is no second copy to keep in sync anymore). Claude Code loads this file automatically
at session start; if you're a different agent/tool, read it explicitly before touching
build config or DSP code.

## 2. Taking over an in-progress task, or the whole project

[HANDOFF.md](HANDOFF.md) — owner's working agreements, current repo state, a chronological
log of what changed recently, recurring pitfalls, and pending work. If a person says
"continue where the last session left off" or "take over this project," this is where that
context lives — read it before forming a plan.

**Important:** a large amount of work is deliberately kept uncommitted between sessions
(see HANDOFF.md §3 for the exact list of modified files and what each change covers).
Before doing anything, run `git status` and `git diff` — the working tree is very
probably ahead of the last commit (`969361e`) by ~1400 lines across `main.cpp`,
`audio_engine.*`, `amy.c`, docs, and several HAL/simulator files.
To get a named snapshot of the current state before an experiment:
```bash
git stash push -u -m "checkpoint before <experiment name>"
# to restore:
git stash pop
```

## 3. Understanding the architecture before changing anything non-trivial

[SOFTWARE.md](SOFTWARE.md) — the full mode list, the AMY channel/oscillator map, bus order,
the FX list and its activation-order chain, the generic modulation-slot automation engine,
and dedicated deep-dive sections for the more involved modes. This is the map to consult
before editing audio_engine.cpp or main.cpp's mode-dispatch code.

## 4. Adding a new mode

[TEMPLATES.md](TEMPLATES.md) — the shared UI conventions (pots/buttons/joystick/LED/OLED)
every mode is built from. Read this before writing a new mode — it's almost always
copy-a-template-and-fill-in-the-blanks, not novel design, and it documents the exact
checklist of touch-points (enums, arrays sized by mode count, menu wiring, etc.) that are
easy to leave incomplete — GrvEP has had real crashes from exactly that (see CLAUDE.md's
"recurring bug classes" and HANDOFF.md's pitfalls section for named examples).

[NEW_MODE_CHECKLIST.md](NEW_MODE_CHECKLIST.md) — the mechanical companion to TEMPLATES.md:
the concrete list of every file/array/switch a new mode must touch (`config.h` enums,
`kIsSeq[]`/`kVizMode[]`/`kMenuCatSizes[]` positional arrays, `ctrlLabelsFor()`,
`handleButton()`/`handleNoteKeyAudio()`/pot-dispatch/`drawScreen()`/`updateLedsAndShow()`
cases), plus the mistakes already made once in this project and the symmetric checklist
for *removing* a mode cleanly (relevant precedent: MODE_303A/MODE_303B were built, fully
wired, then explicitly rejected and removed in one session — see HANDOFF.md §4 entry AE).

## 5. Working on hardware specifics

[HARDWARE.md](HARDWARE.md) — MCU, pin map, and the peripherals (keyboard controller, OLED,
LED strip, joystick, pot mux, SD card, power) as wired on the real board.

## 6. Working on the simulator, or verifying any change

[SIMULATOR.md](SIMULATOR.md) — internals of the desktop SDL2 simulator (shared state,
threads, how pots/OLED/LEDs are reconstructed on a PC), the build, and — most importantly —
the headless verification recipes (audio RMS/peak tap, OLED screenshot capture, boot-time
test injection) that stand in for a test suite this project doesn't have. Read this before
reporting a change as "done."

## 7. Working on the web/WASM build

[WEB_SIMULATOR.md](WEB_SIMULATOR.md) — the JS↔WASM I/O bridge, exported functions, build
flags, and how it differs from the ESP32 and SDL2 targets.

## 8. Historical design reasoning

[prompt_claude.txt](prompt_claude.txt) — a raw, chronological log of early design
briefs/feature requests from the project owner. Only worth opening if the other files
don't explain *why* something existing is shaped the way it is — check them first.

## Non-negotiables

(Also in `/CLAUDE.md` — repeated here because they're the ones an agent is most likely to
get wrong on a first pass.)

- DSP changes go in `lib/AMY Synthesizer/src/` **once**. Every build target (ESP32, the
  SDL2 simulator, Android, Web) compiles that same copy.
- There is no automated test suite. "Done" means built cleanly for every target touched
  **and** verified via the simulator (see SIMULATOR.md) — not just "it compiles."
- Commit or push only when explicitly asked — never as a side effect of finishing a task,
  however large.
- Any temporary debug instrumentation added for verification (`TEMP_DBG_*`-style guarded
  blocks, audio taps, screenshot hooks) must be fully removed before calling a fix
  finished — `grep` the diff for it before finishing.

## 9. If the project appears broken or the build is red

**First, triage before touching anything:**

```bash
# 1. What is the uncommitted state?
git status
git diff --stat

# 2. Does the simulator still build?
cd simulator && rm -rf build && ./build.sh 2>&1 | tail -30

# 3. Does the ESP32 target still build?
pio run -e esp32-s3-devkitc-1 2>&1 | tail -30

# 4. Is a leftover simulator process blocking the audio device?
pgrep -af grvep_sim && kill $(pgrep -f grvep_sim)
```

**If the build is broken and you need to recover the last known-good state:**
```bash
# See all stashes (the owner may have saved one before the experiment)
git stash list

# Restore the most recent stash
git stash pop

# Or, go back to the last commit entirely (DESTRUCTIVE — discards all uncommitted work)
git checkout .    # only after confirming git stash list is empty or irrelevant
```

**If the build is broken after an AMY DSP edit:**
```bash
rm -rf .pio/build/esp32-s3-devkitc-1
pio run -e esp32-s3-devkitc-1   # first run after clearing may hit a transient SCons error; re-run
```
AMY lives only in `lib/AMY Synthesizer/src/` — there is no second copy. Any edit there affects
all four targets simultaneously.

**Common causes of a suddenly broken build (from CLAUDE.md's recurring bug classes):**
- Added an `AppMode` enum value without adding entries to `kIsSeq[]` and `kVizMode[]` in `main.cpp`
  (short `const char*` array → `printf("%s", nullptr)` crash at runtime)
- Indexed `fbl[]` bus buffers as interleaved instead of planar (`buf + ch*AMY_BLOCK_SIZE`)
- Sent a full AMY reconfigure event mid-note (kills the voice silently)
- Left a `TEMP_DBG` define or audio tap in the diff (`grep -rn TEMP_DBG src/ simulator/ lib/`)

## If you're not Claude Code

Nothing in this project's conventions (`CLAUDE.md`, this file) is Claude-specific — the
build commands, file locations, and working agreements apply the same regardless of which
agent or tool is doing the work.
