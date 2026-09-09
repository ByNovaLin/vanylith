#include "controller/result_manager.h"

#include "core/secure_memory.h"
#include "core/tron_address.h"

#include <Windows.h>
#include <sddl.h>

#include <memory>

namespace vanityforge {
namespace {

struct LocalFreeDeleter {
  void operator()(void* pointer) const noexcept { LocalFree(pointer); }
};

class Handle final {
 public:
  explicit Handle(HANDLE handle) : handle_(handle) {}
  ~Handle() { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  HANDLE get() const noexcept { return handle_; }

 private:
  HANDLE handle_;
};

}  // namespace

ResultManager::ResultManager(std::filesystem::path root) : root_(std::move(root)) {}

bool ResultManager::save_wallet(const std::string& task_id, const std::string& address,
                                const PrivateKeyBytes& private_key, std::string& error) const {
  PSECURITY_DESCRIPTOR raw_descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"D:P(A;;FA;;;SY)(A;;FA;;;OW)", SDDL_REVISION_1, &raw_descriptor, nullptr)) {
    error = "Unable to create restricted result ACL";
    return false;
  }
  std::unique_ptr<void, LocalFreeDeleter> descriptor(raw_descriptor);
  SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), raw_descriptor, FALSE};

  std::error_code filesystem_error;
  std::filesystem::create_directories(root_, filesystem_error);
  if (filesystem_error) {
    error = "Unable to create results root";
    return false;
  }
  const std::filesystem::path task_directory = root_ / task_id;
  if (!CreateDirectoryW(task_directory.c_str(), &security) && GetLastError() != ERROR_ALREADY_EXISTS) {
    error = "Unable to create protected task result directory";
    return false;
  }

  const std::filesystem::path output_path = task_directory / L"wallets.txt";
  Handle output(CreateFileW(output_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, &security,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (output.get() == INVALID_HANDLE_VALUE) {
    error = "Unable to open protected wallet result file";
    return false;
  }

  std::string key_hex = bytes_to_hex(private_key.data(), private_key.size());
  std::string record = "Address: " + address + "\r\nPrivateKey: " + key_hex + "\r\n\r\n";
  secure_zero(key_hex.data(), key_hex.size());
  DWORD written = 0;
  const DWORD record_size = static_cast<DWORD>(record.size());
  const bool write_ok = WriteFile(output.get(), record.data(), record_size,
                                  &written, nullptr) != FALSE && written == record_size;
  const bool flush_ok = write_ok && FlushFileBuffers(output.get()) != FALSE;
  secure_zero(record.data(), record.size());
  if (!flush_ok) {
    error = "Unable to persist wallet result";
    return false;
  }
  return true;
}

}  // namespace vanityforge
