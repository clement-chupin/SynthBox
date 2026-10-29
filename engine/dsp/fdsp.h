#pragma once
// Ported from stmlib (Mutable Instruments, (c) 2012-2017 Emilie Gillet, MIT licence: see the notice in src/engine/mi/LICENSE) into the
// framework so the Braids / Plaits algorithms (src/engine/mi) depend on our code only. Namespace sc::fdsp ("float DSP" helpers, plus the
// integer table helpers of Braids), kept bit-identical to the originals: replacing a helper by one of our own (svf.h, util.h ...) is done
// under an A/B test (ENGINE_DESIGN.md ADR-039).
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "engine/dsp/q.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// stmlib's macros, as the ported algorithms use them (only src/engine/mi and its adapter include this header)
#define DISALLOW_COPY_AND_ASSIGN(TypeName) \
  TypeName(const TypeName&);               \
  void operator=(const TypeName&)
#define CLIP(x) if (x < -32767) x = -32767; if (x > 32767) x = 32767;
#define CONSTRAIN(var, min, max) \
  if (var < (min)) { \
    var = (min); \
  } else if (var > (max)) { \
    var = (max); \
  }
#define MAKE_INTEGRAL_FRACTIONAL(x) \
  int32_t x ## _integral = static_cast<int32_t>(x); \
  float x ## _fractional = x - static_cast<float>(x ## _integral);
#define ONE_POLE(out, in, coefficient) out += (coefficient) * ((in) - out);
#define SLOPE(out, in, positive, negative) { \
  float error = (in) - out; \
  out += (error > 0 ? positive : negative) * error; \
}
#define SLEW(out, in, delta) { \
  float error = (in) - out; \
  float d = (delta); \
  if (error > d) { \
    error = d; \
  } else if (error < -d) { \
    error = -d; \
  } \
  out += error; \
}

