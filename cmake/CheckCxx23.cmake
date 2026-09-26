include(CheckCXXSourceCompiles)
include(CMakePushCheckState)

function(ServerCoreCheckCxx23Support)
    cmake_push_check_state(RESET)
    check_cxx_source_compiles([=[
        #include <algorithm>
        #include <array>
        #include <expected>
        #include <functional>
        #include <memory>
        #include <string_view>
        #include <type_traits>
        #include <utility>
        #include <version>

        #if !defined(__cpp_lib_expected) || __cpp_lib_expected < 202202L
        #error ServerCore requires C++23 std::expected
        #endif
        #if !defined(__cpp_lib_move_only_function) || __cpp_lib_move_only_function < 202110L
        #error ServerCore requires C++23 std::move_only_function
        #endif
        #if !defined(__cpp_lib_string_contains) || __cpp_lib_string_contains < 202011L
        #error ServerCore requires C++23 string contains
        #endif
        #if !defined(__cpp_lib_ranges_contains) || __cpp_lib_ranges_contains < 202207L
        #error ServerCore requires C++23 ranges::contains
        #endif
        #if !defined(__cpp_lib_to_underlying) || __cpp_lib_to_underlying < 202102L
        #error ServerCore requires C++23 std::to_underlying
        #endif

        static_assert(!std::is_copy_constructible_v<std::move_only_function<int()>>);
        enum class Probe : int { Answer = 42 };

        int main()
        {
            std::move_only_function<std::expected<int, int>()> callback =
                [owned = std::make_unique<int>(42)]() -> std::expected<int, int>
                { return *owned; };
            auto moved = std::move(callback);
            auto value = moved();
            std::expected<int, int> failure(std::unexpect, 7);
            std::expected<void, int> completed;
            constexpr std::array numbers{7, 42};
            return value && *value == 42 && !failure && failure.error() == 7 && completed &&
                           std::string_view("answer").contains('a') &&
                           std::ranges::contains(numbers, std::to_underlying(Probe::Answer))
                       ? 0
                       : 1;
        }
    ]=] SERVERCORE_HAS_REQUIRED_CXX23_LIBRARY)
    cmake_pop_check_state()

    if (NOT SERVERCORE_HAS_REQUIRED_CXX23_LIBRARY)
        message(FATAL_ERROR
                "ServerCore requires a C++23 compiler and standard library with std::expected, "
                "std::move_only_function, string contains, ranges::contains, and std::to_underlying. "
                "Select a toolchain that provides all of them; see the CMake configure log for "
                "the capability probe diagnostics.")
    endif ()
endfunction()
