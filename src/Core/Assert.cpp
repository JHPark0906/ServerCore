#include "ServerCore/Core/Assert.h"

#include <cstdio>
#include <cstdlib>

namespace ServerCore::Core
{
namespace
{
/// <summary>널로 끝나는 문자열을 표준 오류에 그대로 흘린다.</summary>
void WriteRaw(const char* text) noexcept
{
    if (text == nullptr)
    {
        return;
    }

    std::size_t length = 0;
    while (text[length] != '\0')
    {
        ++length;
    }

    std::fwrite(text, 1, length, stderr);
}

/// <summary>
/// 정수를 십진 문자열로 바꿔 표준 오류에 흘린다.
/// </summary>
/// <remarks>
/// 서식 함수를 쓰지 않는 이유는, 이 경로가 이미 프로그램의 가정이 깨진 뒤라서다.
/// 서식 기계를 거치지 않으면 그만큼 덜 의존한다.
/// </remarks>
void WriteInt(int value) noexcept
{
    char digits[16];
    int index = static_cast<int>(sizeof(digits));

    const bool negative = value < 0;
    unsigned int magnitude =
        negative ? 0u - static_cast<unsigned int>(value) : static_cast<unsigned int>(value);

    do
    {
        digits[--index] = static_cast<char>('0' + (magnitude % 10u));
        magnitude /= 10u;
    } while (magnitude != 0u && index > 0);

    if (negative && index > 0)
    {
        digits[--index] = '-';
    }

    std::fwrite(digits + index, 1,
        static_cast<std::size_t>(sizeof(digits)) - static_cast<std::size_t>(index), stderr);
}
}

void ReportAssertFailure(
    const char* expression, const char* file, int line, const char* message) noexcept
{
    // 이 출력만 영문이다. 콘솔 코드페이지가 UTF-8이 아니면 한글이 깨져 나오는데,
    // 계약 위반이 났을 때 마지막으로 남는 한 줄이 읽히지 않으면 안 되기 때문이다.
    WriteRaw("\n[ASSERT] ");
    WriteRaw(file);
    WriteRaw(":");
    WriteInt(line);
    WriteRaw(": ");
    WriteRaw(message);
    WriteRaw("\n         condition: ");
    WriteRaw(expression);
    WriteRaw("\n");
    std::fflush(stderr);

#ifdef _MSC_VER
    // MSVC의 디버그 런타임은 abort()에서 모달 대화상자를 띄울 수 있다. 화면이 없는 서버가
    // 보이지 않는 대화상자 앞에 멈춰 서면 죽는 것보다 나쁘다. 죽으면 감시자가 다시 띄우지만
    // 멈춰 서면 살아 있는 것처럼 보인다. 이 호출은 대화상자가 원래 안 뜨는 구성에서는
    // 아무 일도 하지 않는다.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    std::abort();
}
}