namespace sc {
namespace fdsp {

/* ---- numeric helpers (stmlib/dsp/dsp.h) ---- */

inline float Interpolate(const float* table, float index, float size) {
  index *= size;
  MAKE_INTEGRAL_FRACTIONAL(index)
  float a = table[index_integral];
  float b = table[index_integral + 1];
  return a + (b - a) * index_fractional;
}


inline float InterpolateHermite(const float* table, float index, float size) {
  index *= size;
  MAKE_INTEGRAL_FRACTIONAL(index)
  const float xm1 = table[index_integral - 1];
  const float x0 = table[index_integral + 0];
  const float x1 = table[index_integral + 1];
  const float x2 = table[index_integral + 2];
  const float c = (x1 - xm1) * 0.5f;
  const float v = x0 - x1;
  const float w = c + v;
  const float a = w + v + (x2 - x0) * 0.5f;
  const float b_neg = w + a;
  const float f = index_fractional;
  return (((a * f) - b_neg) * f + c) * f + x0;
}

inline float InterpolateWrap(const float* table, float index, float size) {
  index -= static_cast<float>(static_cast<int32_t>(index));
  index *= size;
  MAKE_INTEGRAL_FRACTIONAL(index)
  float a = table[index_integral];
  float b = table[index_integral + 1];
  return a + (b - a) * index_fractional;
}

inline float SmoothStep(float value) {
  return value * value * (3.0f - 2.0f * value);
}


inline float Crossfade(float a, float b, float fade) {
  return a + (b - a) * fade;
}

inline float SoftLimit(float x) {
  return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}

inline float SoftClip(float x) {
  if (x < -3.0f) {
    return -1.0f;
  } else if (x > 3.0f) {
    return 1.0f;
  } else {
    return SoftLimit(x);
  }
}

inline int32_t Clip16(int32_t x) { return sat16(x); }               // our q.h saturation (identical)
inline uint16_t ClipU16(int32_t x) { return static_cast<uint16_t>(x < 0 ? 0 : (x > 65535 ? 65535 : x)); }
  
inline float Sqrt(float x) { return sqrtf(x); }

inline int16_t SoftConvert(float x) {
  return Clip16(static_cast<int32_t>(SoftLimit(x * 0.5f) * 32768.0f));
}

/* ---- integer table interpolation, Braids (stmlib/utils/dsp.h) ---- */
inline int16_t Interpolate824(const int16_t* table, uint32_t phase)
  __attribute__((always_inline));

inline uint16_t Interpolate824(const uint16_t* table, uint32_t phase)
  __attribute__((always_inline));

inline int16_t Interpolate824(const uint8_t* table, uint32_t phase)
  __attribute__((always_inline));

inline uint16_t Interpolate88(const uint16_t* table, uint16_t index)
  __attribute__((always_inline));

inline int16_t Interpolate88(const int16_t* table, uint16_t index)
  __attribute__((always_inline));

inline int16_t Interpolate1022(const int16_t* table, uint32_t phase)
  __attribute__((always_inline));

inline int16_t Interpolate115(const int16_t* table, uint32_t phase)
  __attribute__((always_inline));

inline int16_t Crossfade(
    const int16_t* table_a,
    const int16_t* table_b,
    uint32_t phase,
    uint16_t balance)
  __attribute__((always_inline));

inline int16_t Crossfade(
    const uint8_t* table_a,
    const uint8_t* table_b,
    uint32_t phase,
    uint16_t balance)
  __attribute__((always_inline));

inline int16_t Crossfade1022(
    const uint8_t* table_a,
    const uint8_t* table_b,
    uint32_t phase,
    uint16_t balance)
  __attribute__((always_inline));

inline int16_t Crossfade115(
    const uint8_t* table_a,
    const uint8_t* table_b,
    uint16_t phase,
    uint16_t balance)
  __attribute__((always_inline));

inline int16_t Mix(int16_t a, int16_t b, uint16_t balance) {
  return (a * (65535 - balance) + b * balance) >> 16;
}

inline uint16_t Mix(uint16_t a, uint16_t b, uint16_t balance) {
  return (a * (65535 - balance) + b * balance) >> 16;
}

inline int16_t Interpolate824(const int16_t* table, uint32_t phase) {
  int32_t a = table[phase >> 24];
  int32_t b = table[(phase >> 24) + 1];
  return a + ((b - a) * static_cast<int32_t>((phase >> 8) & 0xffff) >> 16);
}

inline uint16_t Interpolate824(const uint16_t* table, uint32_t phase) {
  uint32_t a = table[phase >> 24];
  uint32_t b = table[(phase >> 24) + 1];
  return a + ((b - a) * static_cast<uint32_t>((phase >> 8) & 0xffff) >> 16);
}

inline int16_t Interpolate824(const uint8_t* table, uint32_t phase) {
  int32_t a = table[phase >> 24];
  int32_t b = table[(phase >> 24) + 1];
  return (a << 8) + \
      ((b - a) * static_cast<int32_t>(phase & 0xffffff) >> 16) - 32768;
}

inline uint16_t Interpolate88(const uint16_t* table, uint16_t index) {
  int32_t a = table[index >> 8];
  int32_t b = table[(index >> 8) + 1];
  return a + ((b - a) * static_cast<int32_t>(index & 0xff) >> 8);
}

inline int16_t Interpolate88(const int16_t* table, uint16_t index) {
  int32_t a = table[index >> 8];
  int32_t b = table[(index >> 8) + 1];
  return a + ((b - a) * static_cast<int32_t>(index & 0xff) >> 8);
}

inline int16_t Interpolate1022(const int16_t* table, uint32_t phase) {
  int32_t a = table[phase >> 22];
  int32_t b = table[(phase >> 22) + 1];
  return a + ((b - a) * static_cast<int32_t>((phase >> 6) & 0xffff) >> 16);
}

inline int16_t Interpolate115(const int16_t* table, uint16_t phase) {
  int32_t a = table[phase >> 5];
  int32_t b = table[(phase >> 5) + 1];
  return a + ((b - a) * static_cast<int32_t>(phase & 0x1f) >> 5);
}

inline int16_t Crossfade(
    const int16_t* table_a,
    const int16_t* table_b,
    uint32_t phase,
    uint16_t balance) {
  int32_t a = Interpolate824(table_a, phase);
  int32_t b = Interpolate824(table_b, phase);
  return a + ((b - a) * static_cast<int32_t>(balance) >> 16);
}

inline int16_t Crossfade(
    const uint8_t* table_a,
    const uint8_t* table_b,
    uint32_t phase,
    uint16_t balance) {
  int32_t a = Interpolate824(table_a, phase);
  int32_t b = Interpolate824(table_b, phase);
  return a + ((b - a) * static_cast<int32_t>(balance) >> 16);
}

inline int16_t Crossfade1022(
    const int16_t* table_a,
    const int16_t* table_b,
    uint32_t phase,
    uint16_t balance) {
  int32_t a = Interpolate1022(table_a, phase);
  int32_t b = Interpolate1022(table_b, phase);
  return a + ((b - a) * static_cast<int32_t>(balance) >> 16);
}

inline int16_t Crossfade115(
    const int16_t* table_a,
    const int16_t* table_b,
    uint16_t phase,
    uint16_t balance) {
  int32_t a = Interpolate115(table_a, phase);
  int32_t b = Interpolate115(table_b, phase);
  return a + ((b - a) * static_cast<int32_t>(balance) >> 16);
}

/* ---- fast reciprocal square root (stmlib/dsp/rsqrt.h) ---- */
template<typename To, typename From>
struct unsafe_bit_cast_t {
  union {
    From from;
    To to;
  };
};

template<typename To, typename From>
To unsafe_bit_cast(From from) {
    unsafe_bit_cast_t<To, From> u;
    u.from = from;
    return u.to;
}


static inline float fast_rsqrt_carmack(float x) {
  uint32_t i;
  float x2, y;
  const float threehalfs = 1.5f;
  y = x;
  i = unsafe_bit_cast<uint32_t, float>(y);
  i = 0x5f3759df - (i >> 1);
  y = unsafe_bit_cast<float, uint32_t>(i);
  x2 = x * 0.5f;
  y = y * (threehalfs - (x2 * y * y));
	return y;
}

static inline float fast_rsqrt_accurate(float fp0) {
  float _min = 1.0e-38;
  float _1p5 = 1.5;
  float fp1, fp2, fp3;

  uint32_t q = unsafe_bit_cast<uint32_t, float>(fp0);
  fp2 = unsafe_bit_cast<float, uint32_t>(0x5F3997BB - ((q >> 1) & 0x3FFFFFFF));
  fp1 = _1p5 * fp0 - fp0;
  fp3 = fp2 * fp2;
  if (fp0 < _min) {
    return fp0 > 0 ? fp2 : 1000.0f;
  }
  fp3 = _1p5 - fp1 * fp3;
  fp2 = fp2 * fp3;
  fp3 = fp2 * fp2;
  fp3 = _1p5 - fp1 * fp3;
  fp2 = fp2 * fp3;
  fp3 = fp2 * fp2;
  fp3 = _1p5 - fp1 * fp3;
  return fp2 * fp3;
}

/* ---- polyBLEP (stmlib/dsp/polyblep.h) ---- */
inline float ThisBlepSample(float t) {
  return 0.5f * t * t;
}

inline float NextBlepSample(float t) {
  t = 1.0f - t;
  return -0.5f * t * t;
}

inline float NextIntegratedBlepSample(float t) {
  const float t1 = 0.5f * t;
  const float t2 = t1 * t1;
  const float t4 = t2 * t2;
  return 0.1875f - t1 + 1.5f * t2 - t4;
}

inline float ThisIntegratedBlepSample(float t) {
  return NextIntegratedBlepSample(1.0f - t);
}
  

/* ---- per-sample parameter smoothing over a block (stmlib/dsp/parameter_interpolator.h) ---- */
class ParameterInterpolator {
 public:
  ParameterInterpolator() { }
  ParameterInterpolator(float* state, float new_value, size_t size) {
    Init(state, new_value, size);
  }

