#include "ServerCore/Core/AtomicFile.h"
#include "Core/Utf8Internal.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <limits>
#include <new>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace ServerCore::Core
{
namespace
{
Status Fail(ErrorCode code = ErrorCode::PlatformError) noexcept
{
    return Status::FailWithoutMessage(code);
}
std::atomic<std::uint64_t> nextTemporary{ 0 };
Result<std::uint64_t> NextTemporary() noexcept
{
    auto value = nextTemporary.load(std::memory_order_relaxed);
    do
    {
        if (value == UINT64_MAX)
            return Result<std::uint64_t>::FromStatus(Fail());
    } while (!nextTemporary.compare_exchange_weak(value, value + 1, std::memory_order_relaxed));
    return Result<std::uint64_t>::FromValue(value);
}
#ifdef _WIN32
Status OsError() noexcept
{
    const auto error = GetLastError();
    return Fail(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                    ? ErrorCode::NotFound
                    : ErrorCode::PlatformError);
}
#else
Status OsError() noexcept
{
    return Fail(errno == ENOENT ? ErrorCode::NotFound : ErrorCode::PlatformError);
}
int SyncDescriptor(int descriptor) noexcept
{
    int result;
    do
    {
        result = fsync(descriptor);
    } while (result < 0 && errno == EINTR);
    return result;
}
#endif
}
struct AtomicFile::State
{
    AtomicFileOptions options;
    std::uint64_t written = 0;
    ErrorCode failure = ErrorCode::Ok;
    bool committed = false, cancelled = false;
#ifdef _WIN32
    HANDLE file = INVALID_HANDLE_VALUE;
    std::wstring target;
#else
    int file = -1, directory = -1;
    std::string temporary, target;
#endif
    Status Cleanup() noexcept
    {
        Status status = Status::Ok();
#ifdef _WIN32
        if (file != INVALID_HANDLE_VALUE)
        {
            if (!committed)
            {
                FILE_DISPOSITION_INFO disposition{ TRUE };
                if (!SetFileInformationByHandle(
                        file, FileDispositionInfo, &disposition, sizeof disposition))
                    status = OsError();
            }
            if (!CloseHandle(file) && status.IsOk())
                status = OsError();
            file = INVALID_HANDLE_VALUE;
        }
#else
        if (!temporary.empty() && directory >= 0)
        {
            if (unlinkat(directory, temporary.c_str(), 0) != 0 && errno != ENOENT)
                status = OsError();
            temporary.clear();
        }
        if (file >= 0)
        {
            if (close(file) != 0 && status.IsOk())
                status = OsError();
            file = -1;
        }
        if (directory >= 0)
        {
            if (close(directory) != 0 && status.IsOk())
                status = OsError();
            directory = -1;
        }
#endif
        return status;
    }
    ~State() { (void)Cleanup(); }
};
AtomicFile::AtomicFile(std::unique_ptr<State> state) noexcept
    : mState(std::move(state))
{
}
AtomicFile::~AtomicFile() = default;
bool AtomicFile::SupportsDirectorySync() noexcept
{
#ifdef _WIN32
    return false;
#else
    return true;
#endif
}
Result<std::unique_ptr<AtomicFile>> AtomicFile::Create(
    std::string_view utf8Target, AtomicFileOptions options)
{
    using R = Result<std::unique_ptr<AtomicFile>>;
    if (utf8Target.empty() || utf8Target.size() > 32768 ||
        utf8Target.find('\0') != std::string_view::npos || !Detail::IsValidUtf8(utf8Target) ||
        !options.maximumBytes || options.maximumBytes > (std::uint64_t{ 1 } << 40) ||
        (options.sync != FileSync::None && options.sync != FileSync::File &&
            options.sync != FileSync::FileAndDirectory))
        return R::FromStatus(Fail(ErrorCode::InvalidArgument));
    if (options.sync == FileSync::FileAndDirectory && !SupportsDirectorySync())
        return R::FromStatus(Fail(ErrorCode::Unimplemented));
    try
    {
        auto target = std::filesystem::absolute(
            std::filesystem::path(std::u8string(utf8Target.begin(), utf8Target.end())))
                          .lexically_normal();
        const auto leaf = target.filename();
        if (leaf.empty() || leaf == "." || leaf == "..")
            return R::FromStatus(Fail(ErrorCode::InvalidArgument));
        const auto leafUtf8 = leaf.u8string();
#ifdef _WIN32
        // Alternate streams and generated short-name alias targets are unsupported.
        if (leaf.native().find_first_of(L":*?\"<>|~") != std::wstring::npos ||
            leaf.native().back() == L'.' || leaf.native().back() == L' ')
            return R::FromStatus(Fail(ErrorCode::InvalidArgument));
#endif
        auto state = std::make_unique<State>();
        state->options = options;
#ifdef _WIN32
        state->target = target.native();
        const auto process = GetCurrentProcessId();
#else
        state->target = leaf.native();
        state->directory = open(target.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (state->directory < 0)
            return R::FromStatus(OsError());
        const auto process = getpid();
#endif
        for (unsigned attempt = 0; attempt < 64; ++attempt)
        {
            auto serial = NextTemporary();
            if (!serial.IsOk())
                return R::FromStatus(std::move(serial).TakeStatus());
            auto name = ".servercore-" + std::to_string(process) + "-" +
                        std::to_string(serial.Value()) + ".tmp";
            // A caller may choose a target in our temporary-name namespace.
            // Never create that target before Commit, including ASCII case aliases.
            if (leafUtf8.size() == name.size() &&
                std::equal(name.begin(), name.end(), leafUtf8.begin(),
                    [](char a, char8_t b)
                    {
                        const auto lower = b >= u8'A' && b <= u8'Z' ? b + (u8'a' - u8'A') : b;
                        return static_cast<unsigned char>(a) == lower;
                    }))
                continue;
#ifdef _WIN32
            auto temporary = target.parent_path() / name;
            if (CompareStringOrdinal(leaf.c_str(), -1, temporary.filename().c_str(), -1, TRUE) ==
                CSTR_EQUAL)
                continue;
            state->file = CreateFileW(temporary.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ,
                nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (state->file == INVALID_HANDLE_VALUE)
            {
                if (GetLastError() == ERROR_FILE_EXISTS || GetLastError() == ERROR_ALREADY_EXISTS)
                    continue;
                return R::FromStatus(OsError());
            }
#else
            state->file = openat(state->directory, name.c_str(),
                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (state->file < 0)
            {
                if (errno == EEXIST)
                    continue;
                return R::FromStatus(OsError());
            }
            state->temporary = std::move(name);
#endif
            return R::FromValue(std::unique_ptr<AtomicFile>(new AtomicFile(std::move(state))));
        }
        return R::FromStatus(Fail(ErrorCode::AlreadyExists));
    }
    catch (const std::bad_alloc&)
    {
        return R::FromStatus(Status::AllocationFailure());
    }
    catch (const std::filesystem::filesystem_error&)
    {
        return R::FromStatus(Fail());
    }
}
Status AtomicFile::Write(std::span<const std::byte> data, std::stop_token cancellation)
{
    auto& s = *mState;
    if (s.committed || s.cancelled)
        return Fail(ErrorCode::Closed);
    if (s.failure != ErrorCode::Ok)
        return Fail(s.failure);
    if (data.size() > s.options.maximumBytes - s.written)
        return Fail(ErrorCode::TooLarge);
    while (!data.empty())
    {
        if (cancellation.stop_requested())
        {
            s.failure = ErrorCode::Cancelled;
            return Fail(s.failure);
        }
        const auto count = std::min<std::size_t>(data.size(), 1024 * 1024);
#ifdef _WIN32
        DWORD written = 0;
        if (!WriteFile(s.file, data.data(), static_cast<DWORD>(count), &written, nullptr) ||
            !written)
        {
            s.failure = ErrorCode::PlatformError;
            return Fail(s.failure);
        }
#else
        ssize_t written;
        do
        {
            written = write(s.file, data.data(), count);
        } while (written < 0 && errno == EINTR && !cancellation.stop_requested());
        if (written <= 0)
        {
            s.failure =
                cancellation.stop_requested() ? ErrorCode::Cancelled : ErrorCode::PlatformError;
            return Fail(s.failure);
        }
#endif
        s.written += static_cast<std::uint64_t>(written);
        data = data.subspan(static_cast<std::size_t>(written));
    }
    return Status::Ok();
}
Status AtomicFile::Commit()
{
    auto& s = *mState;
    if (s.committed || s.cancelled)
        return Fail(ErrorCode::Closed);
    if (s.failure != ErrorCode::Ok)
        return Fail(s.failure);
#ifdef _WIN32
    try
    {
        const auto nameBytes = s.target.size() * sizeof(wchar_t);
        // Max-align allocation for the OS structure, including the full UTF-16 name.
        const auto size = sizeof(FILE_RENAME_INFO) + nameBytes;
        std::vector<std::max_align_t> storage(
            (size + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
        auto* info = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
        std::memset(info, 0, size);
        info->ReplaceIfExists = TRUE;
        info->FileNameLength = static_cast<DWORD>(nameBytes);
        std::memcpy(info->FileName, s.target.data(), nameBytes);
        if (s.options.sync != FileSync::None && !FlushFileBuffers(s.file))
            return OsError();
        if (!SetFileInformationByHandle(s.file, FileRenameInfo, info, static_cast<DWORD>(size)))
            return OsError();
        s.committed = true;
        // Release the writer handle; directory persistence is not claimed on Windows.
        return s.Cleanup();
    }
    catch (const std::bad_alloc&)
    {
        return Status::AllocationFailure();
    }
#else
    if (s.options.sync != FileSync::None && SyncDescriptor(s.file) != 0)
        return OsError();
    if (renameat(s.directory, s.temporary.c_str(), s.directory, s.target.c_str()) != 0)
        return OsError();
    s.committed = true;
    s.temporary.clear();
    auto result = Status::Ok();
    if (s.options.sync == FileSync::FileAndDirectory && SyncDescriptor(s.directory) != 0)
        result = OsError();
    auto cleaned = s.Cleanup();
    return result.IsOk() ? cleaned : result;
#endif
}
Status AtomicFile::Cancel() noexcept
{
    mState->cancelled = true;
    return mState->Cleanup();
}
std::uint64_t AtomicFile::BytesWritten() const noexcept
{
    return mState->written;
}
bool AtomicFile::Committed() const noexcept
{
    return mState->committed;
}
}
