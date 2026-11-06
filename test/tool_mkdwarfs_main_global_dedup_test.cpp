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

#include <random>
#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <dwarfs/binary_literals.h>
#include <dwarfs/vfs_stat.h>

#include "test_tool_main_tester.h"

using namespace dwarfs::test;
using namespace dwarfs;

namespace fs = std::filesystem;

using namespace std::literals::string_view_literals;
using namespace dwarfs::binary_literals;

namespace {

std::string random_string(std::mt19937_64& rng, size_t size) {
  std::string data(size, '\0');
  for (auto& c : data) {
    c = static_cast<char>(rng() & 0xff);
  }
  return data;
}

std::string
read_whole_file(reader::filesystem_v2 const& fs, std::string const& path) {
  auto dev = fs.find(path);
  if (!dev) {
    return {};
  }
  auto iv = dev->inode();
  auto stat = fs.getattr(iv);
  std::error_code ec;
  auto data = fs.read_string(iv.inode_num(), stat.size(), 0, ec);
  EXPECT_FALSE(ec) << "error reading " << path << ": " << ec.message();
  return data;
}

} // namespace

TEST(mkdwarfs_test, global_dedup_roundtrip_and_effectiveness) {
  std::string const image_file = "test.dwarfs";
  std::mt19937_64 rng{12345678};

  auto const a = random_string(rng, 1_MiB);
  auto const p = random_string(rng, 256_KiB);
  auto const s = random_string(rng, 256_KiB);

  auto t = mkdwarfs_tester::create_empty();
  t.add_root_dir();
  t.os->add_file("/a", a);
  // /b contains a copy of /a's content embedded between unique data, so
  // file-level de-duplication cannot kick in; only chunk-level (global)
  // de-duplication can eliminate the duplicate data.
  t.os->add_file("/b", p + a + s);
  t.os->add_file("/c", a);

  ASSERT_EQ(0, t.run({"-i", "/", "-o", image_file, "-l3", "-C", "null",
                      "--no-history", "--dedup-global", "4k"}))
      << t.err();

  auto image_fs = t.fs_from_file(image_file);

  EXPECT_EQ(a, read_whole_file(image_fs, "/a"));
  EXPECT_EQ(p + a + s, read_whole_file(image_fs, "/b"));
  EXPECT_EQ(a, read_whole_file(image_fs, "/c"));

  // With null compression, the image size directly reflects the amount of
  // stored data (plus a small amount of metadata). Unique data is
  // 1 MiB (a) + 256 KiB (p) + 256 KiB (s) = 1.5 MiB; total logical data is
  // 3.5 MiB. Global chunk dedup must eliminate nearly all duplicates.
  auto image = t.fa->get_file(image_file);
  ASSERT_TRUE(image);
  EXPECT_GT(image->size(), 1_MiB + 512_KiB);
  EXPECT_LT(image->size(), (1_MiB + 512_KiB) + 128_KiB)
      << "dedup was not effective";
  EXPECT_LT(image->size(), 2 * (1_MiB + 512_KiB));
}

TEST(mkdwarfs_test, global_dedup_shifted_copy) {
  std::string const image_file = "test.dwarfs";
  std::mt19937_64 rng{987654321};

  auto const a = random_string(rng, 4_MiB);
  auto const insert_bytes = random_string(rng, 8_KiB);
  auto const b = a.substr(0, 2_MiB) + insert_bytes + a.substr(2_MiB);

  auto t = mkdwarfs_tester::create_empty();
  t.add_root_dir();
  t.os->add_file("/a", a);
  t.os->add_file("/b", b);

  // Use small blocks to exercise chunk splitting at block boundaries.
  ASSERT_EQ(0, t.run({"-i", "/", "-o", image_file, "-l3", "-C", "null",
                      "--no-history", "--dedup-global", "4k", "-S16"}))
      << t.err();

  auto image_fs = t.fs_from_file(image_file);

  EXPECT_EQ(a, read_whole_file(image_fs, "/a"));
  EXPECT_EQ(b, read_whole_file(image_fs, "/b"));

  // Content-defined chunking must re-synchronize after the insertion, so
  // nearly all of the 4 MiB duplicate data must be eliminated. The unique
  // data is 4 MiB (a) + 8 KiB (insertion) plus some boundary slack.
  auto image = t.fa->get_file(image_file);
  ASSERT_TRUE(image);
  EXPECT_GT(image->size(), 4_MiB);
  EXPECT_LT(image->size(), 4_MiB + 512_KiB) << "CDC resync did not work";
}

TEST(mkdwarfs_test, global_dedup_reproducible) {
  auto build = [] {
    std::mt19937_64 rng{424242};
    auto t = mkdwarfs_tester::create_empty();
    t.add_root_dir();
    for (int i = 0; i < 10; ++i) {
      t.os->add_file("/file" + std::to_string(i),
                     random_string(rng, 128_KiB + i * 8_KiB));
    }
    // some duplicates to exercise dedup paths
    t.os->add_file("/dup0", random_string(rng, 128_KiB));
    t.os->add_file("/dup1", random_string(rng, 136_KiB));
    EXPECT_EQ(0, t.run({"-i", "/", "-o", "-", "-l3", "--no-history",
                        "--no-progress", "--set-time", "1700000000",
                        "--dedup-global", "4k"}))
        << t.err();
    return t.out();
  };

  auto const image1 = build();
  auto const image2 = build();

  EXPECT_FALSE(image1.empty());
  EXPECT_EQ(image1, image2);
}

