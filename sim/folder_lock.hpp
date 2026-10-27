// SPDX-License-Identifier: AGPL-3.0-or-later
// One simulator per data folder: two would load the same files and overwrite
// each other's. The first holds a lock on simulator.lock in the folder while
// it runs; the system frees it when the process ends, also after a crash. The
// file stays: removing it at the end would let a start that has it open hold
// a lock on a file no one else finds. A link under that name, or anything but
// a plain file, is not followed and gives no lock. Without a lock (such a
// file, a folder it may not write, a file system without locks) the
// simulator still starts and says so.
#pragma once
#include <filesystem>
#include <string>

namespace sim {

class FolderLock {
 public:
  enum class State { Held, Taken, None };

  explicit FolderLock(const std::filesystem::path& dir);
  ~FolderLock();
  FolderLock(const FolderLock&) = delete;
  FolderLock& operator=(const FolderLock&) = delete;

  State state() const { return state_; }
  // Why there is no lock, if it is known
  const std::string& why() const { return why_; }

 private:
  void close();

  State state_ = State::None;
  std::string why_;
#ifdef _WIN32
  void* handle_ = nullptr;
#else
  int fd_ = -1;
#endif
};

}  // namespace sim
