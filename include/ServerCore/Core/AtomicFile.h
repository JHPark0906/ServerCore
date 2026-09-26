#pragma once

#include "ServerCore/Core/Error.h"
#include "ServerCore/Export.h"
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
/// <remarks>
/// 권한: Commit은 이 writer가 새로 만든 임시 파일을 대상 이름으로 옮기므로, 대상에 있던 파일의
/// 권한은 이어지지 않는다. POSIX에서는 임시 파일을 0600으로 만들어(umask가 더 좁힐 수 있다) 교체된
/// 대상도 소유자만 읽고 쓸 수 있게 된다. 다른 계정이 읽던 파일이면 교체 뒤 그 계정의 접근이
/// 막히므로, 필요하면 Commit 뒤에 호출자가 권한을 다시 준다. POSIX의 0600 상한은
/// Core.AtomicFilePublication이 고정한다. Windows에서는 보안 속성 없이 만들므로 임시 파일이 부모
/// 디렉터리의 상속 ACL을 받으며, 이 동작은 시험하지 않는다.
/// </remarks>
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
    /// <remarks>
    /// 게시 전 파일 동기화가 실패하면 이 writer는 끝난다. 이후 Write와 Commit은 같은 오류를
    /// 돌려주고 대상은 바뀌지 않으며, 다시 쓰려면 새 AtomicFile로 처음부터 쓴다. 다시 부른 동기화가
    /// 성공을 보고해도 앞서 잃은 쓰기가 저장 장치에 닿았다는 뜻이 아니기 때문이다.
    /// Core.AtomicFileSyncFailureIsTerminal이 이 동작을 고정한다. rename 실패는 writer를 끝내지 않는다.
    /// </remarks>
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
