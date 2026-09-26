#pragma once
#include "ServerCore/Export.h"

#if !defined(SERVERCORE_ENABLE_TEST_HOOKS)
#error "SessionRegistryTestAccess is available only while ServerCore test hooks are enabled"
#endif

namespace ServerCore::Session::TestAccess
{
/// <summary>다음 SessionRegistry::ForEach의 snapshot 할당이 std::bad_alloc으로 실패하게 한다.</summary>
/// <remarks>
/// 공개 계약이 아니다. 할당 실패를 실제 전역 할당기 없이 그 지점에서만 재현하려는 시험 전용 경계다.
/// 한 번 소비되면 저절로 풀린다.
/// </remarks>
SERVERCORE_TEST_API void FailNextForEachSnapshot() noexcept;

/// <summary>아직 소비되지 않은 snapshot 실패 표식을 지운다.</summary>
SERVERCORE_TEST_API void ClearForEachSnapshotFailure() noexcept;
}
