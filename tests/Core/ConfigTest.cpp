#include "ConfigTestSupport.h"

#include "TestHarness.h"

#include "ServerCore/Core/Config.h"

#include <cstdio>
#include <limits>
#include <string>
#include <string_view>

/// <summary>
/// Config의 파일 형식과 읽기 전용 스냅숏 경계를 고정하는 검사다.
/// </summary>
/// <remarks>
/// 설정 파일은 각 검사가 자기 프로세스에서 쓰는 임시 상대 경로에 만든다. CTest가 검사를 병렬로
/// 돌려도 PID와 순번이 다르므로 서로 같은 파일을 덮지 않는다. Config 본체는 파일을 다 읽은 뒤
/// 값을 따로 보관해야 하므로, 이 검사에서 원본 파일을 다시 써도 먼저 읽은 값이 바뀌면 안 된다.
/// </remarks>
namespace
{
using ServerCoreTest::MakeTemporaryConfigPath;
using ServerCoreTest::MakeUtf8TemporaryConfigPath;
using ServerCoreTest::NativeConfigPath;
using ServerCoreTest::ScopedConfigFile;

template <typename T>
void ExpectFailureCode(const ServerCore::Core::Result<T>& result,
    const ServerCore::Core::ErrorCode expected, const std::string_view what)
{
    ServerCoreTest::ExpectTrue(!result.IsOk(), what);
    if (!result.IsOk())
    {
        ServerCoreTest::ExpectEqual(
            static_cast<int>(expected), static_cast<int>(result.GetStatus().Code()), what);
    }
}

void ExpectLoadFailure(const std::string_view text, const ServerCore::Core::ErrorCode expected,
    const std::string_view what)
{
    const ScopedConfigFile file(text);
    const ServerCore::Core::Result<ServerCore::Core::Config> loaded =
        ServerCore::Core::Config::LoadFromFile(file.Path());
    ExpectFailureCode(loaded, expected, what);
}

void LoadsFlatSnapshot()
{
    const ScopedConfigFile file(
        "\xEF\xBB\xBF   # leading BOM, whitespace, and a comment are allowed\r\n"
        "\r\n"
        "server.title = Summit 데모\r\n"
        "server.description = value = keeps = every separator\n"
        "server.token = token#with-a-hash\n"
        "server.empty =    \r\n"
        "server.worker-count = -42\r\n");

    const ServerCore::Core::Result<ServerCore::Core::Config> loaded =
        ServerCore::Core::Config::LoadFromFile(file.Path());
    ServerCoreTest::ExpectTrue(loaded.IsOk(), "a flat UTF-8 config loads");
    if (!loaded.IsOk())
    {
        return;
    }

    const ServerCore::Core::Result<std::string> title = loaded.Value().GetString("server.title");
    ServerCoreTest::ExpectTrue(title.IsOk(), "a UTF-8 string value is found");
    if (title.IsOk())
    {
        ServerCoreTest::ExpectEqual(
            std::string("Summit 데모"), title.Value(), "a UTF-8 string value is preserved");
    }

    const ServerCore::Core::Result<std::string> description =
        loaded.Value().GetString("server.description");
    ServerCoreTest::ExpectTrue(description.IsOk(), "a value containing equals signs is found");
    if (description.IsOk())
    {
        ServerCoreTest::ExpectEqual(std::string("value = keeps = every separator"),
            description.Value(), "only the first equals sign separates a setting");
    }

    const ServerCore::Core::Result<std::string> token = loaded.Value().GetString("server.token");
    ServerCoreTest::ExpectTrue(token.IsOk(), "a value containing a hash is found");
    if (token.IsOk())
    {
        ServerCoreTest::ExpectEqual(std::string("token#with-a-hash"), token.Value(),
            "a hash in a value is not an inline comment");
    }

    const ServerCore::Core::Result<std::string> empty = loaded.Value().GetString("server.empty");
    ServerCoreTest::ExpectTrue(empty.IsOk(), "an explicit empty value is found");
    if (empty.IsOk())
    {
        ServerCoreTest::ExpectEqual(
            std::string(), empty.Value(), "an explicit empty value stays empty");
    }

    const ServerCore::Core::Result<int> workerCount = loaded.Value().GetInt("server.worker-count");
    ServerCoreTest::ExpectTrue(workerCount.IsOk(), "a decimal integer value is found");
    if (workerCount.IsOk())
    {
        ServerCoreTest::ExpectEqual(-42, workerCount.Value(), "a decimal integer value is parsed");
    }

    file.Rewrite("server.title = changed\n");
    const ServerCore::Core::Result<std::string> retainedTitle =
        loaded.Value().GetString("server.title");
    ServerCoreTest::ExpectTrue(
        retainedTitle.IsOk(), "a value remains readable after the file changes");
    if (retainedTitle.IsOk())
    {
        ServerCoreTest::ExpectEqual(std::string("Summit 데모"), retainedTitle.Value(),
            "a loaded Config is a file snapshot");
    }
}

void ReportsMissingValuesAndFiles()
{
    const std::string missingPath = MakeTemporaryConfigPath();
    (void)std::remove(missingPath.c_str());
    const ServerCore::Core::Result<ServerCore::Core::Config> missingFile =
        ServerCore::Core::Config::LoadFromFile(missingPath);
    ExpectFailureCode(missingFile, ServerCore::Core::ErrorCode::NotFound,
        "a missing config file reports NotFound");

    const ScopedConfigFile file("# an empty config is still a config\n");
    const ServerCore::Core::Result<ServerCore::Core::Config> loaded =
        ServerCore::Core::Config::LoadFromFile(file.Path());
    ServerCoreTest::ExpectTrue(loaded.IsOk(), "an empty config loads");
    if (!loaded.IsOk())
    {
        return;
    }

    const ServerCore::Core::Result<std::string> missingString =
        loaded.Value().GetString("server.name");
    ExpectFailureCode(missingString, ServerCore::Core::ErrorCode::NotFound,
        "a missing string key reports NotFound");

    const ServerCore::Core::Result<int> missingInteger = loaded.Value().GetInt("server.port");
    ExpectFailureCode(missingInteger, ServerCore::Core::ErrorCode::NotFound,
        "a missing integer key reports NotFound");

    const ServerCore::Core::Result<std::string> invalidKey =
        loaded.Value().GetString("server name");
    ExpectFailureCode(invalidKey, ServerCore::Core::ErrorCode::InvalidArgument,
        "a lookup key outside the config key grammar reports InvalidArgument");

    const ServerCore::Core::Result<int> invalidIntegerKey = loaded.Value().GetInt("server port");
    ExpectFailureCode(invalidIntegerKey, ServerCore::Core::ErrorCode::InvalidArgument,
        "an integer lookup key outside the config key grammar reports InvalidArgument");

    const ServerCore::Core::Config emptySnapshot;
    const ServerCore::Core::Result<std::string> defaultMissing =
        emptySnapshot.GetString("server.name");
    ExpectFailureCode(defaultMissing, ServerCore::Core::ErrorCode::NotFound,
        "a default Config is an empty snapshot");

    const ServerCore::Core::Result<int> defaultMissingInteger = emptySnapshot.GetInt("server.port");
    ExpectFailureCode(defaultMissingInteger, ServerCore::Core::ErrorCode::NotFound,
        "a default Config has no integer values");
}

void ValidatesUtf8Paths()
{
    const NativeConfigPath path = MakeUtf8TemporaryConfigPath();
    const ScopedConfigFile file(path.utf8, path.native, "server.name = Summit\n");

    const ServerCore::Core::Result<ServerCore::Core::Config> loaded =
        ServerCore::Core::Config::LoadFromFile(path.utf8);
    ServerCoreTest::ExpectTrue(
        loaded.IsOk(), "a UTF-8 config path loads through its native filename");
    if (loaded.IsOk())
    {
        const ServerCore::Core::Result<std::string> name = loaded.Value().GetString("server.name");
        ServerCoreTest::ExpectTrue(
            name.IsOk(), "a config loaded through a UTF-8 path contains its value");
        if (name.IsOk())
        {
            ServerCoreTest::ExpectEqual(
                std::string("Summit"), name.Value(), "a UTF-8 config path reads the intended file");
        }
    }

    ExpectFailureCode(ServerCore::Core::Config::LoadFromFile(""),
        ServerCore::Core::ErrorCode::InvalidArgument,
        "an empty config path reports InvalidArgument");

    std::string nulPath("config");
    nulPath.push_back('\0');
    ExpectFailureCode(ServerCore::Core::Config::LoadFromFile(nulPath),
        ServerCore::Core::ErrorCode::InvalidArgument,
        "a config path with a NUL byte reports InvalidArgument");

    std::string invalidUtf8Path("config-");
    invalidUtf8Path.push_back(static_cast<char>(0xC3));
    ExpectFailureCode(ServerCore::Core::Config::LoadFromFile(invalidUtf8Path),
        ServerCore::Core::ErrorCode::InvalidArgument,
        "a config path with incomplete UTF-8 reports InvalidArgument");
}

void RejectsInvalidDocuments()
{
    ExpectLoadFailure("server.port 17100\n", ServerCore::Core::ErrorCode::InvalidFormat,
        "a setting without an equals sign reports InvalidFormat");
    ExpectLoadFailure("server port = 17100\n", ServerCore::Core::ErrorCode::InvalidFormat,
        "a setting with an invalid key reports InvalidFormat");
    ExpectLoadFailure("server.port = 17100\nserver.port = 17101\n",
        ServerCore::Core::ErrorCode::InvalidFormat, "a duplicate config key reports InvalidFormat");
    ExpectLoadFailure("server.port = 17100\rserver.name = Summit\n",
        ServerCore::Core::ErrorCode::InvalidFormat, "a bare carriage return reports InvalidFormat");

    std::string invalidUtf8("server.name = ");
    invalidUtf8.push_back(static_cast<char>(0xC3));
    ExpectLoadFailure(invalidUtf8, ServerCore::Core::ErrorCode::InvalidFormat,
        "a config file with incomplete UTF-8 reports InvalidFormat");

    std::string nulText("server.name = Summit");
    nulText.push_back('\0');
    ExpectLoadFailure(nulText, ServerCore::Core::ErrorCode::InvalidFormat,
        "a config file containing a NUL byte reports InvalidFormat");
}

void RejectsInvalidIntegers()
{
    const ScopedConfigFile file("zero = 0\n"
                                "positive = 42\n"
                                "valid = -42\n"
                                "minimum = -2147483648\n"
                                "maximum = 2147483647\n"
                                "plus = +1\n"
                                "suffix = 100ms\n"
                                "overflow = 2147483648\n"
                                "underflow = -2147483649\n"
                                "empty = \n");
    const ServerCore::Core::Result<ServerCore::Core::Config> loaded =
        ServerCore::Core::Config::LoadFromFile(file.Path());
    ServerCoreTest::ExpectTrue(loaded.IsOk(), "a config with textual integer candidates loads");
    if (!loaded.IsOk())
    {
        return;
    }

    const ServerCore::Core::Result<int> valid = loaded.Value().GetInt("valid");
    ServerCoreTest::ExpectTrue(valid.IsOk(), "a signed decimal integer is accepted");
    if (valid.IsOk())
    {
        ServerCoreTest::ExpectEqual(
            -42, valid.Value(), "a signed decimal integer retains its value");
    }

    const ServerCore::Core::Result<int> zero = loaded.Value().GetInt("zero");
    ServerCoreTest::ExpectTrue(zero.IsOk(), "zero is an accepted decimal integer");
    if (zero.IsOk())
    {
        ServerCoreTest::ExpectEqual(0, zero.Value(), "zero retains its value");
    }

    const ServerCore::Core::Result<int> positive = loaded.Value().GetInt("positive");
    ServerCoreTest::ExpectTrue(positive.IsOk(), "a positive decimal integer is accepted");
    if (positive.IsOk())
    {
        ServerCoreTest::ExpectEqual(
            42, positive.Value(), "a positive decimal integer retains its value");
    }

    const ServerCore::Core::Result<int> minimum = loaded.Value().GetInt("minimum");
    ServerCoreTest::ExpectTrue(minimum.IsOk(), "the smallest int value is accepted");
    if (minimum.IsOk())
    {
        ServerCoreTest::ExpectEqual((std::numeric_limits<int>::min)(), minimum.Value(),
            "the smallest int value is preserved");
    }

    const ServerCore::Core::Result<int> maximum = loaded.Value().GetInt("maximum");
    ServerCoreTest::ExpectTrue(maximum.IsOk(), "the largest int value is accepted");
    if (maximum.IsOk())
    {
        ServerCoreTest::ExpectEqual((std::numeric_limits<int>::max)(), maximum.Value(),
            "the largest int value is preserved");
    }

    ExpectFailureCode(loaded.Value().GetInt("plus"), ServerCore::Core::ErrorCode::InvalidFormat,
        "a leading plus sign is not a decimal int");
    ExpectFailureCode(loaded.Value().GetInt("suffix"), ServerCore::Core::ErrorCode::InvalidFormat,
        "a decimal int with a suffix reports InvalidFormat");
    ExpectFailureCode(loaded.Value().GetInt("overflow"), ServerCore::Core::ErrorCode::InvalidFormat,
        "an integer above int range reports InvalidFormat");
    ExpectFailureCode(loaded.Value().GetInt("underflow"),
        ServerCore::Core::ErrorCode::InvalidFormat,
        "an integer below int range reports InvalidFormat");
    ExpectFailureCode(loaded.Value().GetInt("empty"), ServerCore::Core::ErrorCode::InvalidFormat,
        "an empty value is not a decimal int");
}

const ServerCoreTest::CheckRegistration gLoadsFlatSnapshot{ "Config.LoadsFlatSnapshot",
    LoadsFlatSnapshot };
const ServerCoreTest::CheckRegistration gReportsMissingValuesAndFiles{
    "Config.ReportsMissingValuesAndFiles", ReportsMissingValuesAndFiles
};
const ServerCoreTest::CheckRegistration gValidatesUtf8Paths{ "Config.ValidatesUtf8Paths",
    ValidatesUtf8Paths };
const ServerCoreTest::CheckRegistration gRejectsInvalidDocuments{ "Config.RejectsInvalidDocuments",
    RejectsInvalidDocuments };
const ServerCoreTest::CheckRegistration gRejectsInvalidIntegers{ "Config.RejectsInvalidIntegers",
    RejectsInvalidIntegers };
}
