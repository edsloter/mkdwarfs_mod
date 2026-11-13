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
#include <cstdint>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <xxhash.h>

#include <dwarfs/compiler.h>
#include <dwarfs/compression_constraints.h>
#include <dwarfs/error.h>
#include <dwarfs/logger.h>
#include <dwarfs/malloc_byte_buffer.h>
#include <dwarfs/types.h>
#include <dwarfs/util.h>

#include <dwarfs/writer/segmenter.h>

#include <dwarfs/writer/internal/block_manager.h>
#include <dwarfs/writer/internal/cdc_chunker.h>
#include <dwarfs/writer/internal/chunkable.h>
#include <dwarfs/writer/internal/global_dedup_index.h>
#include <dwarfs/writer/internal/global_dedup_segmenter.h>
#include <dwarfs/writer/internal/progress.h>

namespace dwarfs::writer::internal {

namespace {

/**
 * Global Dedup Segmenter Strategy
 *
 * In contrast to the "lookback" segmenter, which can only find duplicate
 * segments in a small, bounded window of recently written blocks, this
 * segmenter de-duplicates content-defined chunks across the *entire*
 * input data set:
 *
 * - Data is split into variable-size chunks using content-defined
 *   chunking (CDC) with a 64-bit gear hash. Chunk boundaries only depend
 *   on the content itself, so identical data always produces identical
 *   chunk boundaries, even if the data is shifted (e.g. by insertions or
 *   deletions in otherwise identical files).
 *
 * - For each chunk, a strong hash (XXH3-128) is computed and looked up
 *   in a global, never-expiring index. On a hit, only a reference to the
 *   already stored chunk is emitted. On a miss, the chunk is stored in
 *   the current block and its location is added to the index.
 *
 * - The index is shared between all category streams, which are processed
 *   sequentially when global de-duplication is enabled. This keeps the
 *   resulting images fully deterministic (reproducible builds) without
 *   requiring any coordination between concurrent workers.
 *
 * - When multi-pass mode is enabled (global_dedup_max_chunk_size > 0),
 *   the segmenter processes data hierarchically: first at the coarsest
 *   level (max), then re-chunks unmatched regions at each finer level
 *   (max/2, max/4, ..., min) in powers of two. Each level only processes
 *   byte ranges that were not matched at coarser levels, saving both
 *   CPU and memory for data that deduplicates at larger granularities.
 *
 * The produced images are fully compatible with the DwarFS format: they
 * only contain ordinary chunks referencing ranges of ordinary blocks.
 */

class global_dedup_progress : public progress::context {
 public:
  using status = progress::context::status;

  global_dedup_progress(std::string context, uint64_t total_size)
      : context_{std::move(context)}
      , bytes_total_{total_size} {}

  status get_status() const override {
    auto f = current_file.load();
    status st;
    st.color = termcolor::GREEN;
    st.context = context_;
    {
      auto ss = status_string_.load();
      if (ss) {
        st.status_string = *ss;
      }
    }
    if (f) {
      st.path.emplace(f.path_as_string());
    }
    st.bytes_processed.emplace(bytes_processed.load());
    st.bytes_total.emplace(bytes_total_);
    st.pass_bytes_processed.emplace(pass_bytes_processed.load());
    return st;
  }

  void set_level_info(std::string const& level_info) {
    status_string_.store(level_info);
  }

  // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes)
  dwarfs::internal::synchronized<const_file_handle> current_file{};
  std::atomic<uint64_t> bytes_processed{0};
  std::atomic<uint64_t> pass_bytes_processed{0};
  // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

