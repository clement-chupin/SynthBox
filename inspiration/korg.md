# Research: Korg Grooveboxes — Volca series, Electribe series, NTS-1

## Overview

Korg's small-format instruments (Volca, Electribe, NTS-1) share a philosophy distinct from Teenage Engineering's: instead of "hold+tap combos on very few buttons," Korg leans on a **fixed, always-visible bank of 16 step/trigger keys plus a handful of clearly-labeled function buttons**, and gets sequencing depth primarily through **"motion sequencing"** (recording live knob/parameter movement into the sequence itself) rather than one-shot punch-in effects. This is a different and complementary lesson for GrvEP: depth through *recorded automation* rather than *momentary performance macros*.

---

## 1. Volca series (Beats, Keys, FM, Sample, Modular, Drum)

### Shared architecture across the line
- All Volcas share: a built-in speaker, battery power, a 16-step sequencer with 16 physical step keys, a 3.5mm SYNC in/out pair (see Sync below), MIDI IN (rarely MIDI OUT without a mod), and — on most models — a touch/ribbon **keyboard strip** for entering notes by sliding a finger, rather than a piano-style keybed.

### Step sequencing UI
- **16 keys = 16 steps**, always visible/lit, giving an immediate at-a-glance readout of which steps are active — no menu diving to see pattern shape.
- **Active Step**: lets you temporarily deactivate specific steps (mute without erasing) for on-the-fly arrangement variation.
- **Step Jump**: instantly jumps playback to whichever step key you're currently holding — a direct performance-scrub feature (like "step preview") that turns the step grid into a live-playable strip, not just an editor.
- **Function+step combos** unlock secondary behaviors (e.g., motion-record toggle, reverse-per-part), keeping the primary 16-key surface single-purpose (steps) while function-layering adds depth — a shallower hold-modifier grammar than TE's, but similar idea.

### Motion Sequencing — the standout idea
- Press a record-enable combo (e.g., `FUNC + S2` on Volca Sample), then **while the pattern plays, turn any knob** — Korg records that knob's movement over time as a per-step (or continuous) automation lane bound into the pattern. Play it back and the knob visibly/audibly re-enacts your gesture.
- This is functionally close to Elektron/TE parameter-locking but recorded **as a continuous performance gesture over the whole loop**, rather than punched in step-by-step — a different capture *paradigm* (record a live sweep vs. dial in discrete values per step) that's worth having *both* options for.
- Parameters commonly recordable via motion sequencing: start point, sample length, hi-cut/filter, playback speed, pitch-EG intensity/attack/decay, amp level, pan, amp-EG attack — i.e., essentially every knob on the panel is fair game, so almost any twist of the hardware becomes "sequenceable" without a dedicated automation-lane UI.
- On the **Volca Drum**, Korg explicitly added **Elektron-style parameter locks** too (hold a step + turn a knob to bake a value into just that step) — showing Korg itself uses *both* motion-sequencing (continuous) and parameter-locks (discrete per-step) side by side as complementary tools, not a single "pick one" design.

