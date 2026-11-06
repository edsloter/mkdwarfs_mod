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

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <dwarfs/byte_buffer.h>
#include <dwarfs/thread_pool.h>
#include <dwarfs/types.h>

namespace dwarfs {

struct compression_constraints;

class logger;

namespace writer {

class writer_progress;

namespace internal {

class block_manager;
class chunkable;
class global_dedup_index;

} // namespace internal

// Durable checkpointing support for the global de-duplication segmenter.
//
// The segmenter periodically flushes its current partial block and invokes
// drain_cb, which must block until every block handed to the writer is
// durable. Only then is the dedup index advanced to a new durable offset and
// commit_cb is invoked to persist the writer/block state. Resuming after a
// crash replays Phase 2; all chunks up to the durable offset are dedup hits
// and are neither recompressed nor rewritten.
struct segmenter_checkpoint_config {
  std::function<void()> drain_cb;
  std::function<void(uint64_t)> commit_cb;
  size_t interval_bytes{0};

  bool enabled() const { return commit_cb && interval_bytes != 0; }
};

class segmenter {
 public:
  struct config {
    std::string context{};
    unsigned blockhash_window_size{12};
    unsigned window_increment_shift{1};
    size_t max_active_blocks{1};
    unsigned bloom_filter_size{4};
    unsigned block_size_bits{22};
    bool enable_sparse_files{false};
    // When set (together with a non-zero chunk size), the segmenter will
    // use content-defined chunking with global de-duplication instead of
    // the bounded lookback window.
    std::shared_ptr<internal::global_dedup_index> global_dedup_index;
    size_t global_dedup_chunk_size{0};
    // Coarsest CDC level for multi-pass (0 = single-pass).
    // When > global_dedup_chunk_size, enables hierarchical multi-pass from
    // global_dedup_max_chunk_size down to global_dedup_chunk_size in
    // powers of two. Levels are processed sequentially; only unmatched
    // regions from each pass are re-chunked at the next finer level.
    size_t global_dedup_max_chunk_size{0};

    // Optional durable checkpointing (see segmenter_checkpoint_config).
    segmenter_checkpoint_config checkpoint;

    // Worker pool used to parallelize hashing in global de-duplication
    // mode (batch processing of chunkables).
    std::shared_ptr<thread_pool> hash_worker_pool;
  };

  using block_ready_cb =
      std::function<void(shared_byte_buffer, size_t logical_block_num)>;

  static uint64_t
  estimate_memory_usage(config const& cfg, compression_constraints const& cc);

  segmenter(logger& lgr, writer_progress& prog,
            std::shared_ptr<internal::block_manager> blkmgr, config const& cfg,
            compression_constraints const& cc, file_size_t total_size,
            block_ready_cb block_ready);

  void add_chunkable(internal::chunkable& chkable) {
    impl_->add_chunkable(chkable);
  }

  void add_chunkables(std::vector<std::unique_ptr<internal::chunkable>>& batch) {
    impl_->add_chunkables(batch);
  }

  void finish() { impl_->finish(); }

  class impl {
   public:
    virtual ~impl() = default;

    virtual void add_chunkable(internal::chunkable& chkable) = 0;

    /**
     * Process a batch of owned chunkables, preserving the given order.
     *
     * The default implementation simply processes the chunkables one by
     * one via add_chunkable(). De-duplication segmenters (such as the
     * global de-duplication segmenter) may override this to process a
     * batch in two phases: a parallel hashing phase followed by a serial,
     * ordered de-duplication/resolution phase (required for reproducible
     * results when parallel hashing is used).
     */
    virtual void add_chunkables(
        std::vector<std::unique_ptr<internal::chunkable>>& batch) {
      for (auto& c : batch) {
        add_chunkable(*c);
      }
    }

    virtual void finish() = 0;
  };

 private:
  std::unique_ptr<impl> impl_;
};

} // namespace writer

} // namespace dwarfs