 private:
  std::string const context_;
  uint64_t const bytes_total_;
  dwarfs::internal::synchronized<std::optional<std::string>> status_string_;
};

struct global_dedup_stats {
  uint64_t chunks_total{0};
  uint64_t chunks_stored{0};
  uint64_t chunks_referenced{0};
  uint64_t bytes_stored{0};
  uint64_t bytes_referenced{0};
};

} // namespace

template <typename LoggerPolicy>
class global_dedup_segmenter_ final : public segmenter::impl {
 public:
  global_dedup_segmenter_(logger& lgr, progress& prog,
                          std::shared_ptr<block_manager> blkmgr,
                          segmenter::config const& cfg,
                          compression_constraints const& cc,
                          file_size_t total_size,
                          segmenter::block_ready_cb block_ready)
      : LOG_PROXY_INIT(lgr)
      , prog_{prog}
      , blkmgr_{std::move(blkmgr)}
      , cfg_{cfg}
      , block_ready_{std::move(block_ready)}
      , pctx_{prog.create_context<global_dedup_progress>(cfg.context,
                                                         total_size)}
      , block_capacity_{static_cast<size_t>(1) << cfg.block_size_bits}
      , params_{make_cdc_params(cfg.global_dedup_chunk_size,
                                cc.granularity.value_or(1), block_capacity_)}
      , index_{cfg.global_dedup_index} {
    DWARFS_CHECK(index_, "global dedup index not configured");
    DWARFS_CHECK(params_.max_size <= block_capacity_,
                 "CDC maximum chunk size must not exceed block size");

    // Build the multi-pass level table if max > min.
    if (cfg.global_dedup_max_chunk_size > cfg.global_dedup_chunk_size) {
      size_t level_target = cfg.global_dedup_max_chunk_size;
      while (level_target >= cfg.global_dedup_chunk_size) {
        levels_.push_back(
            make_cdc_params(level_target, cc.granularity.value_or(1),
                            block_capacity_));
        if (level_target == cfg.global_dedup_chunk_size) {
          break;
        }
        level_target /= 2;
      }
      // Ensure the finest level is always present.
      if (levels_.back().avg_size != params_.avg_size) {
        levels_.push_back(params_);
      }
    } else {
      levels_.push_back(params_);
    }

    scratch_.reserve(block_capacity_);
    block_.reserve(block_capacity_);

    if (levels_.size() > 1) {
      LOG_VERBOSE << cfg_.context << "global dedup multi-pass: "
                  << levels_.size() << " levels from "
                  << size_with_unit(levels_.front().avg_size) << " down to "
                  << size_with_unit(levels_.back().avg_size);
    } else {
      LOG_VERBOSE << cfg_.context << "global dedup using CDC with target "
                  << size_with_unit(params_.avg_size) << " ["
                  << size_with_unit(params_.min_size) << ", "
                  << size_with_unit(params_.max_size) << "]"
                  << (params_.granularity > 1
                          ? fmt::format(", {}-byte granularity",
                                        params_.granularity)
                          : std::string());
    }
  }

  void add_chunkable(chunkable& chkable) override {
    if (chkable.size() == 0) {
      return;
    }

    LOG_TRACE << cfg_.context << "adding " << chkable.description();

    pctx_->current_file.store(chkable.get_file());

    if (levels_.size() > 1) {
      add_chunkable_multi_pass(chkable);
    } else {
      add_chunkable_single_pass(chkable);
    }
  }

  void finish() override {
    flush_block();

    if (stats_.chunks_total > 0) {
      LOG_VERBOSE << cfg_.context << "global dedup: "
                  << stats_.chunks_referenced << "/" << stats_.chunks_total
                  << " chunks deduplicated ("
                  << size_with_unit(stats_.bytes_referenced) << " saved, "
                  << size_with_unit(stats_.bytes_stored) << " stored), index "
                  << index_->size() << " entries (~"
                  << size_with_unit(index_->memory_usage()) << " RAM"
                  << (index_->spill_file_count() > 0
                          ? ", " + std::to_string(index_->spill_file_count()) +
                                " spill files (" +
                                size_with_unit(index_->spill_file_disk_bytes()) +
                                " on disk)"
                          : std::string())
                  << ")";
    }
  }

 private:
  void update_progress(uint64_t bytes) {
    prog_.total_bytes_read += bytes;
    pctx_->bytes_processed += bytes;
  }

