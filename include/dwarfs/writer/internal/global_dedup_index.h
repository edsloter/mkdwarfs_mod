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

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <io.h>
#define dwarfs_fseeko(f, off, whence) _fseeki64(f, static_cast<__int64>(off), whence)
#define dwarfs_fileno(f) _fileno(f)
#define dwarfs_fsync(f) _commit(dwarfs_fileno(f))
#else
#include <unistd.h>
#define dwarfs_fseeko(f, off, whence) fseeko(f, static_cast<off_t>(off), whence)
#define dwarfs_fileno(f) fileno(f)
#define dwarfs_fsync(f) fsync(dwarfs_fileno(f))
#endif

#include <parallel_hashmap/phmap.h>

namespace dwarfs::writer::internal {

/**
 * Index for global, chunk-level de-duplication.
 *
 * Maps the strong hash (XXH3-128) of a content-defined chunk to the
 * location where the chunk is stored in the file system image.
 *
 * The index is shared by all segmenter (category) streams, but it is
 * intentionally *not* thread-safe: when global de-duplication is enabled,
 * the scanner processes category streams sequentially, which guarantees
 * fully deterministic (reproducible) images.
 *
 * When a memory limit is configured and the in-memory map exceeds it,
 * entries are spilled to sorted temporary files on disk. Lookups fall
 * through from RAM to each spill file (newest first) via binary search.
 * Because entries are write-once (each key is inserted at most once),
 * spill files have strictly disjoint key sets.
 *
 * For crash-safe resume, persistence can be enabled. In that mode every
 * newly inserted entry is also appended to a sequential log file (plus a
 * small meta file recording the durable offset). The log can be reloaded
 * after an interruption to reconstruct exactly the deduplication state.
 * Callers are responsible for truncating the log to the durable offset
 * (the offset that corresponds to the last fully written image block)
 * before reloading, so that no chunk is deduplicated against a block that
 * never made it into the image.
 */
class global_dedup_index {
 public:
  struct chunk_location {
    uint32_t block;
    uint32_t offset;
    uint32_t size;
  };

  using key_type = std::pair<uint64_t, uint64_t>;

  struct key_hash {
    size_t operator()(key_type const& key) const {
      auto const hi = key.first;
      auto const lo = key.second;
      return static_cast<size_t>(lo ^ (hi + UINT64_C(0x9E3779B97F4A7C15) +
                                       (hi << 6) + (hi >> 2)));
    }
  };

  /**
   * @param max_memory  Maximum in-memory index size in bytes (0 = unlimited).
   * @param tmp_dir     Directory for spill files (empty = system temp).
   */
  explicit global_dedup_index(size_t max_memory = 0,
                              std::filesystem::path tmp_dir = {})
      : max_memory_{max_memory}
      , tmp_dir_{std::move(tmp_dir)} {
    if (max_memory_ > 0 && tmp_dir_.empty()) {
      tmp_dir_ = std::filesystem::temp_directory_path();
    }
  }

  ~global_dedup_index() {
    log_file_.reset();
    for (auto& sf : spill_files_) {
      sf.file.reset(); // close handle before removing (required on Windows)
      std::filesystem::remove(sf.path, ec_ignore_);
    }
  }

  global_dedup_index(global_dedup_index const&) = delete;
  global_dedup_index& operator=(global_dedup_index const&) = delete;
  global_dedup_index(global_dedup_index&&) = delete;
  global_dedup_index& operator=(global_dedup_index&&) = delete;

  chunk_location const* find(key_type const& key) const {
    // Check in-memory map first (most common case).
    auto it = map_.find(key);
    if (it != map_.end()) {
      return &it->second;
    }

    // Check spill files newest-first (most recent data is most likely to match
    // during active scanning).
    for (auto it_sf = spill_files_.rbegin(); it_sf != spill_files_.rend();
         ++it_sf) {
      if (auto const* loc = binary_search_file(*it_sf, key)) {
        return loc;
      }
    }

    return nullptr;
  }

  void insert(key_type const& key, chunk_location const& loc) {
    if (map_.emplace(key, loc).second) {
      append_log_entry(make_disk_entry(key, loc));
    }

    if (max_memory_ > 0 && memory_usage() > max_memory_) {
      spill_to_disk();
    }
  }

  size_t size() const {
    size_t total = map_.size();
    for (auto const& sf : spill_files_) {
      total += sf.entry_count;
    }
    return total;
  }

  uint64_t memory_usage() const {
    return map_.capacity() * sizeof(typename map_type::slot_type) +
           map_.capacity() + 1;
  }

