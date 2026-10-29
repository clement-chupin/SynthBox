# structure/ — architecture documentation

Deep-dive developer documentation for GrvEP. If you're setting up the project for the
first time or want the essentials (build targets, the single shared AMY copy, testing
methodology, recurring bug classes), start with **`/CLAUDE.md`** at the repo root instead —
it's the short version and it's what any Claude Code session reads automatically. Come back
here for the full detail on a specific subsystem.

## Contents

- **[AGENT.md](AGENT.md)** — the same index as this file, framed as instructions for an AI
  coding agent (Claude Code or otherwise) rather than a human: which doc to read for which
  task, and the non-negotiables an agent is most likely to miss on a first pass.
- **[HANDOFF.md](HANDOFF.md)** — **start here when taking over the project**: working agreements
  with the owner, repository state, what changed recently, recurring pitfalls, pending work,
  how to verify a change, and a map of all documentation.
- **[SOFTWARE.md](SOFTWARE.md)** — the main architecture reference: the full mode list,
  the AMY channel/oscillator map and bus order, the FX list and the activation-order bus
  chain, the generic modulation-slot automation engine, and
  dedicated sections on the more involved modes (303S, the wavetable modular synth, the
  experimental/generative modes).
- **[TEMPLATES.md](TEMPLATES.md)** — the shared UI conventions every mode is built from
  (pots/buttons/joystick/LED/OLED layout for the SYNTH and SEQUENCER templates, plus the
  free-form STANDALONE template). Read this before adding a new mode — most of a new mode's
  wiring is "pick a template, fill in the blanks," not novel design.
- **[NEW_MODE_CHECKLIST.md](NEW_MODE_CHECKLIST.md)** — the mechanical companion to
  TEMPLATES.md: every file/array/switch a new mode must touch (`config.h` enums,
  `kIsSeq[]`/`kVizMode[]`/`kMenuCatSizes[]` positional arrays, `ctrlLabelsFor()`,
  `handleButton()`/`handleNoteKeyAudio()`/pot-dispatch/`drawScreen()`/`updateLedsAndShow()`
  cases), plus the mistakes already made once in this project (don't duplicate
  `drawBtnRow()`'s labels as text, reuse an existing audio engine before writing a new one,
  the symmetric checklist for *removing* a mode cleanly).
- **[HARDWARE.md](HARDWARE.md)** — MCU, pin map, and the peripherals (keyboard controller,
  OLED, LED strip, joystick, pot mux, SD card, power) as wired on the real board.
- **[SIMULATOR.md](SIMULATOR.md)** — internals of the desktop SDL2 simulator: shared state,
  threads, how pots/OLED/LEDs are reconstructed on a PC, the build, and the headless
  verification recipes (audio tap, OLED screenshot, boot-time test injection).
- **[WEB_SIMULATOR.md](WEB_SIMULATOR.md)** — the WebAssembly/Emscripten build: the JS↔WASM
  I/O bridge, exported functions, build flags, and how it differs from the ESP32 and SDL2
  targets.
- **[prompt_claude.txt](prompt_claude.txt)** — a raw, chronological log of early design
  briefs/feature requests from the project owner. Historical record of *why* some things
  are shaped the way they are, not curated documentation — check the other files above
  first; come here only if you need the original reasoning behind an existing design
  decision.

## Other documentation in this repo

- **`/CLAUDE.md`** — start here (see above).
- **[docs/GUIDE_UTILISATEUR.md](../docs/GUIDE_UTILISATEUR.md)** — the end-user manual
  (French), for people playing the instrument, not developing it.
- **[inspiration/](../inspiration/)** — research notes on comparable commercial
  groovebox/synth products (Teenage Engineering, Korg, this.is.NOISE), kept for future
  feature ideas.
