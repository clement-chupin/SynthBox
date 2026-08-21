#!/usr/bin/env python3
"""
SynthViz — GrvEP companion visuel (MIDI)
Écoute les événements MIDI USB du synthé et affiche des animations.

Prérequis :
  pip install pygame mido python-rtmidi

Lancement :
  python3 synthviz.py            # auto-détection port GrvEP
  python3 synthviz.py "GrvEP"   # port explicite (partiel suffit)

Touches :
  1–0 / ← →   changer d'animation
  ↑ ↓          vitesse +/-
  Espace        note test
  B             beat test
  Esc           quitter

MIDI reçu :
  Canal 1  — synth mélodique (SYNTH, OMNI, MOD2…)
  Canal 2  — basse acide (303, 303S)
  Canal 10 — drums GM (Kick=36, Snare=38, CHH=42, OHH=46, Clap=39…)
"""

import sys, math, time, random, colorsys, threading
import pygame
try:
    import mido
except ImportError:
    print("mido non installé — pip install mido python-rtmidi")
    sys.exit(1)

TAU  = math.pi * 2
W, H = 1280, 720
FPS  = 60
CX   = W // 2
CY   = H // 2

# GM drum note → pad index (0-7)
GM_TO_PAD = {36: 0, 38: 1, 42: 2, 46: 3, 39: 4, 41: 5, 43: 6, 49: 7}
PAD_NAMES = ["Kick", "Snare", "CHH", "OHH", "Clap", "FTom", "MTom", "Crash"]

def hsv(h, s=1.0, v=1.0):
    r, g, b = colorsys.hsv_to_rgb(h % 1.0, s, v)
    return (max(0,min(255,int(r*255))), max(0,min(255,int(g*255))), max(0,min(255,int(b*255))))

def note_hue(note, ch=1):
    base = (note / 127.0 * 0.75 + 0.55) % 1.0
    if ch == 2:   return (base + 0.33) % 1.0   # bass → décalé vert/jaune
    return base

def lerp(a, b, t): return a + (b - a) * t


# ═══════════════════════════ ANIMATIONS ═══════════════════════════════════════

class Anim:
    name  = "?"
    speed = 1.0
    def on_note_on(self, note, vel, ch): pass
    def on_note_off(self, note, ch):     pass
    def on_beat(self):                   pass
    def on_drum(self, pad, vel):         pass
    def update(self, dt, t):             pass
    def draw(self, surf):                pass