  size_t spill_file_count() const { return spill_files_.size(); }

  uint64_t spill_file_disk_bytes() const {
    uint64_t total = 0;
    for (auto const& sf : spill_files_) {
      total += sf.entry_count * kEntrySize;
    }
    return total;
  }

  // ---- Persistence / crash-safe resume ----------------------------------

  bool persistent() const noexcept { return static_cast<bool>(log_file_); }

  std::filesystem::path const& persistence_dir() const noexcept {
    return persist_dir_;
  }

  /**
   * Enable persistence, storing the append log and meta file in `dir`.
   * If no log exists yet, a fresh one is created (with a format header).
   * If a log already exists, call load_persisted() to reconstruct the
   * in-memory map before continuing.
   */
  void enable_persistence(std::filesystem::path const& dir) {
    persist_dir_ = dir;
    std::error_code ec;
    std::filesystem::create_directories(persist_dir_, ec);
    if (ec) {
      throw std::runtime_error("cannot create dedup index persistence dir '" +
                               persist_dir_.string() + "': " + ec.message());
    }

    log_path_ = persist_dir_ / "dedup_index.log";
    meta_path_ = persist_dir_ / "dedup_index.meta";

    std::uintmax_t size = 0;
    if (std::filesystem::exists(log_path_)) {
      size = std::filesystem::file_size(log_path_);
    }

    if (size == 0) {
      // Fresh log: write the format header.
      std::FILE* wf = std::fopen(log_path_.string().c_str(), "wb");
      if (!wf) {
        throw std::runtime_error("cannot create dedup index log '" +
                                 log_path_.string() + "'");
      }
      std::fwrite(kLogMagic, 1, sizeof(kLogMagic), wf);
      std::fclose(wf);
      size = sizeof(kLogMagic);
    } else if (size < sizeof(kLogMagic)) {
      throw std::runtime_error("dedup index log '" + log_path_.string() +
                               "' is truncated");
    }

    log_file_.reset(std::fopen(log_path_.string().c_str(), "r+b"));
    if (!log_file_) {
      throw std::runtime_error("cannot open dedup index log '" +
                               log_path_.string() + "' for update");
    }
    dwarfs_fseeko(log_file_.get(), 0, SEEK_END);
    log_size_ = size;
  }

  /**
   * Start a brand new persistence log in `dir`, discarding any previously
   * persisted state (log, meta and spill files). Unlike discard_persistence()
   * this leaves the index with an open, empty append log so subsequent
   * inserts are persisted for a future resume.
   */
  void reset_persistence(std::filesystem::path const& dir) {
    enable_persistence(dir);   // establishes log/meta paths
    discard_persistence();     // removes stale files, clears in-memory state
    enable_persistence(dir);   // creates a fresh, open append log
  }

  /**
   * Flush the append log to the OS and durable storage.
   */
  void flush_log() {
    if (log_file_) {
      std::fflush(log_file_.get());
      dwarfs_fsync(log_file_.get());
    }
  }

  /**
   * Flush the log and return its current logical size. This value can later
   * be committed via set_durable_offset() once the corresponding image block
   * has been fully written.
   */
  uint64_t capture_log_offset() {
    flush_log();
    return log_size_;
  }

  uint64_t log_size() const noexcept { return log_size_; }

  /**
   * Record the durable log offset (must correspond to the end of the last
   * fully written image block) in the meta file, atomically.
   */
  void set_durable_offset(uint64_t off) {
    if (!persistent()) {
      return;
    }
    durable_offset_ = off;
    write_meta();
  }

  uint64_t durable_offset() const noexcept { return durable_offset_; }

  /**
   * Truncate the log to `off` bytes (dropping entries that were appended
   * after the last durable image block), then reopen for append.
   */
  void truncate_log_to(uint64_t off) {
    if (!persistent()) {
      return;
    }
    log_file_.reset();
    std::error_code ec;
    std::filesystem::resize_file(log_path_, off, ec);
    if (ec) {
      throw std::runtime_error("cannot truncate dedup index log '" +
                               log_path_.string() + "': " + ec.message());
    }
    log_file_.reset(std::fopen(log_path_.string().c_str(), "r+b"));
    if (!log_file_) {
      throw std::runtime_error("cannot reopen dedup index log '" +
                               log_path_.string() + "'");
    }
    dwarfs_fseeko(log_file_.get(), 0, SEEK_END);
    log_size_ = off;
  }

