#pragma once

#include <string_view>

/// <summary>
/// ServerCore 라이브러리의 최상위 이름공간이다.
/// 각 게임 프로젝트는 이 라이브러리를 링크한 뒤 자신의 서버 백엔드를 그 위에 올린다.
/// </summary>
namespace ServerCore
{
/// <summary>버전의 주 번호. 호환되지 않는 변경이 있을 때 올린다.</summary>
inline constexpr int VersionMajor = 0;

/// <summary>버전의 부 번호. 호환되는 기능 추가가 있을 때 올린다.</summary>
inline constexpr int VersionMinor = 1;

/// <summary>버전의 수정 번호. 동작을 바꾸지 않는 수정이 있을 때 올린다.</summary>
inline constexpr int VersionPatch = 0;

/// <summary>
/// 라이브러리 버전을 "주.부.수정" 형식의 문자열로 돌려준다.
/// 이 문자열과 위의 세 상수는 같은 값을 가리켜야 하며, 그 일치는 시험이 고정한다.
/// </summary>
/// <returns>
/// 정적 저장기간을 갖는 버전 문자열이다. 호출자가 수명을 관리하지 않으며,
/// 반환된 뷰는 프로그램이 끝날 때까지 유효하다.
/// </returns>
[[nodiscard]] std::string_view GetVersionString() noexcept;
}
