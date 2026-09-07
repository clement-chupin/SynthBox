# Research: this.is.NOISE inc. — NM2, LC-1X+, Gamma

## Overview

**this.is.NOISE inc.** (stylized "this.is.NOISE", commonly abbreviated "TiN") is a small
Toronto-based boutique startup, much less well-known than Teenage Engineering or Korg, whose
products lean heavily into **gesture/motion-to-sound mapping** and **gamified, personality-driven
UI** rather than traditional knob/grid sound design. Their flagship 2025 product, **Gamma**, is
the most relevant to GrvEP — a compact hardware synth built explicitly for people who don't
already know music theory, which is precisely the audience GrvEP's experimental modes target.

---

## 1. NM2 (Noise Machine 2) — brief

A small motion-sensing MIDI controller: an internal tilt/accelerometer sensor translates
physical movement (tilting, shaking, rotating the unit in the hand) into MIDI CC messages sent
to a DAW or synth. Essentially a "wearable-adjacent" gestural controller rather than a
sound-producing instrument on its own — its whole value proposition is turning body movement
into an expressive, continuously-variable control signal without needing an IMU-equipped
instrument built from scratch. Notable mainly as a proof that a single 3-axis tilt sensor,
mapped thoughtfully, is enough to feel expressive as a performance control — no grid, no
buttons required for the core interaction.

## 2. LC-1X+ (Light Controller) — brief

A MIDI-to-lighting bridge: listens to incoming MIDI (notes, CC) and drives external lighting
rigs/LED fixtures in sync, aimed at solo performers who want a lighting show reactive to what
they're actually playing without a separate lighting operator. Relevant mainly as a reminder
that "the instrument also controls a light show" is a viable, low-effort feature for a device
that already has a full LED grid (GrvEP) — the LEDs could double as both instrument feedback
*and* an intentionally choreographed performance visual, not just status display.

---

## 3. Gamma — the main product of interest

Gamma is TiN's 2025 hardware synthesizer, explicitly marketed as approachable for people with
no music-theory background while still being deep enough for experienced musicians — almost
exactly GrvEP's own stated design tension between EXP-style modes and SYNTH-style modes.

### Hardware / keybed
- A single physical keybed of **14 keys**, but split conceptually into two halves that are
  **always both active at once**: a "chord" side and a "melody" side. Playing a key on the
  chord side triggers a full pre-voiced chord (not a single note); playing a key on the melody
  side triggers a single scale-quantized note. The two halves layer together live — you can
  hold a chord while noodling a melody on top, entirely by feel, with zero risk of playing a
  "wrong" note because everything is pre-constrained to the current scale/chord set.
- **13 built-in scales**, always selectable, and the keybed is **always mapped into whichever
  scale is currently selected** — there's no "off" state where raw chromatic notes leak
  through; the instrument is permanently in a scale-locked mode of some kind, which is the
  core mechanism that makes it impossible to sound "wrong."

### Dual thumbsticks — not simple XY CC controllers
- Gamma has **two small joystick/thumbsticks** (very close in spirit to GrvEP's single
  joystick), but they are deliberately *not* mapped to continuous linear parameters (filter
  cutoff, pitch-bend, etc.) the way most synths use a joystick. Instead, each direction/notch
  of a thumbstick selects a **discrete, curated variation** — e.g., one thumbstick browses
  between different *voicings* of the currently-held chord (inversions, added extensions,
  spread voicings) rather than sweeping a filter; the other browses between different **FX
  blend presets** (a curated combination of multiple effects moving together) rather than one
  parameter at a time.
- The underlying idea: **a joystick doesn't have to mean "two independent continuous CCs."**
  It can instead mean "move between hand-picked good-sounding states," which removes the
  possibility of landing on an ugly/broken intermediate value — a curated discrete browser
  disguised as continuous physical motion.

### Effects
- **20+ built-in effects**, but the emphasis (per the thumbstick design above) is on
  **pre-blended combinations** of several effects moving together as one selectable "look,"
  rather than exposing every effect as an independent parameter a beginner has to understand
  in isolation.

