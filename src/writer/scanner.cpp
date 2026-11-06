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
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <functional>
#include <iterator>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include <dwarfs/portability/unistd.h>

#include <fmt/format.h>

#include <range/v3/view/enumerate.hpp>

#include <dwarfs/compiler.h>
#include <dwarfs/error.h>
#include <dwarfs/file_access.h>
#include <dwarfs/history.h>
#include <dwarfs/logger.h>
#include <dwarfs/match.h>
#include <dwarfs/os_access.h>
#include <dwarfs/thread_pool.h>
#include <dwarfs/util.h>
#include <dwarfs/version.h>
#include <dwarfs/writer/categorizer.h>
#include <dwarfs/writer/entry_filter.h>
#include <dwarfs/writer/filesystem_writer.h>
#include <dwarfs/writer/internal/entry_storage.h>
#include <dwarfs/writer/scanner.h>
#include <dwarfs/writer/scanner_options.h>
#include <dwarfs/writer/segmenter_factory.h>
#include <dwarfs/writer/writer_progress.h>

#include <dwarfs/writer/inode_fragments.h>

#include <dwarfs/internal/worker_group.h>
#include <dwarfs/writer/internal/block_manager.h>
#include <dwarfs/writer/internal/checkpoint_journal.h>
#include <dwarfs/writer/internal/file_scanner.h>
#include <dwarfs/writer/internal/inode_types.h>
#include <dwarfs/writer/internal/filesystem_writer_detail.h>
#include <dwarfs/writer/internal/fragment_chunkable.h>
#include <dwarfs/writer/internal/global_entry_data.h>
#include <dwarfs/writer/internal/inode_manager.h>
#include <dwarfs/writer/internal/metadata_builder.h>
#include <dwarfs/writer/internal/metadata_freezer.h>
#include <dwarfs/writer/internal/progress.h>
#include <dwarfs/writer/internal/provisional_entry.h>

namespace dwarfs::writer {

namespace internal {

using namespace dwarfs::internal;
using namespace std::chrono_literals;

namespace {

constexpr std::string_view kEnvVarDumpFilesRaw{"DWARFS_DUMP_FILES_RAW"};
constexpr std::string_view kEnvVarDumpFilesFinal{"DWARFS_DUMP_FILES_FINAL"};
constexpr std::string_view kEnvVarDumpInodes{"DWARFS_DUMP_INODES"};

build_progress_snapshot capture_progress(progress const& p) {
  build_progress_snapshot s;
  s.original_size = p.original_size;
  s.symlink_size = p.symlink_size;
  s.allocated_original_size = p.allocated_original_size;
  s.saved_by_deduplication = p.saved_by_deduplication;
  s.allocated_saved_by_deduplication = p.allocated_saved_by_deduplication;
  s.duplicate_files = p.duplicate_files;
  s.hash_bytes = p.hash.bytes;
  s.similarity_bytes = p.similarity.bytes;
  s.files_scanned = p.files_scanned;
  s.inodes_scanned = p.inodes_scanned;
  s.fragments_found = p.fragments_found;
  s.filesystem_size = p.filesystem_size;
  s.block_count = p.block_count;
  s.compressed_size = p.compressed_size;
  s.blocks_written = p.blocks_written;
  s.saved_by_segmentation = p.saved_by_segmentation;
  return s;
}

void restore_progress(progress& p, build_progress_snapshot const& s) {
  p.original_size = s.original_size;
  p.symlink_size = s.symlink_size;
  p.allocated_original_size = s.allocated_original_size;
  p.saved_by_deduplication = s.saved_by_deduplication;
  p.allocated_saved_by_deduplication = s.allocated_saved_by_deduplication;
  p.duplicate_files = s.duplicate_files;
  p.hash.bytes = s.hash_bytes;
  p.similarity.bytes = s.similarity_bytes;
  p.files_scanned = s.files_scanned;
  p.inodes_scanned = s.inodes_scanned;
  p.fragments_found = s.fragments_found;
  p.filesystem_size = s.filesystem_size;
  p.block_count = s.block_count;
  p.compressed_size = s.compressed_size;
  p.blocks_written = s.blocks_written;
  p.saved_by_segmentation = s.saved_by_segmentation;
}

class visitor_base : public entry_handle_visitor {
 public:
  void visit(file_handle) override {}
  void visit(link_handle) override {}
  void visit(dir_handle) override {}
  void visit(device_handle) override {}
  void visit(other_handle) override {}
};

class dir_set_inode_visitor : public visitor_base {
 public:
  explicit dir_set_inode_visitor(uint32_t& inode_num)
      : inode_num_(inode_num) {}

  void visit(dir_handle p) override { p.set_inode_num(inode_num_++); }

 private:
  uint32_t& inode_num_;
};

class link_set_inode_visitor : public visitor_base {
 public:
  explicit link_set_inode_visitor(uint32_t& inode_num)
      : inode_num_(inode_num) {}

  void visit(link_handle p) override { p.set_inode_num(inode_num_++); }

 private:
  uint32_t& inode_num_;
};

class device_set_inode_visitor : public visitor_base {
 public:
  explicit device_set_inode_visitor(uint32_t& inode_num)
      : inode_num_(inode_num) {}

  void visit(device_handle p) override {
    p.set_inode_num(inode_num_++);
    dev_ids_.push_back(p.posix_device_id());
  }

  std::vector<uint64_t>& device_ids() { return dev_ids_; }

 private:
  std::vector<uint64_t> dev_ids_;
  uint32_t& inode_num_;
};

class pipe_set_inode_visitor : public visitor_base {
 public:
  explicit pipe_set_inode_visitor(uint32_t& inode_num)
      : inode_num_(inode_num) {}

  void visit(other_handle p) override { p.set_inode_num(inode_num_++); }

 private:
  uint32_t& inode_num_;
};

class names_and_symlinks_visitor : public visitor_base {
 public:
  explicit names_and_symlinks_visitor(global_entry_data& data)
      : data_(data) {}

  void visit(link_handle p) override { data_.add_link(p.linkname()); }

