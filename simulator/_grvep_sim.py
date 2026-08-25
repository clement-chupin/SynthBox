#!/usr/bin/env python3
"""
GrvEP Simulator — pygame + Amy Python
Simulates the GrvEP ESP32-S3 synthesizer hardware on PC.
"""

import sys
import os
import time
import math
import pygame
import pygame.gfxdraw

# ─── Amy setup ───────────────────────────────────────────────────────────────
AMY_PATH = os.path.expanduser("~/.local/lib/python3.12/site-packages")
if AMY_PATH not in sys.path:
    sys.path.insert(0, AMY_PATH)

import amy as _amy

# ─── Constants mirrored from config.h ────────────────────────────────────────

SHAPE_NAMES = [
    "SAW","SAWFM","SQR","SIN","SSAW","ACID","BASS","PLCK",
    "WHT","PINK","BRWN",
    "J:BRS","J:STR","J:PNO","J:ORG","J:CHR",
    "D:EP","D:BEL","D:BAS","D:BRS","D:STR","D:ORG","D:VOC",
    "T:LED","T:BAS","HOVR","STAB",
    "WOBB","EPLK","INDS",
    "J:OR2","J:FRG","FMDFT","FMBEL","SDFT",
]

# Wave type per shape (-1=raw wave, -2=ALGO, >=0=Juno patch index)
SHAPE_PATCH = [
    -1,-2,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,
    0,21,7,8,6,
    138,153,142,128,131,144,157,
    -1,-1,-1,-1,
    -1,-1,-1,
    9,42,
    -2,-2,-2,
]

# AMY wave constants per shape (for custom waves)
SHAPE_WAVE = {
    0: _amy.SAW_DOWN,   # SAW
    1: _amy.SAW_DOWN,   # SAWFM
    2: _amy.PULSE,      # SQR
    3: _amy.SINE,       # SIN
    4: _amy.SAW_DOWN,   # SSAW (supersaw — use SAW_DOWN in sim)
    5: _amy.SAW_DOWN,   # ACID
    6: _amy.SAW_DOWN,   # BASS
    7: _amy.SAW_DOWN,   # PLUCK (KS would be ideal but use SAW)
    8: _amy.NOISE,      # WHITE
    9: _amy.NOISE,      # PINK
   10: _amy.NOISE,      # BROWN
}

ENV_NAMES = ["Nrm","Fst","Plk","Pad","Pia"]
# (attack_ms, decay_ms, sustain_0-1, release_ms)
ENV_TABLE = [
    (50,  200, 0.5, 500),
    (1,   50,  0.0, 50),
    (5,   100, 0.0, 50),
    (500, 100, 0.8, 2000),
    (10,  500, 0.3, 800),
]

SCALE_NAMES   = ["Chr","Maj","Min","Pen","Blu","HMn"]
SCALE_SIZES   = [12, 7, 7, 5, 6, 7]
SCALE_INTERVALS = [
    [0,1,2,3,4,5,6,7,8,9,10,11],
    [0,2,4,5,7,9,11],
    [0,2,3,5,7,8,10],
    [0,3,5,7,10],
    [0,3,5,6,7,10],
    [0,2,3,5,7,8,11],
]

MODE_NAMES = [
    "SYNTH","OMNI","SAMPL","LIGHT","SEQ","LPLY",
    "BATT","DIAG","HYBRD","MODUL","MOD2","303","GRAN",
    "GR2","MIDI","TRKR","DRUMS","SSEQ","303S","SS2","ANIM","I303",
]

KBD_ROWS, KBD_COLS = 4, 8
KBD_MENU_ROW, KBD_MENU_COLS = 4, 4

# Menu categories (INSTR/SEQNC/AUTRE)
MENU_CAT_NAMES = ["INSTR","SEQNC","AUTRE"]
MENU_CAT_ITEMS = [
    ["SYNTH","OMNI","SAMPL","HYBRD","MODUL","MOD2","303","I303","GRAN","GR2","MIDI"],
    ["SEQ","303S","TRKR","DRUMS","SSEQ","SS2"],
    ["LIGHT","LPLY","BATT","DIAG","ANIM"],
]
MENU_CAT_MODES = [
    [0, 1, 2, 8, 9, 10, 11, 21, 12, 13, 14],
    [4, 18, 15, 16, 17, 19],
    [3, 5, 6, 7, 20],
]
MENU_COLS = 3

NUM_LEDS = 36

# ─── Note map ────────────────────────────────────────────────────────────────

def build_note_map(scale_idx, octave):
    """Returns a 4×8 array of MIDI notes matching GrvEP's NoteMap::buildMap()."""
    intervals = SCALE_INTERVALS[scale_idx]
    sz = SCALE_SIZES[scale_idx]
    base = 48 + octave * 12  # C3 + octave offset
    nm = [[0]*8 for _ in range(4)]
    for r in range(4):
        for c in range(8):
            idx = (7 - c) * 4 + r
            oct_off = idx // sz
            note_in_scale = idx % sz
            note = base + oct_off * 12 + intervals[note_in_scale]
            nm[r][c] = max(0, min(127, note))
    return nm

# ─── LED helper ──────────────────────────────────────────────────────────────

# Physical LED layout (OOPSIE_LED_FLAG) from Leds.cpp
# Row 4 (menu): 32-35
# Row 3: 31-24 (reversed)
# Row 2: 16-23
# Row 1: 15-8 (reversed)
# Row 0: 0-7
def key_to_led(row, col):
    """Map (row, col) to LED index, matching the GrvEP OOPSIE layout."""
    layout = [
        # row 4 (menu buttons, cols 4-7)
        [None, None, None, None, 32, 33, 34, 35],
        # row 3 (reversed)
        [31, 30, 29, 28, 27, 26, 25, 24],
        # row 2
        [16, 17, 18, 19, 20, 21, 22, 23],
        # row 1 (reversed)
        [15, 14, 13, 12, 11, 10,  9,  8],
        # row 0
        [ 0,  1,  2,  3,  4,  5,  6,  7],
    ]
    rows = [4, 3, 2, 1, 0]  # key_row 0 = bottom led row 0
    if row < 0 or row > 4 or col < 0 or col > 7:
        return None
    return layout[rows[row]][col] if rows[row] is not None else None