  // =========================================================================
  // Single-pass path (backward compatible, identical to original)
  // =========================================================================

  void add_chunkable_single_pass(chunkable& chkable) {
    for (auto const& ext : chkable.extents()) {
      if (cfg_.enable_sparse_files && ext.kind() == extent_kind::hole) {
        chkable.add_hole(ext.size());
        continue;
      }

      if (ext.size() == 0) {
        continue;
      }

      if (params_.granularity > 1) {
        DWARFS_CHECK(ext.size() % params_.granularity == 0,
                     fmt::format("unexpected extent size {} for given "
                                 "granularity {}",
                                 ext.size(), params_.granularity));
      }

      if (ext.supports_raw_bytes()) {
        auto const raw = ext.raw_bytes();
        process_raw_extent(chkable, raw);
        update_progress(raw.size());
      } else {
        process_queued_extent(chkable, ext);
        update_progress(ext.size());
      }
    }
  }

  void process_raw_extent(chunkable& chkable, std::span<std::byte const> data) {
    auto const p = reinterpret_cast<uint8_t const*>(data.data());
    auto const size = data.size();
    size_t start = 0;

    while (start < size) {
      cdc_hasher cdc;
      size_t len = size - start;
      size_t i = start;

      while (i < size) {
        cdc.update(p[i]);
        ++i;
        if (cdc_should_cut(params_, cdc.value(), i - start)) {
          len = i - start;
          break;
        }
      }

      handle_chunk(chkable, p + start, len);
      start += len;
    }
  }

  void process_queued_extent(chunkable& chkable, file_extent const& ext) {
    cdc_hasher cdc;

    scratch_.clear();

    for (auto const& seg : ext.segments()) {
      for (auto b : seg.span<uint8_t>()) {
        cdc.update(b);
        scratch_.push_back(b);
        if (cdc_should_cut(params_, cdc.value(), scratch_.size())) {
          handle_chunk(chkable, scratch_.data(), scratch_.size());
          scratch_.clear();
          cdc.clear();
        }
      }
    }

    if (!scratch_.empty()) {
      handle_chunk(chkable, scratch_.data(), scratch_.size());
    }
  }

  // =========================================================================
  // Multi-pass path
  // =========================================================================

  void add_chunkable_multi_pass(chunkable& chkable) {
    for (auto const& ext : chkable.extents()) {
      if (cfg_.enable_sparse_files && ext.kind() == extent_kind::hole) {
        chkable.add_hole(ext.size());
        continue;
      }

      if (ext.size() == 0) {
        continue;
      }

      if (params_.granularity > 1) {
        DWARFS_CHECK(ext.size() % params_.granularity == 0,
                     fmt::format("unexpected extent size {} for given "
                                 "granularity {}",
                                 ext.size(), params_.granularity));
      }

      if (ext.supports_raw_bytes()) {
        auto const raw = ext.raw_bytes();
        process_raw_extent_multi_pass(chkable, raw);
        prog_.total_bytes_read += raw.size();
      } else {
        process_queued_extent_multi_pass(chkable, ext);
        prog_.total_bytes_read += ext.size();
      }
    }

    pctx_->set_level_info({});
  }

