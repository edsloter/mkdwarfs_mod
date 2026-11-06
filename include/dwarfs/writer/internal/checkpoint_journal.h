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

#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace dwarfs::writer::internal {

/**
 * On-disk journal for the Phase 1 (scan) results, used to resume a build
 * without re-reading or re-hashing any file data.
 *
 * Phase 1 produces, for every inode, its fragment layout (category,
 * subcategory, kind, size) and per-category similarity/nilsimsa hashes, and
 * for every file the mapping to its inode plus metadata inode number. None
 * of the fragment *chunk* data (block/offset/size) is part of this journal:
 * that is Phase 2 output.
 *
 * The journal is written once, after Phase 1 has fully completed (so it is
 * never partially valid); a crash during Phase 1 simply redoes Phase 1.
 * All multi-byte values are little-endian and records are packed, so the
 * format is byte-identical across compilers (MSVC would otherwise pad).
 *
 * Framing: the file starts with a header, followed by zero or more inode
 * records (tag 0x01), and ends with a trailer (tag 0x02). Each inode record
 * carries its own file records. A single tag byte makes the framing robust
 * even though payloads are variable length.
 */

struct checkpoint_job_signature {
  uint64_t inputs_hash{0};
  uint64_t config_hash{0};

  friend bool operator==(checkpoint_job_signature const& a,
                         checkpoint_job_signature const& b) {
    return a.inputs_hash == b.inputs_hash && a.config_hash == b.config_hash;
  }
};

/**
 * Snapshot of the progress counters that are *not* recomputed when a build
 * is resumed. These fall into two groups:
 *
 *  - Phase 1 counters that are only produced while reading/hashing file data
 *    (skipped entirely when resuming from the Phase 1 journal), and
 *  - Phase 2 counters that describe already-committed output (everything up
 *    to the last durable checkpoint).
 *
 * Counters that are naturally rebuilt during a Phase 2 replay (e.g. the
 * chunk count) are deliberately not part of this snapshot.
 */
struct build_progress_snapshot {
  uint64_t original_size{0};
  uint64_t symlink_size{0};
  uint64_t allocated_original_size{0};
  uint64_t saved_by_deduplication{0};
  uint64_t allocated_saved_by_deduplication{0};
  uint64_t duplicate_files{0};
  uint64_t hash_bytes{0};
  uint64_t similarity_bytes{0};
  uint64_t files_scanned{0};
  uint64_t inodes_scanned{0};
  uint64_t fragments_found{0};
  uint64_t filesystem_size{0};
  uint64_t block_count{0};
  uint64_t compressed_size{0};
  uint64_t blocks_written{0};
  uint64_t saved_by_segmentation{0};
};

struct phase1_fragment {
  uint32_t category{0};
  int32_t subcategory{-1}; // -1 = none
  uint8_t kind{0};         // 0 = data, 1 = hole
  uint64_t size{0};
};

struct phase1_similarity {
  uint32_t category{0};
  int32_t subcategory{-1}; // -1 = none
  uint8_t type{0};         // 0 = similarity (u32), 1 = nilsimsa
  uint32_t sim_value{0};
  std::array<uint64_t, 4> nilsimsa{};
};

struct phase1_file {
  uint32_t file_id{0};
  uint32_t inode_id{0};        // owning inode (redundant; for validation)
  uint64_t entry_inode_num{0}; // metadata inode number, 0 if unset
  uint64_t order_index{0};
  uint8_t flags{0}; // bit 0: invalid, bit 1: empty
};

struct phase1_inode {
  uint32_t inode_id{0};
  uint64_t obj_num{0}; // inode_manager inode number, 0 if unset
  std::vector<phase1_file> files;
  std::vector<phase1_fragment> fragments;
  std::vector<phase1_similarity> similarities;
  uint8_t flags{0}; // bit 0: has scan error
};

