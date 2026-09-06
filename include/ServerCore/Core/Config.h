#pragma once

#include "ServerCore/Core/Error.h"

#include <memory>
#include <string>
#include <string_view>

namespace ServerCore::Core
{
/// <summary>
/// 서버가 시작할 때 읽는 설정값 모음이다. 읽기 전용 스냅숏이다.
/// </summary>
/// <remarks>
/// 왜 이 자리에 있는가:
/// 포트 번호, I/O 스레드 수, 프레임 본문 상한처럼 코드가 아니라 배포가 정하는 값들이 있다.
/// 각 층이 제 방식으로 그것을 읽으면 서버마다 설정 형식이 달라진다.
///
/// 파일 형식:
/// - 파일과 경로 문자열은 NUL 바이트 없는 UTF-8이다. 선두 UTF-8 BOM은 허용하고 버린다. 줄 끝은
///   LF 또는 CRLF다.
/// - 빈 줄과, 공백·탭 뒤 첫 글자가 '#'인 줄은 무시한다. 줄 중간의 '#'은 주석이 아니다.
/// - 나머지 줄은 "key = value"다. 첫 '='만 나누는 곳이므로 값에는 '='를 넣을 수 있다.
///   키와 값 양끝의 ASCII 공백·탭은 버리며, 빈 값은 허용한다. 따옴표·escape 문법은 없다.
/// - 키는 대소문자를 구별하는 ASCII [A-Za-z0-9._-]+다. 같은 키를 두 번 쓰면 마지막 값으로
///   조용히 덮지 않고 형식 오류로 멈춘다.
///
/// 스레드 안전성: 적재가 끝난 뒤 저장소는 바뀌지 않으므로, 같은 Config에서 const 조회를 여러
/// 스레드가 동시에 해도 된다. 객체를 파괴하거나 이동하는 것과 조회를 동시에 하는 것은 일반
/// C++ 객체 규칙대로 지원하지 않는다.
///
/// 약속하지 않는 것:
/// - 파일 변경을 감지하지 않는다. 도는 중에 파일을 고쳐도 이 객체는 그대로다.
/// - 어떤 키가 있어야 하는지 스스로 알지 못한다. 필수 키의 목록은 읽는 쪽이 안다.
/// - 인라인 주석, 따옴표, escape, 포함 파일, 환경 변수 치환은 지원하지 않는다.
/// </remarks>
class Config
{
public:
    /// <summary>빈 읽기 전용 스냅숏을 만든다.</summary>
    Config() noexcept = default;
    Config(const Config&) = default;
    Config(Config&&) noexcept = default;
    Config& operator=(const Config&) = delete;
    Config& operator=(Config&&) = delete;
    ~Config();

    /// <summary>UTF-8 경로의 파일에서 설정을 읽는다.</summary>
    /// <returns>
    /// 빈 경로·NUL 바이트·잘못된 UTF-8은 InvalidArgument, 없는 파일은 NotFound, 파일을 읽지
    /// 못하거나 저장소를 만들지 못하면 PlatformError, NUL 바이트를 포함하는 파일 또는 형식이 위
    /// 계약과 다르면 InvalidFormat이다. 값이 하나도 없는 파일은 실패가 아니라 빈 설정이다.
    /// </returns>
    static Result<Config> LoadFromFile(std::string_view path);

    /// <summary>10진 int 값을 읽는다.</summary>
    /// <returns>
    /// 키 문법이 맞지 않으면 InvalidArgument, 키가 없으면 NotFound, 값이 int 범위의 10진 정수가
    /// 아니면 InvalidFormat이다. '+' 부호, 소수점, 접미사, 범위 밖 숫자는 정수가 아니다.
    /// </returns>
    [[nodiscard]] Result<int> GetInt(std::string_view key) const;

    /// <summary>문자열 값을 읽는다.</summary>
    /// <returns>키 문법이 맞지 않으면 InvalidArgument, 키가 없으면 NotFound다.</returns>
    [[nodiscard]] Result<std::string> GetString(std::string_view key) const;

private:
    struct Storage;

    explicit Config(std::shared_ptr<const Storage> storage);

    [[nodiscard]] const std::string* FindValue(std::string_view key) const noexcept;

    std::shared_ptr<const Storage> mStorage;
};
}
