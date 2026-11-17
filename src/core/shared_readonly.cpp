#include "core/shared_readonly.hpp"
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace reaweb {
#ifdef REAWEB_SHARED_READ_TEST_HOOK
void shared_read_test_hook();
#endif
namespace {
std::string failure(const char* code, unsigned error = 0) {
  return std::string("{\"protocol\":1,\"ok\":false,\"code\":\"") + code + "\",\"osError\":" + std::to_string(error) + "}";
}
struct Snapshot {
  uint64_t size = 0;
  std::string mtime, identity;
  bool directory = false;
  bool operator==(const Snapshot& other) const { return size == other.size && mtime == other.mtime && identity == other.identity && directory == other.directory; }
};
#ifdef _WIN32
struct Reader {
  HANDLE value = INVALID_HANDLE_VALUE;
  ~Reader() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
bool snapshot(HANDLE file, Snapshot& result) {
  BY_HANDLE_FILE_INFORMATION information{};
  if (GetFileType(file) != FILE_TYPE_DISK || !GetFileInformationByHandle(file, &information)) return false;
  result.directory = (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  result.size = (uint64_t(information.nFileSizeHigh) << 32) | information.nFileSizeLow;
  result.mtime = std::to_string((uint64_t(information.ftLastWriteTime.dwHighDateTime) << 32) | information.ftLastWriteTime.dwLowDateTime);
  FILE_BASIC_INFO basic{};
  if (GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic))) result.mtime += ":" + std::to_string(basic.ChangeTime.QuadPart);
  result.identity = std::to_string(information.dwVolumeSerialNumber) + ":" + std::to_string(information.nFileIndexHigh) + ":" + std::to_string(information.nFileIndexLow);
  return true;
}
#else
struct Reader {
  int value = -1;
  ~Reader() { if (value >= 0) close(value); }
};
bool snapshot(int file, Snapshot& result) {
  struct stat info{};
  if (fstat(file, &info) != 0 || (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)) || info.st_size < 0) return false;
  result.directory = S_ISDIR(info.st_mode);
  result.size = static_cast<uint64_t>(info.st_size);
#ifdef __APPLE__
  result.mtime = std::to_string(info.st_mtimespec.tv_sec) + ":" + std::to_string(info.st_mtimespec.tv_nsec) + ":" +
    std::to_string(info.st_ctimespec.tv_sec) + ":" + std::to_string(info.st_ctimespec.tv_nsec);
#else
  result.mtime = std::to_string(info.st_mtim.tv_sec) + ":" + std::to_string(info.st_mtim.tv_nsec) + ":" +
    std::to_string(info.st_ctim.tv_sec) + ":" + std::to_string(info.st_ctim.tv_nsec);
#endif
  result.identity = std::to_string(info.st_dev) + ":" + std::to_string(info.st_ino);
  return true;
}
#endif
}

std::string shared_read_range_json(const char* input, const char* offset_text, int count) noexcept {
  try {
    if (!input || !offset_text || !*input || count < 0 || count > 65536) return failure("INVALID_ARGUMENT");
    const size_t length = std::strlen(input), offset_length = std::strlen(offset_text);
    if (length > 32768 || offset_length == 0 || offset_length > 19) return failure("INVALID_ARGUMENT");
    for (size_t index = 0; index < length; ++index) if (static_cast<unsigned char>(input[index]) < 32) return failure("INVALID_ARGUMENT");
    uint64_t offset = 0;
    const auto converted = std::from_chars(offset_text, offset_text + offset_length, offset);
    if (converted.ec != std::errc{} || converted.ptr != offset_text + offset_length || offset > uint64_t(std::numeric_limits<int64_t>::max()))
      return failure("INVALID_ARGUMENT");
    const auto path = std::filesystem::u8path(input);
    if (!path.is_absolute()) return failure("INVALID_ARGUMENT");
    Reader reader;
#ifdef _WIN32
    // FILE_SHARE_DELETE is essential: CRT fopen("rb") permits ordinary writes
    // but can still make a writer's rename/replace/delete fail with error 32.
    reader.value = CreateFileW(path.c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (reader.value == INVALID_HANDLE_VALUE) {
      const DWORD error = GetLastError();
      return failure(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? "NOT_FOUND" : "OPEN_FAILED", error);
    }
#else
    reader.value = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (reader.value < 0) return failure(errno == ENOENT || errno == ENOTDIR ? "NOT_FOUND" : "OPEN_FAILED", errno);
#endif
    Snapshot before, after;
    if (!snapshot(reader.value, before)) return failure("STAT_FAILED");
    if (before.directory && count > 0) return failure("NOT_FILE");
#ifdef REAWEB_SHARED_READ_TEST_HOOK
    shared_read_test_hook();
#endif
    const auto wanted = static_cast<unsigned>(offset < before.size ? std::min<uint64_t>(count, before.size - offset) : 0);
    char bytes[65536];
    unsigned copied = 0;
    if (wanted) {
#ifdef _WIN32
      LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(offset);
      DWORD received = 0;
      if (!SetFilePointerEx(reader.value, position, nullptr, FILE_BEGIN) || !ReadFile(reader.value, bytes, wanted, &received, nullptr))
        return failure("READ_FAILED", GetLastError());
      copied = received;
#else
      while (copied < wanted) {
        const auto received = pread(reader.value, bytes + copied, wanted - copied, static_cast<off_t>(offset + copied));
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) return failure("READ_FAILED", received < 0 ? errno : 0);
        copied += static_cast<unsigned>(received);
      }
#endif
    }
    if (!snapshot(reader.value, after) || !(before == after) || copied != wanted) return failure("FILE_CHANGED");
    static constexpr char digits[] = "0123456789abcdef";
    std::string hex; hex.resize(copied * 2);
    for (unsigned index = 0; index < copied; ++index) {
      const auto value = static_cast<unsigned char>(bytes[index]);
      hex[index * 2] = digits[value >> 4]; hex[index * 2 + 1] = digits[value & 15];
    }
    return "{\"protocol\":1,\"ok\":true,\"dataHex\":\"" + hex + "\",\"size\":\"" + std::to_string(before.size) +
      "\",\"mtime\":\"" + before.mtime + "\",\"fileId\":\"" + before.identity + "\",\"directory\":" + (before.directory ? "true}" : "false}");
  } catch (...) { return failure("READ_FAILED"); }
}
}
