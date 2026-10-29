// WiFi Access Point + WebSocket audio receiver + DSP chain → AMY PCM streaming.
// See wifi_audio.h for the full architecture description.

#include "wifi_audio.h"
#include "config.h"
#include <WiFi.h>
#include <mbedtls/sha1.h>
#include <mbedtls/base64.h>
#include <AMY-Arduino.h>

extern "C" {
void pcm_register_extern16(uint16_t preset_number, const int16_t* data,
                            uint32_t length, uint32_t samplerate,
                            uint8_t midinote, uint32_t loopstart, uint32_t loopend);
void pcm_unload_preset(uint16_t preset_number);
}

// ==================== CONSTANTS ====================
// OSC 174 and presets 242/243 sit just past the granular range (OSCs 142-173, presets 232-256).
// Keep in sync with config.h GRANULAR_ defines.
#define WIFI_OSC          174
#define WIFI_PRESET_A     257   // beyond granular (granular uses 232-256)
#define WIFI_PRESET_B     258
#define WIFI_CHUNK_SAMP  4000   // 250 ms at 16 kHz per chunk
#define WIFI_AMY_RATE    8000   // AMY 2× hardware bug: tell AMY 8000 → plays at 16 kHz
#define WIFI_RING_SIZE   32768  // 2 s jitter ring buffer
#define WIFI_DELAY_SIZE  16000  // 1 s delay line

static const char* WIFI_SSID = "GrvEP";
static const char* WIFI_PASS = nullptr;  // open network — no password

// ==================== HTML PAGE ====================
// Served once; phone browser captures mic and streams PCM via WebSocket.
static const char HTML_PAGE[] PROGMEM = R"rawhtml(<!DOCTYPE html>
<html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>GrvEP Audio</title>
<style>body{background:#111;color:#eee;font-family:sans-serif;padding:20px;max-width:480px}
h2{color:#0af}button{background:#0af;color:#000;border:none;padding:12px 24px;
font-size:16px;border-radius:6px;cursor:pointer;width:100%;margin:8px 0}
#st{padding:10px;background:#222;border-radius:4px;margin:8px 0;font-size:14px}
</style></head><body>
<h2>GrvEP Audio Input</h2>
<div id="st">Idle — press Start to stream mic audio</div>
<button id="btn">▶ Start Streaming</button>
<p style="font-size:12px;color:#888">Connecte le micro du téléphone à la chaîne d'effets GrvEP.<br>
Fonctionne sur Chrome/Safari. Garde l'écran allumé.</p>
<script>
var ws,ctx,proc,src,active=false;
document.getElementById('btn').onclick=async function(){
  if(active){
    if(proc)proc.disconnect();if(src)src.disconnect();
    if(ctx)ctx.close();if(ws)ws.close();
    active=false;document.getElementById('btn').textContent='▶ Start Streaming';
    document.getElementById('st').textContent='Stopped';return;
  }
  try{
    document.getElementById('st').textContent='Requesting mic…';
    var stream=await navigator.mediaDevices.getUserMedia(
      {audio:{sampleRate:16000,channelCount:1,echoCancellation:false,noiseSuppression:false}});
    ctx=new (window.AudioContext||window.webkitAudioContext)({sampleRate:16000});
    ws=new WebSocket('ws://'+location.host+'/audio');
    ws.binaryType='arraybuffer';
    ws.onopen=function(){document.getElementById('st').textContent='Streaming to GrvEP ●';};
    ws.onclose=function(){document.getElementById('st').textContent='Disconnected';active=false;
      document.getElementById('btn').textContent='▶ Start Streaming';};
    ws.onerror=function(){document.getElementById('st').textContent='Connection error';};
    src=ctx.createMediaStreamSource(stream);
    proc=ctx.createScriptProcessor(1024,1,1);
    proc.onaudioprocess=function(e){
      if(!ws||ws.readyState!==1)return;
      var f32=e.inputBuffer.getChannelData(0);
      var i16=new Int16Array(f32.length);
      for(var i=0;i<f32.length;i++)
        i16[i]=Math.max(-32768,Math.min(32767,Math.round(f32[i]*32767)));
      ws.send(i16.buffer);
    };
    src.connect(proc);proc.connect(ctx.destination);
    active=true;document.getElementById('btn').textContent='■ Stop';
  }catch(e){document.getElementById('st').textContent='Error: '+e.message;}
};
</script></body></html>)rawhtml";

