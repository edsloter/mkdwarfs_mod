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

#include <algorithm>
#include <bit>
#include <random>
#include <set>
#include <span>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <dwarfs/binary_literals.h>

#include <dwarfs/writer/internal/cdc_chunker.h>

using namespace dwarfs;
using namespace dwarfs::binary_literals;
using namespace dwarfs::writer::internal;

namespace {

std::vector<uint8_t> make_random_data(std::mt19937_64& rng, size_t size) {
  std::vector<uint8_t> data(size);
  for (auto& b : data) {
    b = static_cast<uint8_t>(rng() & 0xff);
  }
  return data;
}

std::vector<size_t>
chunk_sizes(cdc_params const& params, std::span<uint8_t const> data) {
  std::vector<size_t> sizes;
  size_t start = 0;

  while (start < data.size()) {
    cdc_hasher hasher;
    size_t len = data.size() - start;
    size_t i = start;

    while (i < data.size()) {
      hasher.update(data[i]);
      ++i;
      if (cdc_should_cut(params, hasher.value(), i - start)) {
        len = i - start;
        break;
      }
    }

    sizes.push_back(len);
    start += len;
  }

  return sizes;
}

} // namespace

TEST(cdc_chunker_test, params_granularity_one) {
  auto const p = make_cdc_params(4_KiB, 1, 4_MiB);

  EXPECT_EQ(1_KiB, p.min_size);
  EXPECT_EQ(4_KiB, p.avg_size);
  EXPECT_EQ(16_KiB, p.max_size);
  EXPECT_EQ(1, p.granularity);
  EXPECT_EQ(12, std::popcount(p.mask_long));
  EXPECT_EQ(10, std::popcount(p.mask_short));
  EXPECT_EQ(p.mask_long, p.mask_long >> 52 << 52);
}

TEST(cdc_chunker_test, params_with_granularity) {
  auto const p = make_cdc_params(4_KiB, 4, 4_MiB);

  // target/4 frames rounded up to a power of two: 4096/4 = 1024 frames
  EXPECT_EQ(256 * 4, p.min_size);
  EXPECT_EQ(1024 * 4, p.avg_size);
  EXPECT_EQ(4 * 1024 * 4, p.max_size);
  EXPECT_EQ(4, p.granularity);
  EXPECT_EQ(0, p.min_size % 4);
  EXPECT_EQ(0, p.avg_size % 4);
  EXPECT_EQ(0, p.max_size % 4);

  // granularity that doesn't divide the target: round up to 1024 frames
  auto const p6 = make_cdc_params(4_KiB, 6, 16_MiB);
  EXPECT_EQ(256 * 6, p6.min_size);
  EXPECT_EQ(1024 * 6, p6.avg_size);
  EXPECT_EQ(0, p6.avg_size % 6);
}

TEST(cdc_chunker_test, params_invalid) {
  EXPECT_THROW(make_cdc_params(3_KiB, 1, 4_MiB), std::runtime_error);
  EXPECT_THROW(make_cdc_params(4_KiB, 0, 4_MiB), std::runtime_error);
  // maximum chunk size (4 * target) must fit into the block
  EXPECT_THROW(make_cdc_params(4_KiB, 1, 8_KiB), std::runtime_error);
  EXPECT_NO_THROW(make_cdc_params(4_KiB, 1, 16_KiB));
}

TEST(cdc_chunker_test, deterministic_boundaries) {
  std::mt19937_64 rng{42};
  auto const data = make_random_data(rng, 1_MiB);
  auto const params = make_cdc_params(4_KiB, 1, 4_MiB);

  auto const sizes1 = chunk_sizes(params, data);
  auto const sizes2 = chunk_sizes(params, data);

  EXPECT_THAT(sizes1, testing::ContainerEq(sizes2));
  EXPECT_GT(sizes1.size(), 100);
}

TEST(cdc_chunker_test, chunk_size_bounds) {
  std::mt19937_64 rng{7};
  auto data = make_random_data(rng, 8_MiB);

  for (size_t granularity : {1, 2, 3, 4, 6}) {
    // total data size must be a multiple of the granularity, just like
    // real extents for granular compressors
    auto const size = data.size() - (data.size() % granularity);
    auto const params = make_cdc_params(4_KiB, granularity, 4_MiB);
    auto const sizes = chunk_sizes(params, {data.data(), size});

    ASSERT_GT(sizes.size(), 100);

    uint64_t total = 0;
    for (auto size : sizes) {
      // the last chunk may be smaller than the minimum size
      if (total + size < size_t{8_MiB}) {
        EXPECT_GE(size, params.min_size);
      }
      EXPECT_LE(size, params.max_size);
      EXPECT_EQ(0, size % granularity)
          << "granularity " << granularity << " violated";
      total += size;
    }
    EXPECT_EQ(size, total);

    // average chunk size should be in the vicinity of the target
    auto const avg = static_cast<double>(total) / sizes.size();
    EXPECT_GT(avg, params.min_size);
    EXPECT_LT(avg, params.max_size);
  }
}

TEST(cdc_chunker_test, resync_after_insertion) {
  std::mt19937_64 rng{99};
  auto data = make_random_data(rng, 2_MiB);
  auto const params = make_cdc_params(4_KiB, 1, 4_MiB);

  auto const sizes_before = chunk_sizes(params, data);

  // turn chunk sizes into absolute boundaries
  std::vector<size_t> bounds_before;
  size_t pos = 0;
  for (auto size : sizes_before) {
    pos += size;
    bounds_before.push_back(pos);
  }

  // insert 100 bytes in the middle; boundaries must re-synchronize
  auto const ins = make_random_data(rng, 100);
  data.insert(data.begin() + data.size() / 2, ins.begin(), ins.end());

  auto const sizes_after = chunk_sizes(params, data);

  std::vector<size_t> bounds_after;
  pos = 0;
  for (auto size : sizes_after) {
    pos += size;
    bounds_after.push_back(pos);
  }

  ASSERT_EQ(data.size(), pos);

  // count how many boundaries are shared (ignoring the insertion shift)
  std::set<size_t> const before_set(bounds_before.begin(), bounds_before.end());
  size_t shared = 0;
  for (auto b : bounds_after) {
    if (before_set.contains(b) || before_set.contains(b - 100)) {
      ++shared;
    }
  }

  // the vast majority of boundaries must be re-synchronized
  EXPECT_GT(shared, bounds_before.size() * 9 / 10);
}
