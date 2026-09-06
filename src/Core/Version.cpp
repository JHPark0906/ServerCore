#include "ServerCore/Core/Version.h"

namespace ServerCore
{
namespace
{
/// <summary>
/// 버전 문자열의 실체다. Version.h의 세 상수와 값이 같아야 하고,
/// 어긋나면 VersionTest의 "상수와 문자열이 일치한다" 시험이 붉어진다.
/// </summary>
constexpr std::string_view VersionString = "0.1.0";
}

std::string_view GetVersionString() noexcept
{
    return VersionString;
}
}