  ParameterInterpolator(float* state, float new_value, float step) {
    state_ = state;
    value_ = *state;
    increment_ = (new_value - *state) * step;
  }

  ~ParameterInterpolator() {
    *state_ = value_;
  }
  
  inline void Init(float* state, float new_value, size_t size) {
    state_ = state;
    value_ = *state;
    increment_ = (new_value - *state) / static_cast<float>(size);
  }

  inline float Next() {
    value_ += increment_;
    return value_;
  }

  inline float subsample(float t) {
    return value_ + increment_ * t;
  }
  
 private:
  float* state_;
  float value_;
  float increment_;
};

/* ---- quantizer with hysteresis (stmlib/dsp/hysteresis_quantizer.h) ---- */
class HysteresisQuantizer {
 public:
  HysteresisQuantizer() { }
  ~HysteresisQuantizer() { }

  void Init() {
    quantized_value_ = 0;
  }

  inline int Process(float value, int num_steps) {
    return Process(value, num_steps, 0.25f);
  }

  inline int Process(float value, int num_steps, float hysteresis) {
    return Process(0, value, num_steps, hysteresis);
  }

  inline int Process(int base, float value, int num_steps, float hysteresis) {
    value *= static_cast<float>(num_steps - 1);
    value += static_cast<float>(base);
    float hysteresis_feedback = value > static_cast<float>(quantized_value_)
        ? -hysteresis
        : hysteresis;
    int q = static_cast<int>(value + hysteresis_feedback + 0.5f);
    CONSTRAIN(q, 0, num_steps - 1);
    quantized_value_ = q;
    return q;
  }

