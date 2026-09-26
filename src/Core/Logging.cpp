#include "ServerCore/Core/Logging.h"
#include "Core/Utf8Internal.h"
#include <algorithm>
#include <charconv>

#include <atomic>
#include <memory>
#include <utility>

namespace ServerCore::Core
{
Result<std::string> FormatLogRecord(const LogRecord& record, std::size_t maximum) noexcept
{
    using ResultType = Result<std::string>;
    const auto fail = [](ErrorCode code)
    { return ResultType::FromStatus(Status::FailWithoutMessage(code)); };
    if (record.level < LogLevel::Trace || record.level > LogLevel::Error || !maximum ||
        maximum > MaxStructuredLogBytes || record.fields.size() > MaxLogFields)
        return fail(ErrorCode::InvalidArgument);
    if (record.message.size() > maximum)
        return fail(ErrorCode::TooLarge);
    if (!Detail::IsValidUtf8(record.message))
        return fail(ErrorCode::InvalidArgument);
    for (std::size_t i = 0; i < record.fields.size(); ++i)
    {
        const auto& field = record.fields[i];
        if (field.name.empty() || field.name.size() > MaxLogFieldNameBytes)
            return fail(ErrorCode::InvalidArgument);
        if (field.value.size() > maximum)
            return fail(ErrorCode::TooLarge);
        if (!Detail::IsValidUtf8(field.value))
            return fail(ErrorCode::InvalidArgument);
        for (const unsigned char c : field.name)
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '_' || c == '.' || c == '-'))
                return fail(ErrorCode::InvalidArgument);
        for (std::size_t previous = 0; previous < i; ++previous)
            if (record.fields[previous].name == field.name)
                return fail(ErrorCode::InvalidArgument);
    }
    try
    {
        std::string text;
        text.reserve((std::min)(maximum, std::size_t{ 1024 }));
        bool tooLarge = false;
        const auto append = [&](std::string_view value)
        {
            if (value.size() > maximum - text.size())
            {
                tooLarge = true;
                return;
            }
            text.append(value);
        };
        const auto quoted = [&](std::string_view value)
        {
            append("\"");
            constexpr char hex[] = "0123456789abcdef";
            for (const unsigned char c : value)
            {
                if (tooLarge)
                    break;
                if (c == '"' || c == '\\')
                {
                    const char escaped[]{ '\\', static_cast<char>(c) };
                    append({ escaped, 2 });
                }
                else if (c < 32 || c == 127)
                {
                    const char escaped[]{ '\\', 'u', '0', '0', hex[c >> 4], hex[c & 15] };
                    append({ escaped, 6 });
                }
                else
                {
                    const char byte = static_cast<char>(c);
                    append({ &byte, 1 });
                }
            }
            append("\"");
        };
        static constexpr std::string_view levels[]{ "trace", "debug", "info", "warn", "error" };
        append("{\"level\":");
        quoted(levels[std::to_underlying(record.level)]);
        append(",\"message\":");
        quoted(record.message);
        const auto id = [&](std::string_view name, std::uint64_t value)
        {
            if (!value)
                return;
            append(",");
            quoted(name);
            append(":");
            char number[20];
            const auto converted = std::to_chars(number, number + sizeof(number), value);
            append({ number, static_cast<std::size_t>(converted.ptr - number) });
        };
        id("request_id", record.correlation.requestId);
        id("session_id", record.correlation.sessionId);
        id("task_id", record.correlation.taskId);
        id("connection_id", record.correlation.connectionId);
        append(",\"fields\":{");
        for (std::size_t i = 0; i < record.fields.size(); ++i)
        {
            if (i)
                append(",");
            quoted(record.fields[i].name);
            append(":");
            quoted(record.fields[i].value);
        }
        append("}}");
        if (tooLarge)
            return fail(ErrorCode::TooLarge);
        return ResultType::FromValue(std::move(text));
    }
    catch (...)
    {
        return ResultType::FromStatus(Status::AllocationFailure());
    }
}

