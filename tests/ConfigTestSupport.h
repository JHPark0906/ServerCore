#pragma once

#include "TestHarness.h"

#include <process.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

/// <summary>
/// Config를 쓰는 검사가 같은 임시 파일 규칙을 쓰게 하는 내부 시험 도구다.
/// </summary>
/// <remarks>
/// CTest가 각 검사를 별도 프로세스로 실행하지만, 병렬 실행과 비정상 종료 뒤의 파일이 겹치지
/// 않도록 프로세스 ID와 순번을 함께 파일 이름에 넣는다. UTF-8 경로 검사는 native 경로와 API에
/// 넘길 UTF-8 문자열을 나누어 보관한다.
/// </remarks>
namespace ServerCoreTest
{
inline std::atomic<unsigned long> gConfigTestFileSequence{ 0 };

[[nodiscard]] inline std::string MakeTemporaryConfigPath()
{
    const unsigned long processId = static_cast<unsigned long>(::_getpid());

    std::string path("ServerCoreConfigTest-");
    path.append(std::to_string(processId));
    path.push_back('-');
    path.append(std::to_string(gConfigTestFileSequence.fetch_add(1)));
    path.append(".cfg");
    return path;
}

struct NativeConfigPath
{
    std::string utf8;
    std::filesystem::path native;
};

[[nodiscard]] inline NativeConfigPath MakeUtf8TemporaryConfigPath()
{
    const unsigned long processId = static_cast<unsigned long>(::_getpid());
    const unsigned long sequence = gConfigTestFileSequence.fetch_add(1);

    std::string utf8("ServerCoreConfigTest-");
    utf8.append(std::to_string(processId));
    utf8.push_back('-');
    utf8.append(std::to_string(sequence));
    utf8.append("-경로.cfg");

    std::wstring native(L"ServerCoreConfigTest-");
    native.append(std::to_wstring(processId));
    native.push_back(L'-');
    native.append(std::to_wstring(sequence));
    native.append(L"-경로.cfg");
    return NativeConfigPath{ std::move(utf8), std::filesystem::path(std::move(native)) };
}

class ScopedConfigFile final
{
public:
    explicit ScopedConfigFile(const std::string_view text)
        : mPath(MakeTemporaryConfigPath())
        , mNativePath(mPath)
    {
        Rewrite(text);
    }

    ScopedConfigFile(
        std::string path, std::filesystem::path nativePath, const std::string_view text)
        : mPath(std::move(path))
        , mNativePath(std::move(nativePath))
    {
        Rewrite(text);
    }

    ~ScopedConfigFile()
    {
        std::error_code error;
        (void)std::filesystem::remove(mNativePath, error);
    }

    ScopedConfigFile(const ScopedConfigFile&) = delete;
    ScopedConfigFile& operator=(const ScopedConfigFile&) = delete;
    ScopedConfigFile(ScopedConfigFile&&) = delete;
    ScopedConfigFile& operator=(ScopedConfigFile&&) = delete;

    [[nodiscard]] std::string_view Path() const noexcept { return mPath; }

    void Rewrite(const std::string_view text) const
    {
        std::ofstream file(mNativePath, std::ios::binary | std::ios::trunc);
        ExpectTrue(file.is_open(), "config test file opens for writing");
        if (!file.is_open())
        {
            return;
        }

        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        file.close();
        ExpectTrue(!file.fail(), "config test file finishes writing");
    }

private:
    std::string mPath;
    std::filesystem::path mNativePath;
};
}