// ==================== RING BUFFER ====================
static int16_t* s_ring     = nullptr;
static volatile uint32_t s_rHead = 0, s_rTail = 0;  // head=write, tail=read

static uint32_t ringUsed() {
    uint32_t h = s_rHead, t = s_rTail;
    return (h >= t) ? (h - t) : (WIFI_RING_SIZE - t + h);
}
static uint32_t ringFree() { return WIFI_RING_SIZE - 1 - ringUsed(); }

static void ringWrite(const int16_t* src, uint32_t n) {
    if (n > ringFree()) n = ringFree();
    for (uint32_t i = 0; i < n; i++) {
        s_ring[s_rHead] = src[i];
        s_rHead = (s_rHead + 1) & (WIFI_RING_SIZE - 1);
    }
}
static uint32_t ringRead(int16_t* dst, uint32_t n) {
    uint32_t avail = ringUsed();
    if (n > avail) { memset(dst, 0, n * 2); return 0; }  // underrun → silence
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = s_ring[s_rTail];
        s_rTail = (s_rTail + 1) & (WIFI_RING_SIZE - 1);
    }
    return n;
}

// ==================== DSP STATE ====================
struct Biquad { float z1=0, z2=0, b0=1, b1=0, b2=0, a1=0, a2=0; };
static Biquad s_lpf;
static float  s_drive     = 0.0f;
static float  s_delayLvl  = 0.0f;
static int    s_delayTime = 0;   // samples (0-16000)
static float  s_vol       = 1.0f;
static int16_t* s_delayBuf = nullptr;
static int    s_delayWr   = 0;
static int    s_peakLevel = 0;

static void biquadSetLPF(Biquad& f, float fc, float q, float fs) {
    if (fc <= 0) { f.b0=0; f.b1=0; f.b2=0; f.a1=0; f.a2=0; return; }
    float w0 = 2.0f * M_PI * fc / fs;
    float cs = cosf(w0), sn = sinf(w0);
    float alpha = sn / (2.0f * q);
    float a0inv = 1.0f / (1.0f + alpha);
    f.b0 = (1.0f - cs) * 0.5f * a0inv;
    f.b1 = (1.0f - cs) * a0inv;
    f.b2 = f.b0;
    f.a1 = -2.0f * cs * a0inv;
    f.a2 = (1.0f - alpha) * a0inv;
}
static float biquadProcess(Biquad& f, float x) {
    float y = f.b0 * x + f.z1;
    f.z1 = f.b1 * x - f.a1 * y + f.z2;
    f.z2 = f.b2 * x - f.a2 * y;
    return y;
}
static float softClip(float x, float drive) {
    x *= (1.0f + drive * 6.0f);
    if (x > 1.0f) return 1.0f;
    if (x < -1.0f) return -1.0f;
    return x * (1.5f - 0.5f * x * x);
}

static void applyDSP(int16_t* buf, uint32_t n) {
    int peak = 0;
    for (uint32_t i = 0; i < n; i++) {
        float x = buf[i] / 32768.0f;
        // LPF
        if (s_lpf.b0 > 0.0f || s_lpf.b1 > 0.0f) x = biquadProcess(s_lpf, x);
        // Drive
        if (s_drive > 0.01f) x = softClip(x, s_drive);
        // Delay
        if (s_delayLvl > 0.01f && s_delayTime > 0) {
            int rp = (s_delayWr - s_delayTime + WIFI_DELAY_SIZE) % WIFI_DELAY_SIZE;
            float echo = s_delayBuf[rp] / 32768.0f;
            s_delayBuf[s_delayWr] = (int16_t)constrain((int)(x * 32767), -32768, 32767);
            s_delayWr = (s_delayWr + 1) % WIFI_DELAY_SIZE;
            x += echo * s_delayLvl;
        }
        // Volume
        x *= s_vol;
        // Clamp
        int samp = (int)(x * 32767.0f);
        if (samp >  32767) samp =  32767;
        if (samp < -32768) samp = -32768;
        buf[i] = (int16_t)samp;
        int abv = samp < 0 ? -samp : samp;
        if (abv > peak) peak = abv;
    }
    s_peakLevel = (peak * 127) / 32767;
}

