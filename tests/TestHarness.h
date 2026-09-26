#pragma once

#include <sstream>
#include <string>
#include <string_view>

/// <summary>
/// 검사 실행 틀. 외부 시험 라이브러리에 의존하지 않는다.
/// </summary>
/// <remarks>
/// 왜 이 자리에 있는가:
/// 실행 파일 하나가 통째로 통과·실패만 말하면 ctest가 "1/1"만 뱉는다. 그러면 두 번 돌려
/// 수를 비교할 수 없고, 검사가 늘었는지 줄었는지도 보이지 않는다. 이 틀은 검사마다 이름을
/// 붙여 등록하고, 실행 파일이 이름 하나를 받아 그것만 돌린다. ctest는 검사 하나를 시험
/// 하나로 등록하므로 "N/N"이라는 수가 나온다.
///
/// 왜 외부 라이브러리를 안 쓰는가:
/// 시험 틀을 끌어오면 의존성 해결이라는 실패 지점이 하나 늘어난다. 이름 붙은 검사와 종료
/// 코드만 필요하므로 외부 시험 라이브러리 없이 제공한다.
///
/// 부수 효과 하나:
/// 검사마다 프로세스가 새로 뜨므로 전역 상태가 검사 사이로 새지 않는다. 전역 로거처럼
/// "부팅 때 한 번"인 것을 시험이 구조적으로 어길 수 없다.
///
/// 등록 이름과 ctest 이름이 어긋나는 것을 무엇이 막는가:
/// 실행 파일의 --verify-list가 막는다. ctest에 등록된 이름 전부를 인자로 받아 등록표와
/// 견주고, 한쪽에만 있는 것이 있으면 실패한다. 그 자체가 시험 하나로 등록되어 있으므로
/// 검사를 C++에만 더하거나 CMake에만 더하면 그 시험이 붉어진다.
///
/// 약속하지 않는 것:
/// - 검사 사이의 실행 순서를 약속하지 않는다. ctest가 정한다.
/// - 실패한 검사를 이어서 도는 것을 약속하지 않는다. 한 검사가 끝나면 프로세스가 끝난다.
/// - 검사 안에서 던진 예외를 다루지 않는다. 코어는 예외를 던지지 않기로 되어 있다.
/// </remarks>
namespace ServerCoreTest
{
/// <summary>검사 하나의 본체다. 실패는 Expect 계열로 알린다.</summary>
using CheckFunction = void (*)();

/// <summary>
/// 검사 하나를 이름과 함께 등록한다. 파일 지역 전역 객체로 두면 main보다 먼저 등록된다.
/// </summary>
/// <param name="name">
/// ctest에 등록되는 이름과 같아야 한다. 같지 않으면 --verify-list 시험이 붉어진다.
/// </param>
class CheckRegistration
{
public:
    CheckRegistration(std::string_view name, CheckFunction function);
};

/// <summary>참이어야 하는 것이 참인지 본다.</summary>
/// <param name="what">무엇을 확인했는지. 원인 짐작을 적지 않는다.</param>
void ExpectTrue(bool condition, std::string_view what);

/// <summary>건너뛴 검사의 종료 코드. ctest에는 SKIP_RETURN_CODE로 같은 값을 준다.</summary>
inline constexpr int SkipExitCode = 77;

/// <summary>
/// 환경이 검사의 전제를 갖추지 못해 이 검사를 건너뛴다고 기록한다. 부른 검사는 곧바로 돌아온다.
/// </summary>
/// <remarks>
/// 실패가 없으면 실행 파일은 SkipExitCode로 끝나고 ctest는 통과가 아니라 건너뜀으로 센다.
/// requiredBy로 준 환경 변수가 설정되어 있으면(CI가 그 전제를 갖췄다고 선언한 경우) 건너뛰지 않고
/// 실패로 기록한다. 그래야 전제가 조용히 사라져 검사가 빠지는 일이 없다.
/// </remarks>
void Skip(std::string_view reason, const char* requiredBy = nullptr);

/// <summary>ExpectEqual이 어긋남을 알리는 창구다. 검사가 직접 부르지 않는다.</summary>
void ReportMismatch(const std::string& expected, const std::string& actual, std::string_view what);

/// <summary>두 값이 같은지 보고, 다르면 무엇이 달랐는지만 적는다.</summary>
template <typename T> void ExpectEqual(const T& expected, const T& actual, std::string_view what)
{
    if (expected == actual)
    {
        return;
    }

    std::ostringstream expectedText;
    std::ostringstream actualText;
    expectedText << expected;
    actualText << actual;
    ReportMismatch(expectedText.str(), actualText.str(), what);
}
}
