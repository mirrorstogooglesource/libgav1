// Copyright 2019 The libgav1 Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "src/dsp/loop_restoration.h"
#include "src/utils/cpu.h"

#if LIBGAV1_ENABLE_NEON
#include <arm_neon.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "src/dsp/arm/common_neon.h"
#include "src/dsp/constants.h"
#include "src/dsp/dsp.h"
#include "src/utils/common.h"
#include "src/utils/constants.h"

namespace libgav1 {
namespace dsp {
namespace low_bitdepth {
namespace {

template <int bytes>
inline uint8x8_t VshrU128(const uint8x16_t a) {
  return vext_u8(vget_low_u8(a), vget_high_u8(a), bytes);
}

template <int bytes>
inline uint16x4_t VshrU128(const uint16x8_t a) {
  return vext_u16(vget_low_u16(a), vget_high_u16(a), bytes / 2);
}

template <int bytes>
inline uint16x8_t VshrU128(const uint16x8x2_t a) {
  return vextq_u16(a.val[0], a.val[1], bytes / 2);
}

// Wiener

// Must make a local copy of coefficients to help compiler know that they have
// no overlap with other buffers. Using 'const' keyword is not enough. Actually
// compiler doesn't make a copy, since there is enough registers in this case.
inline void PopulateWienerCoefficients(
    const RestorationUnitInfo& restoration_info, const int direction,
    int16_t filter[4]) {
  // In order to keep the horizontal pass intermediate values within 16 bits we
  // offset |filter[3]| by 128. The 128 offset will be added back in the loop.
  for (int i = 0; i < 4; ++i) {
    filter[i] = restoration_info.wiener_info.filter[direction][i];
  }
  if (direction == WienerInfo::kHorizontal) {
    filter[3] -= 128;
  }
}

inline int16x8_t WienerHorizontal2(const uint8x8_t s0, const uint8x8_t s1,
                                   const int16_t filter, const int16x8_t sum) {
  const int16x8_t ss = vreinterpretq_s16_u16(vaddl_u8(s0, s1));
  return vmlaq_n_s16(sum, ss, filter);
}

inline int16x8x2_t WienerHorizontal2(const uint8x16_t s0, const uint8x16_t s1,
                                     const int16_t filter,
                                     const int16x8x2_t sum) {
  int16x8x2_t d;
  d.val[0] =
      WienerHorizontal2(vget_low_u8(s0), vget_low_u8(s1), filter, sum.val[0]);
  d.val[1] =
      WienerHorizontal2(vget_high_u8(s0), vget_high_u8(s1), filter, sum.val[1]);
  return d;
}

inline void WienerHorizontalSum(const uint8x8_t s[3], const int16_t filter[4],
                                int16x8_t sum, int16_t* const wiener_buffer) {
  constexpr int offset =
      1 << (8 + kWienerFilterBits - kInterRoundBitsHorizontal - 1);
  constexpr int limit = (offset << 2) - 1;
  const int16x8_t s_0_2 = vreinterpretq_s16_u16(vaddl_u8(s[0], s[2]));
  const int16x8_t s_1 = vreinterpretq_s16_u16(vmovl_u8(s[1]));
  sum = vmlaq_n_s16(sum, s_0_2, filter[2]);
  sum = vmlaq_n_s16(sum, s_1, filter[3]);
  // Calculate scaled down offset correction, and add to sum here to prevent
  // signed 16 bit outranging.
  sum = vrsraq_n_s16(vshlq_n_s16(s_1, 7 - kInterRoundBitsHorizontal), sum,
                     kInterRoundBitsHorizontal);
  sum = vmaxq_s16(sum, vdupq_n_s16(-offset));
  sum = vminq_s16(sum, vdupq_n_s16(limit - offset));
  vst1q_s16(wiener_buffer, sum);
}

inline void WienerHorizontalSum(const uint8x16_t src[3],
                                const int16_t filter[4], int16x8x2_t sum,
                                int16_t* const wiener_buffer) {
  uint8x8_t s[3];
  s[0] = vget_low_u8(src[0]);
  s[1] = vget_low_u8(src[1]);
  s[2] = vget_low_u8(src[2]);
  WienerHorizontalSum(s, filter, sum.val[0], wiener_buffer);
  s[0] = vget_high_u8(src[0]);
  s[1] = vget_high_u8(src[1]);
  s[2] = vget_high_u8(src[2]);
  WienerHorizontalSum(s, filter, sum.val[1], wiener_buffer + 8);
}

inline void WienerHorizontalTap7(const uint8_t* src, const ptrdiff_t src_stride,
                                 const ptrdiff_t width, const int height,
                                 const int16_t filter[4],
                                 int16_t** const wiener_buffer) {
  int y = height;
  do {
    const uint8_t* src_ptr = src;
    uint8x16_t s[8];
    s[0] = vld1q_u8(src_ptr);
    ptrdiff_t x = width;
    do {
      src_ptr += 16;
      s[7] = vld1q_u8(src_ptr);
      s[1] = vextq_u8(s[0], s[7], 1);
      s[2] = vextq_u8(s[0], s[7], 2);
      s[3] = vextq_u8(s[0], s[7], 3);
      s[4] = vextq_u8(s[0], s[7], 4);
      s[5] = vextq_u8(s[0], s[7], 5);
      s[6] = vextq_u8(s[0], s[7], 6);
      int16x8x2_t sum;
      sum.val[0] = sum.val[1] = vdupq_n_s16(0);
      sum = WienerHorizontal2(s[0], s[6], filter[0], sum);
      sum = WienerHorizontal2(s[1], s[5], filter[1], sum);
      WienerHorizontalSum(s + 2, filter, sum, *wiener_buffer);
      s[0] = s[7];
      *wiener_buffer += 16;
      x -= 16;
    } while (x != 0);
    src += src_stride;
  } while (--y != 0);
}

inline void WienerHorizontalTap5(const uint8_t* src, const ptrdiff_t src_stride,
                                 const ptrdiff_t width, const int height,
                                 const int16_t filter[4],
                                 int16_t** const wiener_buffer) {
  int y = height;
  do {
    const uint8_t* src_ptr = src;
    uint8x16_t s[6];
    s[0] = vld1q_u8(src_ptr);
    ptrdiff_t x = width;
    do {
      src_ptr += 16;
      s[5] = vld1q_u8(src_ptr);
      s[1] = vextq_u8(s[0], s[5], 1);
      s[2] = vextq_u8(s[0], s[5], 2);
      s[3] = vextq_u8(s[0], s[5], 3);
      s[4] = vextq_u8(s[0], s[5], 4);
      int16x8x2_t sum;
      sum.val[0] = sum.val[1] = vdupq_n_s16(0);
      sum = WienerHorizontal2(s[0], s[4], filter[1], sum);
      WienerHorizontalSum(s + 1, filter, sum, *wiener_buffer);
      s[0] = s[5];
      *wiener_buffer += 16;
      x -= 16;
    } while (x != 0);
    src += src_stride;
  } while (--y != 0);
}

inline void WienerHorizontalTap3(const uint8_t* src, const ptrdiff_t src_stride,
                                 const ptrdiff_t width, const int height,
                                 const int16_t filter[4],
                                 int16_t** const wiener_buffer) {
  int y = height;
  do {
    const uint8_t* src_ptr = src;
    uint8x16_t s[4];
    s[0] = vld1q_u8(src_ptr);
    ptrdiff_t x = width;
    do {
      src_ptr += 16;
      s[3] = vld1q_u8(src_ptr);
      s[1] = vextq_u8(s[0], s[3], 1);
      s[2] = vextq_u8(s[0], s[3], 2);
      int16x8x2_t sum;
      sum.val[0] = sum.val[1] = vdupq_n_s16(0);
      WienerHorizontalSum(s, filter, sum, *wiener_buffer);
      s[0] = s[3];
      *wiener_buffer += 16;
      x -= 16;
    } while (x != 0);
    src += src_stride;
  } while (--y != 0);
}

inline void WienerHorizontalTap1(const uint8_t* src, const ptrdiff_t src_stride,
                                 const ptrdiff_t width, const int height,
                                 int16_t** const wiener_buffer) {
  int y = height;
  do {
    const uint8_t* src_ptr = src;
    ptrdiff_t x = width;
    do {
      const uint8x16_t s = vld1q_u8(src_ptr);
      const uint8x8_t s0 = vget_low_u8(s);
      const uint8x8_t s1 = vget_high_u8(s);
      const int16x8_t d0 = vreinterpretq_s16_u16(vshll_n_u8(s0, 4));
      const int16x8_t d1 = vreinterpretq_s16_u16(vshll_n_u8(s1, 4));
      vst1q_s16(*wiener_buffer + 0, d0);
      vst1q_s16(*wiener_buffer + 8, d1);
      src_ptr += 16;
      *wiener_buffer += 16;
      x -= 16;
    } while (x != 0);
    src += src_stride;
  } while (--y != 0);
}

inline int32x4x2_t WienerVertical2(const int16x8_t a0, const int16x8_t a1,
                                   const int16_t filter,
                                   const int32x4x2_t sum) {
  const int16x8_t a = vaddq_s16(a0, a1);
  int32x4x2_t d;
  d.val[0] = vmlal_n_s16(sum.val[0], vget_low_s16(a), filter);
  d.val[1] = vmlal_n_s16(sum.val[1], vget_high_s16(a), filter);
  return d;
}

inline uint8x8_t WienerVertical(const int16x8_t a[3], const int16_t filter[4],
                                const int32x4x2_t sum) {
  int32x4x2_t d = WienerVertical2(a[0], a[2], filter[2], sum);
  d.val[0] = vmlal_n_s16(d.val[0], vget_low_s16(a[1]), filter[3]);
  d.val[1] = vmlal_n_s16(d.val[1], vget_high_s16(a[1]), filter[3]);
  const uint16x4_t sum_lo_16 = vqrshrun_n_s32(d.val[0], 11);
  const uint16x4_t sum_hi_16 = vqrshrun_n_s32(d.val[1], 11);
  return vqmovn_u16(vcombine_u16(sum_lo_16, sum_hi_16));
}

inline uint8x8_t WienerVerticalTap7Kernel(const int16_t* const wiener_buffer,
                                          const ptrdiff_t wiener_stride,
                                          const int16_t filter[4],
                                          int16x8_t a[7]) {
  int32x4x2_t sum;
  a[0] = vld1q_s16(wiener_buffer + 0 * wiener_stride);
  a[1] = vld1q_s16(wiener_buffer + 1 * wiener_stride);
  a[5] = vld1q_s16(wiener_buffer + 5 * wiener_stride);
  a[6] = vld1q_s16(wiener_buffer + 6 * wiener_stride);
  sum.val[0] = sum.val[1] = vdupq_n_s32(0);
  sum = WienerVertical2(a[0], a[6], filter[0], sum);
  sum = WienerVertical2(a[1], a[5], filter[1], sum);
  a[2] = vld1q_s16(wiener_buffer + 2 * wiener_stride);
  a[3] = vld1q_s16(wiener_buffer + 3 * wiener_stride);
  a[4] = vld1q_s16(wiener_buffer + 4 * wiener_stride);
  return WienerVertical(a + 2, filter, sum);
}

inline uint8x8x2_t WienerVerticalTap7Kernel2(const int16_t* const wiener_buffer,
                                             const ptrdiff_t wiener_stride,
                                             const int16_t filter[4]) {
  int16x8_t a[8];
  int32x4x2_t sum;
  uint8x8x2_t d;
  d.val[0] = WienerVerticalTap7Kernel(wiener_buffer, wiener_stride, filter, a);
  a[7] = vld1q_s16(wiener_buffer + 7 * wiener_stride);
  sum.val[0] = sum.val[1] = vdupq_n_s32(0);
  sum = WienerVertical2(a[1], a[7], filter[0], sum);
  sum = WienerVertical2(a[2], a[6], filter[1], sum);
  d.val[1] = WienerVertical(a + 3, filter, sum);
  return d;
}

inline void WienerVerticalTap7(const int16_t* wiener_buffer,
                               const ptrdiff_t width, const int height,
                               const int16_t filter[4], uint8_t* dst,
                               const ptrdiff_t dst_stride) {
  for (int y = height >> 1; y != 0; --y) {
    uint8_t* dst_ptr = dst;
    ptrdiff_t x = width;
    do {
      uint8x8x2_t d[2];
      d[0] = WienerVerticalTap7Kernel2(wiener_buffer + 0, width, filter);
      d[1] = WienerVerticalTap7Kernel2(wiener_buffer + 8, width, filter);
      vst1q_u8(dst_ptr, vcombine_u8(d[0].val[0], d[1].val[0]));
      vst1q_u8(dst_ptr + dst_stride, vcombine_u8(d[0].val[1], d[1].val[1]));
      wiener_buffer += 16;
      dst_ptr += 16;
      x -= 16;
    } while (x != 0);
    wiener_buffer += width;
    dst += 2 * dst_stride;
  }

  if ((height & 1) != 0) {
    ptrdiff_t x = width;
    do {
      int16x8_t a[7];
      const uint8x8_t d0 =
          WienerVerticalTap7Kernel(wiener_buffer + 0, width, filter, a);
      const uint8x8_t d1 =
          WienerVerticalTap7Kernel(wiener_buffer + 8, width, filter, a);
      vst1q_u8(dst, vcombine_u8(d0, d1));
      wiener_buffer += 16;
      dst += 16;
      x -= 16;
    } while (x != 0);
  }
}

inline uint8x8_t WienerVerticalTap5Kernel(const int16_t* const wiener_buffer,
                                          const ptrdiff_t wiener_stride,
                                          const int16_t filter[4],
                                          int16x8_t a[5]) {
  a[0] = vld1q_s16(wiener_buffer + 0 * wiener_stride);
  a[1] = vld1q_s16(wiener_buffer + 1 * wiener_stride);
  a[2] = vld1q_s16(wiener_buffer + 2 * wiener_stride);
  a[3] = vld1q_s16(wiener_buffer + 3 * wiener_stride);
  a[4] = vld1q_s16(wiener_buffer + 4 * wiener_stride);
  int32x4x2_t sum;
  sum.val[0] = sum.val[1] = vdupq_n_s32(0);
  sum = WienerVertical2(a[0], a[4], filter[1], sum);
  return WienerVertical(a + 1, filter, sum);
}

inline uint8x8x2_t WienerVerticalTap5Kernel2(const int16_t* const wiener_buffer,
                                             const ptrdiff_t wiener_stride,
                                             const int16_t filter[4]) {
  int16x8_t a[6];
  int32x4x2_t sum;
  uint8x8x2_t d;
  d.val[0] = WienerVerticalTap5Kernel(wiener_buffer, wiener_stride, filter, a);
  a[5] = vld1q_s16(wiener_buffer + 5 * wiener_stride);
  sum.val[0] = sum.val[1] = vdupq_n_s32(0);
  sum = WienerVertical2(a[1], a[5], filter[1], sum);
  d.val[1] = WienerVertical(a + 2, filter, sum);
  return d;
}

inline void WienerVerticalTap5(const int16_t* wiener_buffer,
                               const ptrdiff_t width, const int height,
                               const int16_t filter[4], uint8_t* dst,
                               const ptrdiff_t dst_stride) {
  for (int y = height >> 1; y != 0; --y) {
    uint8_t* dst_ptr = dst;
    ptrdiff_t x = width;
    do {
      uint8x8x2_t d[2];
      d[0] = WienerVerticalTap5Kernel2(wiener_buffer + 0, width, filter);
      d[1] = WienerVerticalTap5Kernel2(wiener_buffer + 8, width, filter);
      vst1q_u8(dst_ptr, vcombine_u8(d[0].val[0], d[1].val[0]));
      vst1q_u8(dst_ptr + dst_stride, vcombine_u8(d[0].val[1], d[1].val[1]));
      wiener_buffer += 16;
      dst_ptr += 16;
      x -= 16;
    } while (x != 0);
    wiener_buffer += width;
    dst += 2 * dst_stride;
  }

  if ((height & 1) != 0) {
    ptrdiff_t x = width;
    do {
      int16x8_t a[5];
      const uint8x8_t d0 =
          WienerVerticalTap5Kernel(wiener_buffer + 0, width, filter, a);
      const uint8x8_t d1 =
          WienerVerticalTap5Kernel(wiener_buffer + 8, width, filter, a);
      vst1q_u8(dst, vcombine_u8(d0, d1));
      wiener_buffer += 16;
      dst += 16;
      x -= 16;
    } while (x != 0);
  }
}

inline uint8x8_t WienerVerticalTap3Kernel(const int16_t* const wiener_buffer,
                                          const ptrdiff_t wiener_stride,
                                          const int16_t filter[4],
                                          int16x8_t a[3]) {
  a[0] = vld1q_s16(wiener_buffer + 0 * wiener_stride);
  a[1] = vld1q_s16(wiener_buffer + 1 * wiener_stride);
  a[2] = vld1q_s16(wiener_buffer + 2 * wiener_stride);
  int32x4x2_t sum;
  sum.val[0] = sum.val[1] = vdupq_n_s32(0);
  return WienerVertical(a, filter, sum);
}

inline uint8x8x2_t WienerVerticalTap3Kernel2(const int16_t* const wiener_buffer,
                                             const ptrdiff_t wiener_stride,
                                             const int16_t filter[4]) {
  int16x8_t a[4];
  int32x4x2_t sum;
  uint8x8x2_t d;
  d.val[0] = WienerVerticalTap3Kernel(wiener_buffer, wiener_stride, filter, a);
  a[3] = vld1q_s16(wiener_buffer + 3 * wiener_stride);
  sum.val[0] = sum.val[1] = vdupq_n_s32(0);
  d.val[1] = WienerVertical(a + 1, filter, sum);
  return d;
}

inline void WienerVerticalTap3(const int16_t* wiener_buffer,
                               const ptrdiff_t width, const int height,
                               const int16_t filter[4], uint8_t* dst,
                               const ptrdiff_t dst_stride) {
  for (int y = height >> 1; y != 0; --y) {
    uint8_t* dst_ptr = dst;
    ptrdiff_t x = width;
    do {
      uint8x8x2_t d[2];
      d[0] = WienerVerticalTap3Kernel2(wiener_buffer + 0, width, filter);
      d[1] = WienerVerticalTap3Kernel2(wiener_buffer + 8, width, filter);
      vst1q_u8(dst_ptr, vcombine_u8(d[0].val[0], d[1].val[0]));
      vst1q_u8(dst_ptr + dst_stride, vcombine_u8(d[0].val[1], d[1].val[1]));
      wiener_buffer += 16;
      dst_ptr += 16;
      x -= 16;
    } while (x != 0);
    wiener_buffer += width;
    dst += 2 * dst_stride;
  }

  if ((height & 1) != 0) {
    ptrdiff_t x = width;
    do {
      int16x8_t a[3];
      const uint8x8_t d0 =
          WienerVerticalTap3Kernel(wiener_buffer + 0, width, filter, a);
      const uint8x8_t d1 =
          WienerVerticalTap3Kernel(wiener_buffer + 8, width, filter, a);
      vst1q_u8(dst, vcombine_u8(d0, d1));
      wiener_buffer += 16;
      dst += 16;
      x -= 16;
    } while (x != 0);
  }
}

inline void WienerVerticalTap1Kernel(const int16_t* const wiener_buffer,
                                     uint8_t* const dst) {
  const int16x8_t a0 = vld1q_s16(wiener_buffer + 0);
  const int16x8_t a1 = vld1q_s16(wiener_buffer + 8);
  const uint8x8_t d0 = vqrshrun_n_s16(a0, 4);
  const uint8x8_t d1 = vqrshrun_n_s16(a1, 4);
  vst1q_u8(dst, vcombine_u8(d0, d1));
}

inline void WienerVerticalTap1(const int16_t* wiener_buffer,
                               const ptrdiff_t width, const int height,
                               uint8_t* dst, const ptrdiff_t dst_stride) {
  for (int y = height >> 1; y != 0; --y) {
    uint8_t* dst_ptr = dst;
    ptrdiff_t x = width;
    do {
      WienerVerticalTap1Kernel(wiener_buffer, dst_ptr);
      WienerVerticalTap1Kernel(wiener_buffer + width, dst_ptr + dst_stride);
      wiener_buffer += 16;
      dst_ptr += 16;
      x -= 16;
    } while (x != 0);
    wiener_buffer += width;
    dst += 2 * dst_stride;
  }

  if ((height & 1) != 0) {
    ptrdiff_t x = width;
    do {
      WienerVerticalTap1Kernel(wiener_buffer, dst);
      wiener_buffer += 16;
      dst += 16;
      x -= 16;
    } while (x != 0);
  }
}

// For width 16 and up, store the horizontal results, and then do the vertical
// filter row by row. This is faster than doing it column by column when
// considering cache issues.
void WienerFilter_NEON(const void* const source, void* const dest,
                       const RestorationUnitInfo& restoration_info,
                       const ptrdiff_t source_stride,
                       const ptrdiff_t dest_stride, const int width,
                       const int height, RestorationBuffer* const buffer) {
  constexpr int kCenterTap = kWienerFilterTaps / 2;
  const int16_t* const number_leading_zero_coefficients =
      restoration_info.wiener_info.number_leading_zero_coefficients;
  const int number_rows_to_skip = std::max(
      static_cast<int>(number_leading_zero_coefficients[WienerInfo::kVertical]),
      1);
  const ptrdiff_t wiener_stride = Align(width, 16);
  int16_t* const wiener_buffer_vertical = buffer->wiener_buffer;
  // The values are saturated to 13 bits before storing.
  int16_t* wiener_buffer_horizontal =
      wiener_buffer_vertical + number_rows_to_skip * wiener_stride;
  int16_t filter_horizontal[(kWienerFilterTaps + 1) / 2];
  int16_t filter_vertical[(kWienerFilterTaps + 1) / 2];
  PopulateWienerCoefficients(restoration_info, WienerInfo::kHorizontal,
                             filter_horizontal);
  PopulateWienerCoefficients(restoration_info, WienerInfo::kVertical,
                             filter_vertical);

  // horizontal filtering.
  // Over-reads up to 15 - |kRestorationHorizontalBorder| values.
  const int height_horizontal =
      height + kWienerFilterTaps - 1 - 2 * number_rows_to_skip;
  const auto* const src = static_cast<const uint8_t*>(source) -
                          (kCenterTap - number_rows_to_skip) * source_stride;
  if (number_leading_zero_coefficients[WienerInfo::kHorizontal] == 0) {
    WienerHorizontalTap7(src - 3, source_stride, wiener_stride,
                         height_horizontal, filter_horizontal,
                         &wiener_buffer_horizontal);
  } else if (number_leading_zero_coefficients[WienerInfo::kHorizontal] == 1) {
    WienerHorizontalTap5(src - 2, source_stride, wiener_stride,
                         height_horizontal, filter_horizontal,
                         &wiener_buffer_horizontal);
  } else if (number_leading_zero_coefficients[WienerInfo::kHorizontal] == 2) {
    // The maximum over-reads happen here.
    WienerHorizontalTap3(src - 1, source_stride, wiener_stride,
                         height_horizontal, filter_horizontal,
                         &wiener_buffer_horizontal);
  } else {
    assert(number_leading_zero_coefficients[WienerInfo::kHorizontal] == 3);
    WienerHorizontalTap1(src, source_stride, wiener_stride, height_horizontal,
                         &wiener_buffer_horizontal);
  }

  // vertical filtering.
  // Over-writes up to 15 values.
  auto* dst = static_cast<uint8_t*>(dest);
  if (number_leading_zero_coefficients[WienerInfo::kVertical] == 0) {
    // Because the top row of |source| is a duplicate of the second row, and the
    // bottom row of |source| is a duplicate of its above row, we can duplicate
    // the top and bottom row of |wiener_buffer| accordingly.
    memcpy(wiener_buffer_horizontal, wiener_buffer_horizontal - wiener_stride,
           sizeof(*wiener_buffer_horizontal) * wiener_stride);
    memcpy(buffer->wiener_buffer, buffer->wiener_buffer + wiener_stride,
           sizeof(*buffer->wiener_buffer) * wiener_stride);
    WienerVerticalTap7(wiener_buffer_vertical, wiener_stride, height,
                       filter_vertical, dst, dest_stride);
  } else if (number_leading_zero_coefficients[WienerInfo::kVertical] == 1) {
    WienerVerticalTap5(wiener_buffer_vertical + wiener_stride, wiener_stride,
                       height, filter_vertical, dst, dest_stride);
  } else if (number_leading_zero_coefficients[WienerInfo::kVertical] == 2) {
    WienerVerticalTap3(wiener_buffer_vertical + 2 * wiener_stride,
                       wiener_stride, height, filter_vertical, dst,
                       dest_stride);
  } else {
    assert(number_leading_zero_coefficients[WienerInfo::kVertical] == 3);
    WienerVerticalTap1(wiener_buffer_vertical + 3 * wiener_stride,
                       wiener_stride, height, dst, dest_stride);
  }
}

//------------------------------------------------------------------------------
// SGR

template <int n>
inline uint16x4_t CalculateSgrMA2(const uint32x4_t sum_sq, const uint16x4_t sum,
                                  const uint32_t s) {
  // a = |sum_sq|
  // d = |sum|
  // p = (a * n < d * d) ? 0 : a * n - d * d;
  const uint32x4_t dxd = vmull_u16(sum, sum);
  const uint32x4_t axn = vmulq_n_u32(sum_sq, n);
  // Ensure |p| does not underflow by using saturating subtraction.
  const uint32x4_t p = vqsubq_u32(axn, dxd);

  // z = RightShiftWithRounding(p * s, kSgrProjScaleBits);
  const uint32x4_t pxs = vmulq_n_u32(p, s);
  // vrshrn_n_u32() (narrowing shift) can only shift by 16 and kSgrProjScaleBits
  // is 20.
  const uint32x4_t shifted = vrshrq_n_u32(pxs, kSgrProjScaleBits);
  return vmovn_u32(shifted);
}

inline uint16x4_t CalculateIntermediate4(const uint8x8_t sgr_ma2,
                                         const uint16x4_t sum,
                                         const uint32_t one_over_n) {
  // b2 = ((1 << kSgrProjSgrBits) - a2) * b * one_over_n
  // 1 << kSgrProjSgrBits = 256
  // |a2| = [1, 256]
  // |sgr_ma2| max value = 255
  // |sum| is a box sum with radius 1 or 2.
  // For the first pass radius is 2. Maximum value is 5x5x255 = 6375.
  // For the second pass radius is 1. Maximum value is 3x3x255 = 2295.
  // |one_over_n| = ((1 << kSgrProjReciprocalBits) + (n >> 1)) / n
  // When radius is 2 |n| is 25. |one_over_n| is 164.
  // When radius is 1 |n| is 9. |one_over_n| is 455.
  const uint16x8_t sgr_ma2q = vmovl_u8(sgr_ma2);
  const uint32x4_t m = vmull_u16(vget_high_u16(sgr_ma2q), sum);
  const uint32x4_t b2 = vmulq_n_u32(m, one_over_n);
  // static_cast<int>(RightShiftWithRounding(b2, kSgrProjReciprocalBits));
  // |kSgrProjReciprocalBits| is 12.
  // Radius 2: 255 * 6375 * 164 >> 12 = 65088 (16 bits).
  // Radius 1: 255 * 2295 * 455 >> 12 = 65009 (16 bits).
  return vrshrn_n_u32(b2, kSgrProjReciprocalBits);
}

inline uint16x8_t CalculateIntermediate8(const uint8x8_t sgr_ma2,
                                         const uint16x8_t sum,
                                         const uint32_t one_over_n) {
  // b2 = ((1 << kSgrProjSgrBits) - a2) * b * one_over_n
  // 1 << kSgrProjSgrBits = 256
  // |a2| = [1, 256]
  // |sgr_ma2| max value = 255
  // |sum| is a box sum with radius 1 or 2.
  // For the first pass radius is 2. Maximum value is 5x5x255 = 6375.
  // For the second pass radius is 1. Maximum value is 3x3x255 = 2295.
  // |one_over_n| = ((1 << kSgrProjReciprocalBits) + (n >> 1)) / n
  // When radius is 2 |n| is 25. |one_over_n| is 164.
  // When radius is 1 |n| is 9. |one_over_n| is 455.
  const uint16x8_t sgr_ma2q = vmovl_u8(sgr_ma2);
  const uint32x4_t m0 = vmull_u16(vget_low_u16(sgr_ma2q), vget_low_u16(sum));
  const uint32x4_t m1 = vmull_u16(vget_high_u16(sgr_ma2q), vget_high_u16(sum));
  const uint32x4_t m2 = vmulq_n_u32(m0, one_over_n);
  const uint32x4_t m3 = vmulq_n_u32(m1, one_over_n);
  // static_cast<int>(RightShiftWithRounding(b2, kSgrProjReciprocalBits));
  // |kSgrProjReciprocalBits| is 12.
  // Radius 2: 255 * 6375 * 164 >> 12 = 65088 (16 bits).
  // Radius 1: 255 * 2295 * 455 >> 12 = 65009 (16 bits).
  const uint16x4_t b2_lo = vrshrn_n_u32(m2, kSgrProjReciprocalBits);
  const uint16x4_t b2_hi = vrshrn_n_u32(m3, kSgrProjReciprocalBits);
  return vcombine_u16(b2_lo, b2_hi);
}

inline uint16x8_t Sum3(const uint8x8_t left, const uint8x8_t middle,
                       const uint8x8_t right) {
  const uint16x8_t sum = vaddl_u8(left, middle);
  return vaddw_u8(sum, right);
}

inline uint16x8_t Sum3_16(const uint16x8_t left, const uint16x8_t middle,
                          const uint16x8_t right) {
  const uint16x8_t sum = vaddq_u16(left, middle);
  return vaddq_u16(sum, right);
}

inline uint32x4_t Sum3_32(const uint32x4_t left, const uint32x4_t middle,
                          const uint32x4_t right) {
  const uint32x4_t sum = vaddq_u32(left, middle);
  return vaddq_u32(sum, right);
}

inline uint32x4_t Sum3_32(const uint32x4_t a[3]) {
  return Sum3_32(a[0], a[1], a[2]);
}

inline uint32x4x2_t Sum3_32(const uint32x4x2_t a[3]) {
  uint32x4x2_t d;
  d.val[0] = Sum3_32(a[0].val[0], a[1].val[0], a[2].val[0]);
  d.val[1] = Sum3_32(a[0].val[1], a[1].val[1], a[2].val[1]);
  return d;
}

inline uint16x4_t Sum3(const uint16x4_t a[3]) {
  const uint16x4_t sum = vadd_u16(a[0], a[1]);
  return vadd_u16(sum, a[2]);
}

inline uint16x8_t Sum3_16(const uint16x8_t a[3]) {
  return Sum3_16(a[0], a[1], a[2]);
}

inline uint16x8_t Sum3W_16(const uint8x8_t left, const uint8x8_t middle,
                           const uint8x8_t right) {
  const uint16x8_t sum = vaddl_u8(left, middle);
  return vaddw_u8(sum, right);
}

inline uint32x4_t Sum3W_32(const uint16x4_t left, const uint16x4_t middle,
                           const uint16x4_t right) {
  const uint32x4_t sum = vaddl_u16(left, middle);
  return vaddw_u16(sum, right);
}

inline uint32x4x2_t Sum3W(const uint16x8_t a[3]) {
  const uint16x4_t low0 = vget_low_u16(a[0]);
  const uint16x4_t low1 = vget_low_u16(a[1]);
  const uint16x4_t low2 = vget_low_u16(a[2]);
  const uint16x4_t high0 = vget_high_u16(a[0]);
  const uint16x4_t high1 = vget_high_u16(a[1]);
  const uint16x4_t high2 = vget_high_u16(a[2]);
  uint32x4x2_t sum;
  sum.val[0] = Sum3W_32(low0, low1, low2);
  sum.val[1] = Sum3W_32(high0, high1, high2);
  return sum;
}

template <int index>
inline uint32x4_t Sum3WLo(const uint16x8x2_t a[3]) {
  const uint16x4_t low0 = vget_low_u16(a[0].val[index]);
  const uint16x4_t low1 = vget_low_u16(a[1].val[index]);
  const uint16x4_t low2 = vget_low_u16(a[2].val[index]);
  return Sum3W_32(low0, low1, low2);
}

inline uint16x8_t Sum5(const uint8x8_t a[5]) {
  const uint16x8_t sum01 = vaddl_u8(a[0], a[1]);
  const uint16x8_t sum23 = vaddl_u8(a[2], a[3]);
  const uint16x8_t sum0123 = vaddq_u16(sum01, sum23);
  return vaddw_u8(sum0123, a[4]);
}

inline uint16x4_t Sum5(const uint16x4_t a[5]) {
  const uint16x4_t sum01 = vadd_u16(a[0], a[1]);
  const uint16x4_t sum23 = vadd_u16(a[2], a[3]);
  const uint16x4_t sum = vadd_u16(sum01, sum23);
  return vadd_u16(sum, a[4]);
}

inline uint16x8_t Sum5_16(const uint16x8_t a[5]) {
  const uint16x8_t sum01 = vaddq_u16(a[0], a[1]);
  const uint16x8_t sum23 = vaddq_u16(a[2], a[3]);
  const uint16x8_t sum = vaddq_u16(sum01, sum23);
  return vaddq_u16(sum, a[4]);
}

inline uint32x4_t Sum5_32(const uint32x4_t a0, const uint32x4_t a1,
                          const uint32x4_t a2, const uint32x4_t a3,
                          const uint32x4_t a4) {
  const uint32x4_t sum01 = vaddq_u32(a0, a1);
  const uint32x4_t sum23 = vaddq_u32(a2, a3);
  const uint32x4_t sum = vaddq_u32(sum01, sum23);
  return vaddq_u32(sum, a4);
}

inline uint32x4_t Sum5_32(const uint32x4_t a[5]) {
  return Sum5_32(a[0], a[1], a[2], a[3], a[4]);
}

inline uint32x4x2_t Sum5_32(const uint32x4x2_t a[5]) {
  uint32x4x2_t d;
  d.val[0] =
      Sum5_32(a[0].val[0], a[1].val[0], a[2].val[0], a[3].val[0], a[4].val[0]);
  d.val[1] =
      Sum5_32(a[0].val[1], a[1].val[1], a[2].val[1], a[3].val[1], a[4].val[1]);
  return d;
}

inline uint32x4_t Sum5W_32(const uint16x4_t a[5]) {
  const uint32x4_t sum01 = vaddl_u16(a[0], a[1]);
  const uint32x4_t sum23 = vaddl_u16(a[2], a[3]);
  const uint32x4_t sum0123 = vaddq_u32(sum01, sum23);
  return vaddw_u16(sum0123, a[4]);
}

inline uint32x4x2_t Sum5W_32x2(const uint16x8_t a[5]) {
  uint32x4x2_t sum;
  uint16x4_t low[5], high[5];
  low[0] = vget_low_u16(a[0]);
  low[1] = vget_low_u16(a[1]);
  low[2] = vget_low_u16(a[2]);
  low[3] = vget_low_u16(a[3]);
  low[4] = vget_low_u16(a[4]);
  high[0] = vget_high_u16(a[0]);
  high[1] = vget_high_u16(a[1]);
  high[2] = vget_high_u16(a[2]);
  high[3] = vget_high_u16(a[3]);
  high[4] = vget_high_u16(a[4]);
  sum.val[0] = Sum5W_32(low);
  sum.val[1] = Sum5W_32(high);
  return sum;
}

template <int index>
inline uint32x4_t Sum5WLo(const uint16x8x2_t a[5]) {
  uint16x4_t low[5];
  low[0] = vget_low_u16(a[0].val[index]);
  low[1] = vget_low_u16(a[1].val[index]);
  low[2] = vget_low_u16(a[2].val[index]);
  low[3] = vget_low_u16(a[3].val[index]);
  low[4] = vget_low_u16(a[4].val[index]);
  return Sum5W_32(low);
}

inline uint16x4_t Sum3Horizontal(const uint8x8_t a) {
  const uint8x8_t left = RightShift<8>(a);
  const uint8x8_t middle = RightShift<16>(a);
  const uint8x8_t right = RightShift<24>(a);
  return vget_low_u16(Sum3(left, middle, right));
}

inline uint16x8_t Sum3Horizontal(const uint8x16_t a) {
  const uint8x8_t left = VshrU128<1>(a);
  const uint8x8_t middle = VshrU128<2>(a);
  const uint8x8_t right = VshrU128<3>(a);
  return Sum3(left, middle, right);
}

inline uint32x4_t Sum3WHorizontal(const uint16x8_t a) {
  const uint16x4_t left = VshrU128<2>(a);
  const uint16x4_t middle = VshrU128<4>(a);
  const uint16x4_t right = VshrU128<6>(a);
  return Sum3W_32(left, middle, right);
}

inline uint32x4x2_t Sum3WHorizontal(const uint16x8x2_t a) {
  uint16x8_t b[3];
  b[0] = VshrU128<2>(a);
  b[1] = VshrU128<4>(a);
  b[2] = VshrU128<6>(a);
  return Sum3W(b);
}

void Sum3Horizontal(const uint8_t* const src, uint16x4_t* const row,
                    uint32x4_t* const row_sq) {
  const uint8x8_t s = vld1_u8(src);
  const uint16x8_t sq = vmull_u8(s, s);
  *row = Sum3Horizontal(s);
  *row_sq = Sum3WHorizontal(sq);
}

void Sum3Horizontal(const uint8_t* const src, uint16x8_t* const row,
                    uint32x4x2_t* const row_sq) {
  const uint8x16_t s = vld1q_u8(src);
  uint16x8x2_t sq;
  sq.val[0] = vmull_u8(vget_low_u8(s), vget_low_u8(s));
  sq.val[1] = vmull_u8(vget_high_u8(s), vget_high_u8(s));
  *row = Sum3Horizontal(s);
  *row_sq = Sum3WHorizontal(sq);
}

inline uint16x4_t Sum5Horizontal(const uint8x8_t a) {
  uint8x8_t s[5];
  s[0] = a;
  s[1] = RightShift<8>(a);
  s[2] = RightShift<16>(a);
  s[3] = RightShift<24>(a);
  s[4] = RightShift<32>(a);
  return vget_low_u16(Sum5(s));
}

inline uint16x8_t Sum5Horizontal(const uint8x16_t a) {
  uint8x8_t s[5];
  s[0] = vget_low_u8(a);
  s[1] = VshrU128<1>(a);
  s[2] = VshrU128<2>(a);
  s[3] = VshrU128<3>(a);
  s[4] = VshrU128<4>(a);
  return Sum5(s);
}

inline uint32x4_t Sum5WHorizontal(const uint16x8_t a) {
  uint16x4_t s[5];
  s[0] = vget_low_u16(a);
  s[1] = VshrU128<2>(a);
  s[2] = VshrU128<4>(a);
  s[3] = VshrU128<6>(a);
  s[4] = vget_high_u16(a);
  return Sum5W_32(s);
}

inline uint32x4x2_t Sum5WHorizontal(const uint16x8x2_t a) {
  uint16x8_t s[5];
  s[0] = a.val[0];
  s[1] = VshrU128<2>(a);
  s[2] = VshrU128<4>(a);
  s[3] = VshrU128<6>(a);
  s[4] = VshrU128<8>(a);
  return Sum5W_32x2(s);
}

void Sum5Horizontal(const uint8_t* const src, uint16x4_t* const row,
                    uint32x4_t* const row_sq) {
  const uint8x8_t s = vld1_u8(src);
  const uint16x8_t sq = vmull_u8(s, s);
  *row = Sum5Horizontal(s);
  *row_sq = Sum5WHorizontal(sq);
}

void Sum5Horizontal(const uint8_t* const src, uint16x8_t* const row,
                    uint32x4x2_t* const row_sq) {
  const uint8x16_t s = vld1q_u8(src);
  uint16x8x2_t sq;
  sq.val[0] = vmull_u8(vget_low_u8(s), vget_low_u8(s));
  sq.val[1] = vmull_u8(vget_high_u8(s), vget_high_u8(s));
  *row = Sum5Horizontal(s);
  *row_sq = Sum5WHorizontal(sq);
}

void SumHorizontal(uint8x8_t a[5], uint16x8_t* const row3,
                   uint16x8_t* const row) {
  const uint16x8_t sum04 = vaddl_u8(a[0], a[4]);
  const uint16x8_t sum12 = vaddl_u8(a[1], a[2]);
  *row3 = vaddw_u8(sum12, a[3]);
  *row = vaddq_u16(sum04, *row3);
}

void SumHorizontal(uint16x4_t a[5], uint32x4_t* const row_sq3,
                   uint32x4_t* const row_sq) {
  const uint32x4_t sum04 = vaddl_u16(a[0], a[4]);
  const uint32x4_t sum12 = vaddl_u16(a[1], a[2]);
  *row_sq3 = vaddw_u16(sum12, a[3]);
  *row_sq = vaddq_u32(sum04, *row_sq3);
}

void SumHorizontal(const uint8_t* const src, uint16x4_t* const row3,
                   uint16x4_t* const row, uint32x4_t* const row_sq3,
                   uint32x4_t* const row_sq) {
  const uint8x8_t s = vld1_u8(src);
  const uint16x8_t sq = vmull_u8(s, s);
  {
    uint8x8_t a[5];
    a[0] = s;
    a[1] = RightShift<8>(s);
    a[2] = RightShift<16>(s);
    a[3] = RightShift<24>(s);
    a[4] = RightShift<32>(s);
    uint16x8_t sum123;
    uint16x8_t sum01234;
    SumHorizontal(a, &sum123, &sum01234);
    *row3 = vget_low_u16(sum123);
    *row = vget_low_u16(sum01234);
  }
  {
    uint16x4_t a[5];
    a[0] = vget_low_u16(sq);
    a[1] = VshrU128<2>(sq);
    a[2] = VshrU128<4>(sq);
    a[3] = VshrU128<6>(sq);
    a[4] = vget_high_u16(sq);
    const uint32x4_t sum04 = vaddl_u16(a[0], a[4]);
    const uint32x4_t sum12 = vaddl_u16(a[1], a[2]);
    *row_sq3 = vaddw_u16(sum12, a[3]);
    *row_sq = vaddq_u32(sum04, *row_sq3);
    SumHorizontal(a, row_sq3, row_sq);
  }
}

void SumHorizontal(const uint8_t* const src, uint16x8_t* const row3,
                   uint16x8_t* const row, uint32x4x2_t* const row_sq3,
                   uint32x4x2_t* const row_sq) {
  const uint8x16_t s = vld1q_u8(src);
  uint16x8x2_t sq;
  sq.val[0] = vmull_u8(vget_low_u8(s), vget_low_u8(s));
  sq.val[1] = vmull_u8(vget_high_u8(s), vget_high_u8(s));
  {
    uint8x8_t a[5];
    a[0] = vget_low_u8(s);
    a[1] = VshrU128<1>(s);
    a[2] = VshrU128<2>(s);
    a[3] = VshrU128<3>(s);
    a[4] = VshrU128<4>(s);
    SumHorizontal(a, row3, row);
  }
  {
    uint16x8_t a[5];
    a[0] = sq.val[0];
    a[1] = VshrU128<2>(sq);
    a[2] = VshrU128<4>(sq);
    a[3] = VshrU128<6>(sq);
    a[4] = VshrU128<8>(sq);
    uint16x4_t low[5], high[5];
    low[0] = vget_low_u16(a[0]);
    low[1] = vget_low_u16(a[1]);
    low[2] = vget_low_u16(a[2]);
    low[3] = vget_low_u16(a[3]);
    low[4] = vget_low_u16(a[4]);
    high[0] = vget_high_u16(a[0]);
    high[1] = vget_high_u16(a[1]);
    high[2] = vget_high_u16(a[2]);
    high[3] = vget_high_u16(a[3]);
    high[4] = vget_high_u16(a[4]);
    SumHorizontal(low, &row_sq3->val[0], &row_sq->val[0]);
    SumHorizontal(high, &row_sq3->val[1], &row_sq->val[1]);
  }
}

template <int size>
inline void BoxFilterPreProcess4(uint16x4_t* const row,
                                 uint32x4_t* const row_sq, const uint32_t scale,
                                 uint16_t* const dst) {
  static_assert(size == 3 || size == 5, "");
  // Number of elements in the box being summed.
  constexpr uint32_t n = size * size;
  constexpr uint32_t one_over_n =
      ((1 << kSgrProjReciprocalBits) + (n >> 1)) / n;
  uint16x4_t sum;
  uint32x4_t sum_sq;
  if (size == 3) {
    sum = Sum3(row);
    sum_sq = Sum3_32(row_sq);
  }
  if (size == 5) {
    sum = Sum5(row);
    sum_sq = Sum5_32(row_sq);
  }
  const uint16x4_t z0 = CalculateSgrMA2<n>(sum_sq, sum, scale);
  const uint16x4_t z = vmin_u16(z0, vdup_n_u16(255));
  // Using vget_lane_s16() can save a sign extension instruction.
  // Add 4 0s for memory initialization purpose only.
  const uint8_t lookup[8] = {
      0,
      0,
      0,
      0,
      kSgrMa2Lookup[vget_lane_s16(vreinterpret_s16_u16(z), 0)],
      kSgrMa2Lookup[vget_lane_s16(vreinterpret_s16_u16(z), 1)],
      kSgrMa2Lookup[vget_lane_s16(vreinterpret_s16_u16(z), 2)],
      kSgrMa2Lookup[vget_lane_s16(vreinterpret_s16_u16(z), 3)]};
  const uint8x8_t sgr_ma2 = vld1_u8(lookup);
  const uint16x4_t b2 = CalculateIntermediate4(sgr_ma2, sum, one_over_n);
  const uint16x8_t sgr_ma2_b2 = vcombine_u16(vreinterpret_u16_u8(sgr_ma2), b2);
  vst1q_u16(dst, sgr_ma2_b2);
}

template <int size>
inline void BoxFilterPreProcess8(const uint16x8_t* const row,
                                 const uint32x4x2_t* const row_sq,
                                 const uint32_t s, uint8x8_t* const sgr_ma2,
                                 uint16x8_t* const b2, uint16_t* const dst) {
  static_assert(size == 3 || size == 5, "");
  // Number of elements in the box being summed.
  constexpr uint32_t n = size * size;
  constexpr uint32_t one_over_n =
      ((1 << kSgrProjReciprocalBits) + (n >> 1)) / n;
  uint16x8_t sum;
  uint32x4x2_t sum_sq;
  if (size == 3) {
    sum = Sum3_16(row);
    sum_sq = Sum3_32(row_sq);
  }
  if (size == 5) {
    sum = Sum5_16(row);
    sum_sq = Sum5_32(row_sq);
  }
  const uint16x4_t z0 = CalculateSgrMA2<n>(sum_sq.val[0], vget_low_u16(sum), s);
  const uint16x4_t z1 =
      CalculateSgrMA2<n>(sum_sq.val[1], vget_high_u16(sum), s);
  const uint16x8_t z01 = vcombine_u16(z0, z1);
  // Using vqmovn_u16() needs an extra sign extension instruction.
  const uint16x8_t z = vminq_u16(z01, vdupq_n_u16(255));
  // Using vgetq_lane_s16() can save the sign extension instruction.
  const uint8_t lookup[8] = {
      kSgrMa2Lookup[vgetq_lane_s16(vreinterpretq_s16_u16(z), 0)],
      kSgrMa2Lookup[vgetq_lane_s16(vreinterpretq_s16_u16(z), 1)],
      kSgrMa2Lookup[vgetq_lane_s16(vreinterpretq_s16_u16(z), 2)],
      kSgrMa2Lookup[vgetq_lane_s16(vreinterpretq_s16_u16(z), 3)],
      kSgrMa2Lookup[vgetq_lane_s16(vreinterpretq_s16_u16(z), 4)],
      kSgrMa2Lookup[vgetq_lane_s16(vreinterpretq_s16_u16(z), 5)],
      kSgrMa2Lookup[vgetq_lane_s16(vreinterpretq_s16_u16(z), 6)],
      kSgrMa2Lookup[vgetq_lane_s16(vreinterpretq_s16_u16(z), 7)]};
  *sgr_ma2 = vld1_u8(lookup);
  *b2 = CalculateIntermediate8(*sgr_ma2, sum, one_over_n);
  const uint16x8_t sgr_ma2_b2 =
      vcombine_u16(vreinterpret_u16_u8(*sgr_ma2), vget_high_u16(*b2));
  vst1q_u16(dst, sgr_ma2_b2);
}

inline void Prepare3_8(const uint8x8_t a[2], uint8x8_t* const left,
                       uint8x8_t* const middle, uint8x8_t* const right) {
  *left = vext_u8(a[0], a[1], 5);
  *middle = vext_u8(a[0], a[1], 6);
  *right = vext_u8(a[0], a[1], 7);
}

inline void Prepare3_16(const uint16x8_t a[2], uint16x8_t* const left,
                        uint16x8_t* const middle, uint16x8_t* const right) {
  *left = vextq_u16(a[0], a[1], 5);
  *middle = vextq_u16(a[0], a[1], 6);
  *right = vextq_u16(a[0], a[1], 7);
}

inline uint16x8_t Sum343(const uint8x8_t a[2]) {
  uint8x8_t left, middle, right;
  Prepare3_8(a, &left, &middle, &right);
  const uint16x8_t sum = Sum3W_16(left, middle, right);
  const uint16x8_t sum3 = Sum3_16(sum, sum, sum);
  return vaddw_u8(sum3, middle);
}

inline void Sum343_444(const uint8x8_t a[2], uint16x8_t* const sum343,
                       uint16x8_t* const sum444) {
  uint8x8_t left, middle, right;
  Prepare3_8(a, &left, &middle, &right);
  const uint16x8_t sum = Sum3W_16(left, middle, right);
  const uint16x8_t sum3 = Sum3_16(sum, sum, sum);
  *sum343 = vaddw_u8(sum3, middle);
  *sum444 = vshlq_n_u16(sum, 2);
}

inline uint32x4x2_t Sum343W(const uint16x8_t a[2]) {
  uint16x8_t left, middle, right;
  uint32x4x2_t d;
  Prepare3_16(a, &left, &middle, &right);
  d.val[0] =
      Sum3W_32(vget_low_u16(left), vget_low_u16(middle), vget_low_u16(right));
  d.val[1] = Sum3W_32(vget_high_u16(left), vget_high_u16(middle),
                      vget_high_u16(right));
  d.val[0] = Sum3_32(d.val[0], d.val[0], d.val[0]);
  d.val[1] = Sum3_32(d.val[1], d.val[1], d.val[1]);
  d.val[0] = vaddw_u16(d.val[0], vget_low_u16(middle));
  d.val[1] = vaddw_u16(d.val[1], vget_high_u16(middle));
  return d;
}

inline void Sum343_444W(const uint16x8_t a[2], uint32x4x2_t* const sum343,
                        uint32x4x2_t* const sum444) {
  uint16x8_t left, middle, right;
  Prepare3_16(a, &left, &middle, &right);
  sum444->val[0] =
      Sum3W_32(vget_low_u16(left), vget_low_u16(middle), vget_low_u16(right));
  sum444->val[1] = Sum3W_32(vget_high_u16(left), vget_high_u16(middle),
                            vget_high_u16(right));
  sum343->val[0] = Sum3_32(sum444->val[0], sum444->val[0], sum444->val[0]);
  sum343->val[1] = Sum3_32(sum444->val[1], sum444->val[1], sum444->val[1]);
  sum343->val[0] = vaddw_u16(sum343->val[0], vget_low_u16(middle));
  sum343->val[1] = vaddw_u16(sum343->val[1], vget_high_u16(middle));
  sum444->val[0] = vshlq_n_u32(sum444->val[0], 2);
  sum444->val[1] = vshlq_n_u32(sum444->val[1], 2);
}

inline uint16x8_t Sum565(const uint8x8_t a[2]) {
  uint8x8_t left, middle, right;
  Prepare3_8(a, &left, &middle, &right);
  const uint16x8_t sum = Sum3W_16(left, middle, right);
  const uint16x8_t sum4 = vshlq_n_u16(sum, 2);
  const uint16x8_t sum5 = vaddq_u16(sum4, sum);
  return vaddw_u8(sum5, middle);
}

inline uint32x4_t Sum565W(const uint16x8_t a) {
  const uint16x4_t left = VshrU128<2>(a);
  const uint16x4_t middle = VshrU128<4>(a);
  const uint16x4_t right = VshrU128<6>(a);
  const uint32x4_t sum = Sum3W_32(left, middle, right);
  const uint32x4_t sum4 = vshlq_n_u32(sum, 2);
  const uint32x4_t sum5 = vaddq_u32(sum4, sum);
  return vaddw_u16(sum5, middle);
}

inline uint32x4_t Sum565W(const uint16x8_t a, const uint16x8_t b) {
  const uint16x4_t left = vext_u16(vget_high_u16(a), vget_low_u16(b), 1);
  const uint16x4_t middle = vext_u16(vget_high_u16(a), vget_low_u16(b), 2);
  const uint16x4_t right = vext_u16(vget_high_u16(a), vget_low_u16(b), 3);
  const uint32x4_t sum = Sum3W_32(left, middle, right);
  const uint32x4_t sum4 = vshlq_n_u32(sum, 2);
  const uint32x4_t sum5 = vaddq_u32(sum4, sum);
  return vaddw_u16(sum5, middle);
}

// RightShiftWithRounding(
//   (a * src_ptr[x] + b), kSgrProjSgrBits + shift - kSgrProjRestoreBits);
template <int shift>
inline uint16x4_t FilterOutput(const uint16x4_t src, const uint16x4_t a,
                               const uint32x4_t b) {
  // a: 256 * 32 = 8192 (14 bits)
  // b: 65088 * 32 = 2082816 (21 bits)
  const uint32x4_t axsrc = vmull_u16(a, src);
  // v: 8192 * 255 + 2082816 = 4171876 (22 bits)
  const uint32x4_t v = vaddq_u32(axsrc, b);

  // kSgrProjSgrBits = 8
  // kSgrProjRestoreBits = 4
  // shift = 4 or 5
  // v >> 8 or 9
  // 22 bits >> 8 = 14 bits
  return vrshrn_n_u32(v, kSgrProjSgrBits + shift - kSgrProjRestoreBits);
}

template <int shift>
inline int16x8_t CalculateFilteredOutput(const uint8x8_t src,
                                         const uint16x8_t a,
                                         const uint32x4x2_t b) {
  const uint16x8_t src_u16 = vmovl_u8(src);
  const uint16x4_t dst_lo =
      FilterOutput<shift>(vget_low_u16(src_u16), vget_low_u16(a), b.val[0]);
  const uint16x4_t dst_hi =
      FilterOutput<shift>(vget_high_u16(src_u16), vget_high_u16(a), b.val[1]);
  return vreinterpretq_s16_u16(vcombine_u16(dst_lo, dst_hi));  // 14 bits
}

inline int16x8_t BoxFilterPass1(const uint8x8_t src_u8, const uint8x8_t a2[2],
                                const uint16x8_t b2[2], uint16x8_t sum565_a[2],
                                uint32x4x2_t sum565_b[2]) {
  uint32x4x2_t b_v;
  sum565_a[1] = Sum565(a2);
  sum565_a[1] = vsubq_u16(vdupq_n_u16((5 + 6 + 5) * 256), sum565_a[1]);
  sum565_b[1].val[0] = Sum565W(b2[0], b2[1]);
  sum565_b[1].val[1] = Sum565W(b2[1]);

  uint16x8_t a_v = vaddq_u16(sum565_a[0], sum565_a[1]);
  b_v.val[0] = vaddq_u32(sum565_b[0].val[0], sum565_b[1].val[0]);
  b_v.val[1] = vaddq_u32(sum565_b[0].val[1], sum565_b[1].val[1]);
  return CalculateFilteredOutput<5>(src_u8, a_v, b_v);  // 14 bits
}

inline int16x8_t BoxFilterPass2(const uint8x8_t src_u8, const uint8x8_t a2[2],
                                const uint16x8_t b2[2], uint16x8_t sum343_a[4],
                                uint16x8_t sum444_a[3],
                                uint32x4x2_t sum343_b[4],
                                uint32x4x2_t sum444_b[3]) {
  uint32x4x2_t b_v;
  Sum343_444(a2, &sum343_a[2], &sum444_a[1]);
  sum343_a[2] = vsubq_u16(vdupq_n_u16((3 + 4 + 3) * 256), sum343_a[2]);
  sum444_a[1] = vsubq_u16(vdupq_n_u16((4 + 4 + 4) * 256), sum444_a[1]);
  uint16x8_t a_v = Sum3_16(sum343_a[0], sum444_a[0], sum343_a[2]);
  Sum343_444W(b2, &sum343_b[2], &sum444_b[1]);
  b_v.val[0] =
      Sum3_32(sum343_b[0].val[0], sum444_b[0].val[0], sum343_b[2].val[0]);
  b_v.val[1] =
      Sum3_32(sum343_b[0].val[1], sum444_b[0].val[1], sum343_b[2].val[1]);
  return CalculateFilteredOutput<5>(src_u8, a_v, b_v);  // 14 bits
}

inline void SelfGuidedDoubleMultiplier(
    const uint8x8_t src, const int16x8_t box_filter_process_output[2],
    const int16x4_t w0, const int16x4_t w1, const int16x4_t w2,
    uint8_t* const dst) {
  // |wN| values are signed. |src| values can be treated as int16_t.
  const int16x8_t u =
      vreinterpretq_s16_u16(vshll_n_u8(src, kSgrProjRestoreBits));
  int32x4_t v_lo = vmull_s16(vget_low_s16(u), w1);
  v_lo = vmlal_s16(v_lo, vget_low_s16(box_filter_process_output[0]), w0);
  v_lo = vmlal_s16(v_lo, vget_low_s16(box_filter_process_output[1]), w2);
  int32x4_t v_hi = vmull_s16(vget_high_s16(u), w1);
  v_hi = vmlal_s16(v_hi, vget_high_s16(box_filter_process_output[0]), w0);
  v_hi = vmlal_s16(v_hi, vget_high_s16(box_filter_process_output[1]), w2);
  // |s| is saturated to uint8_t.
  const int16x4_t s_lo =
      vrshrn_n_s32(v_lo, kSgrProjRestoreBits + kSgrProjPrecisionBits);
  const int16x4_t s_hi =
      vrshrn_n_s32(v_hi, kSgrProjRestoreBits + kSgrProjPrecisionBits);
  vst1_u8(dst, vqmovun_s16(vcombine_s16(s_lo, s_hi)));
}

inline void SelfGuidedSingleMultiplier(
    const uint8x8_t src, const int16x8_t box_filter_process_output,
    const int16_t w0, const int16_t w1, uint8_t* dst) {
  // weight: -96 to 96 (Sgrproj_Xqd_Min/Max)
  const int16x8_t u =
      vreinterpretq_s16_u16(vshll_n_u8(src, kSgrProjRestoreBits));
  // u * w1 + u * wN == u * (w1 + wN)
  int32x4_t v_lo = vmull_n_s16(vget_low_s16(u), w1);
  v_lo = vmlal_n_s16(v_lo, vget_low_s16(box_filter_process_output), w0);
  int32x4_t v_hi = vmull_n_s16(vget_high_s16(u), w1);
  v_hi = vmlal_n_s16(v_hi, vget_high_s16(box_filter_process_output), w0);
  const int16x4_t s_lo =
      vrshrn_n_s32(v_lo, kSgrProjRestoreBits + kSgrProjPrecisionBits);
  const int16x4_t s_hi =
      vrshrn_n_s32(v_hi, kSgrProjRestoreBits + kSgrProjPrecisionBits);
  vst1_u8(dst, vqmovun_s16(vcombine_s16(s_lo, s_hi)));
}

inline void BoxFilterProcess(const uint8_t* const src,
                             const ptrdiff_t src_stride,
                             const RestorationUnitInfo& restoration_info,
                             const int width, const int height,
                             const uint16_t scale[2], uint16_t* const temp,
                             uint8_t* const dst, const ptrdiff_t dst_stride) {
  // We have combined PreProcess and Process for the first pass by storing
  // intermediate values in the |a2| region. The values stored are one vertical
  // column of interleaved |a2| and |b2| values and consume 8 * |height| values.
  // This is |height| and not |height| * 2 because PreProcess only generates
  // output for every other row. When processing the next column we write the
  // new scratch values right after reading the previously saved ones.

  // The PreProcess phase calculates a 5x5 box sum for every other row
  //
  // PreProcess and Process have been combined into the same step. We need 12
  // input values to generate 8 output values for PreProcess:
  // 0 1 2 3 4 5 6 7 8 9 10 11
  // 2 = 0 + 1 + 2 +  3 +  4
  // 3 = 1 + 2 + 3 +  4 +  5
  // 4 = 2 + 3 + 4 +  5 +  6
  // 5 = 3 + 4 + 5 +  6 +  7
  // 6 = 4 + 5 + 6 +  7 +  8
  // 7 = 5 + 6 + 7 +  8 +  9
  // 8 = 6 + 7 + 8 +  9 + 10
  // 9 = 7 + 8 + 9 + 10 + 11
  //
  // and then we need 10 input values to generate 8 output values for Process:
  // 0 1 2 3 4 5 6 7 8 9
  // 1 = 0 + 1 + 2
  // 2 = 1 + 2 + 3
  // 3 = 2 + 3 + 4
  // 4 = 3 + 4 + 5
  // 5 = 4 + 5 + 6
  // 6 = 5 + 6 + 7
  // 7 = 6 + 7 + 8
  // 8 = 7 + 8 + 9
  //
  // To avoid re-calculating PreProcess values over and over again we will do a
  // single column of 8 output values and store the second half of them
  // interleaved in |temp|. The first half is not stored, since it is used
  // immediately and becomes useless for the next column. Next we will start the
  // second column. When 2 rows have been calculated we can calculate Process
  // and output the results.

  // Calculate and store a single column. Scope so we can re-use the variable
  // names for the next step.
  uint16_t* ab_ptr = temp;
  const uint8_t* const src_pre_process = src - 2 * src_stride;
  // Calculate intermediate results, including two-pixel border, for example, if
  // unit size is 64x64, we calculate 68x68 pixels.
  {
    const uint8_t* column = src_pre_process - 4;
    uint16x4_t row[5], row3[4];
    uint32x4_t row_sq[5], row_sq3[4];
    SumHorizontal(column, &row3[0], &row[0], &row_sq3[0], &row_sq[0]);
    row[1] = row[0];
    row_sq[1] = row_sq[0];
    column += src_stride;
    SumHorizontal(column, &row3[1], &row[2], &row_sq3[1], &row_sq[2]);

    int y = (height + 2) >> 1;
    do {
      column += src_stride;
      SumHorizontal(column, &row3[2], &row[3], &row_sq3[2], &row_sq[3]);
      column += src_stride;
      SumHorizontal(column, &row3[3], &row[4], &row_sq3[3], &row_sq[4]);
      BoxFilterPreProcess4<5>(row + 0, row_sq + 0, scale[0], ab_ptr + 0);
      BoxFilterPreProcess4<3>(row3 + 0, row_sq3 + 0, scale[1], ab_ptr + 8);
      BoxFilterPreProcess4<3>(row3 + 1, row_sq3 + 1, scale[1], ab_ptr + 16);
      row[0] = row[2];
      row[1] = row[3];
      row[2] = row[4];
      row_sq[0] = row_sq[2];
      row_sq[1] = row_sq[3];
      row_sq[2] = row_sq[4];
      row3[0] = row3[2];
      row3[1] = row3[3];
      row_sq3[0] = row_sq3[2];
      row_sq3[1] = row_sq3[3];
      ab_ptr += 24;
    } while (--y != 0);

    if ((height & 1) != 0) {
      column += src_stride;
      SumHorizontal(column, &row3[2], &row[3], &row_sq3[2], &row_sq[3]);
      row[4] = row[3];
      row_sq[4] = row_sq[3];
      BoxFilterPreProcess4<5>(row + 0, row_sq + 0, scale[0], ab_ptr + 0);
      BoxFilterPreProcess4<3>(row3 + 0, row_sq3 + 0, scale[1], ab_ptr + 8);
    }
  }

  const int16_t w0 = restoration_info.sgr_proj_info.multiplier[0];
  const int16_t w1 = restoration_info.sgr_proj_info.multiplier[1];
  const int16_t w2 = (1 << kSgrProjPrecisionBits) - w0 - w1;
  const int16x4_t w0_v = vdup_n_s16(w0);
  const int16x4_t w1_v = vdup_n_s16(w1);
  const int16x4_t w2_v = vdup_n_s16(w2);
  int x = 0;
  do {
    // |src_pre_process| is X but we already processed the first column of 4
    // values so we want to start at Y and increment from there.
    // X s s s Y s s
    // s s s s s s s
    // s s i i i i i
    // s s i o o o o
    // s s i o o o o

    // Seed the loop with one line of output. Then, inside the loop, for each
    // iteration we can output one even row and one odd row and carry the new
    // line to the next iteration. In the diagram below 'i' values are
    // intermediary values from the first step and '-' values are empty.
    // iiii
    // ---- > even row
    // iiii - odd row
    // ---- > even row
    // iiii
    uint8x8_t a2[2][2];
    uint16x8_t b2[2][2], sum565_a[2], sum343_a[4], sum444_a[3];
    uint32x4x2_t sum565_b[2], sum343_b[4], sum444_b[3];
    ab_ptr = temp;
    b2[0][0] = vld1q_u16(ab_ptr);
    a2[0][0] = vget_low_u8(vreinterpretq_u8_u16(b2[0][0]));
    b2[1][0] = vld1q_u16(ab_ptr + 8);
    a2[1][0] = vget_low_u8(vreinterpretq_u8_u16(b2[1][0]));
    const uint8_t* column = src_pre_process + x;
    uint16x8_t row[5], row3[4];
    uint32x4x2_t row_sq[5], row_sq3[4];
    SumHorizontal(column, &row3[0], &row[0], &row_sq3[0], &row_sq[0]);
    row[1] = row[0];
    row_sq[1] = row_sq[0];
    column += src_stride;
    SumHorizontal(column, &row3[1], &row[2], &row_sq3[1], &row_sq[2]);
    column += src_stride;
    SumHorizontal(column, &row3[2], &row[3], &row_sq3[2], &row_sq[3]);
    column += src_stride;
    SumHorizontal(column, &row3[3], &row[4], &row_sq3[3], &row_sq[4]);

    BoxFilterPreProcess8<5>(row, row_sq, scale[0], &a2[0][1], &b2[0][1],
                            ab_ptr);
    BoxFilterPreProcess8<3>(row3 + 0, row_sq3 + 0, scale[1], &a2[1][1],
                            &b2[1][1], ab_ptr + 8);

    // Pass 1 Process. These are the only values we need to propagate between
    // rows.
    sum565_a[0] = Sum565(a2[0]);
    sum565_a[0] = vsubq_u16(vdupq_n_u16((5 + 6 + 5) * 256), sum565_a[0]);
    sum565_b[0].val[0] = Sum565W(b2[0][0], b2[0][1]);
    sum565_b[0].val[1] = Sum565W(b2[0][1]);
    sum343_a[0] = Sum343(a2[1]);
    sum343_a[0] = vsubq_u16(vdupq_n_u16((3 + 4 + 3) * 256), sum343_a[0]);
    sum343_b[0] = Sum343W(b2[1]);
    b2[1][0] = vld1q_u16(ab_ptr + 16);
    a2[1][0] = vget_low_u8(vreinterpretq_u8_u16(b2[1][0]));
    BoxFilterPreProcess8<3>(row3 + 1, row_sq3 + 1, scale[1], &a2[1][1],
                            &b2[1][1], ab_ptr + 16);
    Sum343_444(a2[1], &sum343_a[1], &sum444_a[0]);
    sum343_a[1] = vsubq_u16(vdupq_n_u16((3 + 4 + 3) * 256), sum343_a[1]);
    sum444_a[0] = vsubq_u16(vdupq_n_u16((4 + 4 + 4) * 256), sum444_a[0]);
    Sum343_444W(b2[1], &sum343_b[1], &sum444_b[0]);

    uint8_t* dst_ptr = dst + x;
    // Calculate one output line. Add in the line from the previous pass and
    // output one even row. Sum the new line and output the odd row. Carry the
    // new row into the next pass.
    for (int y = height >> 1; y != 0; --y) {
      ab_ptr += 24;
      b2[0][0] = vld1q_u16(ab_ptr);
      a2[0][0] = vget_low_u8(vreinterpretq_u8_u16(b2[0][0]));
      b2[1][0] = vld1q_u16(ab_ptr + 8);
      a2[1][0] = vget_low_u8(vreinterpretq_u8_u16(b2[1][0]));
      row[0] = row[2];
      row[1] = row[3];
      row[2] = row[4];
      row_sq[0] = row_sq[2];
      row_sq[1] = row_sq[3];
      row_sq[2] = row_sq[4];
      row3[0] = row3[2];
      row3[1] = row3[3];
      row_sq3[0] = row_sq3[2];
      row_sq3[1] = row_sq3[3];
      column += src_stride;
      SumHorizontal(column, &row3[2], &row[3], &row_sq3[2], &row_sq[3]);
      column += src_stride;
      SumHorizontal(column, &row3[3], &row[4], &row_sq3[3], &row_sq[4]);

      BoxFilterPreProcess8<5>(row, row_sq, scale[0], &a2[0][1], &b2[0][1],
                              ab_ptr);
      BoxFilterPreProcess8<3>(row3 + 0, row_sq3 + 0, scale[1], &a2[1][1],
                              &b2[1][1], ab_ptr + 8);
      int16x8_t p[2];
      const uint8x8_t src0 = vld1_u8(column - 3 * src_stride);
      p[0] = BoxFilterPass1(src0, a2[0], b2[0], sum565_a, sum565_b);
      p[1] = BoxFilterPass2(src0, a2[1], b2[1], sum343_a, sum444_a, sum343_b,
                            sum444_b);
      SelfGuidedDoubleMultiplier(src0, p, w0_v, w1_v, w2_v, dst_ptr);
      dst_ptr += dst_stride;
      const uint8x8_t src1 = vld1_u8(column - 2 * src_stride);
      p[0] = CalculateFilteredOutput<4>(src1, sum565_a[1], sum565_b[1]);
      b2[1][0] = vld1q_u16(ab_ptr + 16);
      a2[1][0] = vget_low_u8(vreinterpretq_u8_u16(b2[1][0]));
      BoxFilterPreProcess8<3>(row3 + 1, row_sq3 + 1, scale[1], &a2[1][1],
                              &b2[1][1], ab_ptr + 16);
      p[1] = BoxFilterPass2(src1, a2[1], b2[1], sum343_a + 1, sum444_a + 1,
                            sum343_b + 1, sum444_b + 1);
      SelfGuidedDoubleMultiplier(src1, p, w0_v, w1_v, w2_v, dst_ptr);
      dst_ptr += dst_stride;
      sum565_a[0] = sum565_a[1];
      sum565_b[0] = sum565_b[1];
      sum343_a[0] = sum343_a[2];
      sum343_a[1] = sum343_a[3];
      sum444_a[0] = sum444_a[2];
      sum343_b[0] = sum343_b[2];
      sum343_b[1] = sum343_b[3];
      sum444_b[0] = sum444_b[2];
    }

    if ((height & 1) != 0) {
      ab_ptr += 24;
      b2[0][0] = vld1q_u16(ab_ptr);
      a2[0][0] = vget_low_u8(vreinterpretq_u8_u16(b2[0][0]));
      b2[1][0] = vld1q_u16(ab_ptr + 8);
      a2[1][0] = vget_low_u8(vreinterpretq_u8_u16(b2[1][0]));
      row[0] = row[2];
      row[1] = row[3];
      row[2] = row[4];
      row_sq[0] = row_sq[2];
      row_sq[1] = row_sq[3];
      row_sq[2] = row_sq[4];
      row3[0] = row3[2];
      row3[1] = row3[3];
      row_sq3[0] = row_sq3[2];
      row_sq3[1] = row_sq3[3];
      column += src_stride;
      SumHorizontal(column, &row3[2], &row[3], &row_sq3[2], &row_sq[3]);
      row[4] = row[3];
      row_sq[4] = row_sq[3];
      BoxFilterPreProcess8<5>(row, row_sq, scale[0], &a2[0][1], &b2[0][1],
                              ab_ptr);
      BoxFilterPreProcess8<3>(row3, row_sq3, scale[1], &a2[1][1], &b2[1][1],
                              ab_ptr + 8);
      int16x8_t p[2];
      const uint8x8_t src0 = vld1_u8(column - 2 * src_stride);
      p[0] = BoxFilterPass1(src0, a2[0], b2[0], sum565_a, sum565_b);
      p[1] = BoxFilterPass2(src0, a2[1], b2[1], sum343_a, sum444_a, sum343_b,
                            sum444_b);
      SelfGuidedDoubleMultiplier(src0, p, w0_v, w1_v, w2_v, dst_ptr);
    }
    x += 8;
  } while (x < width);
}

inline void BoxFilterProcessPass1(const uint8_t* const src,
                                  const ptrdiff_t src_stride,
                                  const RestorationUnitInfo& restoration_info,
                                  const int width, const int height,
                                  const uint32_t scale, uint16_t* const temp,
                                  uint8_t* const dst,
                                  const ptrdiff_t dst_stride) {
  // We have combined PreProcess and Process for the first pass by storing
  // intermediate values in the |a2| region. The values stored are one vertical
  // column of interleaved |a2| and |b2| values and consume 8 * |height| values.
  // This is |height| and not |height| * 2 because PreProcess only generates
  // output for every other row. When processing the next column we write the
  // new scratch values right after reading the previously saved ones.

  // The PreProcess phase calculates a 5x5 box sum for every other row
  //
  // PreProcess and Process have been combined into the same step. We need 12
  // input values to generate 8 output values for PreProcess:
  // 0 1 2 3 4 5 6 7 8 9 10 11
  // 2 = 0 + 1 + 2 +  3 +  4
  // 3 = 1 + 2 + 3 +  4 +  5
  // 4 = 2 + 3 + 4 +  5 +  6
  // 5 = 3 + 4 + 5 +  6 +  7
  // 6 = 4 + 5 + 6 +  7 +  8
  // 7 = 5 + 6 + 7 +  8 +  9
  // 8 = 6 + 7 + 8 +  9 + 10
  // 9 = 7 + 8 + 9 + 10 + 11
  //
  // and then we need 10 input values to generate 8 output values for Process:
  // 0 1 2 3 4 5 6 7 8 9
  // 1 = 0 + 1 + 2
  // 2 = 1 + 2 + 3
  // 3 = 2 + 3 + 4
  // 4 = 3 + 4 + 5
  // 5 = 4 + 5 + 6
  // 6 = 5 + 6 + 7
  // 7 = 6 + 7 + 8
  // 8 = 7 + 8 + 9
  //
  // To avoid re-calculating PreProcess values over and over again we will do a
  // single column of 8 output values and store the second half of them
  // interleaved in |temp|. The first half is not stored, since it is used
  // immediately and becomes useless for the next column. Next we will start the
  // second column. When 2 rows have been calculated we can calculate Process
  // and output the results.

  // Calculate and store a single column. Scope so we can re-use the variable
  // names for the next step.
  uint16_t* ab_ptr = temp;
  const uint8_t* const src_pre_process = src - 2 * src_stride;
  // Calculate intermediate results, including two-pixel border, for example, if
  // unit size is 64x64, we calculate 68x68 pixels.
  {
    const uint8_t* column = src_pre_process - 4;
    uint16x4_t row[5];
    uint32x4_t row_sq[5];
    Sum5Horizontal(column, &row[0], &row_sq[0]);
    row[1] = row[0];
    row_sq[1] = row_sq[0];
    column += src_stride;
    Sum5Horizontal(column, &row[2], &row_sq[2]);

    int y = (height + 2) >> 1;
    do {
      column += src_stride;
      Sum5Horizontal(column, &row[3], &row_sq[3]);
      column += src_stride;
      Sum5Horizontal(column, &row[4], &row_sq[4]);
      BoxFilterPreProcess4<5>(row, row_sq, scale, ab_ptr);
      row[0] = row[2];
      row[1] = row[3];
      row[2] = row[4];
      row_sq[0] = row_sq[2];
      row_sq[1] = row_sq[3];
      row_sq[2] = row_sq[4];
      ab_ptr += 8;
    } while (--y != 0);

    if ((height & 1) != 0) {
      column += src_stride;
      Sum5Horizontal(column, &row[3], &row_sq[3]);
      row[4] = row[3];
      row_sq[4] = row_sq[3];
      BoxFilterPreProcess4<5>(row, row_sq, scale, ab_ptr);
    }
  }

  const int16_t w0 = restoration_info.sgr_proj_info.multiplier[0];
  const int16_t w1 = (1 << kSgrProjPrecisionBits) - w0;
  int x = 0;
  do {
    // |src_pre_process| is X but we already processed the first column of 4
    // values so we want to start at Y and increment from there.
    // X s s s Y s s
    // s s s s s s s
    // s s i i i i i
    // s s i o o o o
    // s s i o o o o

    // Seed the loop with one line of output. Then, inside the loop, for each
    // iteration we can output one even row and one odd row and carry the new
    // line to the next iteration. In the diagram below 'i' values are
    // intermediary values from the first step and '-' values are empty.
    // iiii
    // ---- > even row
    // iiii - odd row
    // ---- > even row
    // iiii
    uint8x8_t a2[2];
    uint16x8_t b2[2], sum565_a[2];
    uint32x4x2_t sum565_b[2];
    ab_ptr = temp;
    b2[0] = vld1q_u16(ab_ptr);
    a2[0] = vget_low_u8(vreinterpretq_u8_u16(b2[0]));
    const uint8_t* column = src_pre_process + x;
    uint16x8_t row[5];
    uint32x4x2_t row_sq[5];
    Sum5Horizontal(column, &row[0], &row_sq[0]);
    row[1] = row[0];
    row_sq[1] = row_sq[0];
    column += src_stride;
    Sum5Horizontal(column, &row[2], &row_sq[2]);
    column += src_stride;
    Sum5Horizontal(column, &row[3], &row_sq[3]);
    column += src_stride;
    Sum5Horizontal(column, &row[4], &row_sq[4]);
    BoxFilterPreProcess8<5>(row, row_sq, scale, &a2[1], &b2[1], ab_ptr);
    // Pass 1 Process. These are the only values we need to propagate between
    // rows.
    sum565_a[0] = Sum565(a2);
    sum565_a[0] = vsubq_u16(vdupq_n_u16((5 + 6 + 5) * 256), sum565_a[0]);
    sum565_b[0].val[0] = Sum565W(b2[0], b2[1]);
    sum565_b[0].val[1] = Sum565W(b2[1]);

    uint8_t* dst_ptr = dst + x;
    // Calculate one output line. Add in the line from the previous pass and
    // output one even row. Sum the new line and output the odd row. Carry the
    // new row into the next pass.
    for (int y = height >> 1; y != 0; --y) {
      ab_ptr += 8;
      b2[0] = vld1q_u16(ab_ptr);
      a2[0] = vget_low_u8(vreinterpretq_u8_u16(b2[0]));
      row[0] = row[2];
      row[1] = row[3];
      row[2] = row[4];
      row_sq[0] = row_sq[2];
      row_sq[1] = row_sq[3];
      row_sq[2] = row_sq[4];
      column += src_stride;
      Sum5Horizontal(column, &row[3], &row_sq[3]);
      column += src_stride;
      Sum5Horizontal(column, &row[4], &row_sq[4]);
      BoxFilterPreProcess8<5>(row, row_sq, scale, &a2[1], &b2[1], ab_ptr);
      const uint8x8_t src0 = vld1_u8(column - 3 * src_stride);
      const int16x8_t p0 = BoxFilterPass1(src0, a2, b2, sum565_a, sum565_b);
      SelfGuidedSingleMultiplier(src0, p0, w0, w1, dst_ptr);
      dst_ptr += dst_stride;
      const uint8x8_t src1 = vld1_u8(column - 2 * src_stride);
      const int16x8_t p1 =
          CalculateFilteredOutput<4>(src1, sum565_a[1], sum565_b[1]);
      SelfGuidedSingleMultiplier(src1, p1, w0, w1, dst_ptr);
      dst_ptr += dst_stride;
      sum565_a[0] = sum565_a[1];
      sum565_b[0] = sum565_b[1];
    }

    if ((height & 1) != 0) {
      ab_ptr += 8;
      b2[0] = vld1q_u16(ab_ptr);
      a2[0] = vget_low_u8(vreinterpretq_u8_u16(b2[0]));
      row[0] = row[2];
      row[1] = row[3];
      row[2] = row[4];
      row_sq[0] = row_sq[2];
      row_sq[1] = row_sq[3];
      row_sq[2] = row_sq[4];
      column += src_stride;
      Sum5Horizontal(column, &row[3], &row_sq[3]);
      row[4] = row[3];
      row_sq[4] = row_sq[3];
      BoxFilterPreProcess8<5>(row, row_sq, scale, &a2[1], &b2[1], ab_ptr);
      const uint8x8_t src0 = vld1_u8(column - 2 * src_stride);
      const int16x8_t p0 = BoxFilterPass1(src0, a2, b2, sum565_a, sum565_b);
      SelfGuidedSingleMultiplier(src0, p0, w0, w1, dst_ptr);
    }
    x += 8;
  } while (x < width);
}

inline void BoxFilterProcessPass2(const uint8_t* src,
                                  const ptrdiff_t src_stride,
                                  const RestorationUnitInfo& restoration_info,
                                  const int width, const int height,
                                  const uint32_t scale, uint16_t* const temp,
                                  uint8_t* const dst,
                                  const ptrdiff_t dst_stride) {
  // Calculate intermediate results, including one-pixel border, for example, if
  // unit size is 64x64, we calculate 66x66 pixels.
  // Because of the vectors this calculates start in blocks of 4 so we actually
  // get 68 values.
  uint16_t* ab_ptr = temp;
  const uint8_t* const src_pre_process = src - 2 * src_stride;
  {
    const uint8_t* column = src_pre_process - 4;
    uint16x4_t row[3];
    uint32x4_t row_sq[3];
    Sum3Horizontal(column, &row[0], &row_sq[0]);
    column += src_stride;
    Sum3Horizontal(column, &row[1], &row_sq[1]);
    int y = height + 2;
    do {
      column += src_stride;
      Sum3Horizontal(column, &row[2], &row_sq[2]);
      BoxFilterPreProcess4<3>(row, row_sq, scale, ab_ptr);
      row[0] = row[1];
      row[1] = row[2];
      row_sq[0] = row_sq[1];
      row_sq[1] = row_sq[2];
      ab_ptr += 8;
    } while (--y != 0);
  }

  assert(restoration_info.sgr_proj_info.multiplier[0] == 0);
  const int16_t w1 = restoration_info.sgr_proj_info.multiplier[1];
  const int16_t w0 = (1 << kSgrProjPrecisionBits) - w1;
  int x = 0;
  do {
    ab_ptr = temp;
    uint8x8_t a2[2];
    uint16x8_t b2[2], sum343_a[3], sum444_a[2];
    uint32x4x2_t sum343_b[3], sum444_b[2];
    b2[0] = vld1q_u16(ab_ptr);
    a2[0] = vget_low_u8(vreinterpretq_u8_u16(b2[0]));
    const uint8_t* column = src_pre_process + x;
    uint16x8_t row[3];
    uint32x4x2_t row_sq[3];
    Sum3Horizontal(column, &row[0], &row_sq[0]);
    column += src_stride;
    Sum3Horizontal(column, &row[1], &row_sq[1]);
    column += src_stride;
    Sum3Horizontal(column, &row[2], &row_sq[2]);
    BoxFilterPreProcess8<3>(row, row_sq, scale, &a2[1], &b2[1], ab_ptr);
    sum343_a[0] = Sum343(a2);
    sum343_a[0] = vsubq_u16(vdupq_n_u16((3 + 4 + 3) * 256), sum343_a[0]);
    sum343_b[0] = Sum343W(b2);
    ab_ptr += 8;
    b2[0] = vld1q_u16(ab_ptr);
    a2[0] = vget_low_u8(vreinterpretq_u8_u16(b2[0]));
    row[0] = row[1];
    row[1] = row[2];
    row_sq[0] = row_sq[1];
    row_sq[1] = row_sq[2];
    column += src_stride;
    Sum3Horizontal(column, &row[2], &row_sq[2]);
    BoxFilterPreProcess8<3>(row, row_sq, scale, &a2[1], &b2[1], ab_ptr);
    Sum343_444(a2, &sum343_a[1], &sum444_a[0]);
    sum343_a[1] = vsubq_u16(vdupq_n_u16((3 + 4 + 3) * 256), sum343_a[1]);
    sum444_a[0] = vsubq_u16(vdupq_n_u16((4 + 4 + 4) * 256), sum444_a[0]);
    Sum343_444W(b2, &sum343_b[1], &sum444_b[0]);

    uint8_t* dst_ptr = dst + x;
    int y = height;
    do {
      ab_ptr += 8;
      b2[0] = vld1q_u16(ab_ptr);
      a2[0] = vget_low_u8(vreinterpretq_u8_u16(b2[0]));
      row[0] = row[1];
      row[1] = row[2];
      row_sq[0] = row_sq[1];
      row_sq[1] = row_sq[2];
      column += src_stride;
      Sum3Horizontal(column, &row[2], &row_sq[2]);
      BoxFilterPreProcess8<3>(row, row_sq, scale, &a2[1], &b2[1], ab_ptr);
      const uint8x8_t src_u8 = vld1_u8(column - 2 * src_stride);
      int16x8_t p = BoxFilterPass2(src_u8, a2, b2, sum343_a, sum444_a, sum343_b,
                                   sum444_b);
      SelfGuidedSingleMultiplier(src_u8, p, w0, w1, dst_ptr);
      sum343_a[0] = sum343_a[1];
      sum343_a[1] = sum343_a[2];
      sum444_a[0] = sum444_a[1];
      sum343_b[0] = sum343_b[1];
      sum343_b[1] = sum343_b[2];
      sum444_b[0] = sum444_b[1];
      dst_ptr += dst_stride;
    } while (--y != 0);
    x += 8;
  } while (x < width);
}

// If |width| is non-multiple of 8, up to 7 more pixels are written to |dest| in
// the end of each row. It is safe to overwrite the output as it will not be
// part of the visible frame.
void SelfGuidedFilter_NEON(const void* const source, void* const dest,
                           const RestorationUnitInfo& restoration_info,
                           const ptrdiff_t source_stride,
                           const ptrdiff_t dest_stride, const int width,
                           const int height, RestorationBuffer* const buffer) {
  const int index = restoration_info.sgr_proj_info.index;
  const int radius_pass_0 = kSgrProjParams[index][0];  // 2 or 0
  const int radius_pass_1 = kSgrProjParams[index][2];  // 1 or 0
  const auto* src = static_cast<const uint8_t*>(source);
  auto* dst = static_cast<uint8_t*>(dest);
  if (radius_pass_1 == 0) {
    // |radius_pass_0| and |radius_pass_1| cannot both be 0, so we have the
    // following assertion.
    assert(radius_pass_0 != 0);
    BoxFilterProcessPass1(src, source_stride, restoration_info, width, height,
                          kSgrScaleParameter[index][0], buffer->sgf_buffer, dst,
                          dest_stride);
  } else if (radius_pass_0 == 0) {
    BoxFilterProcessPass2(src, source_stride, restoration_info, width, height,
                          kSgrScaleParameter[index][1], buffer->sgf_buffer, dst,
                          dest_stride);
  } else {
    BoxFilterProcess(src, source_stride, restoration_info, width, height,
                     kSgrScaleParameter[index], buffer->sgf_buffer, dst,
                     dest_stride);
  }
}

void Init8bpp() {
  Dsp* const dsp = dsp_internal::GetWritableDspTable(kBitdepth8);
  assert(dsp != nullptr);
  dsp->loop_restorations[0] = WienerFilter_NEON;
  dsp->loop_restorations[1] = SelfGuidedFilter_NEON;
}

}  // namespace
}  // namespace low_bitdepth

void LoopRestorationInit_NEON() { low_bitdepth::Init8bpp(); }

}  // namespace dsp
}  // namespace libgav1

#else  // !LIBGAV1_ENABLE_NEON
namespace libgav1 {
namespace dsp {

void LoopRestorationInit_NEON() {}

}  // namespace dsp
}  // namespace libgav1
#endif  // LIBGAV1_ENABLE_NEON