  void process_raw_extent_multi_pass(chunkable& chkable,
                                     std::span<std::byte const> data) {
    auto const* const base = reinterpret_cast<uint8_t const*>(data.data());
    auto const total_size = data.size();

    // current_unmatched: list of (offset_into_data, size) regions that
    // have not yet been matched at any level.
    std::vector<std::pair<size_t, size_t>> current_unmatched;
    std::vector<std::pair<size_t, size_t>> next_unmatched;

    current_unmatched.emplace_back(0, total_size);

    size_t total_unmatched_bytes = total_size;
    auto const num_levels = levels_.size();

    for (size_t li = 0; li < num_levels; ++li) {
      auto const& lparams = levels_[li];
      bool const is_last = (li + 1 == num_levels);
      next_unmatched.clear();

      pctx_->set_level_info(fmt::format(
          "pass {}/{}: CDC @ {}/{} [{} unmatched]",
          li + 1, num_levels, size_with_unit(lparams.avg_size),
          size_with_unit(lparams.max_size),
          size_with_unit(total_unmatched_bytes)));
      pctx_->pass_bytes_processed = 0;

      for (auto const& [offset, size] : current_unmatched) {
        process_region_at_level(chkable, base + offset, offset, size, lparams,
                                is_last, next_unmatched);
      }

      total_unmatched_bytes = 0;
      for (auto const& [off, sz] : next_unmatched) {
        total_unmatched_bytes += sz;
      }

      current_unmatched = std::move(next_unmatched);

      if (current_unmatched.empty()) {
        break;
      }
    }

    // Any leftover unmatched data goes straight into blocks.
    for (auto const& [offset, size] : current_unmatched) {
      handle_chunk(chkable, base + offset, size);
      pctx_->bytes_processed += size;
      pctx_->pass_bytes_processed += size;
    }
  }

  void process_queued_extent_multi_pass(chunkable& chkable,
                                        file_extent const& ext) {
    // Buffer the entire extent into scratch_, then run multi-pass.
    scratch_.clear();
    for (auto const& seg : ext.segments()) {
      for (auto b : seg.span<uint8_t>()) {
        scratch_.push_back(b);
      }
    }

    if (scratch_.empty()) {
      return;
    }

    auto const* const base = scratch_.data();
    auto const total_size = scratch_.size();

    std::vector<std::pair<size_t, size_t>> current_unmatched;
    std::vector<std::pair<size_t, size_t>> next_unmatched;

    current_unmatched.emplace_back(0, total_size);

    size_t total_unmatched_bytes = total_size;
    auto const num_levels = levels_.size();

    for (size_t li = 0; li < num_levels; ++li) {
      auto const& lparams = levels_[li];
      bool const is_last = (li + 1 == num_levels);
      next_unmatched.clear();

      pctx_->set_level_info(fmt::format(
          "pass {}/{}: CDC @ {}/{} [{} unmatched]",
          li + 1, num_levels, size_with_unit(lparams.avg_size),
          size_with_unit(lparams.max_size),
          size_with_unit(total_unmatched_bytes)));
      pctx_->pass_bytes_processed = 0;

      for (auto const& [offset, size] : current_unmatched) {
        process_region_at_level(chkable, base + offset, offset, size, lparams,
                                is_last, next_unmatched);
      }

      total_unmatched_bytes = 0;
      for (auto const& [off, sz] : next_unmatched) {
        total_unmatched_bytes += sz;
      }

      current_unmatched = std::move(next_unmatched);

      if (current_unmatched.empty()) {
        break;
      }
    }

    for (auto const& [offset, size] : current_unmatched) {
      handle_chunk(chkable, base + offset, size);
      pctx_->bytes_processed += size;
    }
  }

