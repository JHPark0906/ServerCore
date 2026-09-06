#include "ServerCore/Core/Logging.h"

#include <atomic>
#include <memory>
#include <utility>

namespace ServerCore::Core
{
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
    OwnedLogger() = std::move(logger);

    // 널을 넣으면 버리는 로거로 돌아간다. 그래야 GetGlobalLogger가 어느 경우에도 널을
    // 돌려주지 않는다는 약속이 설치 뒤에도 유지된다.
    ILogger* target = OwnedLogger() ? OwnedLogger().get() : &Discarding();
    CurrentLogger().store(target, std::memory_order_release);
}

ILogger& GetGlobalLogger()
{
    return *CurrentLogger().load(std::memory_order_acquire);
}
}
