// SPDX-License-Identifier: AGPL-3.0-only
//
// Copyright (C) 2026 Edward Sloter
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

/* vim:set ts=2 sw=2 sts=2 et: */
/**
 * \author     Marcus Holland-Moritz (github@mhxnet.de)
 * \copyright  Copyright (c) Marcus Holland-Moritz
 *
 * This file is part of dwarfs.
 *
 * dwarfs is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * dwarfs is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with dwarfs.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

#include <dwarfs/compiler.h>

namespace dwarfs::writer::internal {

/**
 * Content-defined chunking (CDC) using a 64-bit gear (Gearhash/Rabin-style)
 * rolling hash with normalized, FastCDC-like two-phase cut points.
 *
 * The gear table is generated at compile time from a fixed seed, so chunk
 * boundaries are fully deterministic across platforms, compilers and runs.
 */

namespace detail {

constexpr uint64_t cdc_splitmix64(uint64_t x) {
  x += UINT64_C(0x9E3779B97F4A7C15);
  x = (x ^ (x >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
  x = (x ^ (x >> 27)) * UINT64_C(0x94D049BB133111EB);
  return x ^ (x >> 31);
}

consteval std::array<uint64_t, 256> make_cdc_gear_table() {
  std::array<uint64_t, 256> table{};
  uint64_t state{UINT64_C(0x243F6A8885A308D3)};
  for (size_t i = 0; i < table.size(); ++i) {
    state = cdc_splitmix64(state);
    table[i] = state;
  }
  return table;
}

inline constexpr auto kCdcGearTable = make_cdc_gear_table();

} // namespace detail

struct cdc_params {
  /// Minimum chunk size (a hard cut happens at max_size at the latest).
  size_t min_size;
  /// Targeted average chunk size.
  size_t avg_size;
  /// Maximum chunk size (hard cut).
  size_t max_size;
  /// Data granularity (e.g. PCM audio frame size); all chunk sizes will be
  /// multiples of this. Must be >= 1.
  size_t granularity;
  /// Mask for the "hard" phase (chunk length in [min_size, avg_size)).
  uint64_t mask_long;
  /// Mask for the "easy" phase (chunk length in [avg_size, max_size)).
  uint64_t mask_short;
};

/**
 * Compute CDC parameters for a given target average chunk size.
 *
 * The target must be a power of two. The minimum chunk size is target/4,
 * the maximum chunk size is 4*target (clamped to the block size).
 *
 * For granularities > 1, the cut point condition is only evaluated at
 * offsets that are multiples of the granularity, and the probability is
 * adjusted such that the expected chunk size is still roughly the target.
 * The effective target is rounded up to a multiple of the granularity
 * (in terms of power-of-two frame counts).
 */
inline cdc_params make_cdc_params(size_t target, size_t granularity,
                                  size_t block_size) {
  if (target == 0 || (target & (target - 1)) != 0) {
    throw std::runtime_error("CDC target chunk size must be a power of two: " +
                             std::to_string(target));
  }
  if (granularity == 0) {
    throw std::runtime_error("CDC granularity must be positive");
  }

  size_t const g = granularity;

  // Round the per-frame target up to a power of two, with a minimum of 4
  // frames (so that min = target/4 is at least one frame).
  size_t frames = std::max<size_t>(4, std::bit_ceil(target / g));

  size_t const min_frames = std::max<size_t>(1, frames / 4);
  size_t max_frames = frames * 4;

  size_t const block_frames = block_size / g;
  if (block_frames < max_frames) {
    throw std::runtime_error("CDC maximum chunk size exceeds block size");
  }

  int const bits_long = std::countr_zero(frames);
  int const bits_short = std::max(1, bits_long - 2);

  auto top_mask = [](int bits) {
    bits = std::min(bits, 64);
    return bits >= 64 ? ~UINT64_C(0)
                      : ((UINT64_C(1) << bits) - 1) << (64 - bits);
  };

  return cdc_params{
      .min_size = min_frames * g,
      .avg_size = frames * g,
      .max_size = max_frames * g,
      .granularity = g,
      .mask_long = top_mask(bits_long),
      .mask_short = top_mask(bits_short),
  };
}

class cdc_hasher {
 public:
  cdc_hasher() = default;

  DWARFS_FORCE_INLINE void update(uint8_t byte) {
    hash_ = (hash_ << 1) + detail::kCdcGearTable[byte];
  }

  DWARFS_FORCE_INLINE uint64_t value() const { return hash_; }

  DWARFS_FORCE_INLINE void clear() { hash_ = 0; }

 private:
  uint64_t hash_{0};
};

/**
 * Decide whether to cut a chunk of the given current length, based on the
 * current rolling hash value. Note that the length is relative to the start
 * of the current chunk.
 */
DWARFS_FORCE_INLINE bool
cdc_should_cut(cdc_params const& params, uint64_t hash, size_t length) {
  if (length < params.min_size) {
    return false;
  }
  if (length >= params.max_size) {
    return true;
  }
  if (params.granularity > 1 && (length % params.granularity) != 0) {
    return false;
  }
  auto const mask = length < params.avg_size ? params.mask_long
                                             : params.mask_short;
  return (hash & mask) == 0;
}

} // namespace dwarfs::writer::internal
