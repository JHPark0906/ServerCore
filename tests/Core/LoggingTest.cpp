#include "TestHarness.h"

#include "ServerCore/Core/Logging.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

/// <summary>
/// 전역 로거가 설치 전후로 무엇을 돌려주는지 고정하는 검사다.
/// </summary>
/// <remarks>
/// 이 검사들이 서로를 더럽히지 않는 이유는 검사마다 프로세스가 새로 뜨기 때문이다.
/// 로거 설치는 "부팅 때 한 번"이므로, 한 프로세스에서 여러 검사를 돌렸다면 앞 검사가
/// 설치한 로거가 뒤 검사에 남았을 것이다.
/// </remarks>
namespace
{
/// <summary>받은 기록을 세어 두는 시험용 로거다.</summary>
/// <remarks>
/// 스레드 안전성: 스레드 안전. 인터페이스가 구현에 요구하는 것이 그것이라 대역도 그것을
/// 지킨다. 이 검사들은 한 스레드에서만 쓰지만, 요구를 어긴 대역으로 시험하면 그 요구가
/// 지켜지는지 아무도 보지 않게 된다.
/// </remarks>
class RecordingLogger final : public ServerCore::Core::ILogger
{
public:
    void Write(ServerCore::Core::LogLevel level, std::string_view message) noexcept override
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        ++mCount;
        mLastLevel = level;
        mLastMessage = message;
    }

    [[nodiscard]] std::size_t Count() const
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        return mCount;
    }

    [[nodiscard]] ServerCore::Core::LogLevel LastLevel() const
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        return mLastLevel;
    }

    [[nodiscard]] std::string LastMessage() const
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        return mLastMessage;
    }

private:
    mutable std::mutex mMutex;
    std::size_t mCount = 0;
    ServerCore::Core::LogLevel mLastLevel = ServerCore::Core::LogLevel::Trace;
    std::string mLastMessage;
};

void LoggerIsUsableBeforeInstall()
{
    // 설치하기 전에 부른다. 널이 돌아왔다면 이 줄에서 프로세스가 죽고 검사가 실패한다.
    ServerCore::Core::ILogger& beforeInstall = ServerCore::Core::GetGlobalLogger();
    beforeInstall.Write(ServerCore::Core::LogLevel::Info, "written before any logger is installed");

    const auto recorder = std::make_shared<RecordingLogger>();
    ServerCore::Core::SetGlobalLogger(recorder);

    ServerCoreTest::ExpectTrue(&beforeInstall != recorder.get(),
        "the logger used before install is not the one installed later");
    ServerCoreTest::ExpectEqual(std::size_t{ 0 }, recorder->Count(),
        "writes made before install do not reach the logger installed later");
}

void InstalledLoggerReceivesLevelAndMessage()
{
    const auto recorder = std::make_shared<RecordingLogger>();
    ServerCore::Core::SetGlobalLogger(recorder);

    ServerCore::Core::GetGlobalLogger().Write(ServerCore::Core::LogLevel::Error, "port 17000 busy");

    ServerCoreTest::ExpectEqual(
        std::size_t{ 1 }, recorder->Count(), "the installed logger receives the write");
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::LogLevel::Error),
        static_cast<int>(recorder->LastLevel()),
        "the installed logger receives the level it was given");
    ServerCoreTest::ExpectEqual(std::string("port 17000 busy"), recorder->LastMessage(),
        "the installed logger receives the message it was given");
}

void InstallingAgainReplacesTheLogger()
{
    const auto first = std::make_shared<RecordingLogger>();
    const auto second = std::make_shared<RecordingLogger>();

    ServerCore::Core::SetGlobalLogger(first);
    ServerCore::Core::SetGlobalLogger(second);
    ServerCore::Core::GetGlobalLogger().Write(ServerCore::Core::LogLevel::Warn, "replaced");

    ServerCoreTest::ExpectEqual(
        std::size_t{ 0 }, first->Count(), "the replaced logger receives nothing");
    ServerCoreTest::ExpectEqual(
        std::size_t{ 1 }, second->Count(), "the logger installed last receives the write");
}

const ServerCoreTest::CheckRegistration gLoggerIsUsableBeforeInstall{
    "Logging.LoggerIsUsableBeforeInstall", LoggerIsUsableBeforeInstall
};
const ServerCoreTest::CheckRegistration gInstalledLoggerReceivesLevelAndMessage{
    "Logging.InstalledLoggerReceivesLevelAndMessage", InstalledLoggerReceivesLevelAndMessage
};
const ServerCoreTest::CheckRegistration gInstallingAgainReplacesTheLogger{
    "Logging.InstallingAgainReplacesTheLogger", InstallingAgainReplacesTheLogger
};
}