  // Process a contiguous byte region at a given CDC level.
  // Matched chunks are emitted immediately.  Unmatched chunks are appended
  // to next (if not the last level) or stored in blocks.
  //
  // base_offset is the offset of `data` from the start of the original
  // extent, so that unmatched regions stored in `next` are always
  // expressed as offsets from the extent start (suitable for the
  // outer loop at the next level which does base + offset).
  void process_region_at_level(chunkable& chkable, uint8_t const* data,
                               size_t base_offset, size_t size,
                               cdc_params const& lparams, bool is_last,
                               std::vector<std::pair<size_t, size_t>>& next) {
    size_t start = 0;
    size_t local_processed = 0;

    auto flush_progress = [this, &local_processed]() mutable {
      pctx_->bytes_processed += local_processed;
      pctx_->pass_bytes_processed += local_processed;
      local_processed = 0;
    };

    while (start < size) {
      cdc_hasher cdc;
      size_t chunk_len = size - start; // hard cut at region boundary
      size_t i = start;

      while (i < size) {
        cdc.update(data[i]);
        ++i;
        if (cdc_should_cut(lparams, cdc.value(), i - start)) {
          chunk_len = i - start;
          break;
        }
      }

      auto const hash = XXH3_128bits(data + start, chunk_len);
      global_dedup_index::key_type const key{hash.low64, hash.high64};

      // Feed the speedometer with the bytes chunked at this level. Note
      // that the same bytes are re-scanned at finer levels, so this
      // counter reflects CDC throughput, not unique input bytes.
      local_processed += chunk_len;
      if (local_processed >= (UINT64_C(1) << 20)) {
        flush_progress();
      }

      if (auto const* loc = index_->find(key)) {
        DWARFS_CHECK(loc->size == chunk_len,
                     "global dedup: chunk size mismatch for identical hash");
        ++stats_.chunks_total;
        chkable.add_chunk(loc->block, loc->offset, loc->size);
        ++prog_.chunk_count;
        prog_.saved_by_global_dedup += chunk_len;
        ++stats_.chunks_referenced;
        stats_.bytes_referenced += chunk_len;
      } else if (!is_last) {
        next.emplace_back(base_offset + start, chunk_len);
      } else {
        handle_chunk(chkable, data + start, chunk_len);
      }

      start += chunk_len;
    }

    if (local_processed > 0) {
      flush_progress();
    }
  }

  // =========================================================================
  // Shared block management
  // =========================================================================

  void handle_chunk(chunkable& chkable, uint8_t const* data, size_t len) {
    auto const hash = XXH3_128bits(data, len);
    return handle_chunk_with_hash(chkable, data, len, hash.low64, hash.high64);
  }

  void handle_chunk_with_hash(chunkable& chkable, uint8_t const* data, size_t len,
                               uint64_t lo, uint64_t hi) {
    ++stats_.chunks_total;
    bytes_since_checkpoint_ += len;

    global_dedup_index::key_type const key{lo, hi};

    if (auto const* loc = index_->find(key)) {
      DWARFS_CHECK(loc->size == len,
                   "global dedup: chunk size mismatch for identical hash");
      chkable.add_chunk(loc->block, loc->offset, loc->size);
      ++prog_.chunk_count;
      prog_.saved_by_global_dedup += len;
      ++stats_.chunks_referenced;
      stats_.bytes_referenced += len;
      maybe_checkpoint();
      return;
    }

    if (block_.size() + len > block_capacity_ && block_.size() > 0) {
      flush_block();
    }

    if (!block_active_) {
      block_num_ = blkmgr_->get_logical_block();
      block_active_ = true;
    }

    auto const offset = block_.size();

    block_.append(data, len);

    chkable.add_chunk(block_num_, offset, len);
    ++prog_.chunk_count;
    prog_.filesystem_size += len;
    ++stats_.chunks_stored;
    stats_.bytes_stored += len;

    index_->insert(key,
                   global_dedup_index::chunk_location{
                       static_cast<uint32_t>(block_num_),
                       static_cast<uint32_t>(offset),
                       static_cast<uint32_t>(len)});

    if (block_.size() == block_capacity_) {
      flush_block();
    }

    maybe_checkpoint();
  }

  struct local_dedup_chunk {
    uint64_t lo{0};
    uint64_t hi{0};
    size_t size{0};
    size_t data_offset{0};
    uint8_t const* base{nullptr};
  };

  // =========================================================================
  // Batched parallel hashing / ordered de-duplication
  // =========================================================================

  void add_chunkables(
      std::vector<std::unique_ptr<internal::chunkable>>& batch) override {
    if (levels_.size() > 1) {
      // Multi-pass processing resolves finer levels only for regions that
      // remained unmatched after coarser levels; this has to stay serial
      // within the ordered stream, so fall back to the streaming path.
      for (auto& c : batch) {
        add_chunkable(*c);
      }
      return;
    }

    if (!cfg_.hash_worker_pool) {
      // Parallel hashing is optional; without a worker pool we simply
      // process the batch serially.
      for (auto& c : batch) {
        add_chunkable(*c);
      }
      return;
    }

    process_batch(batch);
  }

