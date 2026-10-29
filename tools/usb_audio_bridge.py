#!/usr/bin/env python3
"""GrvEP-fusion USB audio bridge (host side).

Not a real USB Audio Class device — the ESP32-S3 mirrors its own final mixed audio
out over the existing native-USB serial/CDC connection (the same link already used
for debug logging and MIDI), framed with a 4-byte sync marker ("GRV2"). This script
opens that serial port, starts the stream, and plays the incoming PCM back through
the host's own sound output via `sounddevice` (PortAudio) — so you can hear (or
record) what the GrvEP is playing without a physical audio jack, while developing or
testing away from the real DAC/speaker.

Firmware side: src/audio_engine.cpp's usbAudioBridgeTick(), called once per
AMY_BLOCK_SIZE=128-frame block from lib/AMY Synthesizer/src/i2s.c's own real-time
audio task (esp32-s3-devkitc-1 build only — this protocol does nothing on the
simulator/Android/Web targets).

GRV1 -> GRV2: the original protocol mirrored full-rate 44.1kHz/16-bit stereo PCM, but
measured hardware testing showed this link only sustains ~20 blocks/sec against the
~345/sec real-time needs (TinyUSB's own background CDC task appears starved by the
always-on, high-priority real-time audio task — not fixable from the host side, and
not the serial baud rate: raising it made things worse). Result: ~94% of blocks were
silently dropped, which is exactly what produced "bruit horrible" when playing notes.
Fix: the firmware now mirrors a deliberately-downsampled signal that actually fits in
the achievable bandwidth — mono, 8-bit, decimated to 7350Hz (44100/6) with an
anti-alias low-pass filter — telephone-ish quality but continuous, not glitchy.

Protocol: host sends a single 'A' byte to start the stream, 'S' to stop it. Once
started, the device sends repeating units of: 4-byte sync marker b"GRV2", followed by
exactly PAYLOAD_BYTES bytes of signed 8-bit mono PCM at SAMPLE_RATE. Debug text the
firmware also prints to the same serial port is harmless noise here — it's simply
skipped over while scanning for the next sync marker, exactly like a reference
project this was adapted from.

Virtual microphone (Linux/PulseAudio or PipeWire's pulse-compat layer, via `pactl`):
on start, this script creates a null-sink ("GrvEP_Synth_Sink") and remaps its monitor
as a proper source named "GrvEP_Synth_Mic" — this shows up as a normal selectable
input/microphone device in Audacity, Discord, etc. (not filed under a "Monitor of..."
entry, which some apps hide by default). The incoming audio is written to BOTH that
virtual sink (so other apps can pick it up as a mic) and your real default output (so
you still hear it live). Both PulseAudio modules are unloaded on exit, so nothing is
left behind between runs.

Usage:
    python3 usb_audio_bridge.py [SERIAL_PORT]

If SERIAL_PORT is omitted, defaults to /dev/ttyACM0 (Linux). On Windows, pass
something like "COM4"; on macOS, something like "/dev/cu.usbmodemXXXX" — the virtual
mic step is Linux/pactl-only and is skipped automatically on other platforms (you'll
still hear it through your normal output).

Dependencies: pip install pyserial sounddevice
"""
import serial
import sounddevice as sd
import subprocess
import sys
import time

SAMPLE_RATE = 44100 // 6   # 7350 Hz — see kDecimFactor in src/audio_engine.cpp
CHANNELS = 1
PAYLOAD_BYTES = 64   # kPayloadBytes in src/audio_engine.cpp
SYNC_MARKER = b"GRV2"
DEFAULT_PORT = "/dev/ttyACM0"
VIRTUAL_SINK_NAME = "GrvEP_Synth_Sink"
VIRTUAL_MIC_NAME = "GrvEP_Synth_Mic"


