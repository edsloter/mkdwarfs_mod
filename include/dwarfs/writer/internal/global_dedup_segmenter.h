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

#include <memory>

#include <dwarfs/types.h>
#include <dwarfs/writer/segmenter.h>

namespace dwarfs {

class logger;

struct compression_constraints;

namespace writer {

namespace internal {

class block_manager;
class progress;

/**
 * Create a segmenter that performs global, chunk-level de-duplication
 * using content-defined chunking. Must only be called when the global
 * de-duplication parameters in the segmenter config are set.
 */
std::unique_ptr<segmenter::impl> create_global_dedup_segmenter(
    logger& lgr, progress& prog, std::shared_ptr<block_manager> blkmgr,
    segmenter::config const& cfg, compression_constraints const& cc,
    file_size_t total_size, segmenter::block_ready_cb block_ready);

} // namespace internal

} // namespace writer

} // namespace dwarfs
