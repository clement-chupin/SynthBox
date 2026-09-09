# structure/ — architecture documentation

Deep-dive developer documentation for GrvEP. If you're setting up the project for the
first time or want the essentials (build targets, the AMY dual-copy sync rule, testing
methodology, recurring bug classes), start with **`/CLAUDE.md`** at the repo root instead —
it's the short version and it's what any Claude Code session reads automatically. Come back
here for the full detail on a specific subsystem.

## Contents

- **[SOFTWARE.md](SOFTWARE.md)** — the main architecture reference: the full mode list,
  the AMY channel/oscillator map (which fixed ranges belong to which mode), the FX chain
  (bus-level vs per-channel effects, the generic modulation-slot automation engine), and
  dedicated sections on the more involved modes (303S, the wavetable modular synth, the
  experimental/generative modes).
- **[TEMPLATES.md](TEMPLATES.md)** — the shared UI conventions every mode is built from
  (pots/buttons/joystick/LED/OLED layout for the SYNTH and SEQUENCER templates, plus the
  free-form STANDALONE template). Read this before adding a new mode — most of a new mode's
  wiring is "pick a template, fill in the blanks," not novel design.
- **[HARDWARE.md](HARDWARE.md)** — MCU, pin map, and the peripherals (keyboard controller,
  OLED, LED strip, joystick, pot mux, SD card, power) as wired on the real board.
- **[SIMULATOR.md](SIMULATOR.md)** — internals of the desktop SDL2 simulator: shared state,
  threads, how pots/OLED/LEDs are reconstructed on a PC, the build.
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
