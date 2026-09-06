#pragma once

#include <cstdint>

namespace ServerCore::Core
{
/// <summary>이 서버 프로세스 안에서 단조 증가하는 경과 시간이다.</summary>
/// <remarks>
/// 원점은 이 함수를 처음 쓰는 순간의 프로세스 수명 안 기준점이며, 벽시계 조정과 무관하다.
/// 코어는 uint64 밀리초를 준다. 와이어에 더 좁은 값을 싣는지는 게임이 자기 wrap 규칙과 함께
/// 정한다.
/// </remarks>
[[nodiscard]] std::uint64_t MillisecondsSinceProcessStart() noexcept;
}