namespace detail {

inline constexpr uint8_t kTagInode = 0x01;
inline constexpr uint8_t kTagTrailer = 0x02;

#pragma pack(push, 1)
struct journal_header {
  char magic[8];
  uint32_t version;
  uint32_t reserved;
  uint64_t inputs_hash;
  uint64_t config_hash;
};

struct journal_trailer {
  uint64_t inode_count;
  uint64_t file_count;
  uint64_t unique_inode_count;
  uint64_t file_inode_count;
  uint64_t inputs_hash;
  uint64_t config_hash;
  build_progress_snapshot progress;
};

struct disk_fragment {
  uint32_t category;
  int32_t subcategory;
  uint8_t kind;
  uint8_t pad[3];
  uint64_t size;
};

struct disk_similarity {
  uint32_t category;
  int32_t subcategory;
  uint8_t type;
  uint8_t pad[3];
  uint32_t sim_value;
  uint64_t nilsimsa[4];
};

struct disk_file {
  uint32_t file_id;
  uint32_t inode_id;
  uint64_t entry_inode_num;
  uint64_t order_index;
  uint8_t flags;
};
#pragma pack(pop)

inline constexpr char kPhase1Magic[8] = {'D', 'W', 'R', 'S', 'P', '1', 'J', ' '};
inline constexpr uint32_t kPhase1Version = 2;

inline void write_exact(std::FILE* f, void const* p, size_t n) {
  if (n != 0 && std::fwrite(p, 1, n, f) != n) {
    throw std::runtime_error("checkpoint journal: write failed");
  }
}

inline void read_exact(std::FILE* f, void* p, size_t n) {
  if (n != 0 && std::fread(p, 1, n, f) != n) {
    throw std::runtime_error("checkpoint journal: unexpected end of file");
  }
}

} // namespace detail

class phase1_journal_writer {
 public:
  phase1_journal_writer(std::filesystem::path const& path,
                        checkpoint_job_signature sig)
      : path_{path}
      , sig_{sig} {
    file_ = std::fopen(path_.string().c_str(), "wb");
    if (!file_) {
      throw std::runtime_error("cannot create checkpoint journal '" +
                               path_.string() + "'");
    }
    detail::journal_header hdr{};
    std::memcpy(hdr.magic, detail::kPhase1Magic, sizeof(hdr.magic));
    hdr.version = detail::kPhase1Version;
    hdr.inputs_hash = sig_.inputs_hash;
    hdr.config_hash = sig_.config_hash;
    detail::write_exact(file_, &hdr, sizeof(hdr));
  }

  ~phase1_journal_writer() {
    if (file_) {
      std::fclose(file_);
    }
  }

  phase1_journal_writer(phase1_journal_writer const&) = delete;
  phase1_journal_writer& operator=(phase1_journal_writer const&) = delete;

  void add_inode(phase1_inode const& ino) {
    detail::write_exact(file_, &detail::kTagInode, sizeof(uint8_t));
    uint32_t fc = static_cast<uint32_t>(ino.files.size());
    uint32_t gc = static_cast<uint32_t>(ino.fragments.size());
    uint32_t sc = static_cast<uint32_t>(ino.similarities.size());
    detail::write_exact(file_, &ino.inode_id, sizeof(ino.inode_id));
    detail::write_exact(file_, &ino.obj_num, sizeof(ino.obj_num));
    detail::write_exact(file_, &fc, sizeof(fc));
    for (auto const& f : ino.files) {
      detail::disk_file d{f.file_id, f.inode_id, f.entry_inode_num,
                          f.order_index, f.flags};
      detail::write_exact(file_, &d, sizeof(d));
    }
    detail::write_exact(file_, &gc, sizeof(gc));
    for (auto const& fr : ino.fragments) {
      detail::disk_fragment d{fr.category, fr.subcategory, fr.kind, {}, fr.size};
      detail::write_exact(file_, &d, sizeof(d));
    }
    detail::write_exact(file_, &sc, sizeof(sc));
    for (auto const& si : ino.similarities) {
      detail::disk_similarity d{};
      d.category = si.category;
      d.subcategory = si.subcategory;
      d.type = si.type;
      d.sim_value = si.sim_value;
      std::memcpy(d.nilsimsa, si.nilsimsa.data(), sizeof(d.nilsimsa));
      detail::write_exact(file_, &d, sizeof(d));
    }
    detail::write_exact(file_, &ino.flags, sizeof(ino.flags));
    ++inode_count_;
    file_count_ += fc;
  }