 private:
  global_entry_data& data_;
};

class save_directories_visitor : public visitor_base {
 public:
  using dir_id_vector = metadata_builder::dir_id_vector;

  explicit save_directories_visitor(size_t num_directories)
      : directory_ids_(std::bit_width(num_directories), num_directories) {}

  void visit(dir_handle p) override {
    auto const ino = p.inode_num().value();
    directory_ids_.at(ino) = p.id();
  }

  dir_id_vector const& get_directory_ids() { return directory_ids_; }

 private:
  dir_id_vector directory_ids_;
};

class save_shared_files_visitor : public visitor_base {
 public:
  using shared_files_table_type =
      dwarfs::thrift::metadata::metadata::shared_files_table_member_type;

  explicit save_shared_files_visitor(uint32_t inode_begin, uint32_t inode_end,
                                     uint32_t num_unique_files)
      : begin_shared_{inode_begin + num_unique_files}
      , num_unique_{num_unique_files} {
    DWARFS_CHECK(inode_end - inode_begin >= num_unique_files,
                 "inconsistent file count");
    shared_files_.resize(inode_end - begin_shared_);
  }

  void visit(file_handle p) override {
    if (auto ino = p.inode_num().value(); ino >= begin_shared_) {
      auto ufi = p.unique_file_id();
      DWARFS_CHECK(ufi >= num_unique_, "inconsistent file id");
      DWARFS_NOTHROW(shared_files_.at(ino - begin_shared_)) = ufi - num_unique_;
    }
  }

  shared_files_table_type& get_shared_files() { return shared_files_; }

 private:
  uint32_t const begin_shared_;
  uint32_t const num_unique_;
  shared_files_table_type shared_files_;
};

std::string status_string(progress const& p, size_t width) {
  auto cp = p.current.load();
  std::string label, path;

  cp | match{
           [&label, &path](const_file_handle const& fh) {
             label = "scanning: ";
             path = fh.path_as_string();
           },
           [&label, &path](const_inode_handle const& ih) {
             label = "writing: ";
             path = ih.any().path_as_string();
           },
           [](std::monostate) {},
       };

  if (!path.empty()) {
    utf8_sanitize(path);
    shorten_path_string(
        path, static_cast<char>(std::filesystem::path::preferred_separator),
        width - label.size());
  }

  return label + path;
}

} // namespace

template <typename LoggerPolicy>
class scanner_ final : public scanner::impl {
 public:
  scanner_(logger& lgr, worker_group& wg, segmenter_factory& sf,
           os_access const& os, scanner_options const& options);

  void add_filter(std::unique_ptr<entry_filter>&& filter) override;

  void
  scan(filesystem_writer& fs_writer, std::filesystem::path const& path,
       writer_progress& wprog,
       std::optional<std::span<std::filesystem::path const>> list,
       std::shared_ptr<file_access const> fa,
       std::function<void(library_dependencies&)> const& extra_deps) override;

 private:
  entry_handle scan_tree(entry_storage& tree, std::filesystem::path const& path,
                         progress& prog, file_scanner& fs);

  entry_handle
  scan_list(entry_storage& tree, std::filesystem::path const& rootpath,
            std::span<std::filesystem::path const> list, progress& prog,
            file_scanner& fs);

  entry_handle add_entry(entry_storage& tree, std::filesystem::path const& path,
                         dir_handle parent, progress& prog, file_scanner& fs,
                         bool debug_filter = false);

  void dump_state(std::string_view env_var, std::string_view what,
                  std::shared_ptr<file_access const> const& fa,
                  std::function<void(std::ostream&)> const& dumper) const;