Status WriteLog(ILogger& logger, const LogRecord& record) noexcept
{
    if (auto* structured = dynamic_cast<IStructuredLogger*>(&logger))
        return structured->TryWriteRecord(record);
    auto encoded = FormatLogRecord(record);
    if (!encoded.IsOk())
        return std::move(encoded).TakeStatus();
    logger.Write(record.level, encoded.Value());
    return Status::Ok();
}

namespace
{
/// <summary>아무 데도 쓰지 않는 로거다. 설치된 것이 없을 때 이것을 준다.</summary>
/// <remarks>
/// 왜 널 대신 이것인가:
/// 부팅 도중의 기록이 널 검사를 빠뜨린 자리에서 프로세스를 죽이는 일을 막으려는 것이다.
/// 로거는 부팅 첫 단계에 설치되지만 그 전에도 기록을 남기려는 코드가 있을 수 있다.
///
/// 스레드 안전성: 스레드 안전. 아무 상태도 없으므로 동시에 불러도 된다.
/// </remarks>
class DiscardingLogger final : public ILogger
{
public:
    void Write(LogLevel, std::string_view) noexcept override {}
};

/// <summary>버리는 로거의 실체다.</summary>
DiscardingLogger& Discarding() noexcept
{
    static DiscardingLogger logger;
    return logger;
}

/// <summary>설치된 로거의 소유권을 들고 있는 자리다.</summary>
/// <remarks>
/// 조회에 쓰지 않는다. shared_ptr을 읽는 것은 그 자체로 원자적이지 않아서, 조회는 아래
/// 원시 포인터 쪽으로 한다. 이쪽은 대상이 살아 있게 붙잡아 두는 일만 한다.
/// </remarks>
std::shared_ptr<ILogger>& OwnedLogger()
{
    static std::shared_ptr<ILogger> owned;
    return owned;
}

/// <summary>지금 쓸 로거를 가리킨다. 조회는 전부 이것을 읽는다.</summary>
/// <remarks>
/// 원자적 포인터로 두는 이유는, 헤더가 설치를 부팅 때 한 번으로 정해 두었더라도 조회는
/// 여러 스레드에서 동시에 일어나기 때문이다. 설치가 한 번뿐이라는 것에 기대어 평범한
/// 포인터로 두면, 그 한 번과 겹치는 조회가 있을 때 경합이 된다. 값이 하나뿐이라 비용도
/// 거의 없다.
/// </remarks>
std::atomic<ILogger*>& CurrentLogger() noexcept
{
    static std::atomic<ILogger*> current{ &Discarding() };
    return current;
}
}

void SetGlobalLogger(std::shared_ptr<ILogger> logger)
{
    // 널을 넣으면 버리는 로거로 돌아간다. 그래야 GetGlobalLogger가 어느 경우에도 널을
    // 돌려주지 않는다는 약속이 설치 뒤에도 유지된다.
    ILogger* target = logger ? logger.get() : &Discarding();

    // 새 로거를 조회 자리에 먼저 올리고, 이전 로거는 함수를 나가며 놓는다. 반대 순서면 이전 로거가
    // 파괴되는 동안의 조회가 파괴 중인 그 로거를 받는다.
    // Logging.ReplacingPublishesTheNewLoggerBeforeDestroyingTheOld가 이 순서를 고정한다.
    // 이 순서가 도는 중의 교체를 안전하게 만들지는 않는다. 교체 전에 받아 둔 참조는 여전히 이전
    // 로거를 가리킨다.
    const std::shared_ptr<ILogger> previous = std::exchange(OwnedLogger(), std::move(logger));
    CurrentLogger().store(target, std::memory_order_release);
}

ILogger& GetGlobalLogger()
{
    return *CurrentLogger().load(std::memory_order_acquire);
}
}