  /**
   * Reload index entries from the persisted log into the in-memory map.
   * Any stale spill files are discarded first. Heavy: reads the whole log
   * sequentially, so it should be called once at startup.
   */
  void load_persisted() {
    if (!persistent()) {
      return;
    }

    for (auto& sf : spill_files_) {
      sf.file.reset();
      std::filesystem::remove(sf.path, ec_ignore_);
    }
    spill_files_.clear();

    // Without a meta file, no block has been committed as durable, so only
    // the log header may be trusted. Reloading uncommitted entries would
    // create references to image blocks that were never written.
    durable_offset_ = std::filesystem::exists(meta_path_)
                          ? read_meta()
                          : sizeof(kLogMagic);
    if (durable_offset_ > log_size_) {
      durable_offset_ = log_size_;
    }
    if (durable_offset_ < sizeof(kLogMagic)) {
      durable_offset_ = sizeof(kLogMagic);
    }
    truncate_log_to(durable_offset_);
    map_.clear();
    map_.rehash(0);

    std::FILE* rf = std::fopen(log_path_.string().c_str(), "rb");
    if (!rf) {
      throw std::runtime_error("cannot read dedup index log '" +
                               log_path_.string() + "'");
    }

    char magic[sizeof(kLogMagic)];
    if (std::fread(magic, 1, sizeof(magic), rf) != sizeof(magic) ||
        std::memcmp(magic, kLogMagic, sizeof(magic)) != 0) {
      std::fclose(rf);
      throw std::runtime_error("dedup index log '" + log_path_.string() +
                               "' has an invalid header");
    }

    constexpr size_t kChunk = 1 << 16;
    std::vector<disk_entry> buf(kChunk);
    size_t loaded = 0;
    for (;;) {
      size_t n = std::fread(buf.data(), kEntrySize, buf.size(), rf);
      for (size_t i = 0; i < n; ++i) {
        insert_loaded(entry_key(buf[i]), entry_location(buf[i]));
      }
      loaded += n;
      if (n < buf.size()) {
        break;
      }
    }
    std::fclose(rf);
    (void)loaded;
  }

  /**
   * Delete all persisted index state. Call after the image has been fully
   * written and validated.
   */
  void discard_persistence() {
    log_file_.reset();
    if (!persist_dir_.empty()) {
      std::filesystem::remove(log_path_, ec_ignore_);
      std::filesystem::remove(meta_path_, ec_ignore_);
    }
    for (auto& sf : spill_files_) {
      sf.file.reset();
      std::filesystem::remove(sf.path, ec_ignore_);
    }
    spill_files_.clear();
    map_.clear();
    map_.rehash(0);
    log_size_ = 0;
    durable_offset_ = 0;
  }

 private:
  // On-disk entry layout: key (16 bytes) + location (12 bytes) = 28 bytes.
  // Explicitly packed so the on-disk/log format is identical on all
  // compilers (MSVC would otherwise pad this to 32 bytes).
#pragma pack(push, 1)
  struct disk_entry {
    uint64_t key_hi;
    uint64_t key_lo;
    uint32_t block;
    uint32_t offset;
    uint32_t size;
  };
#pragma pack(pop)

  static_assert(sizeof(disk_entry) == 28, "unexpected disk_entry padding");

  static constexpr size_t kEntrySize = sizeof(disk_entry);
  static constexpr char kLogMagic[8] = {'D', 'W', 'R', 'S', 'D', 'I',
                                        'D', 'X'};
  static constexpr uint32_t kMetaMagic = 0x44574D44; // "DMWD"

  struct spill_file {
    std::filesystem::path path;
    size_t entry_count{0};
    std::unique_ptr<FILE, decltype(&fclose)> file{nullptr, fclose};
  };

  static disk_entry make_disk_entry(key_type const& key,
                                    chunk_location const& loc) {
    disk_entry e;
    e.key_hi = key.first;
    e.key_lo = key.second;
    e.block = loc.block;
    e.offset = loc.offset;
    e.size = loc.size;
    return e;
  }

  static key_type entry_key(disk_entry const& e) {
    return {e.key_hi, e.key_lo};
  }

  static chunk_location entry_location(disk_entry const& e) {
    return {e.block, e.offset, e.size};
  }

  static bool entry_less(disk_entry const& a, disk_entry const& b) {
    if (a.key_hi != b.key_hi) {
      return a.key_hi < b.key_hi;
    }
    return a.key_lo < b.key_lo;
  }

  void append_log_entry(disk_entry const& e) {
    if (!log_file_) {
      return;
    }
    std::fwrite(&e, kEntrySize, 1, log_file_.get());
    log_size_ += kEntrySize;
  }

  void insert_loaded(key_type const& key, chunk_location const& loc) {
    map_.emplace(key, loc);
    if (max_memory_ > 0 && memory_usage() > max_memory_) {
      spill_to_disk();
    }
  }