  void finish(uint64_t unique_inode_count, uint64_t file_inode_count,
              build_progress_snapshot const& progress) {
    detail::write_exact(file_, &detail::kTagTrailer, sizeof(uint8_t));
    detail::journal_trailer tr{};
    tr.inode_count = inode_count_;
    tr.file_count = file_count_;
    tr.unique_inode_count = unique_inode_count;
    tr.file_inode_count = file_inode_count;
    tr.inputs_hash = sig_.inputs_hash;
    tr.config_hash = sig_.config_hash;
    tr.progress = progress;
    detail::write_exact(file_, &tr, sizeof(tr));
    std::fflush(file_);
    std::fclose(file_);
    file_ = nullptr;
  }

 private:
  std::filesystem::path path_;
  checkpoint_job_signature sig_;
  std::FILE* file_{nullptr};
  uint64_t inode_count_{0};
  uint64_t file_count_{0};
};

struct phase1_journal_data {
  checkpoint_job_signature signature;
  uint64_t unique_inode_count{0};
  uint64_t file_inode_count{0};
  build_progress_snapshot progress;
  std::vector<phase1_inode> inodes;
  std::vector<phase1_file> files;
};

inline phase1_journal_data
read_phase1_journal(std::filesystem::path const& path) {
  using namespace detail;
  std::FILE* f = std::fopen(path.string().c_str(), "rb");
  if (!f) {
    throw std::runtime_error("cannot open checkpoint journal '" + path.string() +
                             "'");
  }
  struct closer {
    std::FILE* f;
    ~closer() { std::fclose(f); }
  } c{f};

  journal_header hdr{};
  read_exact(f, &hdr, sizeof(hdr));
  if (std::memcmp(hdr.magic, kPhase1Magic, sizeof(hdr.magic)) != 0 ||
      hdr.version != kPhase1Version) {
    throw std::runtime_error("checkpoint journal '" + path.string() +
                             "' has an invalid or unsupported header");
  }

  phase1_journal_data data;
  data.signature = {hdr.inputs_hash, hdr.config_hash};

  for (;;) {
    uint8_t tag = 0;
    read_exact(f, &tag, sizeof(tag));

    if (tag == kTagTrailer) {
      journal_trailer tr{};
      read_exact(f, &tr, sizeof(tr));
      if (tr.inputs_hash != hdr.inputs_hash ||
          tr.config_hash != hdr.config_hash) {
        throw std::runtime_error("checkpoint journal '" + path.string() +
                                 "' signature mismatch");
      }
      if (tr.inode_count != data.inodes.size() ||
          tr.file_count != data.files.size()) {
        throw std::runtime_error("checkpoint journal '" + path.string() +
                                 "' record count mismatch");
      }
      data.unique_inode_count = tr.unique_inode_count;
      data.file_inode_count = tr.file_inode_count;
      data.progress = tr.progress;
      break;
    }

    if (tag != kTagInode) {
      throw std::runtime_error("checkpoint journal '" + path.string() +
                               "' has an unknown record tag");
    }

    phase1_inode ino;
    read_exact(f, &ino.inode_id, sizeof(ino.inode_id));
    read_exact(f, &ino.obj_num, sizeof(ino.obj_num));
    uint32_t fc = 0;
    read_exact(f, &fc, sizeof(fc));
    ino.files.resize(fc);
    for (uint32_t i = 0; i < fc; ++i) {
      disk_file d{};
      read_exact(f, &d, sizeof(d));
      ino.files[i] = {d.file_id, d.inode_id, d.entry_inode_num, d.order_index,
                      d.flags};
    }
    uint32_t gc = 0;
    read_exact(f, &gc, sizeof(gc));
    ino.fragments.resize(gc);
    for (uint32_t i = 0; i < gc; ++i) {
      disk_fragment d{};
      read_exact(f, &d, sizeof(d));
      ino.fragments[i] = {d.category, d.subcategory, d.kind, d.size};
    }
    uint32_t sc = 0;
    read_exact(f, &sc, sizeof(sc));
    ino.similarities.resize(sc);
    for (uint32_t i = 0; i < sc; ++i) {
      disk_similarity d{};
      read_exact(f, &d, sizeof(d));
      phase1_similarity s;
      s.category = d.category;
      s.subcategory = d.subcategory;
      s.type = d.type;
      s.sim_value = d.sim_value;
      std::memcpy(s.nilsimsa.data(), d.nilsimsa, sizeof(d.nilsimsa));
      ino.similarities[i] = s;
    }
    read_exact(f, &ino.flags, sizeof(ino.flags));

    for (auto const& file : ino.files) {
      data.files.push_back(file);
    }
    data.inodes.push_back(std::move(ino));
  }

  return data;
}

