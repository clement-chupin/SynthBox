# Research: Teenage Engineering — Pocket Operators, OP-1/OP-1 Field, OP-Z, EP-133 K.O. II

## Overview

Teenage Engineering (TE) is a Swedish company whose entire product range is built on one repeated bet: take a device with almost no controls (a handful of buttons, no touchscreen, tiny or no display) and make it feel *deep* through layered button-combos, color/LED feedback, and consistent conventions that transfer from product to product. This is directly relevant to GrvEP's hardware constraints (grid + joystick + pots + 4 buttons, tiny OLED) — TE has spent 15 years optimizing exactly this problem.

---

## 1. Pocket Operator series (PO-12/16/20/32/33/35 etc.)

### Hardware
- A 16-key grid (4x4) arranged in two rows of 8, doubling as: step sequencer, note keyboard, and menu selector, depending on which of 4 side buttons (BPM, PATTERN/FX, WRITE, PLAY/STOP) is held.
- A tiny segmented LCD (mostly showing a scrolling character animation + BPM/step number), not a graphical screen.
- Red LEDs "chase" across the 4x4 grid during playback, one lit key per active step — the only "sequencer position" feedback, and it doubles as a dance-y visual toy.

### Sequencer / step-programming paradigm
- **Two entry modes on the same grid**: (1) tapping keys 1-16 *live* plays sounds/notes; (2) holding **WRITE** and tapping keys **punches in** a step at the current playhead position, quantized to the current swing setting. No separate "note grid vs. step grid" — the same 16 buttons serve both, switched by a held modifier.
- **Per-step retrigger/roll**: hold a step key, then tap **BPM** repeatedly to insert 2x/4x/8x/16x retriggers on that specific step — a single modifier button cycles through multiplier depths rather than needing dedicated "roll" buttons.
- **Parameter locks**: hold a step, turn/tap a parameter — the value is baked into that step only (Elektron-style p-locking, but with just one modifier button instead of a whole locks menu).
- **16 patterns in memory**, chainable into songs (up to 128 steps of pattern-chain on some models) by holding pattern-select and tapping in sequence.

### Sound-shaping / FX with minimal knobs
- **16 "punch-in" effects**, one per grid key, accessed by holding the **FX** button and tapping a key — instantly applies (while held) a whole pre-designed transformation: solo-chords, solo-drums, stutter/glitch, filter sweep, fill-in, retrigger, "blinds" (gating), rising arpeggio, etc. This is the single most important idea: **effects are not knob-tweakable parameters, they're one-tap "moves"** — pre-baked musical transformations bound to a spatial grid position, momentarily engaged like a performance macro, not a mixer setting.
- Punch-in FX can *themselves* be parameter-locked into the sequence, so a live "move" can become a permanent part of a pattern.
- Very few (or zero) traditional knobs; sound design happens through button-hold + tap combos on the same 16-24 buttons, using consistent per-model muscle memory once learned.

