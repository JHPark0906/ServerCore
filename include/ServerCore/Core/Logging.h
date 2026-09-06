#pragma once

#include <memory>
#include <string_view>

namespace ServerCore::Core
{
/// <summary>기록의 중요도다. 낮은 것부터 높은 것 순으로 적혀 있다.</summary>
enum class LogLevel
{
    Trace,
    Debug,
    Info,
    Warn,
    Error
};

/// <summary>
/// 기록을 어디에 남길지를 정하는 자리다. 코어는 이 인터페이스만 알고 구현은 밖에서 넣는다.
/// </summary>
/// <remarks>
/// 왜 이 자리에 있는가:
/// 서버는 화면이 없다. 로그가 유일한 관측 창구이므로 로깅은 다른 어떤 층보다 아래에 있어야
/// 모든 층이 네트워크를 끌어오지 않고도 기록을 남길 수 있다.
///
/// 호출 계약:
/// - 구현은 반드시 스레드 안전해야 한다. I/O 스레드와 작업 스레드가 동시에 부른다.
/// - Write는 예외를 던지지 않아야 한다. 기록에 실패했다고 서버가 죽으면 안 된다.
///
/// 약속하지 않는 것:
/// - 기록의 순서를 약속하지 않는다. 스레드가 여럿이면 시간 순서와 기록 순서가 다를 수 있다.
/// - 기록이 디스크에 닿았는지를 약속하지 않는다.
/// </remarks>
class ILogger
{
public:
    virtual ~ILogger() = default;

    /// <param name="level">중요도.</param>
    /// <param name="message">
    /// 완성된 한 줄. 이 호출이 끝나면 유효하지 않을 수 있으므로 보관하려면 복사한다.
    /// </param>
    virtual void Write(LogLevel level, std::string_view message) noexcept = 0;
};

/// <summary>
/// 코어 전체가 쓸 로거를 설치한다.
/// </summary>
/// <remarks>
/// 누가 이것을 부르는가:
/// 실행 환경 층(Runtime::ServerHost)의 부팅 절차가 첫 단계에서 한 번 부른다. 시험이 자기
/// 로거를 끼워 넣더라도 프로덕션 경로에 부르는 자리가 따로 있다는 뜻이다.
///
/// 스레드 안전성: 부팅 때 한 번만 부르는 것을 전제한다. 도는 중에 바꾸는 것은 지원하지 않는다.
/// </remarks>
void SetGlobalLogger(std::shared_ptr<ILogger> logger);

/// <summary>
/// 설치된 로거를 준다. 설치된 것이 없으면 아무 데도 쓰지 않는 로거를 준다.
/// </summary>
/// <remarks>
/// 설치 전에도 널을 돌려주지 않는 이유는, 부팅 도중의 기록이 널 검사를 빠뜨린 자리에서
/// 프로세스를 죽이는 일을 막기 위해서다.
///
/// 약속하지 않는 것:
/// - 설치 전의 기록이 어딘가에 남는다고 약속하지 않는다. 그 기록은 사라진다.
/// </remarks>
[[nodiscard]] ILogger& GetGlobalLogger();
}
