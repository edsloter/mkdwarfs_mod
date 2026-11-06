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

#include <dwarfs/writer/internal/entry_storage.h>
#include <dwarfs/writer/internal/inode_fragments_view.h>

namespace dwarfs::writer::internal {

auto inode_fragments_view::size() const noexcept -> size_type {
  return storage_->get_inode_fragment_count(id_);
}

fragment_category inode_fragments_view::get_single_category() const {
  assert(size() == 1);
  return this->operator[](0).category();
}

std::unordered_map<fragment_category, file_size_t>
inode_fragments_view::get_category_sizes() const {
  std::unordered_map<fragment_category, file_size_t> result;

  for (auto const& f : *this) {
    result[f.category()] += f.size();
  }

  return result;
}

} // namespace dwarfs::writer::internal
