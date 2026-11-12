#include <rex/input/sdl/pad_ownership.h>

#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace rex::input::sdl {
namespace {

constexpr size_t kMaxPads = 8;
constexpr size_t kPadsPerInstance = 4;

class FileLock {
 public:
  explicit FileLock(const std::filesystem::path& path) {
#ifdef _WIN32
    handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle_ != INVALID_HANDLE_VALUE) {
      OVERLAPPED overlapped{};
      if (LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &overlapped)) {
        locked_ = true;
        overlapped_ = overlapped;
      }
    }
#else
    fd_ = open(path.c_str(), O_CREAT | O_RDWR, 0600);
    if (fd_ >= 0 && flock(fd_, LOCK_EX) == 0) locked_ = true;
#endif
  }
  ~FileLock() {
#ifdef _WIN32
    if (locked_) UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &overlapped_);
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
    if (locked_) flock(fd_, LOCK_UN);
    if (fd_ >= 0) close(fd_);
#endif
  }
  explicit operator bool() const { return locked_; }

 private:
  bool locked_ = false;
#ifdef _WIN32
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  OVERLAPPED overlapped_{};
#else
  int fd_ = -1;
#endif
};

std::string Encode(std::string_view value) {
  constexpr char digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(value.size() * 2);
  for (unsigned char ch : value) {
    out.push_back(digits[ch >> 4]);
    out.push_back(digits[ch & 15]);
  }
  return out;
}

int HexValue(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

std::string Decode(std::string_view value) {
  if (value.size() % 2) return {};
  std::string out;
  out.reserve(value.size() / 2);
  for (size_t i = 0; i < value.size(); i += 2) {
    const int hi = HexValue(value[i]), lo = HexValue(value[i + 1]);
    if (hi < 0 || lo < 0) return {};
    out.push_back(static_cast<char>((hi << 4) | lo));
  }
  return out;
}

std::filesystem::path RegistryPath() {
  std::error_code ec;
  auto dir = std::filesystem::temp_directory_path(ec);
  if (ec) return {};
  return dir / "rex-bo2-sdl-pad-ownership-v1.txt";
}

using Assignments = std::map<std::string, int>;

Assignments ReadAssignments(const std::filesystem::path& path) {
  Assignments assignments;
  std::ifstream input(path);
  int owner;
  std::string encoded;
  while (input >> owner >> encoded) {
    if (owner >= 0 && owner <= 15) {
      auto identity = Decode(encoded);
      if (!identity.empty()) assignments.emplace(std::move(identity), owner);
    }
  }
  return assignments;
}

bool WriteAssignments(const std::filesystem::path& path, const Assignments& assignments) {
  auto temporary = path;
  temporary += ".tmp." + std::to_string(
#ifdef _WIN32
      GetCurrentProcessId()
#else
      getpid()
#endif
      );
  {
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) return false;
    for (const auto& [identity, owner] : assignments) output << owner << ' ' << Encode(identity) << '\n';
    output.flush();
    if (!output) return false;
  }
#ifndef _WIN32
  if (chmod(temporary.c_str(), 0600) != 0) {
    std::error_code remove_error;
    std::filesystem::remove(temporary, remove_error);
    return false;
  }
#endif
  std::error_code ec;
  std::filesystem::rename(temporary, path, ec);
  if (!ec) return true;
  // Windows does not replace an existing destination with rename.
  std::filesystem::remove(path, ec);
  ec.clear();
  std::filesystem::rename(temporary, path, ec);
  if (ec) std::filesystem::remove(temporary, ec);
  return !ec;
}

}  // namespace

std::string MakePadIdentity(std::string_view guid, std::string_view serial,
                            std::string_view path) {
  if (guid.empty() || (serial.empty() && path.empty())) return {};
  return std::string(guid) + (serial.empty() ? "|path|" : "|serial|") +
         std::string(serial.empty() ? path : serial);
}

PadOwnership::PadOwnership() {
  registry_path_ = RegistryPath();
  const char* value = std::getenv("BO2_INSTANCE");
  if (!value) return;
  const std::string text(value);
  int instance = -1;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), instance);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || instance < 0 ||
      instance > 15) {
    instance_ = -1;
    return;
  }
  instance_ = instance;
}

PadOwnership::PadOwnership(int instance, std::filesystem::path registry_path)
    : instance_(instance), registry_path_(std::move(registry_path)) {}

bool PadOwnership::Owns(std::string_view identity) const {
  if (!instance_) return true;
  if (*instance_ < 0 || *instance_ > 15) return false;
  if (identity.empty()) return false;
  const auto& path = registry_path_;
  if (path.empty()) return false;
  auto lock_path = path;
  lock_path += ".lock";
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  FileLock lock(lock_path);
  if (!lock) return false;

  auto assignments = ReadAssignments(path);
  if (auto existing = assignments.find(std::string(identity)); existing != assignments.end()) {
    return existing->second == *instance_;
  }
  if (assignments.size() >= kMaxPads) return false;
  size_t owned = 0;
  for (const auto& [key, owner] : assignments) owned += owner == *instance_;
  if (owned >= kPadsPerInstance) return false;
  assignments.emplace(std::string(identity), *instance_);
  return WriteAssignments(path, assignments);
}

}  // namespace rex::input::sdl
