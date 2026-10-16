"""
Pre-build patch: force FastLED RMT IDF5 driver to use DMA_ENABLED instead of DMA_AUTO.

Without DMA, the RMT refills its 96-symbol buffer via ISR (~12 ISR calls per 36-LED frame).
If AMY's I2S audio ISR fires during one of those refills, the gap in signal is long enough
to trigger a RESET on the SK6812 strip, causing all subsequent LEDs to flicker.

With DMA, GDMA streams the entire 1024-symbol buffer continuously — zero CPU involvement.
"""
import os
Import("env")

target = os.path.join(
    env["PROJECT_DIR"], ".pio", "libdeps",
    env["PIOENV"],
    "FastLED", "src", "platforms", "esp", "32", "rmt_5",
    "idf5_clockless_rmt_esp32.h"
)

PATCH_MARKER = "FASTLED_DMA_FORCED"
OLD = "return fl::RmtController5::DMA_AUTO;"
NEW = "return fl::RmtController5::DMA_ENABLED; // " + PATCH_MARKER

if not os.path.exists(target):
    print("patch_fastled_dma: file not found, skipping: " + target)
else:
    with open(target, "r") as f:
        content = f.read()
    if PATCH_MARKER in content:
        print("patch_fastled_dma: already patched, OK")
    elif OLD in content:
        content = content.replace(OLD, NEW)
        with open(target, "w") as f:
            f.write(content)
        print("patch_fastled_dma: patched DMA_AUTO -> DMA_ENABLED in " + target)
    else:
        print("patch_fastled_dma: WARNING — target string not found, FastLED version may have changed")
