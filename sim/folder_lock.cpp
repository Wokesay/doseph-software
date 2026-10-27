// SPDX-License-Identifier: AGPL-3.0-or-later
#include "folder_lock.hpp"

#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sim {

namespace {
const char* const kNotPlain = "simulator.lock is a link or not a plain file, left as it is";
}

FolderLock::FolderLock(const std::filesystem::path& dir) {
  namespace fs = std::filesystem;
  std::error_code ec;
  // Created as the storage creates it: for the owner only
  if (fs::create_directories(dir, ec) && !ec) {
#ifndef _WIN32
    std::error_code ignored;
    fs::permissions(dir, fs::perms::owner_all, ignored);
#endif
  }
  if (ec) {
    why_ = ec.message();
    return;
  }
  const fs::path file = dir / "simulator.lock";
#ifdef _WIN32
  // No sharing for writes: a second open for writing fails while this one holds it. The link
  // itself is opened, not what it points to, and refused below.
  HANDLE h = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    const DWORD err = GetLastError();
    if (err == ERROR_SHARING_VIOLATION) state_ = State::Taken;
    else why_ = std::system_category().message(static_cast<int>(err));
    return;
  }
  handle_ = h;
  BY_HANDLE_FILE_INFORMATION info;
  if (!GetFileInformationByHandle(h, &info) || (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
    why_ = kNotPlain;
    close();
    return;
  }
  state_ = State::Held;
#else
  // O_CLOEXEC: a browser started from here does not inherit the lock. O_NOFOLLOW: a link is not
  // followed. O_NONBLOCK: a planted pipe does not hold the start.
  fd_ = ::open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
  if (fd_ < 0) {
    why_ = errno == ELOOP ? kNotPlain : std::generic_category().message(errno);
    return;
  }
  struct stat st {};
  if (::fstat(fd_, &st) != 0 || !S_ISREG(st.st_mode)) {
    why_ = kNotPlain;
    close();
    return;
  }
  if (::flock(fd_, LOCK_EX | LOCK_NB) == 0) {
    state_ = State::Held;
    return;
  }
  if (errno == EWOULDBLOCK) state_ = State::Taken;
  else why_ = std::generic_category().message(errno);
  close();
#endif
}

FolderLock::~FolderLock() { close(); }

void FolderLock::close() {
#ifdef _WIN32
  if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
  handle_ = nullptr;
#else
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
#endif
}

}  // namespace sim
