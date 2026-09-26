#include "ServerCore/C/Files.h"
#include "C/Internal.h"
#include "ServerCore/Core/AtomicFile.h"
namespace C = ServerCore::CDetail;
namespace Core = ServerCore::Core;
// C 동기화 상수를 FileSync로 그대로 옮긴다(static_cast). 번호가 갈리면 여기서 멈춘다.
static_assert(SC_FILE_SYNC_NONE == static_cast<int>(Core::FileSync::None) &&
              SC_FILE_SYNC_FILE == static_cast<int>(Core::FileSync::File) &&
              SC_FILE_SYNC_DIRECTORY == static_cast<int>(Core::FileSync::FileAndDirectory));
struct sc_atomic_file
{
    std::unique_ptr<Core::AtomicFile> native;
};
extern "C"
{
    sc_status sc_atomic_file_options_init(sc_atomic_file_options* value, size_t size) noexcept
    {
        if (!value || size < sizeof(*value))
            return SC_INVALID_ARGUMENT;
        *value = { SC_ABI_VERSION, sizeof(*value), 64 * 1024 * 1024, SC_FILE_SYNC_FILE, 0 };
        return SC_OK;
    }
    int sc_file_supports_directory_sync(void) noexcept
    {
        return Core::AtomicFile::SupportsDirectorySync();
    }
    sc_status sc_atomic_file_create(
        sc_bytes target, const sc_atomic_file_options* options, sc_atomic_file** out) noexcept
    {
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        if (!C::Valid(target) || !C::Version(options) || options->reserved)
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]() -> sc_status
            {
                auto result = Core::AtomicFile::Create(C::Text(target),
                    { options->maximum_bytes, static_cast<Core::FileSync>(options->sync) });
                if (!result.IsOk())
                    return C::Code(result.GetStatus());
                *out = new sc_atomic_file{ std::move(result.Value()) };
                return SC_OK;
            });
    }
    sc_status sc_atomic_file_write(sc_atomic_file* value, sc_bytes data) noexcept
    {
        return value && C::Valid(data)
                   ? C::Protect([&] { return C::Code(value->native->Write(C::Bytes(data))); })
                   : SC_INVALID_ARGUMENT;
    }
    sc_status sc_atomic_file_commit(sc_atomic_file* value) noexcept
    {
        return value ? C::Protect([&] { return C::Code(value->native->Commit()); })
                     : SC_INVALID_ARGUMENT;
    }
    int sc_atomic_file_committed(const sc_atomic_file* value) noexcept
    {
        return value && value->native->Committed();
    }
    uint64_t sc_atomic_file_written(const sc_atomic_file* value) noexcept
    {
        return value ? value->native->BytesWritten() : 0;
    }
    sc_status sc_atomic_file_cancel(sc_atomic_file* value) noexcept
    {
        return value ? C::Code(value->native->Cancel()) : SC_INVALID_ARGUMENT;
    }
    void sc_atomic_file_destroy(sc_atomic_file* value) noexcept
    {
        delete value;
    }
}
