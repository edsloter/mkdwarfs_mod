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

#include <dwarfs/writer/categorizer.h>
#include <dwarfs/writer/segmenter_factory.h>

#include <dwarfs/writer/internal/global_dedup_index.h>

namespace dwarfs::writer {

namespace internal {

class segmenter_factory_ final : public segmenter_factory::impl {
 public:
  segmenter_factory_(logger& lgr, writer_progress& prog,
                     std::shared_ptr<categorizer_manager> catmgr,
                     segmenter_factory::config const& cfg)
      : lgr_{lgr}
      , prog_{prog}
      , catmgr_{std::move(catmgr)}
      , cfg_{cfg} {
    if (cfg_.global_dedup_chunk_size) {
      global_index_ = std::make_shared<global_dedup_index>(
          cfg_.max_index_memory);
      if (!cfg_.global_dedup_persist_dir.empty()) {
        if (cfg_.global_dedup_resume) {
          global_index_->enable_persistence(cfg_.global_dedup_persist_dir);
          global_index_->load_persisted();
        } else {
          global_index_->reset_persistence(cfg_.global_dedup_persist_dir);
        }
      }
    }
  }

  segmenter create(fragment_category cat, file_size_t cat_size,
                   compression_constraints const& cc,
                   std::shared_ptr<block_manager> blkmgr,
                   segmenter::block_ready_cb block_ready,
                   segmenter_checkpoint_config const& checkpoint = {},
                   std::shared_ptr<thread_pool> hash_worker_pool = nullptr)
      const override {
    return {lgr_,
            prog_,
            std::move(blkmgr),
            make_segmenter_config(cat, checkpoint, hash_worker_pool),
            cc,
            cat_size,
            std::move(block_ready)};
  }

  size_t get_block_size() const override {
    return static_cast<size_t>(1) << cfg_.block_size_bits;
  }

  uint64_t estimate_memory_usage(
      fragment_category cat, file_size_t cat_size,
      compression_constraints const& cc) const override {
    auto const cfg = make_segmenter_config(cat, {}, nullptr);

    if (cfg.global_dedup_chunk_size > 0) {
      // Worst-case estimate for the global dedup index: one entry per
      // chunk if no data is duplicated at all. phmap overhead is roughly
      // 40-50 bytes per entry.
      static constexpr uint64_t kBytesPerIndexEntry{48};
      return static_cast<uint64_t>(cat_size) /
                 cfg.global_dedup_chunk_size * kBytesPerIndexEntry +
             (static_cast<uint64_t>(1) << cfg.block_size_bits);
    }

    return segmenter::estimate_memory_usage(cfg, cc);
  }

  bool serializes_categories() const override {
    return cfg_.global_dedup_chunk_size.has_value();
  }

 private:
  segmenter::config
  make_segmenter_config(fragment_category cat,
                        segmenter_checkpoint_config const& checkpoint,
                        std::shared_ptr<thread_pool> hash_worker_pool) const {
    segmenter::config cfg;

    if (catmgr_) {
      cfg.context = category_prefix(catmgr_, cat);
    }

    cfg.blockhash_window_size = cfg_.blockhash_window_size.get(cat);
    cfg.window_increment_shift = cfg_.window_increment_shift.get(cat);
    cfg.max_active_blocks = cfg_.max_active_blocks.get(cat);
    cfg.bloom_filter_size = cfg_.bloom_filter_size.get(cat);
    cfg.block_size_bits = cfg_.block_size_bits;
    cfg.enable_sparse_files = cfg_.enable_sparse_files;

    if (global_index_) {
      cfg.global_dedup_index = global_index_;
      cfg.global_dedup_chunk_size = *cfg_.global_dedup_chunk_size;
      cfg.global_dedup_max_chunk_size = cfg_.global_dedup_max_chunk_size;
      cfg.checkpoint = checkpoint;
    }

    cfg.hash_worker_pool = hash_worker_pool;

    return cfg;
  }

  logger& lgr_;
  writer_progress& prog_;
  std::shared_ptr<categorizer_manager> catmgr_;
  segmenter_factory::config cfg_;
  std::shared_ptr<global_dedup_index> global_index_;
};

} // namespace internal

segmenter_factory::segmenter_factory(
    logger& lgr, writer_progress& prog,
    std::shared_ptr<categorizer_manager> catmgr, config const& cfg)
    : impl_(std::make_unique<internal::segmenter_factory_>(
          lgr, prog, std::move(catmgr), cfg)) {}

segmenter_factory::segmenter_factory(logger& lgr, writer_progress& prog,
                                     config const& cfg)
    : segmenter_factory(lgr, prog, nullptr, cfg) {}

segmenter_factory::segmenter_factory(logger& lgr, writer_progress& prog)
    : segmenter_factory(lgr, prog, config{}) {}

segmenter_factory::segmenter_factory(
    logger& lgr, writer_progress& prog,
    std::shared_ptr<categorizer_manager> catmgr)
    : segmenter_factory(lgr, prog, std::move(catmgr), config{}) {}

} // namespace dwarfs::writer
