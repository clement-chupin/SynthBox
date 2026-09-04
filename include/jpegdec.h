#pragma once
// ESP32 image decode shim — backs the cross-target imgDecodeGray() used by
// loadOrConvertBvid() (src/main.cpp) to auto-convert photos to .bvid.
// JPEG uses the Arduino-ESP32 framework's bundled esp_jpeg component (TJpgDec-based
// under the hood, no platformio.ini lib_deps entry needed, same "framework already
// ships it" pattern as mp3dec.h for MP3 decoding) — it supports decoding straight to
// a downscaled size, which keeps a large phone photo's decode buffer small.
// PNG has no equivalent framework-provided decoder and no library on this target
// supports decode-time downscale for it (unlike the JPEG path above) — a whole-
// image decode (tried first via vendored stb_image) genuinely needs ~2x the
// native-channel-count image size in PSRAM at once (the inflated raw scanline
// buffer and the defiltered output buffer coexist during PNG defiltering,
// confirmed by reading stb_image's own create_png_image/convert_format), which
// scales with source resolution and can't be shrunk by asking for fewer output
// channels — a modest 818x317 RGBA photo already needs ~2MB peak. So PNG is
// decoded here by a small hand-written *streaming* decoder instead: it walks
// PNG chunks itself, feeds IDAT payloads through the ROM-resident miniz raw
// inflate coroutine (tinfl_decompress, esp_rom/include/miniz.h — already linked
// into every Arduino-ESP32 build, no new dependency) one row at a time, defilters that one
// row using only itself + the previous row, converts it to luma, and streams it
// straight into a bounded long-edge-128 output accumulator — never holding more
// than ~2 scanlines + the small 128-wide-or-shorter output in memory, regardless
// of source resolution. Supports baseline (non-interlaced) PNGs at bit depth
// 1/2/4/8/16 (grayscale), 1/2/4/8 (palette) or 8/16 (truecolor/alpha) — i.e.
// every PNG variant except Adam7-interlaced, which is rejected with a message
// pointing at the existing offline tools/to_bvid.py converter (rare from
// phones/screenshots/editors, which default to non-interlaced).
// simulator/hal/jpegdec.h is the equivalent shim for the desktop simulator and
// the android-native build (both formats via stb_image, whole-image decode is
// fine there — desktop RAM is ample); the two are never compiled together.

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <esp_heap_caps.h>
#include <jpeg_decoder.h>
#include "miniz.h"