  void write_meta() {
    if (meta_path_.empty()) {
      return;
    }
    auto tmp = meta_path_;
    tmp += ".tmp";
    {
      std::FILE* wf = std::fopen(tmp.string().c_str(), "wb");
      if (!wf) {
        return;
      }
      std::fwrite(&kMetaMagic, sizeof(kMetaMagic), 1, wf);
      std::fwrite(&durable_offset_, sizeof(durable_offset_), 1, wf);
      std::fflush(wf);
      dwarfs_fsync(wf);
      std::fclose(wf);
    }
    std::error_code ec;
    std::filesystem::rename(tmp, meta_path_, ec);
    if (ec) {
      std::filesystem::remove(meta_path_, ec_ignore_);
      std::filesystem::rename(tmp, meta_path_, ec_ignore_);
    }
  }

  uint64_t read_meta() const {
    constexpr uint64_t kNoCommittedBlocks = sizeof(kLogMagic);
    std::FILE* rf = std::fopen(meta_path_.string().c_str(), "rb");
    if (!rf) {
      return kNoCommittedBlocks;
    }
    uint32_t magic = 0;
    uint64_t off = kNoCommittedBlocks;
    if (std::fread(&magic, sizeof(magic), 1, rf) != 1 ||
        magic != kMetaMagic ||
        std::fread(&off, sizeof(off), 1, rf) != 1) {
      off = kNoCommittedBlocks;
    }
    std::fclose(rf);
    return off;
  }

  void spill_to_disk() {
    // Collect all entries from the in-memory map.
    std::vector<disk_entry> entries;
    entries.reserve(map_.size());
    for (auto const& [key, loc] : map_) {
      entries.push_back(make_disk_entry(key, loc));
    }

    // Sort by key for binary search.
    std::sort(entries.begin(), entries.end(), entry_less);

    // Write to a new spill file, then reopen for reading.
    spill_file sf;
    sf.path = tmp_dir_ / ("dwarfs_dedup_spill_" +
                           std::to_string(spill_file_counter_++) + ".bin");
    sf.entry_count = entries.size();

    {
      FILE* wf = std::fopen(sf.path.string().c_str(), "wb");
      if (!wf) {
        return;
      }
      std::fwrite(entries.data(), kEntrySize, entries.size(), wf);
      std::fflush(wf);
      std::fclose(wf);
    }

    // Open for reading and keep the handle open for binary search.
    sf.file.reset(std::fopen(sf.path.string().c_str(), "rb"));

    spill_files_.push_back(std::move(sf));

    // Clear the in-memory map.
    map_.clear();
    map_.rehash(0);
  }

  chunk_location const* binary_search_file(spill_file const& sf,
                                           key_type const& key) const {
    if (sf.entry_count == 0 || !sf.file) {
      return nullptr;
    }

    // Binary search by seeking — only reads kEntrySize bytes per comparison.
    size_t lo = 0;
    size_t hi = sf.entry_count;

    while (lo < hi) {
      size_t mid = lo + (hi - lo) / 2;
      disk_entry mid_entry;

      if (dwarfs_fseeko(sf.file.get(), mid * kEntrySize, SEEK_SET) != 0 ||
          std::fread(&mid_entry, kEntrySize, 1, sf.file.get()) != 1) {
        return nullptr;
      }

      if (mid_entry.key_hi < key.first ||
          (mid_entry.key_hi == key.first && mid_entry.key_lo < key.second)) {
        lo = mid + 1;
      } else if (mid_entry.key_hi > key.first ||
                 (mid_entry.key_hi == key.first &&
                  mid_entry.key_lo > key.second)) {
        hi = mid;
      } else {
        // Found
        last_found_ = entry_location(mid_entry);
        return &last_found_;
      }
    }

    return nullptr;
  }

  using map_type =
      phmap::flat_hash_map<key_type, chunk_location, key_hash>;

  size_t max_memory_{0};
  std::filesystem::path tmp_dir_;
  int spill_file_counter_{0};
  std::vector<spill_file> spill_files_;
  mutable chunk_location last_found_{};

  map_type map_;

  // Persistence state.
  std::filesystem::path persist_dir_;
  std::filesystem::path log_path_;
  std::filesystem::path meta_path_;
  std::unique_ptr<FILE, decltype(&fclose)> log_file_{nullptr, fclose};
  uint64_t log_size_{0};
  uint64_t durable_offset_{0};
  std::error_code ec_ignore_;
};

} // namespace dwarfs::writer::internal
