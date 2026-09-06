#include "TestHarness.h"

#include "ServerCore/Core/Version.h"

#include <string>

/// <summary>
/// 버전 상수와 버전 문자열이 어긋나지 않는지 고정하는 검사다.
/// </summary>
/// <remarks>
/// 이 검사가 존재하는 이유는 버전이 중요해서가 아니라, 검증 배선이 실제로 도는지를 보여 줄
/// 대상이 하나 필요하기 때문이다.
///
/// 실패 메시지에는 무엇이 기대와 달랐는지만 적는다. 원인 짐작은 적지 않는다.
/// 짐작은 코드가 변한 뒤에도 남아서 다음 사람의 첫 가설을 낡은 것으로 고정한다.
/// </remarks>
namespace
{
/// <summary>Version.h의 세 상수로부터 기대되는 버전 문자열을 만든다.</summary>
/// <remarks>
/// 문자열을 손으로 다시 적지 않고 상수로 조립한다. 손으로 적으면 검사가 라이브러리와
/// 같은 값을 두 군데에 두는 것이 되어, 한쪽만 고쳤을 때 잡아야 할 어긋남을 못 잡는다.
/// </remarks>
std::string ComposeExpectedVersion()
{
    return std::to_string(ServerCore::VersionMajor) + "." +
           std::to_string(ServerCore::VersionMinor) + "." +
           std::to_string(ServerCore::VersionPatch);
}

void StringMatchesConstants()
{
    ServerCoreTest::ExpectEqual(ComposeExpectedVersion(),
        std::string(ServerCore::GetVersionString()), "version constants vs version string");
}

void StringIsNotEmpty()
{
    // 위 검사는 양쪽이 함께 비어도 통과할 수 있으므로 하한을 따로 건다.
    ServerCoreTest::ExpectTrue(
        !ServerCore::GetVersionString().empty(), "version string is not empty");
}

const ServerCoreTest::CheckRegistration gStringMatchesConstants{ "Version.StringMatchesConstants",
    StringMatchesConstants };
const ServerCoreTest::CheckRegistration gStringIsNotEmpty{ "Version.StringIsNotEmpty",
    StringIsNotEmpty };
}
