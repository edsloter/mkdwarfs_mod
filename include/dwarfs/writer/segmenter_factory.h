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

#include <filesystem>
#include <memory>
#include <optional>

#include <dwarfs/types.h>
#include <dwarfs/writer/categorized_option.h>
#include <dwarfs/writer/segmenter.h>

namespace dwarfs {

class logger;

struct compression_constraints;

namespace writer {

class categorizer_manager;
class writer_progress;

namespace internal {

class global_dedup_index;

} // namespace internal

class segmenter_factory {
 public:
  struct config {
    categorized_option<unsigned> blockhash_window_size;
    categorized_option<unsigned> window_increment_shift;
    categorized_option<size_t> max_active_blocks;
    categorized_option<unsigned> bloom_filter_size;
    unsigned block_size_bits{22};
    bool enable_sparse_files{false};
    // When set, enables global, chunk-level de-duplication with the
    // given target chunk size (must be a power of two).
    std::optional<size_t> global_dedup_chunk_size;
    // Maximum CDC chunk size for multi-pass (0 = single-pass).
    // When set and > global_dedup_chunk_size, enables hierarchical
    // multi-pass from global_dedup_max_chunk_size down to
    // global_dedup_chunk_size in powers of two.
    size_t global_dedup_max_chunk_size{0};
    // Maximum RAM for the global dedup index in bytes (0 = unlimited).
    // When exceeded, entries spill to sorted temporary files on disk.
    size_t max_index_memory{0};
    // Maximum bytes of chunk data to hash in one parallel batch.
    size_t hash_batch_bytes{256 * 1024 * 1024};
    // When set, the global dedup index is persisted in this directory so it
    // can be reused on resume.
    std::filesystem::path global_dedup_persist_dir;
    // When true, load a previously persisted index instead of starting fresh.
    bool global_dedup_resume{false};
  };

  segmenter_factory(logger& lgr, writer_progress& prog);
  segmenter_factory(logger& lgr, writer_progress& prog, config const& cfg);
  segmenter_factory(logger& lgr, writer_progress& prog,
                    std::shared_ptr<categorizer_manager> catmgr);
  segmenter_factory(logger& lgr, writer_progress& prog,
                    std::shared_ptr<categorizer_manager> catmgr,
                    config const& cfg);

  segmenter create(fragment_category cat, file_size_t cat_size,
                   compression_constraints const& cc,
                   std::shared_ptr<internal::block_manager> blkmgr,
                   segmenter::block_ready_cb block_ready,
                   segmenter_checkpoint_config const& checkpoint = {},
                   std::shared_ptr<thread_pool> hash_worker_pool = nullptr)
      const {
    return impl_->create(cat, cat_size, cc, std::move(blkmgr),
                         std::move(block_ready), checkpoint, hash_worker_pool);
  }

  size_t get_block_size() const { return impl_->get_block_size(); }

  uint64_t estimate_memory_usage(fragment_category cat, file_size_t cat_size,
                                 compression_constraints const& cc) const {
    return impl_->estimate_memory_usage(cat, cat_size, cc);
  }

  // Returns true if category streams must be processed sequentially in
  // order to guarantee reproducible images (i.e. when global
  // de-duplication is enabled).
  bool serializes_categories() const {
    return impl_->serializes_categories();
  }

  class impl {
   public:
    virtual ~impl() = default;

    virtual segmenter create(fragment_category cat, file_size_t cat_size,
                             compression_constraints const& cc,
                             std::shared_ptr<internal::block_manager> blkmgr,
                             segmenter::block_ready_cb block_ready,
                             segmenter_checkpoint_config const& checkpoint = {},
                             std::shared_ptr<thread_pool> hash_worker_pool = nullptr)
        const = 0;
    virtual size_t get_block_size() const = 0;
    virtual uint64_t estimate_memory_usage(
        fragment_category cat, file_size_t cat_size,
        compression_constraints const& cc) const = 0;
    virtual bool serializes_categories() const = 0;
  };

 private:
  std::unique_ptr<impl> impl_;
};

} // namespace writer

} // namespace dwarfs