  template<typename T>
  const T& Lookup(const T* array, float value, int num_steps) {
    return array[Process(value, num_steps)];
  }

 private:
  int quantized_value_;
  
  DISALLOW_COPY_AND_ASSIGN(HysteresisQuantizer);
};


// Note: currently refactoring this aspect of all Mutable Instruments modules.
// The codebase will progressively use only this class, at which point the other
// version will be deprecated

class HysteresisQuantizer2 {
 public:
  HysteresisQuantizer2() { }
  ~HysteresisQuantizer2() { }

  void Init(int num_steps, float hysteresis, bool symmetric) {
    num_steps_ = num_steps;
    hysteresis_ = hysteresis;

    scale_ = static_cast<float>(symmetric ? num_steps - 1 : num_steps);
    offset_ = symmetric ? 0.0f : -0.5f;

    quantized_value_ = 0;
  }

  inline int Process(float value) {
    return Process(0, value);
  }

  inline int Process(int base, float value) {
    value *= scale_;
    value += offset_;
    value += static_cast<float>(base);

    float hysteresis_sign = value > static_cast<float>(quantized_value_)
        ? -1.0f
        : +1.0f;
    int q = static_cast<int>(value + hysteresis_sign * hysteresis_ + 0.5f);
    CONSTRAIN(q, 0, num_steps_ - 1);
    quantized_value_ = q;
    return q;
  }

  template<typename T>
  const T& Lookup(const T* array, float value) {
    return array[Process(value)];
  }
  
  inline int num_steps() const {
    return num_steps_;
  }
  
  inline int quantized_value() const {
    return quantized_value_;
  }

 private:
  int num_steps_;
  float hysteresis_;
  
  float scale_;
  float offset_;

  int quantized_value_;
  
  DISALLOW_COPY_AND_ASSIGN(HysteresisQuantizer2);
};

/* ---- scratch memory handed to an engine's Init (stmlib/utils/buffer_allocator.h) ---- */
class BufferAllocator {
 public:
  BufferAllocator() { }
  ~BufferAllocator() { }
  
  BufferAllocator(void* buffer, size_t size) {
    Init(buffer, size);
  }
  
  inline void Init(void* buffer, size_t size) {
    buffer_ = static_cast<uint8_t*>(buffer);
    size_ = size;
    Free();
  }

  template<typename T>
  inline T* Allocate() {
    return Allocate<T>(1);
  }
  
  template<typename T>
  inline T* Allocate(size_t size) {
    size_t size_bytes = sizeof(T) * size;
    if (size_bytes <= free_) {
      T* start = static_cast<T*>(static_cast<void*>(next_));
      next_ += size_bytes;
      free_ -= size_bytes;
      return start;
    } else {
      return NULL;
    }
  }
  
  inline void Free() {
    next_ = buffer_;
    free_ = size_;
  }
  
  inline size_t free() const { return free_; }

 private:
  uint8_t* next_;
  uint8_t* buffer_;
  size_t free_;
  size_t size_;

  DISALLOW_COPY_AND_ASSIGN(BufferAllocator);
};

}  // namespace fdsp
}  // namespace sc
