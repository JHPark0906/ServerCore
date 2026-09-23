#pragma once

#include "ServerCore/Export.h"
#include "ServerCore/Core/Error.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>
#include <string_view>

namespace ServerCore::Core
{
enum class FileSync
{
    None,
    File,
    FileAndDirectory
};
struct AtomicFileOptions
{
    std::uint64_t maximumBytes = 64 * 1024 * 1024;
    FileSync sync = FileSync::File;
};
// Blocking file operations, intended for application worker threads. Serialize
// access to one writer. Target parent must be a trusted existing directory.
// The exclusive temporary file is created in that directory and cleaned on drop.
// Commit replaces the directory entry, never truncates the old file in place.
// Filesystem support is required; rename atomicity is distinct from crash durability.
// Windows targets require regular long names, not generated short-name aliases;
// '~' in the target leaf is rejected along with alternate streams and wildcards.
class AtomicFile
{
public:
    SERVERCORE_API static Result<std::unique_ptr<AtomicFile>> Create(
        std::string_view utf8Target, AtomicFileOptions options = {});
    SERVERCORE_API static bool SupportsDirectorySync() noexcept;
    SERVERCORE_API ~AtomicFile();
    AtomicFile(const AtomicFile&) = delete;
    AtomicFile& operator=(const AtomicFile&) = delete;
    SERVERCORE_API Status Write(std::span<const std::byte> data, std::stop_token cancellation = {});
    // On a post-rename directory-sync error Committed() is true: the replacement
    // is visible, but its persistence could not be confirmed. Do not retry blindly.
    SERVERCORE_API Status Commit();
    SERVERCORE_API Status Cancel() noexcept;
    SERVERCORE_API std::uint64_t BytesWritten() const noexcept;
    SERVERCORE_API bool Committed() const noexcept;

private:
    struct State;
    explicit AtomicFile(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> mState;
};
}