# ── 1. STARFIELD ──────────────────────────────────────────────────────────────
class Starfield(Anim):
    name = "STARS"
    N    = 600
    def __init__(self):
        self.stars  = [self._new(random.random()) for _ in range(self.N)]
        self.burst  = 0.0
        self.sparks = []
    def _new(self, z=1.0):
        return [random.uniform(-1,1), random.uniform(-1,1), max(0.02,z)]
    def on_note_on(self, note, vel, ch):
        h = note_hue(note, ch)
        for _ in range(max(4, vel//9)):
            a = random.uniform(0, TAU)
            s = random.uniform(0.8, 4.0) * (vel/127.0)
            self.sparks.append({'x': note/127.0*W, 'y': CY+random.randint(-120,120),
                'vx': math.cos(a)*s*5, 'vy': math.sin(a)*s*3,
                'life': 1.0, 'decay': random.uniform(0.015,0.04),
                'col': hsv(h), 'r': random.randint(2,5)})
    def on_beat(self): self.burst = 1.0
    def update(self, dt, t):
        spd = self.speed * dt * (0.4 + self.burst*2.5)
        self.burst = max(0.0, self.burst - dt*1.8)
        for s in self.stars:
            s[2] -= spd
            if s[2] <= 0.003: s[:] = self._new()
        for e in self.sparks:
            e['x'] += e['vx']*dt; e['y'] += e['vy']*dt
            e['vy'] += 30*dt; e['life'] -= e['decay']
        self.sparks = [e for e in self.sparks if e['life'] > 0]
    def draw(self, surf):
        surf.fill((0,0,10))
        for x,y,z in self.stars:
            sx,sy = int(CX+x/z*W*0.5), int(CY+y/z*H*0.5)
            r = max(1, int((1-z)*4))
            br = max(0,min(255,int((1-z)*255)))
            if 0<=sx<W and 0<=sy<H: pygame.draw.circle(surf,(br,br,br),(sx,sy),r)
        for e in self.sparks:
            col = tuple(max(0,min(255,int(c*e['life']))) for c in e['col'])
            x,y = int(e['x']),int(e['y'])
            if 0<=x<W and 0<=y<H: pygame.draw.circle(surf,col,(x,y),e['r'])


# ── 2. LISSAJOUS ──────────────────────────────────────────────────────────────
class Lissajous(Anim):
    name = "LISSAJOUS"
    def __init__(self):
        self.trail = pygame.Surface((W,H)); self.trail.fill((0,0,0))
        self.fade  = pygame.Surface((W,H)); self.fade.fill((0,0,0)); self.fade.set_alpha(14)
        self.a=2.0; self.b=3.0; self.phase=0.0; self.hue=0.0; self.bscale=1.0
        self.notes=[]
    def on_note_on(self, note, vel, ch):
        self.notes.append(note)
        if len(self.notes)>=2:
            d = abs(self.notes[-1]-self.notes[-2])
            self.a=1+d//7; self.b=1+(d%7)+0.5*(d//14)
        else:
            self.a=1+(note%5); self.b=1+((note//5)%5)+0.3*((note//25)%3)
    def on_note_off(self, note, ch): self.notes=[n for n in self.notes if n!=note]
    def on_beat(self): self.bscale=1.25
    def update(self, dt, t):
        self.phase=(self.phase+dt*self.speed*0.9)
        self.hue=(self.hue+dt*0.04)%1.0
        self.bscale=lerp(self.bscale,1.0,dt*5)
    def draw(self, surf):
        self.trail.blit(self.fade,(0,0))
        N=700; RX=W*0.43*self.bscale; RY=H*0.43*self.bscale
        pts=[]
        for i in range(N+1):
            p=i/N*TAU
            pts.append((int(CX+math.sin(self.a*p+self.phase*0.25)*RX),
                         int(CY+math.sin(self.b*p)*RY)))
        for i in range(len(pts)-1):
            pygame.draw.line(self.trail,hsv((self.hue+i/N*0.5)%1.0),pts[i],pts[i+1],2)
        surf.blit(self.trail,(0,0))


# ── 3. PLASMA ORBS ────────────────────────────────────────────────────────────
class Plasma(Anim):
    name="PLASMA"
    def __init__(self):
        self.orbs=[{'ox':random.uniform(0,TAU),'oy':random.uniform(0,TAU),
                    'fx':random.uniform(0.25,1.1),'fy':random.uniform(0.2,1.0),
                    'r':random.randint(120,320),'h':random.random()} for _ in range(14)]
        self.nh=0.0; self.ni=0.0; self.beat=0.0
        self.layer=pygame.Surface((W,H),pygame.SRCALPHA)
    def on_note_on(self,note,vel,ch): self.nh=note_hue(note,ch); self.ni=vel/127.0
    def on_beat(self): self.beat=1.0
    def update(self,dt,t): self.ni=max(0,self.ni-dt*0.4); self.beat=max(0,self.beat-dt*2)
    def draw(self,surf):
        surf.fill((0,0,0)); t=time.time()*self.speed
        self.layer.fill((0,0,0,0))
        for o in self.orbs:
            x=int(CX+math.sin(o['ox']+t*o['fx'])*W*0.36)
            y=int(CY+math.sin(o['oy']+t*o['fy'])*H*0.36)
            r=o['r']+int(self.beat*100)
            col=hsv((o['h']+t*0.018+self.nh*self.ni*0.5)%1.0)
            pygame.draw.circle(self.layer,(*col,max(0,min(255,30+int(self.ni*25)))),(x,y),r)
        surf.blit(self.layer,(0,0))
        if self.beat>0.05:
            s2=pygame.Surface((W,H),pygame.SRCALPHA)
            pygame.draw.circle(s2,(255,255,255,int(self.beat*50)),(CX,CY),int(80+self.beat*180))
            surf.blit(s2,(0,0))


# ── 4. TUNNEL ─────────────────────────────────────────────────────────────────
class Tunnel(Anim):
    name="TUNNEL"
    def __init__(self): self.off=0.0; self.rot=0.0; self.hue=0.0; self.beat=0.0; self.nh=0.5; self.nv=0.0
    def on_note_on(self,note,vel,ch): self.nh=note_hue(note,ch); self.nv=vel/127.0
    def on_beat(self): self.beat=1.0
    def update(self,dt,t):
        self.off+=dt*self.speed*0.65; self.rot+=dt*self.speed*0.18
        self.hue=(self.hue+dt*0.03)%1.0; self.beat=max(0,self.beat-dt*3); self.nv=max(0,self.nv-dt*0.6)
    def draw(self,surf):
        surf.fill((0,0,0))
        for i in range(25,-1,-1):
            z=(i+(self.off%1.0))/25; sc=z*z
            hw,hh=int(sc*W*0.54),int(sc*H*0.54)
            if hw<3 or hh<3: continue
            col=hsv((self.hue+i/25*0.4+self.nh*self.nv)%1.0,0.9,0.35+0.65*sc+self.beat*0.25)
            a=self.rot+i*0.1; ca,sa=math.cos(a),math.sin(a)
            def R(px,py): return (int(CX+px*ca-py*sa),int(CY+px*sa+py*ca))
            corners=[R(-hw,-hh),R(hw,-hh),R(hw,hh),R(-hw,hh)]
            pygame.draw.polygon(surf,col,corners,max(1,int(3*sc)+(2 if self.beat>0.5 else 0)))
        if self.beat>0.05:
            s=pygame.Surface((W,H),pygame.SRCALPHA); s.fill((255,255,255,int(self.beat*70))); surf.blit(s,(0,0))


# ── 5. WAVE GRID ──────────────────────────────────────────────────────────────
class WaveGrid(Anim):
    name="WAVEGRID"
    def __init__(self): self.rips=[]; self.hue=0.0; self.bamp=0.0
    def on_note_on(self,note,vel,ch):
        self.rips.append({'x':note/127.0*W,'y':CY,'t':time.time(),'amp':vel/127.0*55,'h':note_hue(note,ch)})
        if len(self.rips)>12: self.rips.pop(0)
    def on_beat(self): self.bamp=1.0
    def update(self,dt,t): self.hue=(self.hue+dt*0.025)%1.0; self.bamp=max(0,self.bamp-dt*2)
    def draw(self,surf):
        surf.fill((0,0,12)); t=time.time()*self.speed; now=time.time()
        for row in range(19):
            gy=row*H//18; pts=[]; rh=self.hue
            for gx in range(0,W+1,8):
                w=(math.sin(gx*0.012+t*1.1)*14+math.sin(gx*0.007+row*0.4+t*0.8)*10)*(1+self.bamp*3)
                for r in self.rips:
                    age=now-r['t']; dist=math.hypot(gx-r['x'],gy-r['y'])
                    fade=max(0,1-age*0.55)*max(0,1-dist/550)
                    w+=math.sin(dist*0.035-age*9)*r['amp']*fade
                    if fade>0.1: rh=r['h']
                pts.append((gx,max(0,min(H-1,int(gy+w)))))
            if len(pts)>1: pygame.draw.lines(surf,hsv((rh+row/18*0.25)%1.0,0.85,0.45+0.4*abs(self.bamp)),False,pts,2)


# ── 6. RIBBONS ────────────────────────────────────────────────────────────────
class Ribbons(Anim):
    name="RIBBONS"
    def __init__(self):
        self.ribs=[]
        self.bg=[{'freq':random.uniform(0.4,1.8),'amp':random.randint(50,160),
                  'phase':random.uniform(0,TAU),'h':random.random(),'w':random.randint(1,3)} for _ in range(7)]
        self.trail=pygame.Surface((W,H)); self.trail.fill((0,0,0))
        self.fade=pygame.Surface((W,H)); self.fade.fill((0,0,0)); self.fade.set_alpha(10)
    def on_note_on(self,note,vel,ch):
        self.ribs.append({'note':note,'vel':vel,'h':note_hue(note,ch),
            'freq':0.4+(note%12)/12.0*3.2,'amp':35+vel/127.0*210,
            'phase':random.uniform(0,TAU),'ybase':H*0.15+note/127.0*H*0.7,'life':1.0,'dying':False})
    def on_note_off(self,note,ch):
        for r in self.ribs:
            if r['note']==note: r['dying']=True
    def on_beat(self):
        for r in self.ribs: r['phase']+=math.pi*0.5
    def update(self,dt,t):
        for r in self.ribs:
            if r['dying']: r['life']=max(0,r['life']-dt*1.2)
        self.ribs=[r for r in self.ribs if r['life']>0.01]
    def draw(self,surf):
        self.trail.blit(self.fade,(0,0)); t=time.time()*self.speed
        for rb in self.bg:
            pts=[(gx,max(0,min(H-1,int(CY+math.sin(gx*rb['freq']*0.007+t*0.4+rb['phase'])*rb['amp']))))
                 for gx in range(0,W+1,10)]
            if len(pts)>1: pygame.draw.lines(self.trail,hsv((rb['h']+t*0.008)%1.0,0.55,0.22),False,pts,rb['w'])
        for rb in self.ribs:
            pts=[(gx,max(0,min(H-1,int(rb['ybase']+math.sin(gx*rb['freq']*0.009+t*2.2+rb['phase'])*rb['amp']))))
                 for gx in range(0,W+1,4)]
            if len(pts)>1: pygame.draw.lines(self.trail,hsv(rb['h'],1.0,min(1.0,rb['life'])),False,pts,max(1,rb['vel']//38))
        surf.blit(self.trail,(0,0))


# ── 7. KALEIDOSCOPE ───────────────────────────────────────────────────────────
class Kaleidoscope(Anim):
    name="KALEIDO"; NSYM=8
    def __init__(self):
        self.shapes=[]; self.angle=0.0; self.hue=0.0; self.bs=1.0; self._spawn(20)
    def _spawn(self,n=6):
        R=min(CX,CY)
        for _ in range(n):
            self.shapes.append({'r':random.uniform(30,R*0.9),'a':random.uniform(0,TAU/self.NSYM),
                'dr':random.uniform(-0.4,0.4),'da':random.uniform(-0.015,0.015),
                'h':random.random(),'sz':random.randint(3,18),'life':1.0,
                'kind':random.choice(['circle','line','tri','rect'])})
    def on_note_on(self,note,vel,ch):
        self.hue=note_hue(note,ch); R=min(CX,CY)
        for _ in range(max(3,vel//22)):
            self.shapes.append({'r':random.uniform(20,R*0.88),'a':random.uniform(0,TAU/self.NSYM),
                'dr':random.uniform(-1.2,1.2)*(vel/127.0),'da':random.uniform(-0.04,0.04),
                'h':(note_hue(note,ch)+random.uniform(-0.12,0.12))%1.0,'sz':max(3,vel//12),
                'life':1.0,'kind':random.choice(['circle','tri','rect'])})
    def on_beat(self): self.bs=1.35
    def update(self,dt,t):
        self.angle=(self.angle+dt*self.speed*0.32)%TAU; self.hue=(self.hue+dt*0.018)%1.0
        self.bs=lerp(self.bs,1.0,dt*4); R=min(CX,CY)
        for s in self.shapes:
            s['r']+=s['dr']*dt*28; s['a']+=s['da']
            if s['r']<8 or s['r']>R*1.05: s['dr']=-s['dr']; s['r']=max(8,min(s['r'],R))
            s['life']-=dt*0.04
        self.shapes=[s for s in self.shapes if s['life']>0]
        if len(self.shapes)<10: self._spawn(10)
        if len(self.shapes)>55: self.shapes=self.shapes[-55:]
    def draw(self,surf):
        surf.fill((0,0,0))
        # tunnel zoom: 6 rings continuously drifting from center to edge
        t_now=time.time()*self.speed*0.20
        N_RINGS=6
        phases=[((i/N_RINGS)+t_now)%1.0 for i in range(N_RINGS)]
        order=sorted(range(N_RINGS),key=lambda i:-phases[i])  # far first
        for i in order:
            phase=phases[i]
            scale=0.15*(7.0**phase)  # exponential growth center→edge
            alpha=min(1.0,phase/0.10)*max(0.0,1.0-(phase-0.78)/0.22)
            if alpha<=0: continue
            lag=phase*TAU*0.18
            for s in self.shapes:
                col=hsv(s['h'],1.0,min(1.0,(s['life']+0.2)*alpha))
                for sym in range(self.NSYM):
                    a=s['a']+sym*TAU/self.NSYM+self.angle-lag; r=s['r']*self.bs*scale
                    x,y=int(CX+math.cos(a)*r),int(CY+math.sin(a)*r); sz=max(1,int(s['sz']*min(1.0,scale)))
                    if s['kind']=='circle' and 0<=x<W and 0<=y<H: pygame.draw.circle(surf,col,(x,y),sz)
                    elif s['kind']=='line':
                        x2,y2=int(CX+math.cos(a+0.35)*r*1.25),int(CY+math.sin(a+0.35)*r*1.25)
                        pygame.draw.line(surf,col,(x,y),(x2,y2),max(1,sz//4))
                    elif s['kind']=='tri':
                        pts=[(int(x+math.cos(a+i*TAU/3)*sz),int(y+math.sin(a+i*TAU/3)*sz)) for i in range(3)]
                        pygame.draw.polygon(surf,col,pts)
                    elif s['kind']=='rect': pygame.draw.rect(surf,col,(x-sz//2,y-sz//2,sz,sz))


# ── 8. MATRIX RAIN ────────────────────────────────────────────────────────────
class MatrixRain(Anim):
    name="MATRIX"; NCOLS=52; CH=16
    def __init__(self):
        self.cw=W//self.NCOLS; self.nr=H//self.CH+2
        self.drops=[random.randint(-self.nr,0) for _ in range(self.NCOLS)]
        self.speeds=[random.uniform(0.25,1.1) for _ in range(self.NCOLS)]
        self.hue=0.33; self.flashes={}
        self.font=pygame.font.SysFont("monospace",self.CH,bold=True)
        chars="0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ@#$%&"
        self.surfs=[self.font.render(c,True,(255,255,255)) for c in chars]
    def on_note_on(self,note,vel,ch): self.hue=note_hue(note,ch)
    def on_drum(self,pad,vel):
        for _ in range(4): self.flashes[random.randint(0,self.NCOLS-1)]=1.0
    def on_beat(self):
        for i in range(self.NCOLS): self.speeds[i]=min(2.8,self.speeds[i]*1.4)
    def update(self,dt,t):
        for i in range(self.NCOLS):
            self.drops[i]+=self.speeds[i]*dt*28
            if self.drops[i]>self.nr: self.drops[i]=random.randint(-self.nr//2,0); self.speeds[i]=random.uniform(0.25,1.1)
        for c in list(self.flashes): self.flashes[c]-=dt*2.5; (self.flashes.pop(c) if self.flashes[c]<=0 else None)
    def draw(self,surf):
        surf.fill((0,0,0))
        for col in range(self.NCOLS):
            drop=int(self.drops[col]); x=col*self.cw; fl=self.flashes.get(col,0)
            for row in range(self.nr):
                dist=drop-row
                if dist<0 or dist>22: continue
                br=max(0.0,1.0-dist/22.0)
                if dist==0: cc=(220,255,220)
                elif fl>0: v=max(0,min(255,int(br*255*fl))); cc=(v,v,v)
                else: cc=hsv(self.hue,0.8,br*0.9)
                cs=random.choice(self.surfs)
                colored=pygame.Surface(cs.get_size()); colored.fill(cc)
                colored.blit(cs,(0,0),special_flags=pygame.BLEND_RGBA_MULT)
                surf.blit(colored,(x+(self.cw-cs.get_width())//2,row*self.CH))


# ── 9. RADAR ──────────────────────────────────────────────────────────────────
class Radar(Anim):
    name="RADAR"; R=min(CX,CY)-30
    def __init__(self): self.angle=0.0; self.blips=[]; self.beat=0.0
    def on_note_on(self,note,vel,ch):
        # Distribute notes across full circle (not just one quadrant)
        a=(note/127.0)*TAU
        r=self.R*(0.25+0.75*vel/127.0)
        self.blips.append({'a':a,'r':r,'life':1.0,'col':hsv(note_hue(note,ch)),'sz':max(3,vel//15)})
    def on_beat(self): self.beat=1.0
    def update(self,dt,t):
        self.angle=(self.angle+dt*self.speed*1.4)%TAU
        for b in self.blips: b['life']=max(0.0,1.0-((self.angle-b['a'])%TAU)/TAU)
        self.blips=[b for b in self.blips if b['life']>0.02]
        self.beat=max(0,self.beat-dt*2.5)
    def draw(self,surf):
        surf.fill((0,0,0))
        # tunnel rings expanding from center
        t_now=time.time()*self.speed*0.45
        for ti in range(7):
            phase=((ti/7)+t_now)%1.0
            r_draw=int(self.R*phase)
            if r_draw<2: continue
            br=int(max(0,(1.0-max(0,(phase-0.72)/0.28))*60))
            if br>0: pygame.draw.circle(surf,(0,br,int(br*0.3)),(CX,CY),r_draw,1)
        for i in range(1,5): pygame.draw.circle(surf,(0,40+i*8,0),(CX,CY),self.R*i//4,1)
        pygame.draw.line(surf,(0,28,0),(CX-self.R,CY),(CX+self.R,CY),1)
        pygame.draw.line(surf,(0,28,0),(CX,CY-self.R),(CX,CY+self.R),1)
        N_ARMS=3
        for arm_k in range(N_ARMS):
            arm_a=self.angle-arm_k*(TAU/3); arm_fade=1.0-arm_k*0.35
            for k in range(45):
                a=arm_a-k/45*(TAU/4); al=int((1-k/45)*55*arm_fade)
                pygame.draw.line(surf,(0,al,int(al*0.25)),(CX,CY),(int(CX+math.cos(a)*self.R),int(CY+math.sin(a)*self.R)),2)
            pygame.draw.line(surf,(0,int(255*arm_fade),int(80*arm_fade)),(CX,CY),(int(CX+math.cos(arm_a)*self.R),int(CY+math.sin(arm_a)*self.R)),2)
        for b in self.blips:
            x,y=int(CX+math.cos(b['a'])*b['r']),int(CY+math.sin(b['a'])*b['r'])
            col=tuple(max(0,min(255,int(c*b['life']))) for c in b['col'])
            pygame.draw.circle(surf,col,(x,y),b['sz'])
            if b['life']>0.3: pygame.draw.circle(surf,col,(x,y),b['sz']+4,1)
        if self.beat>0.05:
            s=pygame.Surface((W,H),pygame.SRCALPHA); s.fill((0,255,80,int(self.beat*35))); surf.blit(s,(0,0))
        pygame.draw.circle(surf,(0,200,60),(CX,CY),5)


# ── 10. MANDALA ───────────────────────────────────────────────────────────────
class Mandala(Anim):
    name="MANDALA"; NSYM=10
    def __init__(self):
        self.angle=0.0; self.hue=0.0; self.bs=1.0; self.arms=[]
        R=min(CX,CY)-30
        for _ in range(5):
            self.arms.append({'r1':random.randint(70,int(R*0.65)),'r2':random.randint(15,75),
                's1':random.uniform(0.4,1.8)*random.choice([1,-1]),
                's2':random.uniform(0.9,3.5)*random.choice([1,-1]),
                'p1':random.uniform(0,TAU),'p2':random.uniform(0,TAU),
                'h':random.random(),'sz':random.randint(1,3)})
    def on_note_on(self,note,vel,ch):
        self.hue=note_hue(note,ch)
        for arm in self.arms:
            arm['h']=(note_hue(note,ch)+random.uniform(-0.18,0.18))%1.0
            arm['s2']+=vel/127.0*2.0*random.choice([1,-1])
    def on_beat(self): self.bs=1.4
    def update(self,dt,t):
        self.angle=(self.angle+dt*self.speed*0.38)%TAU; self.hue=(self.hue+dt*0.018)%1.0
        self.bs=lerp(self.bs,1.0,dt*3.5)
        for arm in self.arms: arm['p1']+=arm['s1']*dt; arm['p2']+=arm['s2']*dt
    def draw(self,surf):
        surf.fill((0,0,8)); R=(min(CX,CY)-20)*self.bs
        for k in range(self.NSYM*2):
            a=k/(self.NSYM*2)*TAU+self.angle
            pygame.draw.line(surf,hsv((self.hue+k/(self.NSYM*2)*0.35)%1.0,0.7,0.35),
                             (CX,CY),(int(CX+math.cos(a)*R),int(CY+math.sin(a)*R)),1)
        # tunnel zoom: 5 copies of each arm spiraling from center to edge
        t_now=time.time()*self.speed*0.15
        N_RINGS=5
        phases=[((i/N_RINGS)+t_now)%1.0 for i in range(N_RINGS)]
        order=sorted(range(N_RINGS),key=lambda i:-phases[i])
        for i in order:
            phase=phases[i]
            rscale=0.18*(5.5**phase)
            alpha=min(1.0,phase/0.10)*max(0.0,1.0-(phase-0.78)/0.22)*0.90
            if alpha<=0: continue
            lag=phase*0.85; N=100
            for arm in self.arms:
                pts_b=[]
                for j in range(N):
                    p=j/N*TAU; r1=arm['r1']*self.bs*rscale; r2=arm['r2']*rscale
                    pts_b.append((r1*math.cos(arm['s1']*p+arm['p1']-lag)+r2*math.cos(arm['s2']*p+arm['p2']-lag),
                                   r1*math.sin(arm['s1']*p+arm['p1']-lag)+r2*math.sin(arm['s2']*p+arm['p2']-lag)))
                for sym in range(self.NSYM):
                    ao=sym*TAU/self.NSYM+self.angle-lag*0.5; ca,sa=math.cos(ao),math.sin(ao)
                    rot=[(int(CX+dx*ca-dy*sa),int(CY+dx*sa+dy*ca)) for dx,dy in pts_b]
                    if len(rot)>1: pygame.draw.lines(surf,hsv((arm['h']+sym/self.NSYM*0.28+phase*0.2)%1.0,0.95,alpha),True,rot,arm['sz'])


# ═══════════════════════════ MAIN APP ═════════════════════════════════════════

ANIM_CLASSES = [Starfield,Lissajous,Plasma,Tunnel,WaveGrid,Ribbons,Kaleidoscope,MatrixRain,Radar,Mandala]

# GM note → (pad, name) pour affichage
GM_LABEL = {36:"Kick",38:"Snare",42:"CHH",46:"OHH",39:"Clap",41:"FTom",43:"MTom",49:"Crash"}
CH_LABEL = {1:"SYNTH",2:"BASS",10:"DRUM"}


class SynthViz:
    def __init__(self, port_name):
        pygame.init()
        self.screen  = pygame.display.set_mode((W,H), pygame.SCALED | pygame.RESIZABLE)
        pygame.display.set_caption("SynthViz — GrvEP")
        self.clock   = pygame.time.Clock()
        self.font_m  = pygame.font.SysFont("monospace",14,bold=True)
        self.font_s  = pygame.font.SysFont("monospace",11)

        self.anims    = [A() for A in ANIM_CLASSES]
        self.anim_idx = 0
        self.speed    = 1.0
        self._fullscreen = False

        self.connected   = False
        self.port_name   = port_name
        self.active_notes= {}   # note → (vel, ch)
        self.beat_pulse  = 0.0
        self.last_event  = "---"
        self.bpm         = 120
        self._beat_times = []   # timestamps of kicks for BPM estimation
        self._lock       = threading.Lock()
        self._note_q     = []
        self._drum_q     = []
        self._beat_q     = []

        threading.Thread(target=self._midi_thread, daemon=True).start()

    # ── MIDI thread ───────────────────────────────────────────────────────────
    def _midi_thread(self):
        while True:
            try:
                with mido.open_input(self.port_name, callback=self._on_midi) as port:
                    self.connected = True
                    print(f"Connecté : {port.name}")
                    while self.connected:
                        time.sleep(0.5)
            except Exception as e:
                self.connected = False
                time.sleep(2.0)

    def _on_midi(self, msg):
        ch = msg.channel + 1  # mido est 0-indexed, on affiche 1-indexed
        if msg.type == 'note_on' and msg.velocity > 0:
            with self._lock:
                self.active_notes[msg.note] = (msg.velocity, ch)
                self._note_q.append(('on', msg.note, msg.velocity, ch))
            if ch == 10:
                pad = GM_TO_PAD.get(msg.note, 0)
                with self._lock: self._drum_q.append((pad, msg.velocity))
                if msg.note == 36:  # kick = beat
                    now = time.time()
                    self._beat_times.append(now)
                    self._beat_times = self._beat_times[-8:]
                    if len(self._beat_times) >= 2:
                        intervals = [self._beat_times[i+1]-self._beat_times[i]
                                     for i in range(len(self._beat_times)-1)]
                        avg = sum(intervals)/len(intervals)
                        if 0.2 < avg < 3.0:
                            self.bpm = max(40,min(300,int(60/avg)))
                with self._lock: self._beat_q.append(1)
        elif msg.type in ('note_off', 'note_on') and msg.velocity == 0:
            with self._lock:
                self.active_notes.pop(msg.note, None)
                self._note_q.append(('off', msg.note, 0, ch))

    def _flush(self):
        with self._lock:
            nq=self._note_q[:]; dq=self._drum_q[:]; bq=self._beat_q[:]
            self._note_q.clear(); self._drum_q.clear(); self._beat_q.clear()
        anim=self.anims[self.anim_idx]
        for ev in nq:
            if ev[0]=='on':
                anim.on_note_on(ev[1],ev[2],ev[3])
                self.last_event=f"Note {ev[1]} vel {ev[2]} ch{ev[3]}"
            else: anim.on_note_off(ev[1],ev[3])
        for pad,vel in dq:
            anim.on_drum(pad,vel)
            self.beat_pulse=max(self.beat_pulse,0.55)
            self.last_event=f"Drum {PAD_NAMES[pad]} vel {vel}"
        for _ in bq:
            anim.on_beat(); self.beat_pulse=1.0

    # ── HUD ───────────────────────────────────────────────────────────────────
    def _hud(self):
        bar_h=28
        pygame.draw.rect(self.screen,(8,8,16),(0,H-bar_h,W,bar_h))
        pygame.draw.line(self.screen,(40,40,65),(0,H-bar_h),(W,H-bar_h),1)
        tab_w=W//len(ANIM_CLASSES)
        for i,anim in enumerate(self.anims):
            active=(i==self.anim_idx)
            pygame.draw.rect(self.screen,(25,55,100) if active else (12,12,22),(i*tab_w,H-bar_h,tab_w-1,bar_h))
            num=self.font_s.render(str((i+1)%10),True,(100,160,255) if active else (35,45,70))
            self.screen.blit(num,(i*tab_w+3,H-bar_h+2))
            lbl=self.font_s.render(anim.name,True,(190,220,255) if active else (55,65,95))
            self.screen.blit(lbl,(i*tab_w+(tab_w-lbl.get_width())//2,H-bar_h+(bar_h-lbl.get_height())//2))
        dot=(0,210,70) if self.connected else (110,35,35)
        pygame.draw.circle(self.screen,dot,(W-12,13),6)
        info=self.font_m.render(f"{self.bpm}bpm  {'MIDI OK' if self.connected else 'no MIDI'}  {self.last_event}",
                                True,(140,180,140))
        self.screen.blit(info,(W-info.get_width()-24,5))
        hint=self.font_s.render("1-0/←→ anim   ↑↓ vitesse   Espace note   B beat   F11 plein écran   Esc quitter",True,(38,42,58))
        self.screen.blit(hint,(8,5))

    # ── Main loop ─────────────────────────────────────────────────────────────
    def run(self):
        running=True; prev=time.time()
        while running:
            now=time.time(); dt=min(now-prev,0.05); prev=now
            for ev in pygame.event.get():
                if ev.type==pygame.QUIT: running=False
                elif ev.type==pygame.KEYDOWN:
                    k=ev.key
                    if k==pygame.K_ESCAPE: running=False
                    elif k==pygame.K_LEFT:  self.anim_idx=(self.anim_idx-1)%len(ANIM_CLASSES)
                    elif k==pygame.K_RIGHT: self.anim_idx=(self.anim_idx+1)%len(ANIM_CLASSES)
                    elif k==pygame.K_UP:   self.speed=min(6.0,self.speed+0.25)
                    elif k==pygame.K_DOWN: self.speed=max(0.15,self.speed-0.25)
                    elif pygame.K_1<=k<=pygame.K_9: self.anim_idx=k-pygame.K_1
                    elif k==pygame.K_0: self.anim_idx=9
                    elif k==pygame.K_SPACE:
                        note=random.randint(48,84); vel=random.randint(60,127)
                        with self._lock: self._note_q.append(('on',note,vel,1))
                    elif k==pygame.K_b:
                        with self._lock: self._beat_q.append(1)
                    elif k==pygame.K_F11:
                        self._fullscreen=not self._fullscreen
                        pygame.display.toggle_fullscreen()

            self._flush()
            self.anims[self.anim_idx].speed=self.speed
            self.anims[self.anim_idx].update(dt,now)
            self.anims[self.anim_idx].draw(self.screen)

            self.beat_pulse=max(0.0,self.beat_pulse-dt*4.5)
            if self.beat_pulse>0.05:
                s=pygame.Surface((W,H),pygame.SRCALPHA)
                s.fill((255,255,255,int(self.beat_pulse*22))); self.screen.blit(s,(0,0))

            self._hud()
            pygame.display.flip()
            self.clock.tick(FPS)
        pygame.quit()


# ── Détection port MIDI ────────────────────────────────────────────────────────

def find_port(hint=""):
    ports = mido.get_input_names()
    if not ports:
        print("Aucun port MIDI trouvé. Le GrvEP est-il branché et flashé ?")
        return None
    if hint:
        for p in ports:
            if hint.lower() in p.lower():
                return p
    # cherche "GrvEP" ou "MIDI" dans le nom
    for p in ports:
        if "grvep" in p.lower() or "synthbox" in p.lower():
            return p
    print("Ports MIDI disponibles :")
    for i,p in enumerate(ports): print(f"  {i}: {p}")
    print(f"→ Utilise : {ports[0]}")
    return ports[0]


if __name__ == "__main__":
    hint = sys.argv[1] if len(sys.argv) > 1 else ""
    port = find_port(hint)
    if port is None:
        sys.exit(1)
    print(f"SynthViz — port MIDI : {port}")
    print("1-0/←→ : animation   ↑↓ : vitesse   Espace : note test   B : beat   Esc : quitter")
    SynthViz(port).run()
