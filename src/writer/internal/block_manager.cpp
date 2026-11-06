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

#include <cassert>

#include <dwarfs/writer/internal/block_manager.h>

namespace dwarfs::writer::internal {

size_t block_manager::get_logical_block() const {
  size_t block_no;
  {
    std::lock_guard lock{mx_};
    block_no = num_blocks_++;
  }
  return block_no;
}

void block_manager::set_written_block(size_t logical_block,
                                      size_t written_block,
                                      fragment_category category) {
  std::lock_guard lock{mx_};
  assert(logical_block < num_blocks_);
  if (block_map_.size() < num_blocks_) {
    block_map_.resize(num_blocks_);
  }
  block_map_[logical_block] = std::make_pair(written_block, category);
}

void block_manager::map_logical_blocks(
    chunks_type& vec,
    std::optional<inode_hole_mapper> const& hole_mapper) const {
  std::lock_guard lock{mx_};
  for (auto&& c : vec) {
    if (hole_mapper && hole_mapper->is_hole(c)) {
      continue;
    }
    size_t block = c.block().value();
    assert(block < num_blocks_);
    c.block() = block_map_.at(block).value().first;
  }
}

std::vector<fragment_category>
block_manager::get_written_block_categories() const {
  std::vector<fragment_category> result;

  {
    std::lock_guard lock{mx_};

    result.resize(num_blocks_);

    for (auto& b : block_map_) {
      auto& mapping = b.value();
      result[mapping.first] = mapping.second;
    }
  }

  return result;
}

size_t block_manager::num_blocks() const {
  std::lock_guard lock{mx_};
  return num_blocks_;
}

std::pair<size_t, std::vector<block_manager::persisted_entry>>
block_manager::snapshot() const {
  std::lock_guard lock{mx_};

  std::vector<persisted_entry> entries;
  entries.reserve(block_map_.size());

  for (size_t i = 0; i < block_map_.size(); ++i) {
    if (block_map_[i].has_value()) {
      entries.push_back(
          persisted_entry{i, block_map_[i]->first, block_map_[i]->second});
    }
  }

  return {num_blocks_, std::move(entries)};
}

void block_manager::restore(
    size_t num_blocks, std::vector<block_manager::persisted_entry> const& entries) {
  std::lock_guard lock{mx_};

  num_blocks_ = num_blocks;
  block_map_.clear();
  block_map_.resize(num_blocks_);

  for (auto const& e : entries) {
    if (e.logical_block < num_blocks_) {
      block_map_[e.logical_block] =
          std::make_pair(e.written_block, e.category);
    }
  }
}

} // namespace dwarfs::writer::internal