/**
 * Phase 2 (block building) checkpoint state.
 *
 * Unlike Phase 1, Phase 2 cannot cheaply journal "what has been done so
 * far", because the actual output is a partially written image file plus
 * the global de-duplication index.  Instead we periodically capture a
 * *quiescent* snapshot of everything needed to continue appending to the
 * image:
 *
 *  - the committed size of the image file (everything after it is
 *    truncated away on resume),
 *  - the writer's section bookkeeping,
 *  - the logical -> physical block map, and
 *  - the durable offset of the dedup index log.
 *
 * On resume, Phase 2 is replayed from the start; every chunk that was
 * already committed is found in the restored index and referenced without
 * recompressing or rewriting anything, while chunks that were in flight
 * when the checkpoint was taken are re-stored.
 */

struct phase2_block_entry {
  uint64_t logical{0};
  uint64_t physical{0};
  uint32_t category{0};
  int32_t subcategory{-1};
};

struct phase2_state {
  checkpoint_job_signature signature;
  uint64_t index_durable_offset{0};
  uint64_t index_entry_count{0};
  uint64_t num_blocks{0};
  uint64_t section_number{0};
  uint64_t image_size{0};
  uint64_t header_size{0};
  build_progress_snapshot progress;
  std::vector<uint64_t> section_index;
  std::vector<phase2_block_entry> block_map;
};

namespace detail {

inline constexpr char kPhase2Magic[8] = {'D', 'W', 'R', 'S', 'P', '2', 'S', ' '};
inline constexpr uint32_t kPhase2Version = 2;
inline constexpr char kPhase2Tag = 'S';

#pragma pack(push, 1)
struct phase2_header {
  char magic[8];
  uint32_t version;
  uint32_t reserved;
  uint64_t inputs_hash;
  uint64_t config_hash;
};

struct disk_phase2_block {
  uint64_t logical;
  uint64_t physical;
  uint32_t category;
  int32_t subcategory;
};
#pragma pack(pop)

} // namespace detail

