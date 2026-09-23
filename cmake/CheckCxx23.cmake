include(CheckCXXSourceCompiles)
include(CMakePushCheckState)

function(ServerCoreCheckCxx23Support)
    cmake_push_check_state(RESET)
    check_cxx_source_compiles([=[
        #include <expected>
        #include <functional>
        #include <memory>
        #include <type_traits>
        #include <utility>
        #include <version>

        #if !defined(__cpp_lib_expected) || __cpp_lib_expected < 202202L
        #error ServerCore requires C++23 std::expected
        #endif
        #if !defined(__cpp_lib_move_only_function) || __cpp_lib_move_only_function < 202110L
        #error ServerCore requires C++23 std::move_only_function
        #endif

        static_assert(!std::is_copy_constructible_v<std::move_only_function<int()>>);

        int main()
        {
            std::move_only_function<std::expected<int, int>()> callback =
                [owned = std::make_unique<int>(42)]() -> std::expected<int, int>
                { return *owned; };
            auto moved = std::move(callback);
            auto value = moved();
            std::expected<int, int> failure(std::unexpect, 7);
            std::expected<void, int> completed;
            return value && *value == 42 && !failure && failure.error() == 7 && completed ? 0 : 1;
        }
    ]=] SERVERCORE_HAS_REQUIRED_CXX23_LIBRARY)
    cmake_pop_check_state()

    if (NOT SERVERCORE_HAS_REQUIRED_CXX23_LIBRARY)
        message(FATAL_ERROR
                "ServerCore requires a C++23 compiler and standard library with std::expected "
                "(__cpp_lib_expected >= 202202L) and std::move_only_function "
                "(__cpp_lib_move_only_function >= 202110L). Select a toolchain that provides "
                "both; see the CMake configure log for the capability probe diagnostics.")
    endif ()
endfunction()