# ─── Audio Engine ─────────────────────────────────────────────────────────────

class AudioEngine:
    OSC_COUNT = 8
    CUTOFF_MAX = 18000.0
    CUTOFF_MIN = 20.0

    def __init__(self):
        self.osc_note = [None] * self.OSC_COUNT
        self.osc_time = [0.0] * self.OSC_COUNT
        self.shape_idx = 0
        self.filter_cutoff = 18000.0
        self.filter_reso = 1.5
        self.filter_type = _amy.FILTER_NONE
        self.reverb_level = 0.0
        self.env_idx = 0
        self.volume = 0.7
        self._ready = False
        self._juno_patch = None

    def init(self):
        try:
            _amy.live()
            time.sleep(0.2)
            self._ready = True
            self._setup_oscs()
        except Exception as e:
            print(f"[Amy] init error: {e}")

    def _setup_oscs(self):
        if not self._ready:
            return
        wave = self._current_wave()
        atk, dec, sus, rel = ENV_TABLE[self.env_idx]
        bp = f"0,0,{atk},{sus},{dec+atk},{sus},{dec+atk+1000},{0},0,0"
        for i in range(self.OSC_COUNT):
            try:
                _amy.send(osc=i, wave=wave, amp_coefs="0,0,0.7,0,0,0,0",
                          bp0=f"0,0,{atk},1,{dec+atk},{sus},{dec+atk+rel},{0},0,0",
                          filter_type=_amy.FILTER_NONE)
            except Exception:
                pass

    def _current_wave(self):
        patch = SHAPE_PATCH[self.shape_idx]
        if patch == -1:
            return SHAPE_WAVE.get(self.shape_idx, _amy.SAW_DOWN)
        return _amy.SAW_DOWN  # Juno/DX7 → use SAW as fallback in sim

    def set_shape(self, shape_idx):
        self.shape_idx = shape_idx
        self._setup_oscs()

    def set_envelope(self, env_idx):
        self.env_idx = env_idx
        self._setup_oscs()

    def _find_free_osc(self, midi_note):
        # Prefer osc already playing this note
        for i, n in enumerate(self.osc_note):
            if n == midi_note:
                return i
        # Find unused osc
        for i, n in enumerate(self.osc_note):
            if n is None:
                return i
        # Steal oldest
        return min(range(self.OSC_COUNT), key=lambda i: self.osc_time[i])

    def note_on(self, midi_note, velocity=0.7):
        if not self._ready:
            return
        osc = self._find_free_osc(midi_note)
        freq = 440.0 * (2.0 ** ((midi_note - 69) / 12.0))
        vel = min(1.0, max(0.0, velocity * self.volume))
        try:
            _amy.send(osc=osc, freq=freq, vel=vel)
        except Exception as e:
            print(f"[Amy] note_on error: {e}")
        self.osc_note[osc] = midi_note
        self.osc_time[osc] = time.time()

    def note_off(self, midi_note):
        if not self._ready:
            return
        for i, n in enumerate(self.osc_note):
            if n == midi_note:
                try:
                    _amy.send(osc=i, vel=0)
                except Exception:
                    pass
                self.osc_note[i] = None

    def all_notes_off(self):
        if not self._ready:
            return
        for i in range(self.OSC_COUNT):
            try:
                _amy.send(osc=i, vel=0)
            except Exception:
                pass
            self.osc_note[i] = None

    def set_filter(self, cutoff_hz, resonance):
        if not self._ready:
            return
        self.filter_cutoff = cutoff_hz
        self.filter_reso = resonance
        bypass = (cutoff_hz >= self.CUTOFF_MAX or cutoff_hz <= self.CUTOFF_MIN
                  or self.filter_type == _amy.FILTER_NONE)
        ft = _amy.FILTER_NONE if bypass else self.filter_type
        for i in range(self.OSC_COUNT):
            try:
                _amy.send(osc=i, filter_type=ft,
                          filter_freq=cutoff_hz if not bypass else 0,
                          resonance=resonance)
            except Exception:
                pass

    def set_filter_type(self, ft):
        self.filter_type = ft
        self.set_filter(self.filter_cutoff, self.filter_reso)

    def set_reverb(self, level):
        self.reverb_level = level
        if not self._ready:
            return
        try:
            _amy.reverb(level=level, liveness=0.85, damping=0.5)
        except Exception as e:
            print(f"[Amy] reverb error: {e}")

    def set_volume(self, vol):
        self.volume = vol

    def set_pitch_bend(self, semitones):
        # Adjust all active oscs by pitch bend
        if not self._ready:
            return
        for i, n in enumerate(self.osc_note):
            if n is not None:
                freq = 440.0 * (2.0 ** ((n - 69 + semitones) / 12.0))
                try:
                    _amy.send(osc=i, freq=freq)
                except Exception:
                    pass

# ─── Simulator State ──────────────────────────────────────────────────────────