static inline bool imgDecodeGrayJpeg(const uint8_t* buf, size_t len, uint8_t** outGray, int* outW, int* outH) {
    esp_jpeg_image_cfg_t cfg = {};
    cfg.indata      = (uint8_t*)buf;
    cfg.indata_size = (uint32_t)len;
    cfg.out_format  = JPEG_IMAGE_FORMAT_RGB565;
    cfg.out_scale   = JPEG_IMAGE_SCALE_0;

    esp_jpeg_image_output_t info = {};
    esp_err_t infoErr = esp_jpeg_get_image_info(&cfg, &info);
    if (infoErr != ESP_OK || info.width == 0 || info.height == 0) {
        Serial.printf("IMG: esp_jpeg_get_image_info failed (err=%d w=%u h=%u)\n", (int)infoErr, info.width, info.height);
        return false;
    }

    uint16_t longEdge = info.width > info.height ? info.width : info.height;
    if      (longEdge > 2048) cfg.out_scale = JPEG_IMAGE_SCALE_1_8;
    else if (longEdge > 1024) cfg.out_scale = JPEG_IMAGE_SCALE_1_4;
    else if (longEdge > 512)  cfg.out_scale = JPEG_IMAGE_SCALE_1_2;
    else                      cfg.out_scale = JPEG_IMAGE_SCALE_0;

    uint32_t divisor  = 1u << (uint32_t)cfg.out_scale;  // SCALE_0=1, 1_2=2, 1_4=4, 1_8=8
    uint32_t decW     = (info.width  + divisor - 1) / divisor;
    uint32_t decH     = (info.height + divisor - 1) / divisor;
    uint32_t rgbSize  = decW * decH * 2;  // RGB565, 2 bytes/pixel

    uint8_t* rgbBuf = (uint8_t*)ps_malloc(rgbSize);
    if (!rgbBuf) { Serial.printf("IMG: ps_malloc(%u) failed for JPEG rgb buffer\n", rgbSize); return false; }
    cfg.outbuf      = rgbBuf;
    cfg.outbuf_size = rgbSize;

    esp_jpeg_image_output_t out = {};
    esp_err_t decErr = esp_jpeg_decode(&cfg, &out);
    if (decErr != ESP_OK || out.width == 0 || out.height == 0) {
        Serial.printf("IMG: esp_jpeg_decode failed (err=%d w=%u h=%u)\n", (int)decErr, out.width, out.height);
        free(rgbBuf);
        return false;
    }

    uint8_t* gray = (uint8_t*)ps_malloc((size_t)out.width * out.height);
    if (!gray) { free(rgbBuf); return false; }
    const uint16_t* px = (const uint16_t*)rgbBuf;
    uint32_t n = (uint32_t)out.width * out.height;
    for (uint32_t i = 0; i < n; i++) {
        uint16_t p = px[i];
        uint8_t r = (uint8_t)(((p >> 11) & 0x1F) << 3);
        uint8_t g = (uint8_t)(((p >> 5)  & 0x3F) << 2);
        uint8_t b = (uint8_t)((p & 0x1F) << 3);
        gray[i] = (uint8_t)((r * 77 + g * 151 + b * 28) >> 8);  // standard luma weights
    }
    free(rgbBuf);

    *outGray = gray;
    *outW    = out.width;
    *outH    = out.height;
    return true;
}