  LOG_PROXY_DECL(LoggerPolicy);
  worker_group& wg_;
  scanner_options const& options_;
  segmenter_factory& segmenter_factory_;
  os_access const& os_;
  std::vector<std::unique_ptr<entry_filter>> filters_;
  std::unordered_set<std::string> invalid_filenames_;
  bool resume_{false};
  std::optional<phase1_journal_data> journal_;
  std::optional<phase2_state> phase2_;
};

phase1_journal_data collect_phase1_journal(
    entry_storage& tree, std::vector<fragment_category> const& categories,
    bool list_mode) {
  phase1_journal_data data;

  for (size_t i = 0; i < tree.inode_count(); ++i) {
    inode_id const iid{static_cast<uint32_t>(i)};
    auto const ino = tree.handle(iid);

    phase1_inode rec;
    rec.inode_id = static_cast<uint32_t>(i);
    rec.obj_num = ino.num();

    for (auto fid : ino.all_file_ids()) {
      auto const fh = tree.handle(fid);
      phase1_file fr;
      fr.file_id = static_cast<uint32_t>(fid.index());
      fr.inode_id = rec.inode_id;
      if (auto n = fh.inode_num()) {
        fr.entry_inode_num = *n;
      }
      if (list_mode) {
        fr.order_index = fh.order_index();
      }
      if (fh.is_invalid()) {
        fr.flags |= 1;
      }
      rec.files.push_back(fr);
    }

    for (auto const& frag : ino.fragments_view()) {
      auto const cat = frag.category();
      phase1_fragment f;
      f.category = cat.value();
      f.subcategory = cat.has_subcategory()
                          ? static_cast<int32_t>(cat.subcategory())
                          : -1;
      f.kind = 0; // Phase 1 only ever produces data fragments
      f.size = frag.size();
      rec.fragments.push_back(f);
    }

    for (auto const& cat : categories) {
      if (auto s = ino.similarity_hash(cat)) {
        phase1_similarity si;
        si.category = cat.value();
        si.subcategory = cat.has_subcategory()
                             ? static_cast<int32_t>(cat.subcategory())
                             : -1;
        si.type = 0;
        si.sim_value = *s;
        rec.similarities.push_back(si);
      }
      if (auto const* n = ino.nilsimsa_similarity_hash(cat)) {
        phase1_similarity si;
        si.category = cat.value();
        si.subcategory = cat.has_subcategory()
                             ? static_cast<int32_t>(cat.subcategory())
                             : -1;
        si.type = 1;
        si.nilsimsa = *n;
        rec.similarities.push_back(si);
      }
    }

    data.inodes.push_back(std::move(rec));
  }

  return data;
}

void apply_phase1_journal(entry_storage& tree,
                          phase1_journal_data const& data, bool list_mode) {
  for (size_t i = 0; i < data.inodes.size(); ++i) {
    if (data.inodes[i].inode_id != i) {
      DWARFS_THROW(runtime_error, "checkpoint journal inode ids are not dense");
    }
  }

  for (auto const& rec : data.inodes) {
    auto ino = tree.create_inode();
    if (ino.id().index() != rec.inode_id) {
      DWARFS_THROW(runtime_error,
                   "checkpoint journal inode order does not match tree");
    }

    inode_fragments frags;
    for (auto const& f : rec.fragments) {
      fragment_category cat{f.category};
      if (f.subcategory >= 0) {
        cat.set_subcategory(static_cast<uint32_t>(f.subcategory));
      }
      frags.emplace_back(cat, f.size);
    }
    if (!frags.empty()) {
      tree.set_inode_fragments(ino.id(), frags);
    }

    if (!rec.similarities.empty()) {
      std::vector<inode_similarity_hash_data> sims;
      sims.reserve(rec.similarities.size());
      for (auto const& s : rec.similarities) {
        fragment_category cat{s.category};
        if (s.subcategory >= 0) {
          cat.set_subcategory(static_cast<uint32_t>(s.subcategory));
        }
        if (s.type == 0) {
          sims.emplace_back(cat, s.sim_value);
        } else {
          sims.emplace_back(cat, s.nilsimsa);
        }
      }
      tree.set_inode_similarity(ino.id(), sims);
    }

    tree.set_inode_num(ino.id(), rec.obj_num);

    file_id_vector fv;
    for (auto const& f : rec.files) {
      fv.push_back(file_id{entry_id{entry_type::E_FILE, f.file_id}});
    }
    tree.set_files_for_inode(ino.id(), std::move(fv));
  }

  for (auto const& f : data.files) {
    auto fh =
        tree.handle(file_id{entry_id{entry_type::E_FILE, f.file_id}});
    fh.set_inode(inode_id{f.inode_id});
    fh.set_inode_num(f.entry_inode_num);
    if (list_mode) {
      fh.set_order_index(f.order_index);
    }
    if (f.flags & 1) {
      fh.set_invalid();
    }
  }
}

template <typename LoggerPolicy>
void scanner_<LoggerPolicy>::add_filter(
    std::unique_ptr<entry_filter>&& filter) {
  filters_.push_back(std::move(filter));
}

template <typename LoggerPolicy>
scanner_<LoggerPolicy>::scanner_(logger& lgr, worker_group& wg,
                                 segmenter_factory& sf, os_access const& os,
                                 scanner_options const& options)
    : LOG_PROXY_INIT(lgr)
    , wg_{wg}
    , options_{options}
    , segmenter_factory_{sf}
    , os_{os} {}

DWARFS_PUSH_WARNING
DWARFS_GCC14_DISABLE_WARNING("-Wnrvo")

template <typename LoggerPolicy>
entry_handle
scanner_<LoggerPolicy>::add_entry(entry_storage& tree,
                                  std::filesystem::path const& path,
                                  dir_handle parent, progress& prog,
                                  file_scanner& fs, bool debug_filter) {
  try {
    auto ent = internal::provisional_entry(os_, path, parent);

    if constexpr (!std::is_same_v<std::filesystem::path::value_type, char>) {
      try {
        auto tmp [[maybe_unused]] = path.filename().u8string();
      } catch (std::system_error const& e) {
        LOG_ERROR << fmt::format(
            R"(invalid file path in "{}", storing as "{}": {})",
            path_to_utf8_string_sanitized(path.parent_path()), ent.name(),
            error_cp_to_utf8(e.what()));

        prog.errors++;

        if (!invalid_filenames_.emplace(path_to_utf8_string_sanitized(path))
                 .second) {
          LOG_ERROR << fmt::format(
              "cannot store \"{}\" as the name already exists", ent.name());
          return {};
        }
      }
    }

    bool const exclude = std::ranges::any_of(filters_, [&ent](auto const& f) {
      return f->filter(ent) == filter_action::remove;
    });

    if (debug_filter) {
      (*options_.debug_filter_function)(exclude, ent);
    }

    if (exclude) {
      if (!debug_filter) {
        LOG_DEBUG << "excluding " << ent.unix_dpath();
      }

      return {};
    }

    switch (ent.type()) {
    case entry_type::E_DEVICE:
      if (!options_.with_devices) {
        return {};
      }
      break;

    case entry_type::E_OTHER:
      if (!options_.with_specials) {
        return {};
      }
      break;

    default:
      break;
    }

    auto const size = ent.size();
    auto pe = ent.commit(tree);

    switch (pe.type()) {
    case entry_type::E_DIR:
      // prog.current.store(pe.get());
      prog.dirs_found++;
      if (!debug_filter) {
        pe.scan(os_, prog);
      }
      break;

    case entry_type::E_FILE:
      assert(path == pe.fs_path());
      assert(size == pe.size());

      if (!debug_filter && size > 0 && os_.access(path, R_OK)) {
        LOG_ERROR << "cannot access " << path_to_utf8_string_sanitized(path)
                  << ", creating empty file";
        pe.set_empty();
        prog.errors++;
      }

      prog.files_found++;

      if (!debug_filter) {
        if (resume_) {
          // Restoring from a Phase 1 checkpoint journal: allocate the packed
          // file data without reading or hashing the file contents.
          pe.as_file().create_data();
        } else {
          fs.scan(pe.as_file());
        }
      }
      break;

    case entry_type::E_LINK:
      prog.symlinks_found++;
      if (!debug_filter) {
        pe.scan(os_, prog);
      }
      prog.symlinks_scanned++;
      break;

    case entry_type::E_DEVICE:
    case entry_type::E_OTHER:
      prog.specials_found++;
      if (!debug_filter) {
        pe.scan(os_, prog);
      }
      break;

    default:
      LOG_ERROR << "unsupported entry type: " << static_cast<int>(pe.type())
                << " (" << path_to_utf8_string_sanitized(path) << ")";
      prog.errors++;
      break;
    }

    return pe;
  } catch (std::system_error const& e) {
    LOG_ERROR << fmt::format("error reading entry (path={}): {}",
                             path_to_utf8_string_sanitized(path),
                             exception_str(e));
    prog.errors++;
  }

  return {};
}

DWARFS_POP_WARNING

template <typename LoggerPolicy>
void scanner_<LoggerPolicy>::dump_state(
    std::string_view env_var, std::string_view what,
    std::shared_ptr<file_access const> const& fa,
    std::function<void(std::ostream&)> const& dumper) const {
  if (auto dumpfile = os_.getenv(env_var)) {
    if (fa) {
      LOG_VERBOSE << "dumping " << what << " to " << *dumpfile;
      std::error_code ec;
      auto ofs = fa->open_output(*dumpfile, ec);
      if (ec) {
        LOG_ERROR << "cannot open '" << *dumpfile << "': " << ec.message();
      } else {
        dumper(ofs->os());
        ofs->close(ec);
        if (ec) {
          LOG_ERROR << "cannot close '" << *dumpfile << "': " << ec.message();
        }
      }
    } else {
      LOG_ERROR << "cannot dump " << what << ": no file access";
    }
  }
}

template <typename LoggerPolicy>
entry_handle
scanner_<LoggerPolicy>::scan_tree(entry_storage& tree,
                                  std::filesystem::path const& path,
                                  progress& prog, file_scanner& fs) {
  auto root = internal::provisional_entry(os_, path).commit(tree);
  bool const debug_filter = options_.debug_filter_function.has_value();

  std::vector<entry_id> stack({root.id()});
  std::filesystem::path name;
  std::vector<entry_id> subdirs;
  prog.dirs_found++;

  while (!stack.empty()) {
    auto parent = tree.handle(stack.back()).as_dir();

    DWARFS_CHECK(parent, "expected directory");

    stack.pop_back();
    auto ppath = parent.fs_path();

    try {
      auto d = os_.opendir(ppath);

      while (d->read(name)) {
        if (auto pe = add_entry(tree, name, parent, prog, fs, debug_filter)) {
          if (pe.is_dir()) {
            subdirs.push_back(pe.id());
          }
        }
      }

      stack.insert(stack.end(), subdirs.rbegin(), subdirs.rend());

      subdirs.clear();

      prog.dirs_scanned++;
    } catch (std::system_error const& e) {
      LOG_ERROR << "cannot read directory '"
                << path_to_utf8_string_sanitized(ppath)
                << "': " << exception_str(e);
      prog.errors++;
    }
  }

  return root;
}

template <typename LoggerPolicy>
entry_handle
scanner_<LoggerPolicy>::scan_list(entry_storage& tree,
                                  std::filesystem::path const& rootpath,
                                  std::span<std::filesystem::path const> list,
                                  progress& prog, file_scanner& fs) {
  if (!filters_.empty()) {
    DWARFS_THROW(runtime_error, "cannot use filters with file lists");
  }

  auto ti = LOG_TIMED_INFO;

  LOG_DEBUG << "creating root directory '"
            << path_to_utf8_string_sanitized(rootpath) << "'";

  auto root = internal::provisional_entry(os_, rootpath).commit(tree);

  auto ensure_path = [this, &tree, &prog, &fs](
                         std::filesystem::path const& path, entry_handle root) {
    LOG_TRACE << "ensuring path '" << path_to_utf8_string_sanitized(path)
              << "'";

    for (auto const& component : path) {
      LOG_TRACE << "checking '" << path_to_utf8_string_sanitized(component)
                << "'";
      if (auto d = root.as_dir()) {
        if (auto e = d.find(component.string())) {
          root = e;
        } else {
          LOG_DEBUG << "adding directory '"
                    << path_to_utf8_string_sanitized(component) << "'";
          root = add_entry(tree, d.fs_path() / component, d, prog, fs);
          if (root && root.is_dir()) {
            prog.dirs_scanned++;
          } else {
            DWARFS_THROW(runtime_error,
                         fmt::format("invalid path '{}'",
                                     path_to_utf8_string_sanitized(path)));
          }
        }
      } else {
        DWARFS_THROW(runtime_error,
                     fmt::format("invalid path '{}'",
                                 path_to_utf8_string_sanitized(path)));
      }
    }

    return root;
  };

  std::unordered_map<std::string, dir_handle> dir_cache;
  uint32_t file_order_index{0};

  for (auto const& listpath : list) {
    std::filesystem::path relpath;

    if (listpath.has_root_directory()) {
      if (!rootpath.has_root_directory()) {
        std::string rootpath_str;
        if (rootpath.empty()) {
          rootpath_str = "empty";
        } else {
          rootpath_str = "'" + path_to_utf8_string_sanitized(rootpath) + "'";
        }
        DWARFS_THROW(runtime_error,
                     fmt::format("absolute paths in input list require "
                                 "absolute input path, but input path is {}",
                                 rootpath_str));
      }

      auto mm = std::ranges::mismatch(listpath, rootpath);

      if (mm.in2 == rootpath.end()) {
        relpath = listpath.lexically_relative(rootpath);
      } else {
        LOG_ERROR << "ignoring path '"
                  << path_to_utf8_string_sanitized(listpath)
                  << "' not below input path '"
                  << path_to_utf8_string_sanitized(rootpath) << "'";
        continue;
      }
    } else {
      relpath = listpath.lexically_normal();
    }

    // Remove trailing slash(es) if present, fixes gh #370.
    if (!relpath.has_filename()) {
      relpath = relpath.parent_path();
    }

    auto parent = relpath.parent_path();
    dir_handle pd{};

    LOG_TRACE << "adding path '" << path_to_utf8_string_sanitized(relpath)
              << "' (parent: " << parent
              << ", root: " << path_to_utf8_string_sanitized(rootpath)
              << ", orig: " << path_to_utf8_string_sanitized(listpath) << ")";

    if (auto it = dir_cache.find(parent.string()); it != dir_cache.end()) {
      pd = it->second;
    } else {
      pd = ensure_path(parent, root).as_dir();

      if (pd) {
        dir_cache.emplace(parent.string(), pd);
      } else {
        DWARFS_THROW(runtime_error,
                     fmt::format("invalid path '{}'",
                                 path_to_utf8_string_sanitized(listpath)));
      }
    }

    if (pd.find(relpath)) {
      LOG_INFO << "skipping duplicate entry '"
               << path_to_utf8_string_sanitized(relpath) << "' in input list";
      continue;
    }

    LOG_TRACE << "adding entry '"
              << path_to_utf8_string_sanitized(rootpath / relpath) << "'";

    if (auto pe = add_entry(tree, rootpath / relpath, pd, prog, fs)) {
      if (pe.is_dir()) {
        prog.dirs_scanned++;
      } else if (pe.is_file()) {
        auto fp = pe.as_file();
        fp.set_order_index(file_order_index++);
      }
    }
  }

  ti << "scanned input list";

  return root;
}

template <typename LoggerPolicy>
void scanner_<LoggerPolicy>::scan(
    filesystem_writer& fs_writer, std::filesystem::path const& path,
    writer_progress& wprog,
    std::optional<std::span<std::filesystem::path const>> list,
    std::shared_ptr<file_access const> fa,
    std::function<void(library_dependencies&)> const& extra_deps) {
  auto& prog = wprog.get_internal();
  auto& fsw = fs_writer.get_internal();

  if (!options_.debug_filter_function) {
    LOG_INFO << "scanning " << path;
  }

  prog.set_status_function(status_string);

  if (!options_.checkpoint.dir.empty() && options_.checkpoint.resume) {
    auto const journal_path = options_.checkpoint.dir / "phase1.journal";
    checkpoint_job_signature const expected{options_.checkpoint.inputs_hash,
                                            options_.checkpoint.config_hash};
    if (std::filesystem::exists(journal_path)) {
      try {
        auto jd = read_phase1_journal(journal_path);
        if (jd.signature == expected) {
          resume_ = true;
          journal_ = std::move(jd);
          LOG_INFO << "resuming from phase 1 checkpoint journal: "
                   << journal_path;
        } else {
          LOG_WARN << "phase 1 checkpoint journal does not match this job; "
                      "scanning from scratch";
        }
      } catch (std::exception const& e) {
        LOG_WARN << "cannot read phase 1 checkpoint journal '" << journal_path
                 << "': " << e.what() << "; scanning from scratch";
      }
    }
  }

  // Phase 2 state (image size, block map, writer bookkeeping) is read
  // independently of Phase 1: even if the Phase 1 journal is missing and we
  // have to re-scan, the already-written image prefix can still be reused.
  if (!options_.checkpoint.dir.empty() && options_.checkpoint.resume) {
    auto const p2_path = options_.checkpoint.dir / "phase2.state";
    checkpoint_job_signature const expected{options_.checkpoint.inputs_hash,
                                            options_.checkpoint.config_hash};
    if (std::filesystem::exists(p2_path)) {
      try {
        auto st = read_phase2_state(p2_path);
        if (st.signature == expected) {
          phase2_ = std::move(st);
          LOG_INFO << "resuming from phase 2 checkpoint state: " << p2_path
                   << " (image size "
                   << size_with_unit(static_cast<uint64_t>(phase2_->image_size))
                   << ", " << phase2_->num_blocks << " blocks)";
        } else {
          LOG_WARN << "phase 2 checkpoint state does not match this job; "
                      "ignoring it";
        }
      } catch (std::exception const& e) {
        LOG_WARN << "cannot read phase 2 checkpoint state '" << p2_path << "': "
                 << e.what() << "; ignoring it";
      }
    }
  }

  auto tree = std::make_optional<entry_storage>(options_.metadata);

  inode_manager im(LOG_GET_LOGGER, *tree, prog, path, options_.inode,
                   list.has_value());
  auto fs = std::make_optional<file_scanner>(
      LOG_GET_LOGGER, *tree, wg_, os_, im, prog,
      file_scanner::options{
          .hash_algo = options_.file_hash_algorithm,
          .debug_inode_create = os_.getenv(kEnvVarDumpFilesRaw) ||
                                os_.getenv(kEnvVarDumpFilesFinal),
      });

  auto root = list ? scan_list(*tree, path, *list, prog, *fs)
                   : scan_tree(*tree, path, prog, *fs);

  if (options_.debug_filter_function) {
    LOG_VERBOSE << "entry storage:\n" << tree->dump();
    return;
  }

  if (options_.remove_empty_dirs) {
    LOG_INFO << "removing empty directories...";
    tree->remove_empty_dirs(prog);
  }

  LOG_INFO << "waiting for background scanners...";

  wg_.wait();

  // stop progress briefly to avoid race while freezing entry storage
  prog.current.store(std::monostate{});
  prog.set_status_function_and_drain(
      [](progress const&, size_t) { return "freezing entries"; });

  LOG_VERBOSE << "entry storage (before freezing):\n" << tree->dump_entries();

  tree->freeze_entries(LOG_GET_LOGGER, prog);

  auto set_status = [&prog](std::string_view status) {
    prog.set_status_function(
        [msg = std::string(status)](progress const&, size_t) { return msg; });
  };

  LOG_INFO << "scanning CPU time: "
           << time_with_unit(wg_.try_get_cpu_time().value_or(0ns));

  LOG_INFO << "assigning directory and link inodes...";
  set_status("assigning directory and link inodes");

  uint32_t first_link_inode = 0;
  dir_set_inode_visitor dsiv(first_link_inode);
  root.accept(dsiv, true);

  uint32_t first_file_inode = first_link_inode;
  link_set_inode_visitor lsiv(first_file_inode);
  root.accept(lsiv, true);

  uint32_t first_device_inode = first_file_inode;
  uint32_t num_unique_files = 0;

  if (resume_) {
    DWARFS_CHECK(journal_.has_value(), "resume requested without journal");

    LOG_INFO << "restoring file inodes from checkpoint...";
    set_status("restoring file inodes");
    apply_phase1_journal(*tree, *journal_, list.has_value());
    restore_progress(prog, journal_->progress);

    num_unique_files = static_cast<uint32_t>(journal_->unique_inode_count);
    first_device_inode =
        first_file_inode + static_cast<uint32_t>(journal_->file_inode_count);
  } else {
    dump_state(kEnvVarDumpFilesRaw, "raw files", fa,
               [&fs](auto& os) { fs->dump(os); });

    LOG_INFO << "finalizing file inodes...";
    set_status("finalizing file inodes");
    fs->finalize(first_device_inode);

    dump_state(kEnvVarDumpFilesFinal, "final files", fa,
               [&fs](auto& os) { fs->dump(os); });

    num_unique_files = fs->num_unique();
  }

  fs.reset();

  // this must be done after finalizing the inodes since this is when
  // the file vectors are populated
  if (!resume_ && im.has_invalid_inodes()) {
    LOG_INFO << "trying to recover any invalid inodes...";
    im.try_scan_invalid(wg_, os_);
    wg_.wait();
  }

  tree->drop_file_hashes();

  LOG_VERBOSE << "entry storage (after dropping file hashes):\n"
              << tree->dump_entries();

  LOG_VERBOSE << "inode storage (before freezing):\n" << tree->dump_inodes();

  set_status("freezing inodes");
  tree->freeze_inodes();

  auto original_size = [&] {
    return options_.metadata.enable_sparse_files
               ? prog.allocated_original_size.load()
               : prog.original_size.load();
  };

  auto saved_by_deduplication = [&] {
    return options_.metadata.enable_sparse_files
               ? prog.allocated_saved_by_deduplication.load()
               : prog.saved_by_deduplication.load();
  };

  LOG_INFO << "saved " << size_with_unit(saved_by_deduplication()) << " / "
           << size_with_unit(original_size()) << " ("
           << ratio_to_string(saved_by_deduplication(), original_size())
           << ") in " << prog.duplicate_files << "/" << prog.files_found
           << " duplicate files";

  auto frag_info = im.fragment_category_info();

  if (auto catmgr = options_.inode.categorizer_mgr) {
    for (auto const& ci : frag_info.info) {
      LOG_VERBOSE << "found " << ci.fragment_count << " "
                  << catmgr->category_name(ci.category) << " fragments ("
                  << size_with_unit(ci.total_size) << ")";
    }
  }

  if (!resume_ && !options_.checkpoint.dir.empty()) {
    LOG_INFO << "writing phase 1 checkpoint journal...";
    set_status("writing phase 1 checkpoint journal");

    checkpoint_job_signature const sig{options_.checkpoint.inputs_hash,
                                       options_.checkpoint.config_hash};
    auto data = collect_phase1_journal(*tree, frag_info.categories,
                                       list.has_value());
    data.signature = sig;

    phase1_journal_writer writer(options_.checkpoint.dir / "phase1.journal",
                                 sig);
    for (auto const& ino : data.inodes) {
      writer.add_inode(ino);
    }
    writer.finish(num_unique_files,
                  static_cast<uint64_t>(first_device_inode - first_file_inode),
                  capture_progress(prog));
  }

  auto ge_data = std::make_optional<global_entry_data>(options_.metadata);
  metadata_builder mdb(LOG_GET_LOGGER, options_.metadata);

  LOG_INFO << "assigning device inodes...";
  set_status("assigning device inodes");
  uint32_t first_pipe_inode = first_device_inode;
  device_set_inode_visitor devsiv(first_pipe_inode);
  root.accept(devsiv);
  mdb.set_devices(std::move(devsiv.device_ids()));

  LOG_INFO << "assigning pipe/socket inodes...";
  set_status("assigning pipe/socket inodes");
  uint32_t last_inode = first_pipe_inode;
  pipe_set_inode_visitor pipsiv(last_inode);
  root.accept(pipsiv);

  mdb.set_symlink_table_size(first_file_inode - first_link_inode);

  wg_.add_job([&] {
    LOG_INFO << "saving names and symlinks...";
    names_and_symlinks_visitor nlv(*ge_data);
    root.accept(nlv);

    {
      auto tv = LOG_TIMED_VERBOSE;
      ge_data->update_index();
      tv << "global name and link indices updated";
    }

    LOG_INFO << "updating name and link indices...";
    root.walk([&](entry_handle ep) {
      ep.update(*ge_data);
      if (auto lp = ep.as_link()) {
        mdb.add_symlink_table_entry(
            ep.inode_num().value() - first_link_inode,
            ge_data->get_symlink_table_entry(lp.linkname()));
      }
    });
  });

  dump_state(kEnvVarDumpInodes, "inodes", fa, [&im](auto& os) { im.dump(os); });

  LOG_INFO << "building blocks...";

  prog.set_status_function(status_string);

  // TODO:
  // - get rid of multiple worker groups
  // - instead, introduce "batches" to which work can be added, and
  //   which gets run on a worker groups; each batch keeps track of
  //   its CPU time and affects thread naming

  auto blockmgr = std::make_shared<block_manager>();

  if (phase2_) {
    std::vector<block_manager::persisted_entry> entries;
    entries.reserve(phase2_->block_map.size());
    for (auto const& e : phase2_->block_map) {
      fragment_category cat(e.category);
      if (e.subcategory >= 0) {
        cat.set_subcategory(static_cast<uint32_t>(e.subcategory));
      }
      entries.push_back({static_cast<size_t>(e.logical),
                         static_cast<size_t>(e.physical), cat});
    }
    blockmgr->restore(static_cast<size_t>(phase2_->num_blocks), entries);

    filesystem_writer_checkpoint_state wst;
    wst.section_number = phase2_->section_number;
    wst.image_size = phase2_->image_size;
    wst.header_size = phase2_->header_size;
    wst.section_index = phase2_->section_index;
    fsw.restore_state(wst);

    restore_progress(prog, phase2_->progress);

    LOG_INFO << "restored phase 2 state: " << blockmgr->num_blocks()
             << " logical blocks, section number " << phase2_->section_number
             << ", image size "
             << size_with_unit(static_cast<uint64_t>(phase2_->image_size));
  }

  {
    size_t const num_threads = options_.num_segmenter_workers;
    worker_group wg_ordering(LOG_GET_LOGGER, os_, "ordering",
                             {.num_workers = num_threads});
    worker_group wg_blockify(LOG_GET_LOGGER, os_, "blockify",
                             {.num_workers = num_threads});

    // With global de-duplication enabled, category streams must be
    // processed sequentially (in the deterministic category order) in
    // order to guarantee reproducible images.
    bool const serialize_categories = segmenter_factory_.serializes_categories();

    std::shared_ptr<thread_pool> hash_worker_pool;
    if (serialize_categories) {
      hash_worker_pool = std::make_shared<thread_pool>(
          log_.get_logger(), os_, "global_dedup_hash",
          options_.num_segmenter_workers);
    }

    fsw.configure(frag_info.categories, num_threads);

    if (serialize_categories) {
      LOG_VERBOSE
          << "processing categories sequentially for global deduplication";
    }

    for (auto category : frag_info.categories) {
      auto cat_size = frag_info.category_size.at(category);
      auto catmgr = options_.inode.categorizer_mgr.get();
      std::string meta;

      if (catmgr) {
        meta = catmgr->category_metadata(category);
        if (!meta.empty()) {
          LOG_VERBOSE << category_prefix(catmgr, category)
                      << "metadata: " << meta;
        }
      }

      auto cc = fsw.get_compression_constraints(category.value(), meta);

      LOG_DEBUG << category_prefix(catmgr, category)
                << "segmenter will use up to "
                << size_with_unit(segmenter_factory_.estimate_memory_usage(
                       category, cat_size, cc));

      segmenter_checkpoint_config checkpoint_cfg;
      bool const phase2_checkpoint =
          serialize_categories && !options_.checkpoint.dir.empty() &&
          options_.checkpoint.phase2;

      if (phase2_checkpoint) {
        checkpoint_cfg.interval_bytes = options_.checkpoint.phase2_interval_bytes;
        checkpoint_cfg.drain_cb = [&fsw] { fsw.wait_idle(); };
        checkpoint_cfg.commit_cb = [this, &fsw, &prog,
                                    blockmgr](uint64_t index_off) {
          auto [num_blocks, entries] = blockmgr->snapshot();
          auto wst = fsw.snapshot_state();

          phase2_state st;
          st.signature = {options_.checkpoint.inputs_hash,
                          options_.checkpoint.config_hash};
          st.index_durable_offset = index_off;
          st.num_blocks = num_blocks;
          st.section_number = wst.section_number;
          st.image_size = wst.image_size;
          st.header_size = wst.header_size;
          st.progress = capture_progress(prog);
          st.section_index = std::move(wst.section_index);
          st.block_map.reserve(entries.size());
          for (auto const& e : entries) {
            phase2_block_entry pe;
            pe.logical = e.logical_block;
            pe.physical = e.written_block;
            pe.category = e.category.value();
            pe.subcategory = e.category.has_subcategory()
                                 ? static_cast<int32_t>(e.category.subcategory())
                                 : -1;
            st.block_map.push_back(pe);
          }

          write_phase2_state(options_.checkpoint.dir / "phase2.state", st);

          LOG_INFO << "phase 2 checkpoint: "
                   << size_with_unit(static_cast<uint64_t>(st.image_size))
                   << " written, " << st.num_blocks << " blocks, index offset "
                   << st.index_durable_offset;
        };
      }

      wg_blockify.add_job([this, catmgr, blockmgr, category, cat_size, meta, cc,
                           &prog, &fsw, &im, &wg_ordering,
                           checkpoint_cfg = std::move(checkpoint_cfg),
                           &hash_worker_pool]() mutable {
        auto span = im.ordered_span(category, wg_ordering);
        auto tv = LOG_CPU_TIMED_VERBOSE;

        auto seg = segmenter_factory_.create(
            category, cat_size, cc, blockmgr,
            [category, meta, blockmgr, &fsw](auto block,
                                             auto logical_block_num) {
              fsw.write_block(
                  category, std::move(block),
                  [blockmgr, logical_block_num,
                   category](auto physical_block_num) {
                    blockmgr->set_written_block(logical_block_num,
                                                physical_block_num, category);
                  },
                  meta);
            },
            checkpoint_cfg,
            hash_worker_pool);

        std::vector<std::unique_ptr<internal::chunkable>> dedup_batch;
        size_t const dedup_batch_limit =
            std::max<size_t>(1, options_.num_segmenter_workers);
        auto flush_dedup_batch = [&dedup_batch, &seg] {
          if (!dedup_batch.empty()) {
            seg.add_chunkables(dedup_batch);
          }
          dedup_batch.clear();
        };

        for (auto ino : span) {
          prog.current.store(ino);

          // TODO: factor this code out
          auto f = ino.any();

          if (auto size = f.size(); size > 0 && !f.is_invalid()) {
            auto for_each_fragment = [&](auto&& func) {
              file_off_t offset{0};
              for (auto&& frag : ino.fragments_view()) {
                if (frag.category() == category) {
                  func(frag, offset);
                  prog.fragments_written++;
                }
                offset += frag.size();
              }
            };

            if (options_.hollow_filesystem) {
              for_each_fragment(
                  [&](auto&& frag, auto) { frag.add_hole(frag.size()); });
            } else {
              auto [mm, _, errors] = ino.mmap_any(os_);

              if (mm) {
                auto& mm_ref = mm; // workaround for AppleClang 15
                for_each_fragment([&](auto&& frag, auto offset) {
                  auto fc = std::make_unique<internal::fragment_chunkable>(
                      ino, frag, offset, mm_ref, catmgr);
                  dedup_batch.push_back(std::move(fc));
                  if (dedup_batch.size() >= dedup_batch_limit) {
                    flush_dedup_batch();
                  }
                });
              } else {
                for (auto& [fp, e] : errors) {
                  LOG_ERROR << "failed to map file " << fp.path_as_string()
                            << ": " << exception_str(e)
                            << ", creating empty inode";
                  ++prog.errors;
                }
                for (auto const& frag : ino.fragments_view()) {
                  if (frag.category() == category) {
                    prog.fragments_found--;
                  }
                }
              }
            }
          }

          prog.inodes_written++; // TODO: remove?
        }

        flush_dedup_batch();

        seg.finish();
        fsw.finish_category(category);

        tv << category_prefix(catmgr, category) << "segmenting finished";
      });

      if (serialize_categories) {
        wg_blockify.wait();
      }
    }

    LOG_INFO << "waiting for segmenting/blockifying to finish...";

    // We must wait for blockify first, since the blockify jobs are what
    // trigger the ordering jobs.
    wg_blockify.wait();
    wg_ordering.wait();

    LOG_INFO << "total ordering CPU time: "
             << time_with_unit(wg_ordering.try_get_cpu_time().value_or(0ns));

    LOG_INFO << "total segmenting CPU time: "
             << time_with_unit(wg_blockify.try_get_cpu_time().value_or(0ns));
  }

  // seg.finish();
  wg_.wait();

  prog.current.store(std::monostate{});
  prog.set_status_function_and_drain(
      [](progress const&, size_t) { return std::string(); });

  mdb.set_block_size(segmenter_factory_.get_block_size());

  LOG_INFO << "saving chunks...";
  set_status("saving chunks");
  mdb.gather_chunks(im, *blockmgr, prog.chunk_count);

  LOG_INFO << "saving directories...";
  set_status("saving directories");
  {
    save_directories_visitor sdv(first_link_inode);
    root.accept(sdv);
    mdb.gather_entries(*tree, sdv.get_directory_ids(), *ge_data, last_inode);
  }

  LOG_INFO << "saving shared files table...";
  set_status("saving shared files table");
  {
    save_shared_files_visitor ssfv(first_file_inode, first_device_inode,
                                   num_unique_files);
    root.accept(ssfv);
    mdb.set_shared_files_table(std::move(ssfv.get_shared_files()));
  }

  if (auto catmgr = options_.inode.categorizer_mgr) {
    std::unordered_map<fragment_category::value_type, uint32_t>
        category_indices;
    std::unordered_map<fragment_category, uint32_t> category_metadata_indices;
    std::vector<std::string> category_names;
    std::vector<std::string> category_metadata;

    category_indices.reserve(frag_info.info.size());
    category_names.reserve(frag_info.info.size());

    for (auto const& ci : frag_info.info) {
      if (category_indices.emplace(ci.category, category_names.size()).second) {
        category_names.emplace_back(catmgr->category_name(ci.category));
      }
    }

    for (auto const& cat : frag_info.categories) {
      auto metadata = catmgr->category_metadata(cat);
      if (!metadata.empty()) {
        if (category_metadata_indices.emplace(cat, category_metadata.size())
                .second) {
          category_metadata.emplace_back(std::move(metadata));
        }
      }
    }

    auto written_categories = blockmgr->get_written_block_categories();
    std::vector<uint32_t> block_categories(written_categories.size());
    std::map<uint32_t, uint32_t> block_cat_metadata;

    std::ranges::transform(
        written_categories, block_categories.begin(),
        [&](auto const& cat) { return category_indices.at(cat.value()); });

    for (auto const& [i, cat] : ranges::views::enumerate(written_categories)) {
      if (auto it = category_metadata_indices.find(cat);
          it != category_metadata_indices.end()) {
        block_cat_metadata.emplace(i, it->second);
      }
    }

    mdb.set_category_names(std::move(category_names));
    mdb.set_block_categories(std::move(block_categories));
    mdb.set_category_metadata_json(std::move(category_metadata));
    mdb.set_block_category_metadata(std::move(block_cat_metadata));
  }

  LOG_VERBOSE << ge_data->to_string();

  set_status("gathering global entry data");
  {
    auto tv = LOG_TIMED_VERBOSE;
    mdb.gather_global_entry_data(*tree, *ge_data);
    tv << "global entry data gathered for metadata";
  }

  // free all entry storage
  LOG_VERBOSE << "entry/inode storage:\n" << tree->dump();
  tree.reset();
  ge_data.reset();

  set_status("building metadata");
  auto metadata = std::make_optional(mdb.build());

  set_status("freezing metadata");
  auto [schema, data] = metadata_freezer(LOG_GET_LOGGER).freeze(*metadata);

  // free memory again
  metadata.reset();

  LOG_VERBOSE << "uncompressed metadata size: " << size_with_unit(data.size());

  fsw.write_metadata_v2_schema(schema);
  fsw.write_metadata_v2(data);

  if (options_.enable_history) {
    history hist(options_.history);
    hist.append(options_.command_line_arguments, extra_deps);
    fsw.write_history(hist.serialize());
  }

  LOG_INFO << "waiting for compression to finish...";
  set_status("waiting for block compression to finish");

  fsw.flush();

  std::string orig_size = size_with_unit(prog.original_size);
  std::string comp_pct =
      ratio_to_string(prog.compressed_size, prog.original_size);

  if (options_.metadata.enable_sparse_files &&
      prog.original_size != prog.allocated_original_size) {
    orig_size += fmt::format(" ({} allocated)",
                             size_with_unit(prog.allocated_original_size));
    if (prog.allocated_original_size > 0) {
      comp_pct += " / " + ratio_to_string(prog.compressed_size,
                                          prog.allocated_original_size);
    }
  }

  LOG_INFO << "compressed " << orig_size << " to "
           << size_with_unit(prog.compressed_size) << " (" << comp_pct << ")";
}

} // namespace internal

scanner::scanner(logger& lgr, thread_pool& pool, segmenter_factory& sf,
                 os_access const& os, scanner_options const& options)
    : impl_(
          make_unique_logging_object<impl, internal::scanner_, logger_policies>(
              lgr, pool.get_worker_group(), sf, os, options)) {}

} // namespace dwarfs::writer
