#pragma once

#include <string_view>

namespace ServerCore::Core::Detail
{
/// <summary>바이트열이 Unicode scalar value만으로 이루어진 UTF-8인지 확인한다.</summary>
/// <remarks>
/// JSON과 Config가 같은 경계에서 UTF-8을 판단해야 하지만, Protocol은 Core에 의존하는 위층이다.
/// 따라서 구현을 Protocol에 두면 Config가 역방향으로 의존하게 된다. 이 비공개 Core 도구를 두
/// 층이 함께 써서 한 구현만 유지한다.
/// </remarks>
[[nodiscard]] bool IsValidUtf8(std::string_view text) noexcept;
}