### "GammaGotchi" — gamified OLED companion
- The onboard OLED screen hosts a small **virtual-pet-style mascot/character** (explicitly
  compared by the company to a Tamagotchi) that reacts visually to what's being played —
  turning the screen into a source of playful, ambient feedback and encouragement rather than
  a text/numeric parameter readout. This is a pure engagement/delight layer sitting on top of
  the actual sound engine, costing nothing in terms of core synthesis functionality.

### Looper — captures gesture state, not just notes
- Gamma's built-in looper doesn't just record note-on/note-off events; it also captures the
  **effects state and chord-voicing selection** in effect at the time, so played-back loops
  reproduce the exact "vibe" (FX blend, voicing) the performer had dialed in when the loop was
  recorded — not just the notes. This directly addresses a common looper frustration
  (recording a phrase, then fiddling with an FX knob afterward, and losing the exact texture
  the phrase was played with).

---

## Ideas for GrvEP

Concrete, hardware-appropriate translations, given GrvEP's 4×8 key grid, 1 joystick, 7 pots
(2 fixed), 4 buttons, 128×128 OLED — with special attention to the EXP/EXP2/EXP3/LIFE/SWARM
"no music theory required" family of modes, since Gamma's entire design brief matches that
philosophy almost exactly:

1. **Chord/melody split grid zones**: for a beginner-facing mode, reserve one region of the
   4×8 grid (e.g. bottom 2 rows) as "chord pads" — each key triggers a full pre-voiced chord in
   the current scale — while the remaining rows stay single-note melody keys, both playable
   simultaneously. This could be a new EXP-family mode, or a toggle inside the existing SYNTH
   mode's scale/arp overlay, giving non-musicians instant access to harmonically-correct
   chords without knowing what a chord even is.

2. **Always-in-key scale-lock as a first-class, selectable option**: GrvEP's scale system
   already exists (see `kExp2Scale*[]` tables per the plan file), but Gamma's insight is
   making scale-lock the *default, ever-present* state for a beginner mode rather than an
   overlay users must remember to open — worth auditing whether EXP/EXP2/EXP3 always
   constrain output to a scale today, or whether raw/chromatic notes can leak through in some
   path.

3. **Joystick-as-curated-variation-browser**: for at least one mode, try remapping the
   joystick from "continuous XY parameter sweep" to "step through N hand-picked good states"
   (e.g., X-axis steps through 4-5 pre-designed FX-combo presets rather than one continuous FX
   parameter; Y-axis steps through chord-voicing variants of whatever's currently held) —
   directly reusable in MODE_MODULAR's mod-matrix work or as a new EXP variant, and guarantees
   every joystick position sounds intentional rather than risking an ugly in-between value.

4. **A lightweight companion/mascot mode** (GammaGotchi idea): purely a delight-layer overlay
   for the OLED — a small animated character reacting to played notes/velocity (bounces on
   note-on, "happier" the more active the pattern) — costs nothing functionally, could be
   layered onto LIFE or SWARM in particular since both are already visually generative and
   playful in spirit, matching the "arcade/playful skin" idea also raised in the Teenage
   Engineering research doc.

5. **Loop/pattern recording that captures FX + performance state, not just notes**: if/when
   GrvEP gains a looper or pattern-record feature beyond the existing step sequencers, capture
   which FX were active (and their automation/mod-slot state, cf. the new FX-automation
   system) alongside notes, so played-back patterns reproduce the exact texture they were
   recorded with — directly actionable given the FX-automation `ModSlot` engine already built
   this session.

6. **Treat the LED grid as a generative/expressive output, not only a status display** (LC-1X+
   idea): LIFE and SWARM already animate the LED grid as part of their core mechanic; this
   validates extending that treatment to other modes too — e.g., a subtle "breathing"/color
   pulse tied to the current LFO phase in MODE_MODULAR, turning FX-automation feedback into
   something visually pleasing rather than purely diagnostic.

7. **Consider IMU/tilt as a future expressive control axis** (NM2 idea): purely speculative,
   contingent on hardware GrvEP doesn't currently have — if a future revision adds even a
   cheap accelerometer, a single tilt axis mapped to something like filter cutoff or vibrato
   depth could add expressive nuance without consuming any of the already-scarce buttons/pots;
   flagged here as a "worth remembering if the BOM ever changes" idea rather than a near-term
   actionable one.