inline void write_phase2_state(std::filesystem::path const& path,
                               phase2_state const& st) {
  using namespace detail;
  auto tmp = path;
  tmp += ".tmp";

  std::FILE* f = std::fopen(tmp.string().c_str(), "wb");
  if (!f) {
    throw std::runtime_error("cannot create phase 2 checkpoint state '" +
                             tmp.string() + "'");
  }

  struct closer {
    std::FILE* f;
    bool ok{false};
    ~closer() {
      if (f) {
        std::fclose(f);
      }
    }
  } c{f};

  phase2_header hdr{};
  std::memcpy(hdr.magic, kPhase2Magic, sizeof(hdr.magic));
  hdr.version = kPhase2Version;
  hdr.inputs_hash = st.signature.inputs_hash;
  hdr.config_hash = st.signature.config_hash;

  write_exact(f, &hdr, sizeof(hdr));
  write_exact(f, &kPhase2Tag, sizeof(uint8_t));
  write_exact(f, &st.index_durable_offset, sizeof(uint64_t));
  write_exact(f, &st.index_entry_count, sizeof(uint64_t));
  write_exact(f, &st.num_blocks, sizeof(uint64_t));
  write_exact(f, &st.section_number, sizeof(uint64_t));
  write_exact(f, &st.image_size, sizeof(uint64_t));
  write_exact(f, &st.header_size, sizeof(uint64_t));
  write_exact(f, &st.progress, sizeof(st.progress));
  uint64_t const sic = st.section_index.size();
  write_exact(f, &sic, sizeof(uint64_t));
  for (auto v : st.section_index) {
    write_exact(f, &v, sizeof(uint64_t));
  }
  uint64_t const bmc = st.block_map.size();
  write_exact(f, &bmc, sizeof(uint64_t));
  for (auto const& b : st.block_map) {
    disk_phase2_block d{b.logical, b.physical, b.category, b.subcategory};
    write_exact(f, &d, sizeof(d));
  }

  std::fflush(f);
  std::fclose(f);
  c.f = nullptr;

  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    // Windows cannot rename over an existing file.
    std::filesystem::remove(path, ec);
    ec.clear();
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
      throw std::runtime_error("cannot commit phase 2 checkpoint state '" +
                               path.string() + "': " + ec.message());
    }
  }
}

inline phase2_state read_phase2_state(std::filesystem::path const& path) {
  using namespace detail;
  std::FILE* f = std::fopen(path.string().c_str(), "rb");
  if (!f) {
    throw std::runtime_error("cannot open phase 2 checkpoint state '" +
                             path.string() + "'");
  }
  struct closer {
    std::FILE* f;
    ~closer() { std::fclose(f); }
  } c{f};

  phase2_header hdr{};
  read_exact(f, &hdr, sizeof(hdr));
  if (std::memcmp(hdr.magic, kPhase2Magic, sizeof(hdr.magic)) != 0 ||
      hdr.version != kPhase2Version) {
    throw std::runtime_error("phase 2 checkpoint state '" + path.string() +
                             "' has an invalid or unsupported header");
  }

  phase2_state st;
  st.signature = {hdr.inputs_hash, hdr.config_hash};

  uint8_t tag = 0;
  read_exact(f, &tag, sizeof(tag));
  if (tag != kPhase2Tag) {
    throw std::runtime_error("phase 2 checkpoint state '" + path.string() +
                             "' has an unknown record tag");
  }

  read_exact(f, &st.index_durable_offset, sizeof(uint64_t));
  read_exact(f, &st.index_entry_count, sizeof(uint64_t));
  read_exact(f, &st.num_blocks, sizeof(uint64_t));
  read_exact(f, &st.section_number, sizeof(uint64_t));
  read_exact(f, &st.image_size, sizeof(uint64_t));
  read_exact(f, &st.header_size, sizeof(uint64_t));
  read_exact(f, &st.progress, sizeof(st.progress));

  uint64_t sic = 0;
  read_exact(f, &sic, sizeof(uint64_t));
  st.section_index.resize(sic);
  for (uint64_t i = 0; i < sic; ++i) {
    read_exact(f, &st.section_index[i], sizeof(uint64_t));
  }

  uint64_t bmc = 0;
  read_exact(f, &bmc, sizeof(uint64_t));
  st.block_map.resize(bmc);
  for (uint64_t i = 0; i < bmc; ++i) {
    disk_phase2_block d{};
    read_exact(f, &d, sizeof(d));
    st.block_map[i] = {d.logical, d.physical, d.category, d.subcategory};
  }

  return st;
}

} // namespace dwarfs::writer::internal