class SimState:
    def __init__(self):
        self.mode_idx = 0  # MODE_SYNTH
        self.shape_idx = 13  # SHAPE_JUNO_PIANO
        self.env_idx = 0
        self.scale_idx = 0
        self.octave = 0
        self.volume = 0.7
        # Pots: [Vol, Cut, Res, TL, TR, BL, BR]
        self.pot_values = [0.7, 1.0, 0.2, 0.5, 0.5, 0.5, 0.5]
        self.joy_x = 0.0
        self.joy_y = 0.0
        self.active_notes = {}  # midi_note -> True
        self.leds = [(0, 0, 0)] * NUM_LEDS
        self.key_states = [[False]*8 for _ in range(5)]
        self.note_map = build_note_map(0, 0)
        # FX
        self.fx_lpf = False
        self.fx_hpf = False
        self.fx_reverb = False
        self.fx_delay = False
        # Menu
        self.menu_open = False
        self.menu_cat = 0
        self.menu_on_tab = True
        self.menu_row = 0
        self.menu_col = 0
        # Pot dragging state
        self.dragging_pot = None  # index of pot being dragged
        self.drag_start_y = 0
        self.drag_start_val = 0.0
        # Joystick drag
        self.dragging_joy = False
        self.joy_cx = 0
        self.joy_cy = 0

    def update_note_map(self):
        self.note_map = build_note_map(self.scale_idx, self.octave)

    def update_leds(self):
        leds = [(0, 0, 0)] * NUM_LEDS
        for r in range(KBD_ROWS):
            for c in range(KBD_COLS):
                if self.key_states[r][c]:
                    idx = key_to_led(r, c)
                    if idx is not None:
                        note = self.note_map[r][c]
                        hue = (note % 12) / 12.0
                        leds[idx] = hsv_to_rgb(hue, 1.0, 0.8)
        # Menu button LEDs (always dim blue)
        for mi in range(4):
            idx = key_to_led(4, 4 + mi)
            if idx is not None:
                leds[idx] = (0, 20, 60)
        self.leds = leds

def hsv_to_rgb(h, s, v):
    import colorsys
    r, g, b = colorsys.hsv_to_rgb(h, s, v)
    return (int(r*255), int(g*255), int(b*255))

# ─── OLED Renderer ───────────────────────────────────────────────────────────

class OLEDRenderer:
    W, H = 128, 128
    BG = (0, 0, 0)
    FG = (255, 255, 255)
    DIM = (130, 130, 130)

    def __init__(self):
        self.surface = pygame.Surface((self.W, self.H))
        try:
            self.font_sm = pygame.font.SysFont("monospace", 8, bold=False)
            self.font_md = pygame.font.SysFont("monospace", 10, bold=False)
            self.font_lg = pygame.font.SysFont("monospace", 13, bold=True)
        except Exception:
            self.font_sm = pygame.font.Font(None, 9)
            self.font_md = pygame.font.Font(None, 11)
            self.font_lg = pygame.font.Font(None, 14)

    def _text(self, txt, x, y, font=None, color=None):
        if font is None:
            font = self.font_md
        if color is None:
            color = self.FG
        surf = font.render(str(txt), True, color)
        self.surface.blit(surf, (x, y))

    def _box(self, x, y, w, h, fill=True):
        if fill:
            pygame.draw.rect(self.surface, self.FG, (x, y, w, h))
        else:
            pygame.draw.rect(self.surface, self.FG, (x, y, w, h), 1)

    def _line(self, x1, y1, x2, y2):
        pygame.draw.line(self.surface, self.FG, (x1, y1), (x2, y2))

    def _hline(self, x, y, w):
        pygame.draw.line(self.surface, self.FG, (x, y), (x+w-1, y))

    def render_synth(self, state):
        """Render SYNTH mode display."""
        s = self.surface
        s.fill(self.BG)

        # Top bar: mode name + shape
        self._box(0, 0, 128, 11)
        txt = f"SYNTH  {SHAPE_NAMES[state.shape_idx]}"
        surf = self.font_md.render(txt, True, self.BG)
        s.blit(surf, (2, 1))

        # Scale, octave
        sc_name = SCALE_NAMES[state.scale_idx]
        oct_str = f"Oct{state.octave:+d}"
        self._text(f"Scl:{sc_name}  {oct_str}", 2, 14, self.font_sm)

        # Envelope
        env_name = ENV_NAMES[state.env_idx]
        self._text(f"Env:{env_name}", 2, 23, self.font_sm)

        # FX status
        fx_strs = []
        if state.fx_lpf: fx_strs.append("LPF")
        if state.fx_hpf: fx_strs.append("HPF")
        if state.fx_reverb: fx_strs.append("REV")
        if state.fx_delay: fx_strs.append("DLY")
        self._text("FX: " + (" ".join(fx_strs) if fx_strs else "---"), 2, 32, self.font_sm)

        # Pot bar (cutoff, resonance)
        cut = state.pot_values[1]
        res = state.pot_values[2]
        self._hline(0, 41, 128)
        self._text("Cut", 2, 43, self.font_sm)
        cut_w = int(cut * 80)
        self._box(22, 44, cut_w, 6)
        self._box(22, 44, 80, 6, fill=False)
        self._text("Res", 2, 52, self.font_sm)
        res_w = int(res * 80)
        self._box(22, 53, res_w, 6)
        self._box(22, 53, 80, 6, fill=False)

        # Active notes
        self._hline(0, 62, 128)
        self._text("Notes:", 2, 64, self.font_sm)
        active = sorted(state.active_notes.keys())
        notes = ["C","C#","D","D#","E","F","F#","G","G#","A","A#","B"]
        note_strs = [f"{notes[n%12]}{n//12-1}" for n in active[:6]]
        self._text(" ".join(note_strs), 2, 73, self.font_sm)

        # Note grid mini-display (4×8)
        self._hline(0, 83, 128)
        cw, ch = 14, 9
        for r in range(4):
            for c in range(8):
                x = 2 + c * cw
                y = 85 + (3-r) * ch
                note = state.note_map[r][c]
                if state.key_states[r][c]:
                    self._box(x, y, cw-1, ch-1)
                    # Note name in black on white
                    n_name = notes[note % 12]
                    surf = self.font_sm.render(n_name, True, self.BG)
                    s.blit(surf, (x+1, y+1))
                else:
                    self._box(x, y, cw-1, ch-1, fill=False)

        # Volume bar at bottom
        self._hline(0, 122, 128)
        vol_w = int(state.volume * 128)
        self._box(0, 124, vol_w, 4)

    def render_mode_303(self, state):
        s = self.surface
        s.fill(self.BG)
        self._box(0, 0, 128, 11)
        surf = self.font_md.render("303", True, self.BG)
        s.blit(surf, (50, 1))
        self._text("TB-303 Style", 10, 15, self.font_sm)
        self._text(f"Cut: {state.pot_values[1]*100:.0f}%", 5, 30, self.font_md)
        self._text(f"Res: {state.pot_values[2]*100:.0f}%", 5, 42, self.font_md)
        self._text("Wave: SAW_DOWN", 5, 56, self.font_sm)

    def render_menu(self, state):
        s = self.surface
        s.fill(self.BG)
        # Tab bar
        TW, TH = 42, 10
        for t in range(3):
            x = t * TW
            active = (state.menu_cat == t)
            if active and state.menu_on_tab:
                self._box(x, 0, TW, TH-1)
                surf = self.font_sm.render(MENU_CAT_NAMES[t], True, self.BG)
                s.blit(surf, (x+3, 1))
            elif active:
                self._hline(x, TH-1, TW)
                self._text(MENU_CAT_NAMES[t], x+3, 1, self.font_sm)
            else:
                self._text(MENU_CAT_NAMES[t], x+3, 1, self.font_sm)
        self._hline(0, TH, 128)

        # Items grid
        items = MENU_CAT_ITEMS[state.menu_cat]
        CW, CH = 42, 12
        MY = TH + 2
        for i, label in enumerate(items):
            r = i // MENU_COLS
            c = i % MENU_COLS
            x = c * CW
            y = MY + r * CH
            if y + CH > 128:
                break
            selected = (not state.menu_on_tab and r == state.menu_row and c == state.menu_col)
            if selected:
                self._box(x, y, CW-1, CH-1)
                surf = self.font_sm.render(label, True, self.BG)
                s.blit(surf, (x+2, y+2))
            else:
                self._box(x, y, CW-1, CH-1, fill=False)
                self._text(label, x+2, y+2, self.font_sm)

    def render_info(self, state, title, lines):
        """Generic info display for other modes."""
        s = self.surface
        s.fill(self.BG)
        self._box(0, 0, 128, 11)
        surf = self.font_md.render(title, True, self.BG)
        s.blit(surf, (2, 1))
        for i, line in enumerate(lines[:8]):
            self._text(line, 2, 14 + i * 13, self.font_sm)

    def render(self, state):
        if state.menu_open:
            self.render_menu(state)
        elif state.mode_idx == 0:  # SYNTH
            self.render_synth(state)
        elif state.mode_idx == 11:  # 303
            self.render_mode_303(state)
        else:
            mode_name = MODE_NAMES[state.mode_idx] if state.mode_idx < len(MODE_NAMES) else "???"
            self.render_info(state, mode_name, ["Mode not in sim.", "", "Use SYNTH or 303"])

    def get_scaled(self):
        return pygame.transform.scale(self.surface, (256, 256))