// ==================== WEBSOCKET HANDSHAKE ====================
static String wsAcceptKey(const String& key) {
    String cat = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t sha[20];
    mbedtls_sha1((const uint8_t*)cat.c_str(), cat.length(), sha);
    uint8_t b64[32]; size_t olen;
    mbedtls_base64_encode(b64, sizeof(b64), &olen, sha, 20);
    b64[olen] = 0;
    return String((char*)b64);
}

// Read one WebSocket frame from client (blocking with timeout).
// Returns payload byte count, 0 for close frame, -1 on error/timeout.
static int wsReadFrame(WiFiClient& c, uint8_t* dst, int maxLen) {
    unsigned long t0 = millis();
    while (c.available() < 2) {
        if (!c.connected() || millis()-t0 > 200) return -1;
        vTaskDelay(1);
    }
    uint8_t h0 = c.read(), h1 = c.read();
    if ((h0 & 0x0F) == 8) return 0;  // close
    if ((h0 & 0x0F) == 9) { /* ping — ignore */ return -1; }
    bool masked = (h1 & 0x80) != 0;
    int len = h1 & 0x7F;
    if (len == 126) {
        while (c.available() < 2) { if (!c.connected()) return -1; vTaskDelay(1); }
        len = ((int)c.read()<<8) | c.read();
    } else if (len == 127) return -1;
    uint8_t mkey[4] = {};
    if (masked) {
        while (c.available() < 4) { if (!c.connected()) return -1; vTaskDelay(1); }
        for (int i=0;i<4;i++) mkey[i]=c.read();
    }
    if (len > maxLen) { /* flush */ while (c.available()) c.read(); return -1; }
    int got=0; t0=millis();
    while (got < len) {
        if (!c.connected() || millis()-t0 > 2000) return -1;
        if (c.available()) dst[got++] = c.read();
        else vTaskDelay(1);
    }
    if (masked) for (int i=0;i<got;i++) dst[i] ^= mkey[i%4];
    return got;
}

// ==================== AMY AUDIO BUFFERS ====================
static int16_t* s_bufA     = nullptr;
static int16_t* s_bufB     = nullptr;
static bool     s_useA     = true;
static volatile bool s_netRunning  = false;  // AP + HTTP server + wifiNetTask (always after init)
static volatile bool s_wifiRunning = false;  // audio output task (MODE_WIFI only)
static volatile bool s_clientConn  = false;

// ==================== TASKS ====================
static WiFiServer  s_httpServer(80);
static WiFiClient  s_wsClient;

