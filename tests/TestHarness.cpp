#include "TestHarness.h"

#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>

namespace ServerCoreTest
{
namespace
{
/// <summary>등록된 검사들이다.</summary>
/// <remarks>
/// 함수 지역 정적으로 두는 이유는, 등록이 main보다 먼저 일어나는데 전역 객체 사이의 초기화
/// 순서가 정해져 있지 않기 때문이다. 처음 쓰는 순간 만들어지면 그 순서가 문제되지 않는다.
/// </remarks>
std::map<std::string, CheckFunction>& Registry()
{
    static std::map<std::string, CheckFunction> registry;
    return registry;
}

int gFailureCount = 0;
}

CheckRegistration::CheckRegistration(std::string_view name, CheckFunction function)
{
    Registry().emplace(std::string(name), function);
}

void ExpectTrue(bool condition, std::string_view what)
{
    if (condition)
    {
        return;
    }

    std::cerr << "[FAIL] " << what << ": expected true, actual false\n";
    ++gFailureCount;
}

void ReportMismatch(const std::string& expected, const std::string& actual, std::string_view what)
{
    std::cerr << "[FAIL] " << what << ": expected \"" << expected << "\", actual \"" << actual
              << "\"\n";
    ++gFailureCount;
}

namespace
{
/// <summary>등록된 이름을 한 줄에 하나씩 낸다.</summary>
int PrintNames()
{
    for (const auto& entry : Registry())
    {
        std::cout << entry.first << "\n";
    }
    return 0;
}

/// <summary>
/// ctest에 등록된 이름들과 이 실행 파일의 등록표가 정확히 같은지 본다.
/// </summary>
/// <returns>어긋나면 1. 어느 쪽에만 있는지를 함께 적는다.</returns>
int VerifyNames(int count, char** names)
{
    std::set<std::string> fromCtest;
    for (int index = 0; index < count; ++index)
    {
        fromCtest.emplace(names[index]);
    }

    int failures = 0;
    for (const auto& entry : Registry())
    {
        if (fromCtest.count(entry.first) == 0)
        {
            std::cerr << "[FAIL] registered in the executable but not in ctest: " << entry.first
                      << "\n";
            ++failures;
        }
    }

    for (const auto& name : fromCtest)
    {
        if (Registry().count(name) == 0)
        {
            std::cerr << "[FAIL] registered in ctest but not in the executable: " << name << "\n";
            ++failures;
        }
    }

    return failures == 0 ? 0 : 1;
}

/// <summary>이름 하나에 해당하는 검사를 돌린다.</summary>
int RunOne(const std::string& name)
{
    const auto found = Registry().find(name);
    if (found == Registry().end())
    {
        std::cerr << "[FAIL] no check is registered under this name: " << name << "\n";
        return 1;
    }

    found->second();

    if (gFailureCount != 0)
    {
        std::cerr << gFailureCount << " check(s) failed\n";
        return 1;
    }

    return 0;
}
}
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: ServerCoreTests <check-name> | --list | --verify-list <names...>\n";
        return 2;
    }

    const std::string first(argv[1]);

    if (first == "--list")
    {
        return ServerCoreTest::PrintNames();
    }

    if (first == "--verify-list")
    {
        return ServerCoreTest::VerifyNames(argc - 2, argv + 2);
    }

    return ServerCoreTest::RunOne(first);
}