### Sync between units
- Plain 3.5mm stereo cable, line-out (right channel) of one unit into line-in (left channel) of the next; a small "SY" sync-mode number (SY0–SY4) sets master/slave roles. Audio and clock share the same cable, split L/R — no separate MIDI/clock cable needed. Dead simple, works between different PO models and even into modular gear (it's just a clock blip).

### UI philosophy
- No icon language to speak of — feedback is almost entirely LED position + tiny animated LCD character, reinforcing "constraint as identity": exposed PCB, raw components, extremely small number of physical inputs multiplexed hard via hold+tap grammar.
- The generative/arcade PO-20 model is literally a synth wrapped as five different chiptune arcade-game sound palettes/patterns — a "playful" framing of otherwise plain synthesis. The **PO-32 Tonic** ports a real drum-*synthesis* engine (Sonic Charge Microtonic) rather than samples: each of its 16 sounds is actually a **morph between two stored synth patches**, with the morph itself controllable/automatable — a compact way to get huge sonic range from 16 slots.
- The **PO-33 KO!** is a sampler: 8 melodic sample voices + 8 slice players from a single 40s recording buffer, with a built-in mic — turning "recording a source sample" and "playing/slicing it as an instrument" into the same small grid.

---

## 2. OP-1 / OP-1 Field

### Concept
A "portable studio" that packs a synth, sampler, 4-track tape recorder, mixer, FX, radio-style noise generator, and sequencer into one small box with a handful of colored-icon-labeled buttons and rotary/pitch dials plus a small graphical display (color on the OP-1 Field, monochrome dot-matrix on the original).

### Synth engines as swappable "cartridges"
- Instead of one flexible-but-complex synth architecture, the OP-1 exposes **8 discrete synth engines** (Cluster, Digital, DrWave, FM, Phase, Pulse, String, plus a Sampler engine), each visualized with its own bespoke animated icon/graphic on the tiny screen (e.g., "Cluster" shows drifting particle blobs). Switching engine = switching entire sound-generation paradigm, each with a *small*, tailored parameter set (usually 4 knob-mappable params) rather than one huge universal engine.
- **DNA synthesis**: each physical OP-1 unit derives unique synthesis seed values from its own hardware serial ID, so identical presets sound subtly different per unit — a "each instrument is a little bit its own creature" idea.

### 4-track tape as the sequencing/arrangement backbone
- Rather than a conventional DAW-style multitrack, the OP-1's "workflow spine" is a 4-track cassette-tape metaphor: record synth/mic/sample takes to tape, bounce/ping-pong between tracks, then use **tape speed/pitch manipulation live** (physically like scrubbing a cassette) as a performance/sound-design tool. Loops, punch-ins, and "tape stop" style effects fall naturally out of this metaphor even on a device with no touchscreen and few buttons — because the metaphor itself teaches the user what the buttons should do.
- OP-1 Field added a proper (if still tiny) step sequencer app layered on top for drum/synth programming, plus a 4-track "tape" now acting more like a resampling loop layer.

### UI philosophy
- Every function is drawn as a small hand-illustrated icon/animation unique to that mode — this is the "component" idea in TE's own words: the box is a shell around dozens of tiny self-contained "instruments" (tape, synth engines, mixer, 4 FX types, sequencer, drum sampler, radio/noise) that all share the same 4-knob + button-strip control surface, so **learning the control surface once means every new component is immediately semi-familiar** even though its content is totally different.
- The tiny screen is used less for menus/text and more as a *character/animation canvas* — reinforcing mood and giving non-verbal feedback (a spinning tape reel, a bouncing waveform) instead of numeric readouts wherever possible.

---

## 3. OP-Z

### Concept
A more "serious" 16-track sequencer/synth aimed at pattern-based production and live performance, visually similar to a PO scaled up, but MIDI/Bluetooth/CV-capable and app-connected.

### Sequencer paradigm
- **16 tracks** (8 synth/sample engines + kick/snare/perc/sample/tape/FX/arp/mod tracks), each got its own step-component grid of **16 keys**, reused as: step entry, note keyboard (with a "PLAY" mode), and per-track FX.
- Distinctive: a **dedicated "tape" track** that's an always-on audio buffer capturing whatever the OP-Z is currently outputting — you can then *sequence the tape itself* (trigger in-point, playback length, speed) as if it were just another instrument track, turning "what just happened" into playable material without a separate recording step. This is a "record everything, curate later" idea worth studying — it removes the friction of deciding in advance what to capture.
- Component-based step programming: TE describes the OP-Z as built from 14 (or so) reusable "step components" (trig, note, punch-in FX, mod, etc.) that any track can be assigned, so the *behavior* of a step column is itself swappable per track rather than fixed.

### Sync / connectivity
- Bluetooth MIDI + companion app for adding visuals ("Photomatic" stop-motion video sync to patterns), Ableton Link, USB MIDI, and an expansion "cartridge" port (the oplab module) that adds CV/gate/extra MIDI I/O — the physical unit stays minimal, and capability is added via a small external adapter rather than more built-in ports.

### UI philosophy
- Same hold+tap grammar as the POs (SHIFT-style layering), but scaled to 16 tracks via a horizontal track-select strip, so the *same* 16-key grid means something different depending which of the 16 track buttons is currently selected — a "context switch by selecting a channel" pattern very applicable to a fixed grid + few buttons.

---

## 4. EP-133 K.O. II

### Concept
TE's most recent (2023) "sampler/sequencer/composer," explicitly aimed at fast beat-making from chopped samples, drums, bass and lead — spiritually a mix of PO-33's sampling and OP-Z's sequencing, in an SP-1200-style 16-pad body.

### Sequencer / pattern structure
- **4 groups × 12 patterns** (drum group, bass group, lead group, sample-chop group), each pattern independently switchable live so you can swap "which 12-pattern variation" is currently playing per group and improvise arrangements by recombination — an easy mental model: 4 independent musical roles, each with a bank of alternate takes, mixed and matched on the fly rather than one single linear "song mode."

### Sample chopping — two paradigms exposed side by side
- **AUTO-CHOP**: automatic beat-detection slices a loaded sample and auto-populates the 16 pads bottom-left→top-right; can switch between "equal length" slicing and "attack/transient" detection slicing.
- **LIVE CHOP**: user taps pads *while the sample plays* to manually mark chop points in real time — chop boundaries are simply the gaps between consecutive pad presses. Two very different "let the machine do it" vs "perform the chopping" workflows exposed as a single mode toggle — good template for offering both an automatic and an expressive/manual variant of any generative feature.

### Punch-in FX + traditional FX
- A dedicated **FX** button applies "always-on" processing (delay, reverb, distortion, chorus, filter, compressor) globally or per track.
- **12 pressure-style Punch-In FX** pads for momentary live performance effects (same "hold to apply a move" idea as the PO series), plus shortcut combos like `SHIFT+SAMPLE = CHOP`, `SHIFT+TEMPO = create loop (inspired by their OB-4 "magic radio" instant-loop feature)`, and `TIMING+SHIFT+pad = held-note repeat`.

---

## Ideas for GrvEP

Concrete, hardware-appropriate translations, given GrvEP's 4×8 key grid, 1 joystick, 7 pots (2 fixed), 4 buttons, 128×128 OLED:

1. **"Hold-a-key-to-lock-a-parameter"** across all sequencer modes: while a step key is held (in step-entry-capable modes), route the currently-touched pot to write a per-step parameter lock (pitch/velocity/decay/FX amount) instead of a global value — this is the single highest-value idea to borrow; it's the core of PO/Volca/OP-Z step depth and needs no extra hardware, just a firmware convention (hold+pot = p-lock write, LED on that key blinks a distinct color while locked).

2. **Punch-in FX bank on a spare button**: dedicate a chord of "hold Btn3 (or whichever isn't otherwise used) + tap a grid key" to fire one of up to 32 (4×8) pre-baked "moves" (stutter, filter sweep, half-time, reverse, bit-crush, arp-up) — momentary while held, and if a key is held into the next step-write, it gets baked into the pattern as a locked FX event. Since GrvEP already has 32 keys, this maps 1:1 onto the grid the same way PO's 16 FX map onto its 16 keys.

3. **Retrigger/roll via BPM-pot combo**: replicate "hold step + tap tempo/pot to cycle retrig multiplier (2/4/8/16x)" as a fast way to add rolls without a separate menu — natural fit since GrvEP already has a BPM pot.

4. **LED-chase as the *only* required playhead feedback**: for modes where the OLED is busy with something else (e.g. a visual generative mode), fall back to red/colored LED chase across the active grid row as playhead indicator, freeing the small screen for other information — already partly true given the existing LED grid.

5. **Sound-morph-between-two-patches per drum voice** (PO-32 Tonic idea): instead of one fixed drum synth patch per sound slot, store two patches per drum voice and let a pot morph continuously between them — cheap way to add expressive range to the existing drum machine mode without more voices or menus.

6. **Dual auto/manual sample-chop modes** (EP-133 idea): if/when granular or sample-slicing features are extended, offer both an automatic transient-detect auto-chop and a live "tap along while it plays" manual chop, toggled with one button — reuses existing grid + joystick, no new hardware.

7. **"Tape"-style always-on capture buffer** (OP-Z tape track / OP-1 tape metaphor): a background circular buffer that's always recording the final mix output; a dedicated mode lets you scrub/trigger/pitch/reverse recent audio from the grid — turns "I didn't mean to record that but it sounded great" into a usable feature, and gives a generative/experimental mode a natural raw-material source (e.g., feed captured audio into the granular engine).

8. **Sync via simple analog clock pulse over a spare jack**, mirroring both PO and Volca sync: if GrvEP ever exposes a 3.5mm jack, a single mono clock-pulse in/out (not full MIDI) is the cheapest possible way to let two GrvEP units (or a GrvEP + Volca/PO) share tempo — much simpler to implement than MIDI clock and matches the DIY/portable ethos.

9. **Icon-per-engine visual identity** (OP-1 idea): give each of GrvEP's ~30 modes (or at least each synth engine within a mode) a small unique animated icon/glyph shown in a fixed screen corner — cheap on a 128×128 OLED, and helps users distinguish modes at a glance without reading menu text, especially useful given how many modes already exist.

10. **"Component" reuse of the grid across contexts**: formalize (if not already) that the 4×8 grid always means "steps OR notes OR menu items OR FX-pads" depending on a currently-held modifier button, and keep that grammar *identical* across every mode — TE's biggest lesson is that consistency of hold+tap conventions across wildly different sound engines is what makes a tiny control surface feel deep rather than confusing.

11. **Playful/arcade framing for a mode** (PO-20 Arcade idea): dress up one of the existing generative modes (Life/boids) with retro chiptune/arcade sound design and simple "score"/visual feedback on the OLED — makes an already-generative mode more inviting without new mechanics, purely a skin/sound-palette exercise.
