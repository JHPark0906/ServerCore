#pragma once
#include "ServerCore/Export.h"

#if !defined(SERVERCORE_ENABLE_TEST_HOOKS)
#error "MessageTestAccess is available only while ServerCore test hooks are enabled"
#endif

namespace ServerCore::Protocol
{
class JsonValue;
}

namespace ServerCore::Protocol::TestAccess
{
/// <summary>ParseMessage가 파싱한 body를 Message에 넘기기 직전에 그 값을 보여 주는 시험 전용 관찰자다.</summary>
/// <remarks>
/// 공개 Protocol 계약이 아니다. 이 헤더는 src 아래에만 있고 시험을 빌드할 때만 쓰인다. 시험은 여기서
/// 본 body 안쪽 원소의 주소를 반환된 Message의 같은 원소 주소와 견주어, body가 깊은 복사 없이
/// 옮겨졌는지를 시간이나 메모리 측정 없이 판정한다.
/// </remarks>
using ParsedBodyObserver = void (*)(const JsonValue& body) noexcept;

/// <summary>다음 ParseMessage 호출부터 쓸 관찰자를 설치한다. 널을 넣으면 해제한다.</summary>
SERVERCORE_TEST_API void SetParsedBodyObserver(ParsedBodyObserver observer) noexcept;
}