def create_virtual_mic():
    """Best-effort: create a PulseAudio/PipeWire null-sink + remapped source so the
    bridged audio shows up as a selectable microphone elsewhere. Returns the list of
    loaded module ids to unload on exit, or [] if pactl isn't available/fails (the
    script still works, just without the virtual-mic step)."""
    module_ids = []
    try:
        r = subprocess.run(
            ["pactl", "load-module", "module-null-sink",
             f"sink_name={VIRTUAL_SINK_NAME}",
             f"sink_properties=device.description={VIRTUAL_SINK_NAME}"],
            capture_output=True, text=True, timeout=5,
        )
        if r.returncode != 0 or not r.stdout.strip():
            print(f"Could not create virtual sink (pactl: {r.stderr.strip()}) — continuing without a virtual mic.")
            return []
        module_ids.append(r.stdout.strip())

        r = subprocess.run(
            ["pactl", "load-module", "module-remap-source",
             f"master={VIRTUAL_SINK_NAME}.monitor",
             f"source_name={VIRTUAL_MIC_NAME}",
             f"source_properties=device.description={VIRTUAL_MIC_NAME}"],
            capture_output=True, text=True, timeout=5,
        )
        if r.returncode != 0 or not r.stdout.strip():
            print(f"Could not create virtual mic source (pactl: {r.stderr.strip()}) — continuing without it.")
            return module_ids
        module_ids.append(r.stdout.strip())

        print(f'Virtual microphone "{VIRTUAL_MIC_NAME}" ready — select it as the input/microphone device in Audacity, Discord, etc.')
        return module_ids
    except FileNotFoundError:
        print("pactl not found — continuing without a virtual mic (PulseAudio/PipeWire required for that step).")
        return []
    except Exception as e:
        print(f"Virtual mic setup failed ({e}) — continuing without it.")
        return module_ids


def destroy_virtual_mic(module_ids):
    for mid in module_ids:
        subprocess.run(["pactl", "unload-module", mid], capture_output=True, text=True, timeout=5)


def find_output_device(name_substring, retries=10, delay=0.3):
    """The virtual sink can take a beat to show up in PortAudio's device list after
    pactl creates it — retry briefly instead of failing immediately."""
    for _ in range(retries):
        for idx, info in enumerate(sd.query_devices()):
            if name_substring in info["name"] and info["max_output_channels"] > 0:
                return idx
        time.sleep(delay)
    return None


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_PORT
    print(f"Connecting to GrvEP-fusion on {port}...")
    try:
        # DTR/RTS must be asserted for the ESP32-S3's native USB-CDC to start
        # transmitting — without this the port opens but the device never sends data.
        ser = serial.Serial(port, baudrate=115200, timeout=1.0)
        ser.dtr = True
        ser.rts = True
        time.sleep(0.5)
        ser.reset_input_buffer()
    except Exception as e:
        print(f"Could not open {port}: {e}")
        sys.exit(1)

    module_ids = create_virtual_mic() if sys.platform.startswith("linux") else []
    mic_stream = None
    if module_ids:
        mic_device = find_output_device(VIRTUAL_SINK_NAME)
        if mic_device is not None:
            mic_stream = sd.RawOutputStream(
                samplerate=SAMPLE_RATE, channels=CHANNELS, dtype="int8",
                device=mic_device, latency="high",
            )
            mic_stream.start()
        else:
            print(f'Virtual sink created but not found in PortAudio\'s device list — "{VIRTUAL_MIC_NAME}" will stay silent.')

    # latency="high" gives PortAudio more internal buffering headroom against the
    # small, somewhat irregularly-timed 64-byte writes below — the earlier "craquements"
    # were partly underrun clicks from feeding tiny chunks at low latency, on top of the
    # expected 8-bit quantization grit.
    speaker_stream = sd.RawOutputStream(samplerate=SAMPLE_RATE, channels=CHANNELS, dtype="int8", latency="high")
    speaker_stream.start()

    print("Waiting for audio stream...")
    audio_active = False

    try:
        while True:
            if not audio_active:
                ser.write(b"A")
                ser.flush()
                time.sleep(0.3)
                if ser.in_waiting > 0:
                    print("\n>>> AUDIO STREAM ACTIVE — play something on the GrvEP <<<")
                    audio_active = True
                    ser.reset_input_buffer()
            else:
                # Resync on the marker — skips over any interleaved debug text
                # harmlessly, same as the reference project this was adapted from.
                ser.read_until(SYNC_MARKER)
                pcm_data = ser.read(PAYLOAD_BYTES)
                if len(pcm_data) == PAYLOAD_BYTES:
                    speaker_stream.write(pcm_data)
                    if mic_stream is not None:
                        mic_stream.write(pcm_data)
    except KeyboardInterrupt:
        print("\nStopping audio bridge...")
    finally:
        ser.write(b"S")
        time.sleep(0.1)
        ser.close()
        speaker_stream.stop()
        speaker_stream.close()
        if mic_stream is not None:
            mic_stream.stop()
            mic_stream.close()
        destroy_virtual_mic(module_ids)


if __name__ == "__main__":
    main()