  size_t estimated_parallel_chunk_bytes(chunkable& chkable) const {
    size_t chunks = 0;
    for (auto const& ext : chkable.extents()) {
      if (cfg_.enable_sparse_files && ext.kind() == extent_kind::hole) {
        continue;
      }
      if (ext.size() == 0 || !ext.supports_raw_bytes()) {
        continue;
      }
      size_t const min_chunk = params_.min_size > 0 ? params_.min_size : 1;
      chunks += static_cast<size_t>(ext.size()) / min_chunk + 1;
    }
    return chunks * sizeof(local_dedup_chunk);
  }

  void process_raw_extent_parallel(chunkable& chkable,
                                   std::span<std::byte const> data) {
    auto const* const p = reinterpret_cast<uint8_t const*>(data.data());
    auto const size = data.size();

    size_t pos = 0;
    size_t const batch_limit =
        cfg_.hash_batch_bytes > 0 ? cfg_.hash_batch_bytes : (size_t{1} << 30);

    while (pos < size) {
      std::vector<std::pair<size_t, size_t>> refs;
      size_t batch_bytes = 0;
      size_t i = pos;

      while (i < size) {
        cdc_hasher cdc;
        size_t len = size - i;
        size_t j = i;
        while (j < size) {
          cdc.update(p[j]);
          ++j;
          if (cdc_should_cut(params_, cdc.value(), j - i)) {
            len = j - i;
            break;
          }
        }
        refs.emplace_back(i, len);
        batch_bytes += len;
        i += len;
        if (batch_bytes >= batch_limit) {
          break;
        }
      }

      std::vector<XXH128_hash_t> hashes(refs.size());
      for (size_t k = 0; k < refs.size(); ++k) {
        cfg_.hash_worker_pool->add_job([this, &hashes, &refs, p, k] mutable {
          hashes[k] = XXH3_128bits(p + refs[k].first, refs[k].second);
        });
      }
      cfg_.hash_worker_pool->wait();

      for (size_t k = 0; k < refs.size(); ++k) {
        auto const off = refs[k].first;
        auto const len = refs[k].second;
        auto const h = hashes[k];
        handle_chunk_with_hash(chkable, p + off, len, h.low64, h.high64);
      }

      pos = i;
    }
  }

  void process_batch(
      std::vector<std::unique_ptr<internal::chunkable>>& batch) {
    if (batch.empty()) {
      return;
    }

    for (auto& up : batch) {
      if (!up) {
        continue;
      }

      auto& chkable = *up;

      if (!chunkable_is_hashable(chkable)) {
        add_chunkable(chkable);
        up.reset();
        continue;
      }

      bool has_queued = false;
      for (auto const& ext : chkable.extents()) {
        if (cfg_.enable_sparse_files && ext.kind() == extent_kind::hole) {
          has_queued = true;
          break;
        }
        if (ext.size() == 0) {
          continue;
        }
        if (!ext.supports_raw_bytes()) {
          has_queued = true;
          break;
        }
      }

      if (has_queued) {
        add_chunkable(chkable);
        up.reset();
        continue;
      }

      for (auto const& ext : chkable.extents()) {
        if (cfg_.enable_sparse_files && ext.kind() == extent_kind::hole) {
          chkable.add_hole(ext.size());
          continue;
        }
        if (ext.size() == 0) {
          continue;
        }
        process_raw_extent_parallel(chkable, ext.raw_bytes());
        update_progress(ext.size());
      }

      up.reset();
    }

    batch.clear();
  }

  bool chunkable_is_hashable(internal::chunkable& chkable) {
    for (auto const& ext : chkable.extents()) {
      if (cfg_.enable_sparse_files && ext.kind() == extent_kind::hole) {
        return false;
      }
      if (ext.size() == 0) {
        continue;
      }
      if (!ext.supports_raw_bytes()) {
        return false;
      }
    }
    return true;
  }

