#include "ServerCore/Core/AtomicFile.h"
#include "ConfigTestSupport.h"
#include "Core/AtomicFileTestAccess.h"
#include <iterator>
#ifndef _WIN32
#include <sys/stat.h>
#endif
namespace
{
using namespace ServerCore::Core;
using ServerCoreTest::ExpectTrue;
std::span<const std::byte> Bytes(std::string_view text)
{
    return std::as_bytes(std::span(text.data(), text.size()));
}
std::string Read(std::string_view path)
{
    std::ifstream input(
        std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
    return { std::istreambuf_iterator<char>(input), {} };
}
std::size_t Temporaries()
{
    std::size_t count = 0;
    const auto prefix =
        ".servercore-" + std::to_string(ServerCoreTest::ConfigTestProcessId()) + "-";
    // Unrelated Unicode filenames may not be representable in the Windows code page.
    const auto nativePrefix = std::filesystem::path(prefix).native();
    for (const auto& entry : std::filesystem::directory_iterator("."))
        if (entry.path().filename().native().starts_with(nativePrefix))
            ++count;
    return count;
}
void AtomicFilePublication()
{
    // Each registered CTest runs in a fresh process, so the first candidate is 0.
    const auto candidate =
        ".servercore-" + std::to_string(ServerCoreTest::ConfigTestProcessId()) + "-0.tmp";
    if (!std::filesystem::exists(candidate))
    {
        auto collision = AtomicFile::Create(candidate, { 16, FileSync::None });
        ExpectTrue(collision.IsOk(), "temporary-style target is supported");
        if (collision.IsOk())
        {
            ExpectTrue(collision.Value()->Write(Bytes("pending")).IsOk() &&
                           !std::filesystem::exists(candidate),
                "a temporary candidate matching the target cannot publish before Commit");
            ExpectTrue(collision.Value()->Cancel().IsOk(), "matching candidate cleanup");
        }
    }
    auto path = ServerCoreTest::MakeUtf8TemporaryConfigPath();
    ServerCoreTest::ScopedConfigFile target(path.utf8, path.native, "old");
    const auto count = Temporaries();
    auto first = AtomicFile::Create(target.Path());
    auto second = AtomicFile::Create(target.Path());
    ExpectTrue(
        first.IsOk() && second.IsOk(), "exclusive adjacent temporaries coexist for same target");
    if (!first.IsOk() || !second.IsOk())
        return;
    ExpectTrue(first.Value()->Write(Bytes("first")).IsOk() &&
                   second.Value()->Write(Bytes("second")).IsOk() && Read(target.Path()) == "old",
        "streaming writes do not truncate existing target");
    ExpectTrue(first.Value()->Commit().IsOk() && first.Value()->Committed() &&
                   Read(target.Path()) == "first",
        "commit publishes complete replacement");
    ExpectTrue(second.Value()->Commit().IsOk() && Read(target.Path()) == "second",
        "last successful commit replaces earlier complete value");
    ExpectTrue(first.Value()->Write(Bytes("!")).Code() == ErrorCode::Closed &&
                   first.Value()->Commit().Code() == ErrorCode::Closed,
        "published writer is terminal");
    ExpectTrue(
        first.Value()->Cancel().IsOk() && Read(target.Path()) == "second" && Temporaries() == count,
        "cleanup never deletes published target");
#ifndef _WIN32
    struct stat info{};
    ExpectTrue(stat(path.native.c_str(), &info) == 0 && (info.st_mode & 0177) == 0,
        "POSIX replacement permissions cannot exceed 0600; umask may narrow them");
#endif
}
void AtomicFileFailureAndCleanup()
{
    ServerCoreTest::ScopedConfigFile target("old");
    const auto count = Temporaries();
    {
        auto file = std::move(AtomicFile::Create(target.Path(), { 3, FileSync::None }).Value());
        ExpectTrue(file->Write(Bytes("toolarge")).Code() == ErrorCode::TooLarge &&
                       file->BytesWritten() == 0,
            "file cap checked before writing");
        ExpectTrue(file->Write(Bytes("new")).IsOk() && Read(target.Path()) == "old",
            "cap refusal does not poison valid later write");
    }
    ExpectTrue(Temporaries() == count && Read(target.Path()) == "old",
        "dropping uncommitted writer cleans temporary and preserves target");
    auto file = std::move(AtomicFile::Create(target.Path()).Value());
    std::stop_source stop;
    stop.request_stop();
    ExpectTrue(file->Write(Bytes("new"), stop.get_token()).Code() == ErrorCode::Cancelled &&
                   file->Commit().Code() == ErrorCode::Cancelled,
        "cancelled partial write cannot publish");
    ExpectTrue(file->Cancel().IsOk() && file->Cancel().IsOk() && Temporaries() == count,
        "explicit cleanup is idempotent");
    auto synced = AtomicFile::Create(target.Path(), { 10, FileSync::FileAndDirectory });
    if (AtomicFile::SupportsDirectorySync())
    {
        ExpectTrue(synced.IsOk() && synced.Value()->Write(Bytes("durable")).IsOk() &&
                       synced.Value()->Commit().IsOk(),
            "explicit parent directory synchronization");
    }
    else
        ExpectTrue(!synced.IsOk() && synced.GetStatus().Code() == ErrorCode::Unimplemented &&
                       Temporaries() == count,
            "unsupported sync is rejected before file creation");
    ExpectTrue(
        !AtomicFile::Create("").IsOk() && !AtomicFile::Create(std::string_view("a\0b", 3)).IsOk(),
        "invalid paths rejected");
#ifdef _WIN32
    ExpectTrue(AtomicFile::Create("SERVER~1.TMP").GetStatus().Code() == ErrorCode::InvalidArgument,
        "generated short-name alias targets are rejected before file creation");
#endif
}
void AtomicFileSyncFailureIsTerminal()
{
    ServerCoreTest::ScopedConfigFile target("old");
    const auto count = Temporaries();
    auto created = AtomicFile::Create(target.Path(), { 16, FileSync::File });
    ExpectTrue(created.IsOk(), "a synchronizing writer is created");
    if (!created.IsOk())
        return;
    auto& file = *created.Value();
    ExpectTrue(file.Write(Bytes("new")).IsOk(), "replacement bytes are written");
    TestAccess::FailNextAtomicFileSync();
    ExpectTrue(!file.Commit().IsOk() && !file.Committed() && Read(target.Path()) == "old",
        "a failed file synchronization fails Commit without publishing");
    // 동기화가 실패한 뒤의 동기화는 성공을 보고할 수 있다. 그 성공으로 게시하면 안 된다.
    const auto retried = file.Commit();
    ExpectTrue(!retried.IsOk() && !file.Committed() && Read(target.Path()) == "old",
        "a retried Commit after a failed synchronization does not publish");
    ExpectTrue(file.Write(Bytes("more")).Code() == retried.Code(),
        "a writer whose synchronization failed refuses later writes with the same error");
    ExpectTrue(file.Cancel().IsOk() && Temporaries() == count && Read(target.Path()) == "old",
        "cleanup after a failed synchronization removes the temporary and keeps the target");
}
ServerCoreTest::CheckRegistration a("Core.AtomicFilePublication", AtomicFilePublication);
ServerCoreTest::CheckRegistration b(
    "Core.AtomicFileFailureAndCleanup", AtomicFileFailureAndCleanup);
ServerCoreTest::CheckRegistration c(
    "Core.AtomicFileSyncFailureIsTerminal", AtomicFileSyncFailureIsTerminal);
}