// HTTP/WebSocket accept task (Core 1) — runs as long as s_netRunning
static void wifiNetTask(void*) {
    static uint8_t frameBuf[4096];
    while (s_netRunning) {
        // Accept new client
        if (!s_clientConn) {
            WiFiClient client = s_httpServer.accept();
            if (client) {
                // Read request line
                String reqLine = client.readStringUntil('\n'); reqLine.trim();
                // Read headers
                String wsKey = "";
                bool isWs = false;
                unsigned long t0 = millis();
                while (client.connected() && millis()-t0 < 2000) {
                    String hdr = client.readStringUntil('\n'); hdr.trim();
                    if (hdr.length() == 0) break;
                    String lo = hdr; lo.toLowerCase();
                    if (lo.startsWith("upgrade:") && lo.indexOf("websocket") >= 0) isWs = true;
                    if (lo.startsWith("sec-websocket-key:"))
                        { wsKey = hdr.substring(19); wsKey.trim(); }
                }
                if (isWs && wsKey.length() > 0) {
                    // WebSocket upgrade
                    String accept = wsAcceptKey(wsKey);
                    client.print("HTTP/1.1 101 Switching Protocols\r\n"
                                 "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                                 "Sec-WebSocket-Accept: ");
                    client.print(accept);
                    client.print("\r\n\r\n");
                    s_wsClient = client;
                    s_clientConn = true;
                    // Reset ring on new connection
                    s_rHead = s_rTail = 0;
                    memset(s_bufA, 0, WIFI_CHUNK_SAMP * 2);
                    memset(s_bufB, 0, WIFI_CHUNK_SAMP * 2);
                } else {
                    // Serve HTML page
                    client.print("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n");
                    client.print(FPSTR(HTML_PAGE));
                    client.stop();
                }
            }
        } else {
            // Feed ring buffer from WebSocket frames
            if (!s_wsClient.connected()) {
                s_wsClient.stop(); s_clientConn = false;
            } else {
                int n = wsReadFrame(s_wsClient, frameBuf, sizeof(frameBuf));
                if (n > 1) {
                    ringWrite((int16_t*)frameBuf, n / 2);
                } else if (n == 0) {
                    s_wsClient.stop(); s_clientConn = false;
                }
            }
        }
        vTaskDelay(1);
    }
    s_wsClient.stop(); s_clientConn = false;
    vTaskDelete(NULL);
}

// Audio output task: drains ring → DSP → AMY PCM chunk (Core 1)
static void wifiAudioOutputTask(void*) {
    // Wait for ring to buffer at least one chunk before starting playback
    while (s_wifiRunning && ringUsed() < WIFI_CHUNK_SAMP) vTaskDelay(5);

    while (s_wifiRunning) {
        int16_t* buf    = s_useA ? s_bufA : s_bufB;
        uint16_t preset = s_useA ? WIFI_PRESET_A : WIFI_PRESET_B;

        // Drain ring into chunk (fill with silence if underrun)
        ringRead(buf, WIFI_CHUNK_SAMP);
        applyDSP(buf, WIFI_CHUNK_SAMP);

        // Trigger AMY — preset was registered at start, buffer contents updated above
        amy_event e = amy_default_event();
        e.osc      = WIFI_OSC;
        e.wave     = PCM;
        e.preset   = preset;
        e.midi_note = 69;
        e.velocity  = 0.8f;
        e.feedback  = 0.0f;  // one-shot
        amy_add_event(&e);

        s_useA = !s_useA;
        // Wait for the chunk to finish playing (~250 ms) before filling next
        vTaskDelay(pdMS_TO_TICKS(WIFI_CHUNK_SAMP * 1000 / 16000 - 20));
    }

    // Silence OSC on exit
    amy_event e = amy_default_event();
    e.osc = WIFI_OSC; e.velocity = 0;
    amy_add_event(&e);

    vTaskDelete(NULL);
}

// ==================== PUBLIC API ====================

// Called once in setup() — starts AP immediately so the network is visible from boot.
void wifiAudioInit() {
    s_ring     = (int16_t*)ps_malloc(WIFI_RING_SIZE  * sizeof(int16_t));
    s_delayBuf = (int16_t*)ps_malloc(WIFI_DELAY_SIZE * sizeof(int16_t));
    s_bufA     = (int16_t*)ps_malloc(WIFI_CHUNK_SAMP * sizeof(int16_t));
    s_bufB     = (int16_t*)ps_malloc(WIFI_CHUNK_SAMP * sizeof(int16_t));
    if (!s_ring || !s_delayBuf || !s_bufA || !s_bufB)
        Serial.println("[WIFI] PSRAM alloc failed");

    // STA→OFF→AP forces full WiFi stack re-init on ESP32-S3 (avoids init-order failures with TinyUSB)
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    delay(100);
    WiFi.mode(WIFI_OFF);
    delay(200);
    WiFi.mode(WIFI_AP);
    delay(300);
    IPAddress apIP(192, 168, 4, 1), nm(255, 255, 255, 0);
    WiFi.softAPConfig(apIP, apIP, nm);
    // Try channels 6, 1, 11 — ch6 is mid-band, often less crowded
    bool ok = false;
    static const uint8_t kChannels[] = {6, 1, 11};
    for (int ci = 0; ci < 3 && !ok; ci++) {
        ok = WiFi.softAP(WIFI_SSID, WIFI_PASS, kChannels[ci], false, 4);
        if (!ok) { delay(200); }
        else { Serial.printf("[WIFI] AP started on ch%d\n", kChannels[ci]); }
    }
    delay(200);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    Serial.printf("[WIFI] AP %s  SSID:'%s'  IP:%s\n",
                  ok ? "OK" : "FAIL",
                  WIFI_SSID, WiFi.softAPIP().toString().c_str());

    s_httpServer.begin();
    s_netRunning = true;
    xTaskCreatePinnedToCore(wifiNetTask, "wifiNet", 8192, NULL, 1, NULL, 1);
}

// Called on MODE_WIFI entry — starts audio output only (AP already running).
void wifiAudioStart() {
    if (s_wifiRunning) return;
    memset(s_ring,     0, WIFI_RING_SIZE  * sizeof(int16_t));
    memset(s_delayBuf, 0, WIFI_DELAY_SIZE * sizeof(int16_t));
    memset(s_bufA,     0, WIFI_CHUNK_SAMP * sizeof(int16_t));
    memset(s_bufB,     0, WIFI_CHUNK_SAMP * sizeof(int16_t));
    s_rHead = s_rTail = 0; s_delayWr = 0; s_useA = true;
    pcm_register_extern16(WIFI_PRESET_A, s_bufA, WIFI_CHUNK_SAMP, WIFI_AMY_RATE, 69, 0, 0);
    pcm_register_extern16(WIFI_PRESET_B, s_bufB, WIFI_CHUNK_SAMP, WIFI_AMY_RATE, 69, 0, 0);
    s_wifiRunning = true;
    xTaskCreatePinnedToCore(wifiAudioOutputTask, "wifiOut", 4096, NULL, 2, NULL, 1);
}

// Called on MODE_WIFI exit — stops audio output; AP and net task keep running.
void wifiAudioStop() {
    s_wifiRunning = false;
    vTaskDelay(pdMS_TO_TICKS(300));
    // Disconnect any active WebSocket client so the ring resets cleanly
    if (s_clientConn) { s_wsClient.stop(); s_clientConn = false; }
    s_rHead = s_rTail = 0;
    pcm_unload_preset(WIFI_PRESET_A);
    pcm_unload_preset(WIFI_PRESET_B);
    Serial.println("[WIFI] audio stopped (AP still running)");
}

void wifiAudioTick() { /* tasks handle all networking */ }

bool     wifiAudioClientConnected() { return s_clientConn; }
uint32_t wifiAudioRingUsed()        { return ringUsed(); }
int      wifiAudioGetLevel()        { return s_peakLevel; }
int      wifiAudioGetStationNum()   { return (int)WiFi.softAPgetStationNum(); }

void wifiAudioSetVolume(float v)                  { s_vol = constrain(v, 0.0f, 2.0f); }
void wifiAudioSetDrive(float d)                   { s_drive = constrain(d, 0.0f, 1.0f); }
void wifiAudioSetDelay(float lvl, float timeFrac) {
    s_delayLvl  = constrain(lvl, 0.0f, 1.0f);
    s_delayTime = (int)(timeFrac * WIFI_DELAY_SIZE);
}
void wifiAudioSetLPF(float cutNorm, float reso) {
    float fc = cutNorm > 0.01f ? 200.0f * powf(40.0f, cutNorm) : 0.0f;  // 200–8000 Hz exp
    float q  = 0.5f + reso * 3.5f;  // 0.5–4.0
    biquadSetLPF(s_lpf, fc, q, 16000.0f);
    if (cutNorm < 0.01f) s_lpf = {};  // fully open = bypass
}