  std::vector<local_dedup_chunk> hash_chunkable(internal::chunkable& chkable) {
    std::vector<local_dedup_chunk> out;
    for (auto const& ext : chkable.extents()) {
      if (cfg_.enable_sparse_files && ext.kind() == extent_kind::hole) {
        continue;
      }
      if (ext.size() == 0) {
        continue;
      }
      if (params_.granularity > 1) {
        DWARFS_CHECK(ext.size() % params_.granularity == 0,
                     fmt::format("unexpected extent size {} for given granularity {}",
                                 ext.size(), params_.granularity));
      }
      if (!ext.supports_raw_bytes()) {
        continue;
      }
      auto const raw = ext.raw_bytes();
      auto const p = reinterpret_cast<uint8_t const*>(raw.data());
      auto const size = raw.size();
      size_t start = 0;
      while (start < size) {
        cdc_hasher cdc;
        size_t len = size - start;
        size_t i = start;
        while (i < size) {
          cdc.update(p[i]);
          ++i;
          if (cdc_should_cut(params_, cdc.value(), i - start)) {
            len = i - start;
            break;
          }
        }
        auto const hash = XXH3_128bits(p + start, len);
        out.push_back(local_dedup_chunk{hash.low64, hash.high64, len, start, p});
        start += len;
      }
    }
    return out;
  }

  // Perform a durable checkpoint if enough input has been processed since the
  // last one. Only called at chunk boundaries, so there is no partial CDC
  // state to carry over: we flush the current block, wait for the writer to
  // drain, and then advance the dedup index's durable offset. Resuming simply
  // replays Phase 2 from the start; everything up to the durable offset is a
  // dedup hit and is therefore neither recompressed nor rewritten.
  void maybe_checkpoint() {
    if (!cfg_.checkpoint.enabled() ||
        bytes_since_checkpoint_ < cfg_.checkpoint.interval_bytes) {
      return;
    }

    flush_block();

    if (cfg_.checkpoint.drain_cb) {
      cfg_.checkpoint.drain_cb();
    }

    auto const off = index_->capture_log_offset();
    index_->set_durable_offset(off);

    cfg_.checkpoint.commit_cb(off);

    bytes_since_checkpoint_ = 0;
  }

  void flush_block() {
    if (block_active_) {
      DWARFS_CHECK(!block_.empty(), "global dedup: unexpected empty block");
      // The block data is shared with the writer, which may still be
      // compressing it asynchronously. Never touch the buffer again;
      // instead, use a fresh buffer for the next block.
      block_ready_(block_.share(), block_num_);
      ++prog_.block_count;
      block_ = malloc_byte_buffer::create_reserve(block_capacity_);
      block_active_ = false;
    }
  }

  LOG_PROXY_DECL(LoggerPolicy);
  progress& prog_;
  std::shared_ptr<block_manager> blkmgr_;
  segmenter::config const cfg_;
  segmenter::block_ready_cb block_ready_;
  std::shared_ptr<global_dedup_progress> pctx_;

  size_t const block_capacity_;
  cdc_params const params_;
  std::shared_ptr<global_dedup_index> index_;

  // Multi-pass levels, ordered coarsest to finest.
  // When levels_.size() == 1, single-pass (backward compatible).
  std::vector<cdc_params> levels_;

  mutable_byte_buffer block_ = malloc_byte_buffer::create();
  std::vector<uint8_t> scratch_;

  size_t block_num_{0};
  bool block_active_{false};
  uint64_t bytes_since_checkpoint_{0};

  global_dedup_stats stats_;
};

std::unique_ptr<segmenter::impl>
create_global_dedup_segmenter(logger& lgr, progress& prog,
                              std::shared_ptr<block_manager> blkmgr,
                              segmenter::config const& cfg,
                              compression_constraints const& cc,
                              file_size_t total_size,
                              segmenter::block_ready_cb block_ready) {
  return make_unique_logging_object<segmenter::impl, global_dedup_segmenter_,
                                    logger_policies>(
      lgr, prog, std::move(blkmgr), cfg, cc, total_size,
      std::move(block_ready));
}

} // namespace dwarfs::writer::internal
