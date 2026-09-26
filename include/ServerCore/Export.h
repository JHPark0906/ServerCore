#pragma once

// CMake propagates SERVERCORE_SHARED to native C++ consumers. The C ABI has
// its own SC_API and does not expose this C++/STL compatibility contract.
#if defined(_WIN32) && defined(SERVERCORE_SHARED)
#if defined(SERVERCORE_BUILDING_LIBRARY)
#define SERVERCORE_API __declspec(dllexport)
#else
#define SERVERCORE_API __declspec(dllimport)
#endif
#elif defined(SERVERCORE_SHARED) && defined(__GNUC__)
#define SERVERCORE_API __attribute__((visibility("default")))
#else
#define SERVERCORE_API
#endif

#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
#define SERVERCORE_TEST_API SERVERCORE_API
#else
#define SERVERCORE_TEST_API
#endif

// An import library cannot carry the static archive's CRT mismatch records.
// Diagnose incompatible native consumers before their STL objects cross a DLL.
#if defined(_MSC_VER) && defined(SERVERCORE_SHARED)
#include <string>
#if !defined(_DLL)
#error ServerCore C++ ABI RuntimeLibrary mismatch: shared consumers require /MD or /MDd.
#endif
#if defined(SERVERCORE_MSVC_DEBUG)
#if SERVERCORE_MSVC_DEBUG
#if !defined(_DEBUG) || _ITERATOR_DEBUG_LEVEL != 2
#error ServerCore C++ ABI mismatch: Debug DLL requires /MDd and _ITERATOR_DEBUG_LEVEL=2.
#endif
#else
#if defined(_DEBUG) || _ITERATOR_DEBUG_LEVEL != 0
#error ServerCore C++ ABI mismatch: Release DLL requires /MD and _ITERATOR_DEBUG_LEVEL=0.
#endif
#endif
#endif
#if defined(SERVERCORE_MSVC_VERSION) && _MSC_VER != SERVERCORE_MSVC_VERSION
#error ServerCore C++ ABI mismatch: use the matching MSVC toolset or rebuild the library.
#endif

// Also enforce the actual imported binary's ABI, including manual consumers and
// CMake's fallback to an installed configuration other than the requested one.
#define SERVERCORE_DETAIL_ABI_NAME_I(compiler, iter, debug)                                        \
    ServerCoreNativeAbi_Msvc##compiler##_Iter##iter##_Debug##debug
#define SERVERCORE_DETAIL_ABI_NAME(compiler, iter, debug)                                          \
    SERVERCORE_DETAIL_ABI_NAME_I(compiler, iter, debug)
#if defined(_DEBUG)
#define SERVERCORE_NATIVE_ABI_SYMBOL SERVERCORE_DETAIL_ABI_NAME(_MSC_VER, _ITERATOR_DEBUG_LEVEL, 1)
#else
#define SERVERCORE_NATIVE_ABI_SYMBOL SERVERCORE_DETAIL_ABI_NAME(_MSC_VER, _ITERATOR_DEBUG_LEVEL, 0)
#endif
#define SERVERCORE_DETAIL_STRING_I(value) #value
#define SERVERCORE_DETAIL_STRING(value) SERVERCORE_DETAIL_STRING_I(value)
#if !defined(SERVERCORE_BUILDING_LIBRARY)
#if defined(_M_IX86)
#pragma comment(linker, "/include:__imp__" SERVERCORE_DETAIL_STRING(SERVERCORE_NATIVE_ABI_SYMBOL))
#else
#pragma comment(linker, "/include:__imp_" SERVERCORE_DETAIL_STRING(SERVERCORE_NATIVE_ABI_SYMBOL))
#endif
#endif
#endif
