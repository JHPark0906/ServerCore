#pragma once
#include "ServerCore/C/Types.h"
#include "TestHarness.h"

#include <cstddef>

/// <summary>
/// 시험 실행 파일이 전역 operator new를 대체해 할당 실패를 주입할 수 있는 구성인가.
/// </summary>
/// <remarks>
/// 대체는 GameExecutionAbiTest.cpp 한 곳에 정의되어 있고, 이 값이 0인 구성에서는 정의되지 않는다.
/// 0인 구성은 셋이다.
/// - Windows DLL 구성: DLL이 자기 operator new를 따로 링크하므로 실행 파일의 대체가 DLL 안의 할당에
///   닿지 않는다.
/// - MSVC 반복자 디버그 구성(_ITERATOR_DEBUG_LEVEL != 0, 곧 Debug): 이 STL은 noexcept인 빈
///   std::string 생성에서도 디버그용 컨테이너 프록시를 할당한다. 그 할당을 실패시키면 표준 라이브러리
///   안에서 std::terminate로 끝나므로, 코드와 무관하게 주입 검사가 끝까지 갈 수 없다.
/// - 주소 새니타이저 구성: 대체가 malloc/free로 내려가면 새니타이저의 new/delete 불일치 검출이
///   시험 실행 파일 전체에서 꺼진다. 그 대가를 치르지 않고, 할당 실패 검사는 다른 구성에 맡긴다.
/// 그 밖의 구성(MSVC Release 정적 링크, GCC·Clang 정적 링크와 ELF 공유 라이브러리)에서는 대체가
/// ServerCore 구현까지 닿아야 한다. 닿는지는 가정하지 않고 AbiTest::AllocationFailureReachesLibrary가
/// 매번 확인한다.
/// </remarks>
#if defined(_WIN32) && defined(SC_CABI_SHARED)
#define SC_TEST_ALLOCATION_HOOK 0
#elif defined(_MSC_VER) && defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL != 0
#define SC_TEST_ALLOCATION_HOOK 0
#elif defined(__SANITIZE_ADDRESS__)
#define SC_TEST_ALLOCATION_HOOK 0
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define SC_TEST_ALLOCATION_HOOK 0
#endif
#endif
#ifndef SC_TEST_ALLOCATION_HOOK
#define SC_TEST_ALLOCATION_HOOK 1
#endif

namespace AbiTest
{
/// <summary>
/// 살아 있는 동안 이 스레드의 operator new 호출을 세어, skip개를 통과시킨 뒤 다음 한 번을
/// std::bad_alloc으로 실패시킨다. 실패는 한 번만 일으키고 그 뒤로는 통과시킨다.
/// </summary>
/// <remarks>
/// 다른 스레드의 할당은 세지도 실패시키지도 않는다. 그래서 워커 스레드가 도는 중에도 호출 스레드가
/// 하는 할당 순서만으로 실패 지점이 정해진다. 걸어 둔 동안에는 시험 자신의 할당(단언 메시지 등)도
/// 대상이 되므로, 걸어 둔 구간에는 시험할 호출 하나만 둔다. 겹쳐 걸지 않는다.
/// </remarks>
class FailNthAllocation
{
public:
    explicit FailNthAllocation(std::size_t skip) noexcept;
    ~FailNthAllocation();
    FailNthAllocation(const FailNthAllocation&) = delete;
    FailNthAllocation& operator=(const FailNthAllocation&) = delete;

    /// <summary>실패를 실제로 일으켰는가. 구간 안의 할당이 skip개 이하였으면 거짓이다.</summary>
    [[nodiscard]] bool Fired() const noexcept;
};

/// <summary>
/// /EHsc는 extern "C" 함수가 던지지 않는다고 가정해 호출부의 catch를 지울 수 있다. 함수 포인터를
/// volatile로 거쳐 그 가정을 끊는다. 그래야 예외가 ABI 밖으로 새는 결함이 프로세스 중단이 아니라
/// 이 시험의 실패로 적힌다.
/// </summary>
template <class Function> Function* Opaque(Function* function) noexcept
{
    Function* volatile value = function;
    return value;
}

/// <summary>skip개 뒤의 할당 하나를 실패시킨 채 call을 한 번 부른다.</summary>
/// <param name="escaped">call에서 C++ 예외가 빠져나왔으면 참이 된다.</param>
/// <returns>주입한 실패가 실제로 일어났는가.</returns>
template <class Call> bool InjectAllocationFailure(std::size_t skip, Call&& call, bool& escaped)
{
    const FailNthAllocation failure(skip);
    try
    {
        call();
    }
    catch (...)
    {
        escaped = true;
    }
    return failure.Fired();
}

/// <summary>
/// 할당 실패 주입이 ServerCore 구현의 할당까지 닿는지 확인한다.
/// </summary>
/// <returns>
/// 닿으면 참. SC_TEST_ALLOCATION_HOOK이 0인 구성이면 확인 없이 거짓이다. 1인 구성에서 닿지 않으면
/// 그 사실을 실패로 적고 거짓을 돌려준다. 그래서 할당 실패 검사가 아무것도 주입하지 못한 채
/// 초록으로 남지 않는다.
/// </returns>
bool AllocationFailureReachesLibrary();
}