static inline uint8_t pngdecPaeth(uint8_t a, uint8_t b, uint8_t c) {
    int p  = (int)a + (int)b - (int)c;
    int pa = abs(p - (int)a), pb = abs(p - (int)b), pc = abs(p - (int)c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

static inline bool imgDecodeGrayPng(const uint8_t* buf, size_t len, uint8_t** outGray, int* outW, int* outH) {
    static const uint8_t kPngSig[8] = {0x89,'P','N','G','\r','\n',0x1a,'\n'};
    if (len < 8 || memcmp(buf, kPngSig, 8) != 0) {
        Serial.println("IMG: PNG bad signature");
        return false;
    }

    uint32_t width = 0, height = 0;
    uint8_t  bitDepth = 0, colorType = 0;
    uint32_t channels = 0;   // samples/pixel as stored in the bitstream (1=grey/palette, 2=grey+a, 3=rgb, 4=rgba)
    uint32_t bpp = 1;        // bytes-per-complete-pixel for filter reconstruction (spec: 1 if bitDepth<8)
    uint32_t rowBytes = 0;   // 1 (filter type byte) + packed pixel bytes
    uint8_t  palette[256][3] = {};  // zeroed so a malformed colorType==3 PNG missing PLTE reads black, not stack garbage
    int      paletteCount = 0;

    uint8_t* curRow  = nullptr;
    uint8_t* prevRow = nullptr;
    uint32_t rowFilled = 0;
    uint32_t srcRow = 0;

    int dstW = 0, dstH = 0;
    uint8_t*  dstGray = nullptr;
    uint32_t* colSum   = nullptr;
    uint16_t* colCount = nullptr;
    int curTargetRow = -1;

    // esp_rom's miniz.h ships with MINIZ_NO_ZLIB_APIS defined (no mz_stream/mz_inflate
    // wrapper in ROM) — only the low-level tinfl_decompress() coroutine is available,
    // which needs a persistent TINFL_LZ_DICT_SIZE (32KB) sliding-window output buffer
    // across calls (it's also where back-references resolve from), not an arbitrary
    // output pointer. Decompressed bytes are copied out of that window into curRow one
    // at a time as they're produced.
    tinfl_decompressor inflator;
    bool inflatorInit = false;
    uint8_t* dict = nullptr;
    size_t   dictOfs = 0;
    bool ok = true;
    bool doneAllRows = false;

    auto finalizeTargetRow = [&](int tr) {
        if (tr < 0 || tr >= dstH) return;
        for (int x = 0; x < dstW; x++) {
            uint32_t c = colCount[x];
            dstGray[tr * dstW + x] = c ? (uint8_t)(colSum[x] / c) : 0;
        }
    };
    auto resetAccum = [&]() {
        memset(colSum, 0, sizeof(uint32_t) * dstW);
        memset(colCount, 0, sizeof(uint16_t) * dstW);
    };
    // Reads one sample (grayscale value or palette index), bit-packed at any depth 1/2/4/8/16.
    auto readSample = [&](const uint8_t* row, uint32_t sampleIdx, uint32_t depth) -> uint32_t {
        if (depth == 8)  return row[sampleIdx];
        if (depth == 16) return row[sampleIdx * 2];  // big-endian sample, high byte only
        uint32_t bitPos = sampleIdx * depth;
        uint32_t byteIdx = bitPos / 8;
        uint32_t shift = 8 - (bitPos % 8) - depth;
        return (row[byteIdx] >> shift) & ((1u << depth) - 1);
    };

    auto processRow = [&]() {
        // Defilter curRow[1..] in place using prevRow (PNG spec filter types 0-4).
        uint8_t filt = curRow[0];
        uint8_t* cur  = curRow + 1;
        uint8_t* prev = prevRow + 1;
        uint32_t n = rowBytes - 1;
        for (uint32_t x = 0; x < n; x++) {
            uint8_t a = (x >= bpp) ? cur[x - bpp]  : 0;
            uint8_t b = prev[x];
            uint8_t c = (x >= bpp) ? prev[x - bpp] : 0;
            uint8_t raw = cur[x];
            switch (filt) {
                case 1: cur[x] = raw + a; break;
                case 2: cur[x] = raw + b; break;
                case 3: cur[x] = raw + (uint8_t)(((uint32_t)a + (uint32_t)b) / 2); break;
                case 4: cur[x] = raw + pngdecPaeth(a, b, c); break;
                default: break;  // 0 = None, already correct
            }
        }

        int targetRow = (int)(((uint64_t)srcRow * (uint64_t)dstH) / (uint64_t)height);
        if (targetRow != curTargetRow) {
            if (curTargetRow >= 0) finalizeTargetRow(curTargetRow);
            curTargetRow = targetRow;
            resetAccum();
        }

        uint32_t maxVal = (1u << bitDepth) - 1;
        for (uint32_t sx = 0; sx < width; sx++) {
            uint8_t r, g, b_, gray;
            if (colorType == 3) {
                uint32_t idx = readSample(cur, sx, bitDepth);
                const uint8_t* p = palette[idx < 256 ? idx : 0];
                r = p[0]; g = p[1]; b_ = p[2];
                gray = (uint8_t)(((uint32_t)r * 77 + (uint32_t)g * 151 + (uint32_t)b_ * 28) >> 8);
            } else if (colorType == 0 || colorType == 4) {
                uint32_t sample = readSample(cur, sx * channels, bitDepth);
                gray = (bitDepth < 8) ? (uint8_t)(sample * 255 / maxVal) : (uint8_t)sample;
            } else {
                // colorType 2 (RGB) or 6 (RGBA), bitDepth 8 or 16 (spec-restricted, always byte-aligned)
                uint32_t bppSample = (bitDepth == 16) ? 2 : 1;
                uint32_t base = sx * channels * bppSample;
                r = cur[base]; g = cur[base + bppSample]; b_ = cur[base + 2 * bppSample];
                gray = (uint8_t)(((uint32_t)r * 77 + (uint32_t)g * 151 + (uint32_t)b_ * 28) >> 8);
            }
            int tx = (int)(((uint64_t)sx * (uint64_t)dstW) / (uint64_t)width);
            if (tx >= dstW) tx = dstW - 1;
            colSum[tx]  += gray;
            colCount[tx]++;
        }

        uint8_t* tmp = prevRow; prevRow = curRow; curRow = tmp;
        srcRow++;
        rowFilled = 0;
        if (srcRow >= height) doneAllRows = true;
    };

    size_t pos = 8;
    bool haveIHDR = false;
    while (ok && !doneAllRows && pos + 8 <= len) {
        uint32_t clen = ((uint32_t)buf[pos]<<24)|((uint32_t)buf[pos+1]<<16)|((uint32_t)buf[pos+2]<<8)|buf[pos+3];
        const uint8_t* ctype = buf + pos + 4;
        size_t dataOff = pos + 8;
        if (dataOff + (size_t)clen + 4 > len) break;  // truncated file

        if (memcmp(ctype, "IHDR", 4) == 0) {
            const uint8_t* d = buf + dataOff;
            width     = ((uint32_t)d[0]<<24)|((uint32_t)d[1]<<16)|((uint32_t)d[2]<<8)|d[3];
            height    = ((uint32_t)d[4]<<24)|((uint32_t)d[5]<<16)|((uint32_t)d[6]<<8)|d[7];
            bitDepth  = d[8];
            colorType = d[9];
            uint8_t interlace = d[12];
            haveIHDR = true;
            if (interlace != 0) {
                Serial.println("IMG: PNG is interlaced (Adam7), not supported — pre-convert with tools/to_bvid.py");
                ok = false; break;
            }
            bool bitOk = (colorType == 0) ? (bitDepth==1||bitDepth==2||bitDepth==4||bitDepth==8||bitDepth==16)
                       : (colorType == 3) ? (bitDepth==1||bitDepth==2||bitDepth==4||bitDepth==8)
                       : (bitDepth == 8 || bitDepth == 16);  // colorType 2/4/6
            if (!bitOk || width == 0 || height == 0 || (colorType!=0 && colorType!=2 && colorType!=3 && colorType!=4 && colorType!=6)) {
                Serial.printf("IMG: PNG unsupported bitDepth=%d colorType=%d — pre-convert with tools/to_bvid.py\n", bitDepth, colorType);
                ok = false; break;
            }
            channels = (colorType==0)?1 : (colorType==2)?3 : (colorType==3)?1 : (colorType==4)?2 : 4;
            bpp = (bitDepth < 8) ? 1 : (uint32_t)((bitDepth/8) * channels);
            uint64_t rowBits = (uint64_t)width * bitDepth * channels;
            rowBytes = 1 + (uint32_t)((rowBits + 7) / 8);
            curRow  = (uint8_t*)malloc(rowBytes);
            prevRow = (uint8_t*)malloc(rowBytes);
            if (!curRow || !prevRow) { Serial.println("IMG: PNG row buffer alloc failed"); ok = false; break; }
            memset(prevRow, 0, rowBytes);

            // Bounded output: long edge scaled down to 128 max, aspect preserved —
            // mirrors the JPEG path's decode-time downscale so bvidEncodeFromGray()
            // (which does the final letterbox/fit/dither) sees the same kind of input
            // either way. Never scale *up* (cap scale at 1.0): the per-source-row/col
            // streaming accumulator below only fills target bins that some source
            // pixel maps into — upscaling would leave target rows/cols no source pixel
            // reaches as uninitialized ps_malloc garbage. A source already <=128 on its
            // long edge passes through at native resolution instead; bvidEncodeFromGray
            // itself safely upscales from there (it samples backward from each output
            // pixel, so it has no such gap risk).
            uint32_t longEdge = (width > height) ? width : height;
            float scale = (longEdge > 128) ? (128.0f / (float)longEdge) : 1.0f;
            dstW = (int)fmaxf(1.0f, roundf((float)width  * scale));
            dstH = (int)fmaxf(1.0f, roundf((float)height * scale));
            dstGray  = (uint8_t*)ps_malloc((size_t)dstW * dstH);
            colSum   = (uint32_t*)malloc(sizeof(uint32_t) * dstW);
            colCount = (uint16_t*)malloc(sizeof(uint16_t) * dstW);
            if (!dstGray || !colSum || !colCount) { Serial.println("IMG: PNG output buffer alloc failed"); ok = false; break; }
            dict = (uint8_t*)malloc(TINFL_LZ_DICT_SIZE);
            if (!dict) { Serial.println("IMG: PNG inflate dict alloc failed"); ok = false; break; }
            tinfl_init(&inflator);
            inflatorInit = true;
            Serial.printf("IMG: PNG %ux%u bitDepth=%d colorType=%d -> streaming to %dx%d\n",
                          (unsigned)width, (unsigned)height, bitDepth, colorType, dstW, dstH);
        } else if (memcmp(ctype, "PLTE", 4) == 0) {
            paletteCount = (int)(clen / 3);
            if (paletteCount > 256) paletteCount = 256;
            memset(palette, 0, sizeof(palette));
            memcpy(palette, buf + dataOff, (size_t)paletteCount * 3);
        } else if (memcmp(ctype, "IDAT", 4) == 0) {
            if (!haveIHDR || !inflatorInit) { ok = false; break; }
            const uint8_t* inPtr = buf + dataOff;
            size_t inRemaining = clen;
            while (!doneAllRows) {
                size_t inBytes  = inRemaining;
                size_t outBytes = TINFL_LZ_DICT_SIZE - dictOfs;  // room to end of dict before wrap
                tinfl_status st = tinfl_decompress(&inflator, inPtr, &inBytes, dict, dict + dictOfs, &outBytes,
                                                    TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_HAS_MORE_INPUT);
                for (size_t i = 0; i < outBytes && !doneAllRows; i++) {
                    curRow[rowFilled++] = dict[dictOfs + i];
                    if (rowFilled >= rowBytes) processRow();
                }
                dictOfs += outBytes;
                if (dictOfs >= TINFL_LZ_DICT_SIZE) dictOfs = 0;
                inPtr += inBytes; inRemaining -= inBytes;
                if (st == TINFL_STATUS_DONE) { doneAllRows = true; break; }
                if (st < 0) { Serial.printf("IMG: PNG tinfl_decompress error %d\n", (int)st); ok = false; break; }
                if (inRemaining == 0) break;  // this chunk is exhausted; next IDAT (if any) continues the stream
            }
        } else if (memcmp(ctype, "IEND", 4) == 0) {
            break;
        }
        pos = dataOff + clen + 4;
    }

    if (dict) free(dict);
    if (curRow)  free(curRow);
    if (prevRow) free(prevRow);
    if (ok && curTargetRow >= 0) finalizeTargetRow(curTargetRow);
    if (colSum)   free(colSum);
    if (colCount) free(colCount);

    if (!ok || !dstGray || srcRow < height) {
        Serial.printf("IMG: PNG stream decode failed (rows %u/%u)\n", (unsigned)srcRow, (unsigned)height);
        if (dstGray) free(dstGray);
        return false;
    }
    *outGray = dstGray;
    *outW = dstW;
    *outH = dstH;
    return true;
}

// Decodes a JPEG or PNG buffer to 8-bit grayscale (row-major, 1 byte/pixel),
// dispatching on the file's magic bytes. On success, *outGray is a heap buffer
// the caller must free(); dimensions come back via *outW/*outH. Returns false on
// failure or if a PNG is too large to decode safely on this device (nothing to
// free either way).
static inline bool imgDecodeGray(const uint8_t* buf, size_t len, uint8_t** outGray, int* outW, int* outH) {
    bool isPng = len >= 8 && buf[0] == 0x89 && buf[1] == 'P' && buf[2] == 'N' && buf[3] == 'G';
    Serial.printf("IMG: magic %02x %02x %02x %02x -> %s\n",
                  len > 0 ? buf[0] : 0, len > 1 ? buf[1] : 0, len > 2 ? buf[2] : 0, len > 3 ? buf[3] : 0,
                  isPng ? "PNG" : "JPEG");
    if (isPng) return imgDecodeGrayPng(buf, len, outGray, outW, outH);
    return imgDecodeGrayJpeg(buf, len, outGray, outW, outH);
}