# ─── Window layout constants ──────────────────────────────────────────────────

WIN_W, WIN_H = 980, 660

OLED_X, OLED_Y = 10, 10          # OLED 256×256
POT_X, POT_Y = 280, 10           # 7 pot sliders
POT_W, POT_H = 50, 130           # each pot area
POT_KNOB_H = 100                 # slider track height
JOY_X, JOY_Y = 660, 10          # joystick center reference
JOY_R = 60                       # joystick area radius
LED_X, LED_Y = 660, 150          # LED grid top-left
LED_R = 9                        # LED circle radius
LED_GW, LED_GH = 22, 22         # LED grid cell size
KEY_X, KEY_Y = 10, 290           # keyboard grid
KEY_W, KEY_H = 55, 45            # each key size
KEY_GAP = 3
BTN_X, BTN_Y = 10, 580           # menu buttons
BTN_W, BTN_H = 80, 42
INFO_X, INFO_Y = 280, 300        # info panel
INFO_W = 380

# ─── Main Window ─────────────────────────────────────────────────────────────

class SimWindow:
    def __init__(self, state, audio):
        pygame.init()
        pygame.display.set_caption("GrvEP Simulator")
        self.screen = pygame.display.set_mode((WIN_W, WIN_H))
        self.state = state
        self.audio = audio
        self.oled = OLEDRenderer()
        self.clock = pygame.time.Clock()
        try:
            self.font_ui = pygame.font.SysFont("monospace", 12, bold=False)
            self.font_ui_bold = pygame.font.SysFont("monospace", 12, bold=True)
            self.font_tiny = pygame.font.SysFont("monospace", 10, bold=False)
        except Exception:
            self.font_ui = pygame.font.Font(None, 13)
            self.font_ui_bold = pygame.font.Font(None, 14)
            self.font_tiny = pygame.font.Font(None, 11)

        # PC keyboard → (row, col) mapping
        self.key_map = {
            # Row 3 (top notes) ← keys 1-8
            pygame.K_1: (3,7), pygame.K_2: (3,6), pygame.K_3: (3,5), pygame.K_4: (3,4),
            pygame.K_5: (3,3), pygame.K_6: (3,2), pygame.K_7: (3,1), pygame.K_8: (3,0),
            # Row 2 ← QWERTYUI
            pygame.K_q: (2,7), pygame.K_w: (2,6), pygame.K_e: (2,5), pygame.K_r: (2,4),
            pygame.K_t: (2,3), pygame.K_y: (2,2), pygame.K_u: (2,1), pygame.K_i: (2,0),
            # Row 1 ← ASDFGHJK
            pygame.K_a: (1,7), pygame.K_s: (1,6), pygame.K_d: (1,5), pygame.K_f: (1,4),
            pygame.K_g: (1,3), pygame.K_h: (1,2), pygame.K_j: (1,1), pygame.K_k: (1,0),
            # Row 0 (bottom) ← ZXCVBNM,
            pygame.K_z: (0,7), pygame.K_x: (0,6), pygame.K_c: (0,5), pygame.K_v: (0,4),
            pygame.K_b: (0,3), pygame.K_n: (0,2), pygame.K_m: (0,1), pygame.K_COMMA: (0,0),
        }
        # Menu buttons: F1-F4
        self.menu_btn_keys = [pygame.K_F1, pygame.K_F2, pygame.K_F3, pygame.K_F4]
        # Track which key was pressed with mouse (to release on mouse-up)
        self._mouse_pressed_key = None  # (row, col)

    # ── event handling ──────────────────────────────────────────────────────

    def handle_event(self, event):
        st = self.state
        au = self.audio

        if event.type == pygame.QUIT:
            return False

        elif event.type == pygame.KEYDOWN:
            self._on_keydown(event)

        elif event.type == pygame.KEYUP:
            self._on_keyup(event)

        elif event.type == pygame.MOUSEBUTTONDOWN:
            self._on_mouse_down(event)

        elif event.type == pygame.MOUSEBUTTONUP:
            self._on_mouse_up(event)

        elif event.type == pygame.MOUSEMOTION:
            self._on_mouse_move(event)

        elif event.type == pygame.MOUSEWHEEL:
            self._on_wheel(event)

        return True

    def _on_keydown(self, event):
        st = self.state
        au = self.audio

        # Note keys
        if event.key in self.key_map and not st.menu_open:
            r, c = self.key_map[event.key]
            if not st.key_states[r][c]:
                st.key_states[r][c] = True
                note = st.note_map[r][c]
                st.active_notes[note] = True
                au.note_on(note, 0.75)
                st.update_leds()
            return

        # Menu buttons F1-F4
        if event.key in self.menu_btn_keys:
            btn = self.menu_btn_keys.index(event.key)
            self._handle_menu_btn(btn)
            return

        # Global shortcuts
        key = event.key
        mods = pygame.key.get_mods()

        if key == pygame.K_SPACE:
            self._toggle_menu()
        elif key == pygame.K_ESCAPE:
            if st.menu_open:
                st.menu_open = False
            else:
                au.all_notes_off()
                for r in range(5): st.key_states[r] = [False]*8
                st.active_notes.clear()
                st.update_leds()
        elif key == pygame.K_LEFTBRACKET and not st.menu_open:
            st.octave = max(-2, st.octave - 1)
            st.update_note_map()
        elif key == pygame.K_RIGHTBRACKET and not st.menu_open:
            st.octave = min(1, st.octave + 1)
            st.update_note_map()
        elif key == pygame.K_TAB and not st.menu_open:
            if mods & pygame.KMOD_SHIFT:
                st.scale_idx = (st.scale_idx - 1) % len(SCALE_NAMES)
            else:
                st.scale_idx = (st.scale_idx + 1) % len(SCALE_NAMES)
            st.update_note_map()
        elif key == pygame.K_UP and not st.menu_open:
            st.shape_idx = (st.shape_idx + 1) % len(SHAPE_NAMES)
            au.set_shape(st.shape_idx)
        elif key == pygame.K_DOWN and not st.menu_open:
            st.shape_idx = (st.shape_idx - 1) % len(SHAPE_NAMES)
            au.set_shape(st.shape_idx)
        # FX toggles
        elif key == pygame.K_F5 and not st.menu_open:
            st.fx_lpf = not st.fx_lpf
            au.set_filter_type(_amy.FILTER_LPF24 if st.fx_lpf else _amy.FILTER_NONE)
        elif key == pygame.K_F6 and not st.menu_open:
            st.fx_reverb = not st.fx_reverb
            au.set_reverb(0.35 if st.fx_reverb else 0.0)
        elif key == pygame.K_F7 and not st.menu_open:
            st.fx_hpf = not st.fx_hpf
            au.set_filter_type(_amy.FILTER_HPF if st.fx_hpf else _amy.FILTER_NONE)
        # Menu navigation
        elif st.menu_open:
            self._menu_key(key)

    def _on_keyup(self, event):
        st = self.state
        if event.key in self.key_map:
            r, c = self.key_map[event.key]
            if st.key_states[r][c]:
                st.key_states[r][c] = False
                note = st.note_map[r][c]
                self.audio.note_off(note)
                st.active_notes.pop(note, None)
                st.update_leds()

    def _handle_menu_btn(self, btn_idx):
        st = self.state
        au = self.audio
        # Btn 0 = B1 = short press → toggle menu; long press handled elsewhere
        # Btn 1 = B2 = cycle arp (not implemented)
        # Btn 2 = B3 = cycle envelope
        # Btn 3 = B4 = cycle octave
        if btn_idx == 0:  # B1 = menu toggle
            self._toggle_menu()
        elif btn_idx == 1:  # B2 = scale
            st.scale_idx = (st.scale_idx + 1) % len(SCALE_NAMES)
            st.update_note_map()
        elif btn_idx == 2:  # B3 = envelope
            st.env_idx = (st.env_idx + 1) % len(ENV_NAMES)
            au.set_envelope(st.env_idx)
        elif btn_idx == 3:  # B4 = octave
            st.octave = st.octave + 1
            if st.octave > 1: st.octave = -2
            st.update_note_map()

    def _toggle_menu(self):
        st = self.state
        st.menu_open = not st.menu_open
        if st.menu_open:
            self.audio.all_notes_off()
            for r in range(5): st.key_states[r] = [False]*8
            st.active_notes.clear()
            st.update_leds()
            st.menu_on_tab = True
            st.menu_row = 0
            st.menu_col = 0

    def _menu_key(self, key):
        st = self.state
        items = MENU_CAT_ITEMS[st.menu_cat]
        n_rows = (len(items) + MENU_COLS - 1) // MENU_COLS

        if key == pygame.K_LEFT:
            if st.menu_on_tab:
                st.menu_cat = max(0, st.menu_cat - 1)
                st.menu_row, st.menu_col = 0, 0
            else:
                if st.menu_col > 0:
                    st.menu_col -= 1
        elif key == pygame.K_RIGHT:
            if st.menu_on_tab:
                st.menu_cat = min(2, st.menu_cat + 1)
                st.menu_row, st.menu_col = 0, 0
            else:
                if st.menu_col < MENU_COLS - 1:
                    st.menu_col += 1
        elif key == pygame.K_DOWN:
            if st.menu_on_tab:
                st.menu_on_tab = False
                st.menu_row, st.menu_col = 0, 0
            elif st.menu_row < n_rows - 1:
                st.menu_row += 1
        elif key == pygame.K_UP:
            if not st.menu_on_tab:
                if st.menu_row > 0:
                    st.menu_row -= 1
                else:
                    st.menu_on_tab = True
        elif key in (pygame.K_RETURN, pygame.K_KP_ENTER):
            self._select_menu()
        elif key == pygame.K_ESCAPE:
            if not st.menu_on_tab:
                st.menu_on_tab = True
            else:
                st.menu_open = False

    def _select_menu(self):
        st = self.state
        if st.menu_on_tab:
            st.menu_on_tab = False
            return
        items = MENU_CAT_ITEMS[st.menu_cat]
        modes = MENU_CAT_MODES[st.menu_cat]
        idx = st.menu_row * MENU_COLS + st.menu_col
        if idx < len(modes):
            st.mode_idx = modes[idx]
            st.menu_open = False
            st.menu_on_tab = True

    def _on_mouse_down(self, event):
        mx, my = event.pos
        st = self.state
        btn = event.button

        if btn == 1:
            # Check pot areas
            for i in range(7):
                px, py = self._pot_pos(i)
                if px <= mx <= px+POT_W and py <= my <= py+POT_H+20:
                    st.dragging_pot = i
                    st.drag_start_y = my
                    st.drag_start_val = st.pot_values[i]
                    return

            # Check joystick
            jcx, jcy = JOY_X + JOY_R, JOY_Y + JOY_R
            if math.hypot(mx - jcx, my - jcy) <= JOY_R:
                st.dragging_joy = True
                st.joy_cx, st.joy_cy = jcx, jcy
                return

            # Check keyboard keys
            self._check_key_click(mx, my, True)

            # Check menu buttons
            self._check_btn_click(mx, my)

            # Check OLED (toggle menu on click)
            if OLED_X <= mx <= OLED_X+256 and OLED_Y <= my <= OLED_Y+256:
                if st.menu_open:
                    # Crude menu click detection
                    pass

    def _on_mouse_up(self, event):
        st = self.state
        if event.button == 1:
            if st.dragging_pot is not None:
                st.dragging_pot = None
                self._apply_pots()
            if st.dragging_joy:
                st.dragging_joy = False
                st.joy_x = 0.0
                st.joy_y = 0.0
                self.audio.set_pitch_bend(0.0)
            # Release the key that was pressed with mouse
            if self._mouse_pressed_key is not None:
                r, c = self._mouse_pressed_key
                self._mouse_pressed_key = None
                if st.key_states[r][c]:
                    st.key_states[r][c] = False
                    note = st.note_map[r][c]
                    self.audio.note_off(note)
                    st.active_notes.pop(note, None)
                    st.update_leds()

    def _on_mouse_move(self, event):
        mx, my = event.pos
        st = self.state

        if st.dragging_pot is not None:
            dy = st.drag_start_y - my
            val = st.drag_start_val + dy / POT_KNOB_H
            st.pot_values[st.dragging_pot] = max(0.0, min(1.0, val))
            self._apply_pots()

        if st.dragging_joy:
            dx = mx - st.joy_cx
            dy = my - st.joy_cy
            dist = math.hypot(dx, dy)
            if dist > JOY_R:
                dx = dx / dist * JOY_R
                dy = dy / dist * JOY_R
            st.joy_x = dx / JOY_R
            st.joy_y = -dy / JOY_R  # Y is inverted in screen coords
            # Pitch bend on X (±2 semitones)
            self.audio.set_pitch_bend(st.joy_x * 2.0)

    def _on_wheel(self, event):
        pass

    def _pot_pos(self, i):
        """Return top-left (x, y) of pot slider area."""
        return (POT_X + i * (POT_W + 5), POT_Y)

    def _apply_pots(self):
        st = self.state
        au = self.audio
        # Pot 0: Volume
        au.set_volume(st.pot_values[0])
        # Pot 1: Cutoff
        cut_norm = st.pot_values[1]
        if cut_norm >= 0.99:
            cut_hz = 18000.0
        else:
            cut_hz = self.audio.CUTOFF_MIN * (self.audio.CUTOFF_MAX / self.audio.CUTOFF_MIN) ** cut_norm
        # Pot 2: Resonance
        res = 0.5 + st.pot_values[2] * 15.5  # 0.5 → 16
        au.set_filter(cut_hz, res)

    def _check_key_click(self, mx, my, pressed):
        """Handle mouse press on keyboard grid."""
        if not pressed:
            return  # release handled via _mouse_pressed_key
        for r in range(4):
            for c in range(8):
                kx = KEY_X + c * (KEY_W + KEY_GAP)
                ky = KEY_Y + (3 - r) * (KEY_H + KEY_GAP)
                if kx <= mx <= kx+KEY_W and ky <= my <= ky+KEY_H:
                    st = self.state
                    if not st.key_states[r][c]:
                        st.key_states[r][c] = True
                        note = st.note_map[r][c]
                        st.active_notes[note] = True
                        self.audio.note_on(note, 0.75)
                        st.update_leds()
                        self._mouse_pressed_key = (r, c)
                    return

    def _check_btn_click(self, mx, my):
        """Handle click on B1-B4 menu buttons."""
        for i in range(4):
            bx = BTN_X + i * (BTN_W + 8)
            by = BTN_Y
            if bx <= mx <= bx+BTN_W and by <= my <= by+BTN_H:
                self._handle_menu_btn(i)
                return

    # ── drawing ─────────────────────────────────────────────────────────────

    def _label(self, txt, x, y, color=(200,200,200), bold=False):
        font = self.font_ui_bold if bold else self.font_ui
        surf = font.render(str(txt), True, color)
        self.screen.blit(surf, (x, y))

    def _tiny(self, txt, x, y, color=(160,160,160)):
        surf = self.font_tiny.render(str(txt), True, color)
        self.screen.blit(surf, (x, y))

    def draw(self):
        scr = self.screen
        st = self.state

        # Background
        scr.fill((18, 18, 24))

        # ── OLED display ──
        self.oled.render(st)
        oled_surf = self.oled.get_scaled()
        # OLED bezel
        pygame.draw.rect(scr, (30, 30, 40), (OLED_X-5, OLED_Y-5, 266, 266), border_radius=6)
        scr.blit(oled_surf, (OLED_X, OLED_Y))
        # OLED border
        pygame.draw.rect(scr, (60, 100, 180), (OLED_X, OLED_Y, 256, 256), 1)
        self._tiny("OLED 128×128", OLED_X, OLED_Y+258)

        # ── Pot sliders ──
        pot_labels = ["Vol", "Cut", "Res", "TL", "TR", "BL", "BR"]
        for i in range(7):
            px, py = self._pot_pos(i)
            val = st.pot_values[i]
            track_y = py + 20
            # Track
            pygame.draw.rect(scr, (40, 40, 60), (px+20, track_y, 10, POT_KNOB_H))
            # Filled portion
            fill_h = int(val * POT_KNOB_H)
            pygame.draw.rect(scr, (60, 130, 220), (px+20, track_y + POT_KNOB_H - fill_h, 10, fill_h))
            # Knob
            knob_y = track_y + POT_KNOB_H - fill_h
            pygame.draw.rect(scr, (200, 200, 255), (px+16, knob_y-3, 18, 7), border_radius=3)
            # Label
            self._tiny(pot_labels[i], px+15, py+3)
            # Value
            self._tiny(f"{val:.2f}", px+10, py+POT_KNOB_H+24)

        # ── Joystick ──
        jcx, jcy = JOY_X + JOY_R, JOY_Y + JOY_R
        # Background circle
        pygame.gfxdraw.aacircle(scr, jcx, jcy, JOY_R, (40, 40, 60))
        pygame.gfxdraw.filled_circle(scr, jcx, jcy, JOY_R, (25, 25, 40))
        # Crosshair
        pygame.draw.line(scr, (50, 50, 70), (JOY_X, jcy), (JOY_X+JOY_R*2, jcy))
        pygame.draw.line(scr, (50, 50, 70), (jcx, JOY_Y), (jcx, JOY_Y+JOY_R*2))
        # Thumb
        tx = int(jcx + st.joy_x * JOY_R)
        ty = int(jcy - st.joy_y * JOY_R)
        pygame.gfxdraw.aacircle(scr, tx, ty, 10, (100, 160, 255))
        pygame.gfxdraw.filled_circle(scr, tx, ty, 10, (80, 130, 220))
        self._tiny("Joystick (drag)", JOY_X, JOY_Y + JOY_R*2 + 5)
        self._tiny(f"X:{st.joy_x:+.2f} Y:{st.joy_y:+.2f}", JOY_X, JOY_Y + JOY_R*2 + 18)

        # ── LED grid ──
        # Layout: row 4 (4 LEDs), row 3 (8 rev), row 2 (8), row 1 (8 rev), row 0 (8)
        # Map each LED index to a grid position
        self._draw_leds(st)

        # ── Keyboard grid ──
        notes_name = ["C","C#","D","D#","E","F","F#","G","G#","A","A#","B"]
        for r in range(4):
            for c in range(8):
                kx = KEY_X + c * (KEY_W + KEY_GAP)
                ky = KEY_Y + (3 - r) * (KEY_H + KEY_GAP)
                note = st.note_map[r][c]
                pressed = st.key_states[r][c]
                if pressed:
                    col = (80, 180, 255)
                elif (note % 12) in (1, 3, 6, 8, 10):
                    col = (40, 40, 55)
                else:
                    col = (55, 55, 75)
                pygame.draw.rect(scr, col, (kx, ky, KEY_W, KEY_H), border_radius=4)
                pygame.draw.rect(scr, (80, 80, 110), (kx, ky, KEY_W, KEY_H), 1, border_radius=4)
                # Note label
                n_name = notes_name[note % 12]
                oct_n = note // 12 - 1
                self._tiny(f"{n_name}{oct_n}", kx+4, ky+KEY_H-15, (200,200,200) if not pressed else (0,0,0))

        # Label above keyboard
        self._tiny(f"Keyboard  Scale:{SCALE_NAMES[st.scale_idx]}  Oct:{st.octave:+d}", KEY_X, KEY_Y-16, (160,160,180))

        # ── Menu buttons ──
        btn_labels = ["B1:Menu", "B2:Scale", "B3:Env", "B4:Oct"]
        btn_colors = [(60,40,80), (40,60,80), (40,80,60), (80,60,40)]
        for i, (lbl, col) in enumerate(zip(btn_labels, btn_colors)):
            bx = BTN_X + i * (BTN_W + 8)
            by = BTN_Y
            active = (i == 0 and st.menu_open)
            bc = (150, 100, 200) if active else col
            pygame.draw.rect(scr, bc, (bx, by, BTN_W, BTN_H), border_radius=5)
            pygame.draw.rect(scr, (100, 100, 140), (bx, by, BTN_W, BTN_H), 1, border_radius=5)
            self._label(lbl, bx+4, by+14, bold=True)

        # ── Info panel ──
        self._draw_info(st)

        # ── Status bar ──
        pygame.draw.rect(scr, (12, 12, 20), (0, WIN_H-22, WIN_W, 22))
        shortcuts = "Notes: 1-8 / Q-I / A-K / Z-, keys  |  [ ] = Oct  |  TAB = Scale  |  ↑↓ = Shape  |  F1=Menu  F2=Scl  F3=Env  F4=Oct  F5=LPF  F6=Rev  F7=HPF  |  ESC=AllOff"
        self._tiny(shortcuts, 5, WIN_H-17, (100, 100, 130))

    def _draw_leds(self, st):
        """Draw the 36 LED grid."""
        # LED layout (row top to bottom in display):
        # Row 4 (menu): LEDs 32-35 at columns 4-7
        # Row 3: 31-24 (reversed), row 2: 16-23, row 1: 15-8 (rev), row 0: 0-7
        led_positions = {}  # led_idx -> (display_x, display_y)

        bx, by = LED_X, LED_Y
        # Row 4 menu buttons: 4 LEDs in a row at the top
        for ci, li in enumerate([32, 33, 34, 35]):
            led_positions[li] = (bx + (4+ci)*LED_GW, by)
        # Row 3 reversed
        for ci, li in enumerate([31,30,29,28,27,26,25,24]):
            led_positions[li] = (bx + ci*LED_GW, by + LED_GH)
        # Row 2
        for ci, li in enumerate(range(16, 24)):
            led_positions[li] = (bx + ci*LED_GW, by + 2*LED_GH)
        # Row 1 reversed
        for ci, li in enumerate([15,14,13,12,11,10,9,8]):
            led_positions[li] = (bx + ci*LED_GW, by + 3*LED_GH)
        # Row 0
        for ci, li in enumerate(range(0, 8)):
            led_positions[li] = (bx + ci*LED_GW, by + 4*LED_GH)

        # Background
        pygame.draw.rect(self.screen, (15, 15, 25),
                         (LED_X-4, LED_Y-4, 8*LED_GW+8, 5*LED_GH+8), border_radius=4)

        for li in range(NUM_LEDS):
            if li not in led_positions:
                continue
            lx, ly = led_positions[li]
            r, g, b = st.leds[li]
            cx, cy = lx + LED_R, ly + LED_R
            # Glow effect
            if r > 20 or g > 20 or b > 20:
                glow = (max(0,r//3), max(0,g//3), max(0,b//3))
                pygame.gfxdraw.aacircle(self.screen, cx, cy, LED_R+3, glow)
                pygame.gfxdraw.filled_circle(self.screen, cx, cy, LED_R+3, glow)
            pygame.gfxdraw.aacircle(self.screen, cx, cy, LED_R, (r, g, b))
            pygame.gfxdraw.filled_circle(self.screen, cx, cy, LED_R, (r, g, b))
            # Outline
            pygame.gfxdraw.aacircle(self.screen, cx, cy, LED_R, (50, 50, 70))

        self._tiny("LEDs (36)", LED_X, LED_Y + 5*LED_GH + 4, (100, 100, 130))

    def _draw_info(self, st):
        """Draw the info panel showing current mode/shape/FX."""
        x, y = INFO_X, INFO_Y
        pygame.draw.rect(self.screen, (22, 22, 35), (x-5, y-5, INFO_W, 270), border_radius=6)
        pygame.draw.rect(self.screen, (50, 50, 80), (x-5, y-5, INFO_W, 270), 1, border_radius=6)

        mode_name = MODE_NAMES[st.mode_idx] if st.mode_idx < len(MODE_NAMES) else "???"
        self._label(f"Mode: {mode_name}", x, y, (255,220,100), bold=True)
        self._label(f"Shape: {SHAPE_NAMES[st.shape_idx]}", x, y+18, (180,220,255))
        self._label(f"Scale: {SCALE_NAMES[st.scale_idx]}  Oct: {st.octave:+d}  Env: {ENV_NAMES[st.env_idx]}", x, y+36)

        # FX
        fx = []
        if st.fx_lpf: fx.append("LPF")
        if st.fx_hpf: fx.append("HPF")
        if st.fx_reverb: fx.append("REV")
        if st.fx_delay: fx.append("DLY")
        self._label(f"FX: {' '.join(fx) if fx else '---'}", x, y+54)

        # Pot values
        self._label("Pots:", x, y+72)
        pot_labels = ["Vol","Cut","Res","TL","TR","BL","BR"]
        for i, (lbl, val) in enumerate(zip(pot_labels, st.pot_values)):
            px = x + (i % 4) * 90
            py = y + 88 + (i // 4) * 16
            self._tiny(f"{lbl}:{val:.2f}", px, py, (160,200,160))

        # Active notes
        notes_name = ["C","C#","D","D#","E","F","F#","G","G#","A","A#","B"]
        active = sorted(st.active_notes.keys())
        note_strs = [f"{notes_name[n%12]}{n//12-1}" for n in active]
        self._label(f"Notes: {' '.join(note_strs[:8])}", x, y+128)

        # Joystick
        self._label(f"Joy: X={st.joy_x:+.2f}  Y={st.joy_y:+.2f}  PB={st.joy_x*2:+.1f}st", x, y+146)

        # Keyboard shortcuts reminder
        pygame.draw.line(self.screen, (50,50,70), (x, y+162), (x+INFO_W-15, y+162))
        shortcuts2 = [
            "F1=Menu  F2=Scale  F3=Env  F4=Oct",
            "[ / ] = Octave down / up",
            "TAB = Next scale  (SHIFT+TAB = prev)",
            "↑ / ↓ = Next / Prev shape",
            "ESC = All notes off",
        ]
        for i, line in enumerate(shortcuts2):
            self._tiny(line, x, y+168+i*14, (100,130,160))

    def run(self):
        running = True
        while running:
            for event in pygame.event.get():
                if not self.handle_event(event):
                    running = False
            self.draw()
            pygame.display.flip()
            self.clock.tick(60)

        pygame.quit()

# ─── Entry point ─────────────────────────────────────────────────────────────

def main():
    print("GrvEP Simulator starting...")
    print("Initializing Amy audio engine...")

    state = SimState()
    audio = AudioEngine()
    audio.init()
    audio.set_shape(state.shape_idx)

    # Apply initial pot values
    audio.set_volume(state.pot_values[0])

    print("Starting window...")
    window = SimWindow(state, audio)

    print("Ready! Window open.")
    print("Keyboard: 1-8 / Q-I / A-K / Z-, = 32 piano keys")
    print("[ ] = octave, TAB = scale, ↑↓ = shape, F1 = menu, ESC = all notes off")

    window.run()

    print("Shutting down...")

if __name__ == "__main__":
    main()