TEST(mkdwarfs_test, global_dedup_edge_cases) {
  std::string const image_file = "test.dwarfs";
  std::mt19937_64 rng{13579};

  auto t = mkdwarfs_tester::create_empty();
  t.add_root_dir();
  t.os->add_file("/empty", "");
  t.os->add_file("/tiny", "x");
  t.os->add_file("/tiny2", "x");
  t.os->add_file("/tiny3", "xy");
  auto const a = random_string(rng, 100_KiB);
  t.os->add_file("/a", a);
  // a file slightly smaller than the minimum chunk size
  t.os->add_file("/small", a.substr(0, 900));

  ASSERT_EQ(0, t.run({"-i", "/", "-o", image_file, "-l3", "-C", "null",
                      "--no-history", "--dedup-global", "4k"}))
      << t.err();

  auto image_fs = t.fs_from_file(image_file);

  EXPECT_EQ("", read_whole_file(image_fs, "/empty"));
  EXPECT_EQ("x", read_whole_file(image_fs, "/tiny"));
  EXPECT_EQ("x", read_whole_file(image_fs, "/tiny2"));
  EXPECT_EQ("xy", read_whole_file(image_fs, "/tiny3"));
  EXPECT_EQ(a, read_whole_file(image_fs, "/a"));
  EXPECT_EQ(a.substr(0, 900), read_whole_file(image_fs, "/small"));

  vfs_stat vfs;
  image_fs.statvfs(&vfs);

  // logical size: a + small + tiny + tiny2 (shared) + tiny3
  EXPECT_EQ(1, vfs.frsize);
  EXPECT_EQ(100_KiB + 900 + 1 + 1 + 2, vfs.blocks);
}

TEST(mkdwarfs_test, global_dedup_invalid_options) {
  {
    auto t = mkdwarfs_tester::create_empty();
    t.add_root_dir();
    EXPECT_EQ(1, t.run({"-i", "/", "-o", "-", "--dedup-global", "3k"}));
    EXPECT_THAT(t.err(), testing::HasSubstr("power of two"));
  }

  {
    auto t = mkdwarfs_tester::create_empty();
    t.add_root_dir();
    // 4 * 2k is larger than the block size of 2^10 bytes
    EXPECT_EQ(1, t.run({"-i", "/", "-o", "-", "--dedup-global", "2k", "-S10"}));
    EXPECT_THAT(t.err(), testing::HasSubstr("block size"));
  }

  {
    auto t = mkdwarfs_tester::create_empty();
    t.add_root_dir();
    EXPECT_EQ(0, t.run({"-i", "/", "-o", "-", "--dedup-global", "512"}));
  }

  {
    // segmenter options are ignored in global dedup mode
    auto t = mkdwarfs_tester::create_empty();
    t.add_root_dir();
    EXPECT_EQ(0, t.run({"-i", "/", "-o", "-", "--no-history",
                        "--dedup-global", "4k", "-B", "2", "-W", "10"}));
    EXPECT_THAT(t.err(), testing::HasSubstr("no effect"));
  }
}

TEST(mkdwarfs_test, global_dedup_beyond_lookback_window) {
  // This test demonstrates the main motivation for global deduplication:
  // duplicate data that occurs *far apart* in the input cannot be found by
  // the default lookback segmenter, but is found by --dedup-global.
  std::string const image_file = "test.dwarfs";
  std::mt19937_64 rng{20250822};

  auto const dup = random_string(rng, 8_MiB);

  auto t = mkdwarfs_tester::create_empty();
  t.add_root_dir();
  // 40 MiB of unique data in between two copies of the same 8 MiB blob
  t.os->add_file("/first", dup);
  t.os->add_file("/filler", random_string(rng, 40_MiB));
  t.os->add_file("/last", dup);

  ASSERT_EQ(0, t.run({"-i", "/", "-o", image_file, "-l3", "-C", "null",
                      "--no-history", "--dedup-global", "4k", "-B", "1"}))
      << t.err();

  auto image = t.fa->get_file(image_file);
  ASSERT_TRUE(image);

  // Unique data is 40 MiB + 8 MiB = 48 MiB; the duplicate 8 MiB copy must
  // not add more than a small amount of boundary slack to the image.
  EXPECT_GT(image->size(), 48_MiB);
  EXPECT_LT(image->size(), 48_MiB + 1_MiB);

  // Verify correctness of both copies
  auto image_fs = t.fs_from_file(image_file);
  EXPECT_EQ(dup, read_whole_file(image_fs, "/first"));
  EXPECT_EQ(dup, read_whole_file(image_fs, "/last"));
}