### Per-model distinctive ideas
- **Volca Beats**: hybrid drum machine — some voices analog-synthesized, others PCM-sample-based, within the same 16-key grid, so different sound-generation methods coexist per-voice without the user needing to think about the difference.
- **Volca FM**: a genuinely complex 6-operator FM engine made approachable via two "super knobs" — MODULATOR and CARRIER — each mapped internally to *multiple* underlying FM parameters at once, so one physical knob gives a musically-sensible macro sweep across a parameter space that would otherwise need a whole page of sliders. Also has **WARP ACTIVE STEP** (temporarily/probabilistically alters the currently playing step's pitch/behavior) and **Pattern Chain** for building longer arrangements from short patterns.
- **Volca Sample**: **frequency isolator** control (single knob boosting/cutting low or high range) used live for build/breakdown transitions — a one-knob mix-FX move rather than a full EQ. Also independent **reverse-per-part** and a "Loop" mode used for granular/stutter-style live mangling.
- **Volca Modular**: fully patchable (50 patch points, 20 included pin cables) semi-modular analog voice — but layered under the *same* Volca sequencer UI as the other models. Its sequencer specifically supports:
  - **Randomization of notes, active-steps, and micro-tuning** — dedicated controls to inject controlled randomness into what would otherwise be a fixed pattern.
  - **Pattern chaining** up to 16 patterns / up to 256 steps for long-form generative development.
  - **Bounce sequence mode** (ping-pongs forward/backward through steps) and **Stochastic sequence mode** (advances with a random forward/backward bias) — two named alternative *step-order algorithms*, not just an on/off randomize toggle. This is a very clean, reusable idea: expose a small menu of sequencer "traversal modes" (forward, backward, ping-pong/bounce, stochastic, random) as a single selectable parameter rather than ad hoc randomize buttons.
- **Volca Drum**: not sample-based at all — a **6-part DSP physical-modeling engine**, each of the 6 parts having 2 layers and no fixed role (any part can be a kick, a cymbal, a texture — parts are generic, not pre-assigned to "kick slot"/"snare slot"). Sound is built from a trigger waveform shaped by **wavefolder, overdrive/bit-reduction, and a waveguide resonator** — a small, cheap-to-compute signal chain (fold → distort → resonate) that produces a huge timbral range from very few parameters, well suited to a microcontroller-class synth engine like AMY.

### Sync
- Old-school **audio-based sync**: a 3.5mm mono cable from SYNC OUT to SYNC IN sends a clock "blip" pulse that advances the receiving unit's sequencer — no MIDI needed, no limit on daisy-chain length as long as SYNC OUT feeds the next SYNC IN down the line. Extremely simple and robust, matches the PO sync approach conceptually (pulse over a cheap cable) though Volca keeps sync and audio on physically separate jacks rather than sharing one stereo cable.
- MIDI IN present for external control; MIDI OUT largely absent on stock units (mod-only), reflecting a deliberate "receive, don't need to transmit" design bias for this class of device.

---

## 2. Electribe series

### Concept
A more "DJ/live-performance" oriented groovebox: 16 trigger pads plus a distinctive **X/Y touch pad** (shared lineage with Korg's Kaossilator/Kaoss Pad line) for melodic and FX performance.

### Sequencing
- Up to **64 steps per part** (16 steps × 4 pages) and up to **16 parts** combined per pattern — a "layer many single-part loops" approach to build a full arrangement, edited and auditioned in parallel (you can tweak a sound's parameters while the sequence keeps playing, immediately hearing the change).
- **Pattern Chain** (added in a later system version) links multiple patterns for successive playback, building songs from patterns without a separate "song mode" editor.

### The X/Y touch pad — "Touch Scale" mode
- Moving a finger across the touch pad plays melodic phrases constrained to a selected scale/key — literally "no wrong notes," similar in spirit to Gamma's always-in-key idea but implemented as a 2D touch surface rather than a keybed. Good evidence this "impossible to sound bad" concept independently reappears across very different companies/products — it's a robust, well-validated pattern.
- The same touch pad doubles as a **live master-FX controller** (à la Kaoss Pad): X and Y axes simultaneously sweep two effect parameters (e.g., filter cutoff + resonance, or delay time + feedback) live during performance — a single 2D gesture surface used for both "generate notes" and "mangle the mix," context-switched by mode.
- **"Seq Reverse" and "Odd Stepper"** are named as aggressive pattern-transformation effects — one-tap transformations applied to the whole running sequence (not just audio FX), i.e. algorithmic mangling of the *pattern data itself* live, a distinct category from audio effects worth calling out separately.

### Motion sequencing
- Same concept as Volca: knob movements recorded and played back as automation within the pattern.

---

## 3. Korg NTS-1 (Nu:Tekt digital kit / mkII)

### Concept
A build-it-yourself, fully open, "logue SDK"-programmable digital synth kit — its distinctive angle isn't the sequencer per se but customizability: user-loadable custom oscillator/effect algorithms via Korg's logue SDK.

### Sequencer
- Compact **8-step sequencer**: long-press SEQ to enter step-record mode, then play notes on the touch-keyboard strip in order — each key-release commits that step and auto-advances to the next, so recording a full 8-step phrase is just "play 8 notes in a row," no separate per-step cursor navigation needed.
- Both **live record** (real-time capture with per-note gate-length/velocity preserved) and **step record** modes are available and can be freely mixed.

### Arpeggiator, tightly coupled to the sequencer
- A single ARP button toggles an arpeggiator that plays automatically while any key is held, with multiple pattern types, several scales, and a dedicated **Random** note-order mode.
- Changing arpeggiator tempo changes sequencer tempo too — the two systems share one clock/tempo concept rather than being independently configurable, reducing the number of "which tempo am I even changing" moments.

### Customization (the NTS-1's real headline feature)
- Because oscillator/effect/modulation "engines" are user-programmable via the logue SDK, the NTS-1 essentially becomes a **platform for community-built engines** rather than a fixed instrument — not directly portable to GrvEP (which isn't aiming to be a dev platform for third parties) but a reminder that exposing an internal "engine slot" architecture (which GrvEP's mode system already somewhat resembles) is a proven way to keep a small, cheap synth feeling inexhaustible over time.

---

## Ideas for GrvEP

### Motion/automation recording (directly extends existing generative ambitions)
1. **"Motion record" mode**: add a mode/button-hold that arms pot-movement recording — while a pattern loops, turning any of the 5 contextual pots writes a continuous automation curve into the pattern (per pot, per pattern), played back on subsequent loops. This is complementary to (not a replacement for) any existing/planned per-step parameter locking — Korg's Volca Drum proves both should coexist: motion record for expressive live sweeps, per-step locks for precise programmed values.
2. **Sequencer traversal-mode menu** (from Volca Modular): expose step order itself as a selectable parameter with named modes — Forward, Backward, Ping-Pong/Bounce, Random, Stochastic (biased random walk) — as one clean settings dropdown per sequencer-capable mode, rather than a scattering of separate "randomize" buttons. This directly deepens GrvEP's existing generative direction with minimal UI cost (one pot/menu item cycles through 4-5 named algorithms).
3. **Explicit note/step/micro-tuning randomization controls** (Volca Modular): rather than one global "randomize" button that overwrites a pattern, offer three independently-dialable randomization *amounts* — how often notes change, how often steps toggle on/off, how much pitch drifts from quantized — via 2-3 pots, letting users bias "how generative" a pattern is on a spectrum instead of it being all-or-nothing.
4. **Pattern-level algorithmic FX ("Seq Reverse"/"Odd Stepper" idea)**: add one-shot buttons that transform the *sequence data itself* live (reverse pattern order, shuffle steps, stretch/compress step count, swap odd/even steps) as distinct from audio effects — cheap to implement (pure data manipulation on the existing step array) and gives an immediate, dramatic "new variation" button well suited to the joystick+button combo (e.g., hold Btn + flick joystick direction = apply a named pattern transform).

### Sound-shaping with few controls
5. **Macro-knob-to-multi-parameter mapping** (Volca FM's Modulator/Carrier knobs): for any complex engine (FM synth mode, granular mode) where individual parameters are numerous, define 1-2 "macro" pot mappings that each sweep a *curated combination* of underlying parameters, so a single pot produces a big, musically coherent timbral change — much more satisfying on 5 contextual pots than exposing 15 raw parameters one pot-page at a time.
6. **One-knob "isolator" style transition control** (Volca Sample): for a build/breakdown live-performance move, a single pot that sweeps from full-range through a low-cut into a high-cut (or vice versa) is cheap DSP and gives an outsized "DJ-style" performance payoff — good candidate for a dedicated FX pot in mix/song-mode contexts.
7. **Touch-scale-style always-in-key note entry** (Electribe X/Y pad, echoing Gamma): since GrvEP already has a joystick, consider a mode where joystick position (not just X but X+Y) maps to a scale-quantized note/chord grid — the joystick becomes a 2D "can't hit a wrong note" performance surface, reusing hardware already present rather than needing new controls.
8. **Generic, role-agnostic voice architecture** (Volca Drum's 6 identical parts/2 layers, no fixed kick/snare assignment): where GrvEP's drum-machine mode currently might hard-assign voices to roles, consider making voice slots generic/reassignable with the same synthesis engine, letting users build unconventional kits — increases variety without new code paths, just relaxing a UI constraint.

### Sync
9. **Confirm/adopt a simple audio-pulse sync option** as a cheap alternative or fallback to MIDI clock: a single mono TS jack carrying a clock "blip," Volca-style, is trivially cheap in hardware and firmware and lets GrvEP sync with the enormous installed base of Volca/PO gear that DIY/hobbyist users are likely to already own — worth prioritizing over building full MIDI clock support if only one sync method is feasible near-term.

### "Mini-game"/exploratory angle
10. **Named "traversal algorithms" as a discoverable menu** (point 2 above) doubles as an exploratory/playful feature: cycling through Bounce/Stochastic/Random step orders while a pattern plays is itself a fun, low-stakes way to explore variations — similar spirit to the existing Life/boids modes, but applicable *inside* the regular step sequencer rather than as a separate mode, meaning generative discovery becomes available everywhere, not siloed to dedicated "experimental" modes.
11. **A "randomization amount" pot as a live-performance instrument** (point 3): turning a single "chaos" pot up and down during a live set, with visual LED feedback showing how many steps are currently being randomized, turns generative randomization into a tactile, watchable performance gesture rather than a one-shot programming action — fits the project's existing appetite for playful, semi-autonomous behavior.
