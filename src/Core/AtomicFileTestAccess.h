#pragma once
#include "ServerCore/Export.h"

#if !defined(SERVERCORE_ENABLE_TEST_HOOKS)
#error "AtomicFileTestAccess is available only while ServerCore test hooks are enabled"
#endif

namespace ServerCore::Core::TestAccess
{
/// <summary>다음 AtomicFile 파일 동기화 한 번을 실패로 돌려주게 하는 시험 전용 지점이다.</summary>
/// <remarks>
/// 공개 Core 계약이 아니다. 이 헤더는 src 아래에만 있고 시험을 빌드할 때만 쓰인다. fsync와
/// FlushFileBuffers의 실패는 시험이 결정적으로 만들 수 없으므로, 실제 동기화 호출을 부르지 않고 그
/// 결과만 실패로 바꾼다. 한 번 쓰면 풀리므로 그 뒤의 동기화는 실제 호출이 성공을 돌려준다. Linux가
/// 쓰기 되돌림 오류를 fd당 한 번만 보고해 재시도한 fsync가 0을 돌려주는 상황을 이렇게 흉내 낸다.
/// </remarks>
SERVERCORE_TEST_API void FailNextAtomicFileSync() noexcept;
}
