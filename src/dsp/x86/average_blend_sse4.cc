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

#include "src/dsp/average_blend.h"
#include "src/utils/cpu.h"

#if LIBGAV1_TARGETING_SSE4_1

#include <xmmintrin.h>

#include <cassert>
#include <cstddef>
#include <cstdint>

#include "src/dsp/constants.h"
#include "src/dsp/dsp.h"
#include "src/dsp/x86/common_sse4.h"
#include "src/utils/common.h"

namespace libgav1 {
namespace dsp {
namespace low_bitdepth {
namespace {

constexpr int kInterPostRoundBit = 4;

inline void AverageBlend4Row(const int16_t* prediction_0,
                             const int16_t* prediction_1, uint8_t* dest) {
  const __m128i pred_0 = LoadLo8(prediction_0);
  const __m128i pred_1 = LoadLo8(prediction_1);
  __m128i res = _mm_add_epi16(pred_0, pred_1);
  res = RightShiftWithRounding_S16(res, kInterPostRoundBit + 1);
  Store4(dest, _mm_packus_epi16(res, res));
}

inline void AverageBlend8Row(const int16_t* prediction_0,
                             const int16_t* prediction_1, uint8_t* dest) {
  const __m128i pred_0 = LoadAligned16(prediction_0);
  const __m128i pred_1 = LoadAligned16(prediction_1);
  __m128i res = _mm_add_epi16(pred_0, pred_1);
  res = RightShiftWithRounding_S16(res, kInterPostRoundBit + 1);
  StoreLo8(dest, _mm_packus_epi16(res, res));
}

inline void AverageBlendLargeRow(const int16_t* prediction_0,
                                 const int16_t* prediction_1, const int width,
                                 uint8_t* dest) {
  int x = 0;
  do {
    const __m128i pred_00 = LoadAligned16(&prediction_0[x]);
    const __m128i pred_01 = LoadAligned16(&prediction_1[x]);
    __m128i res0 = _mm_add_epi16(pred_00, pred_01);
    res0 = RightShiftWithRounding_S16(res0, kInterPostRoundBit + 1);
    const __m128i pred_10 = LoadAligned16(&prediction_0[x + 8]);
    const __m128i pred_11 = LoadAligned16(&prediction_1[x + 8]);
    __m128i res1 = _mm_add_epi16(pred_10, pred_11);
    res1 = RightShiftWithRounding_S16(res1, kInterPostRoundBit + 1);
    StoreUnaligned16(dest + x, _mm_packus_epi16(res0, res1));
    x += 16;
  } while (x < width);
}

void AverageBlend_SSE4_1(const void* prediction_0, const void* prediction_1,
                         const int width, const int height, void* const dest,
                         const ptrdiff_t dest_stride) {
  auto* dst = static_cast<uint8_t*>(dest);
  const auto* pred_0 = static_cast<const int16_t*>(prediction_0);
  const auto* pred_1 = static_cast<const int16_t*>(prediction_1);
  int y = height;

  if (width == 4) {
    do {
      // TODO(b/150326556): |prediction_[01]| values are packed. It is possible
      // to load 8 values at a time.
      AverageBlend4Row(pred_0, pred_1, dst);
      dst += dest_stride;
      pred_0 += width;
      pred_1 += width;

      AverageBlend4Row(pred_0, pred_1, dst);
      dst += dest_stride;
      pred_0 += width;
      pred_1 += width;

      y -= 2;
    } while (y != 0);
    return;
  }

  if (width == 8) {
    do {
      AverageBlend8Row(pred_0, pred_1, dst);
      dst += dest_stride;
      pred_0 += width;
      pred_1 += width;

      AverageBlend8Row(pred_0, pred_1, dst);
      dst += dest_stride;
      pred_0 += width;
      pred_1 += width;

      y -= 2;
    } while (y != 0);
    return;
  }

  do {
    AverageBlendLargeRow(pred_0, pred_1, width, dst);
    dst += dest_stride;
    pred_0 += width;
    pred_1 += width;

    AverageBlendLargeRow(pred_0, pred_1, width, dst);
    dst += dest_stride;
    pred_0 += width;
    pred_1 += width;

    y -= 2;
  } while (y != 0);
}

void Init8bpp() {
  Dsp* const dsp = dsp_internal::GetWritableDspTable(kBitdepth8);
  assert(dsp != nullptr);
#if DSP_ENABLED_8BPP_SSE4_1(AverageBlend)
  dsp->average_blend = AverageBlend_SSE4_1;
#endif
}

}  // namespace
}  // namespace low_bitdepth

#if LIBGAV1_MAX_BITDEPTH >= 10
namespace high_bitdepth {
namespace {

constexpr int kInterPostRoundBitPlusOne = 5;

template <const int width, const int offset>
inline void AverageBlendRow(const uint16_t* pred_0, const uint16_t* pred_1,
                            uint16_t* dst, const ptrdiff_t dest_stride,
                            const __m128i compound_offset,
                            const __m128i round_offset, const __m128i max) {
  // pred_0/1 max range is 16b.
  const __m128i pred_00 = _mm_cvtepu16_epi32(LoadLo8(pred_0 + offset));
  const __m128i pred_01 = _mm_cvtepu16_epi32(LoadLo8(pred_0 + offset + 4));
  const __m128i pred_10 = _mm_cvtepu16_epi32(LoadLo8(pred_1 + offset));
  const __m128i pred_11 = _mm_cvtepu16_epi32(LoadLo8(pred_1 + offset + 4));
  const __m128i pred_add_0 = _mm_add_epi32(pred_00, pred_10);
  const __m128i pred_add_1 = _mm_add_epi32(pred_01, pred_11);
  const __m128i compound_offset_0 = _mm_sub_epi32(pred_add_0, compound_offset);
  const __m128i compound_offset_1 = _mm_sub_epi32(pred_add_1, compound_offset);
  // RightShiftWithRounding and Clip3.
  const __m128i round_0 = _mm_add_epi32(compound_offset_0, round_offset);
  const __m128i round_1 = _mm_add_epi32(compound_offset_1, round_offset);
  const __m128i res_0 = _mm_srai_epi32(round_0, kInterPostRoundBitPlusOne);
  const __m128i res_1 = _mm_srai_epi32(round_1, kInterPostRoundBitPlusOne);
  const __m128i result = _mm_min_epi16(_mm_packus_epi32(res_0, res_1), max);
  if (width != 4) {
    // Store width=8/16/32/64/128.
    StoreUnaligned16(dst + offset, result);
    return;
  }
  assert(width == 4);
  StoreLo8(dst, result);
  StoreHi8(dst + dest_stride, result);
}

void AverageBlend10bpp_SSE4_1(const void* prediction_0,
                              const void* prediction_1, const int width,
                              const int height, void* const dest,
                              const ptrdiff_t dst_stride) {
  auto* dst = static_cast<uint16_t*>(dest);
  const ptrdiff_t dest_stride = dst_stride / sizeof(dst[0]);
  const auto* pred_0 = static_cast<const uint16_t*>(prediction_0);
  const auto* pred_1 = static_cast<const uint16_t*>(prediction_1);
  const __m128i compound_offset =
      _mm_set1_epi32(kCompoundOffset + kCompoundOffset);
  const __m128i round_offset =
      _mm_set1_epi32((1 << kInterPostRoundBitPlusOne) >> 1);
  const __m128i max = _mm_set1_epi16((1 << kBitdepth10) - 1);
  int y = height;

  if (width == 4) {
    const ptrdiff_t dest_stride2 = dest_stride << 1;
    const ptrdiff_t width2 = width << 1;
    do {
      // row0,1
      AverageBlendRow<4, 0>(pred_0, pred_1, dst, dest_stride, compound_offset,
                            round_offset, max);
      dst += dest_stride2;
      pred_0 += width2;
      pred_1 += width2;
      y -= 2;
    } while (y != 0);
    return;
  }
  if (width == 8) {
    do {
      // row0.
      AverageBlendRow<8, 0>(pred_0, pred_1, dst, dest_stride, compound_offset,
                            round_offset, max);
      dst += dest_stride;
      pred_0 += width;
      pred_1 += width;
      // row1.
      AverageBlendRow<8, 0>(pred_0, pred_1, dst, dest_stride, compound_offset,
                            round_offset, max);
      dst += dest_stride;
      pred_0 += width;
      pred_1 += width;
      y -= 2;
    } while (y != 0);
    return;
  }
  if (width == 16) {
    do {
      // row0.
      AverageBlendRow<8, 0>(pred_0, pred_1, dst, dest_stride, compound_offset,
                            round_offset, max);
      AverageBlendRow<8, 8>(pred_0, pred_1, dst, dest_stride, compound_offset,
                            round_offset, max);
      dst += dest_stride;
      pred_0 += width;
      pred_1 += width;
      // row1.
      AverageBlendRow<8, 0>(pred_0, pred_1, dst, dest_stride, compound_offset,
                            round_offset, max);
      AverageBlendRow<8, 8>(pred_0, pred_1, dst, dest_stride, compound_offset,
                            round_offset, max);
      dst += dest_stride;
      pred_0 += width;
      pred_1 += width;
      y -= 2;
    } while (y != 0);
    return;
  }
  if (width == 32) {
    do {
      // pred [0 - 15].
      AverageBlendRow<8, 0>(pred_0, pred_1, dst, dest_stride, compound_offset,
                            round_offset, max);
      AverageBlendRow<8, 8>(pred_0, pred_1, dst, dest_stride, compound_offset,
                            round_offset, max);
      // pred [16 - 31].
      AverageBlendRow<8, 16>(pred_0, pred_1, dst, dest_stride, compound_offset,
                             round_offset, max);
      AverageBlendRow<8, 24>(pred_0, pred_1, dst, dest_stride, compound_offset,
                             round_offset, max);
      dst += dest_stride;
      pred_0 += width;
      pred_1 += width;
    } while (--y != 0);
    return;
  }
  if (width == 64) {
    do {
      // pred [0 - 31].
      AverageBlendRow<8, 0>(pred_0, pred_1, dst, dest_stride, compound_offset,
                            round_offset, max);
      AverageBlendRow<8, 8>(pred_0, pred_1, dst, dest_stride, compound_offset,
                            round_offset, max);
      AverageBlendRow<8, 16>(pred_0, pred_1, dst, dest_stride, compound_offset,
                             round_offset, max);
      AverageBlendRow<8, 24>(pred_0, pred_1, dst, dest_stride, compound_offset,
                             round_offset, max);
      // pred [31 - 63].
      AverageBlendRow<8, 32>(pred_0, pred_1, dst, dest_stride, compound_offset,
                             round_offset, max);
      AverageBlendRow<8, 40>(pred_0, pred_1, dst, dest_stride, compound_offset,
                             round_offset, max);
      AverageBlendRow<8, 48>(pred_0, pred_1, dst, dest_stride, compound_offset,
                             round_offset, max);
      AverageBlendRow<8, 56>(pred_0, pred_1, dst, dest_stride, compound_offset,
                             round_offset, max);
      dst += dest_stride;
      pred_0 += width;
      pred_1 += width;
    } while (--y != 0);
    return;
  }
  assert(width == 128);
  do {
    // pred [0 - 31].
    AverageBlendRow<8, 0>(pred_0, pred_1, dst, dest_stride, compound_offset,
                          round_offset, max);
    AverageBlendRow<8, 8>(pred_0, pred_1, dst, dest_stride, compound_offset,
                          round_offset, max);
    AverageBlendRow<8, 16>(pred_0, pred_1, dst, dest_stride, compound_offset,
                           round_offset, max);
    AverageBlendRow<8, 24>(pred_0, pred_1, dst, dest_stride, compound_offset,
                           round_offset, max);
    // pred [31 - 63].
    AverageBlendRow<8, 32>(pred_0, pred_1, dst, dest_stride, compound_offset,
                           round_offset, max);
    AverageBlendRow<8, 40>(pred_0, pred_1, dst, dest_stride, compound_offset,
                           round_offset, max);
    AverageBlendRow<8, 48>(pred_0, pred_1, dst, dest_stride, compound_offset,
                           round_offset, max);
    AverageBlendRow<8, 56>(pred_0, pred_1, dst, dest_stride, compound_offset,
                           round_offset, max);

    // pred [64 - 95].
    AverageBlendRow<8, 64>(pred_0, pred_1, dst, dest_stride, compound_offset,
                           round_offset, max);
    AverageBlendRow<8, 72>(pred_0, pred_1, dst, dest_stride, compound_offset,
                           round_offset, max);
    AverageBlendRow<8, 80>(pred_0, pred_1, dst, dest_stride, compound_offset,
                           round_offset, max);
    AverageBlendRow<8, 88>(pred_0, pred_1, dst, dest_stride, compound_offset,
                           round_offset, max);
    // pred [96 - 127].
    AverageBlendRow<8, 96>(pred_0, pred_1, dst, dest_stride, compound_offset,
                           round_offset, max);
    AverageBlendRow<8, 104>(pred_0, pred_1, dst, dest_stride, compound_offset,
                            round_offset, max);
    AverageBlendRow<8, 112>(pred_0, pred_1, dst, dest_stride, compound_offset,
                            round_offset, max);
    AverageBlendRow<8, 120>(pred_0, pred_1, dst, dest_stride, compound_offset,
                            round_offset, max);
    dst += dest_stride;
    pred_0 += width;
    pred_1 += width;
  } while (--y != 0);
}

void Init10bpp() {
  Dsp* const dsp = dsp_internal::GetWritableDspTable(kBitdepth10);
  assert(dsp != nullptr);
#if DSP_ENABLED_10BPP_SSE4_1(AverageBlend)
  dsp->average_blend = AverageBlend10bpp_SSE4_1;
#endif
}

}  // namespace
}  // namespace high_bitdepth
#endif  // LIBGAV1_MAX_BITDEPTH >= 10

void AverageBlendInit_SSE4_1() {
  low_bitdepth::Init8bpp();
#if LIBGAV1_MAX_BITDEPTH >= 10
  high_bitdepth::Init10bpp();
#endif  // LIBGAV1_MAX_BITDEPTH >= 10
}

}  // namespace dsp
}  // namespace libgav1

#else   // !LIBGAV1_TARGETING_SSE4_1

namespace libgav1 {
namespace dsp {

void AverageBlendInit_SSE4_1() {}

}  // namespace dsp
}  // namespace libgav1
#endif  // LIBGAV1_TARGETING_SSE4_1