# 설치되는 ServerCore 바이너리의 심볼을 도구(dumpbin 또는 nm)로 직접 읽어 검사한다.
#
# SERVERCORE_SYMBOLS_MODE=hooks (BUILD-1): 시험 전용 hook(TestAccess 함수들)은
# ServerCoreTestHooks에만 들어가고 설치 결과물에는 없어야 한다. "hook이 없다"는 판정이 도구가
# 아무것도 못 읽어서 나온 초록이 아니도록, 먼저 hook 빌드에서 TestAccess 심볼이 실제로 보이는지
# 확인한 뒤에 설치 결과물을 본다.
#
# SERVERCORE_SYMBOLS_MODE=exports (BUILD-2): 공유 라이브러리가 내보내는 집합을 본다.
#   - C ABI: 내보낸 sc_* 집합이 include/ServerCore/C 헤더의 SC_API 선언 집합과 정확히 같다.
#   - C++: 강한 심볼은 모두 ServerCore 이름공간에 속한다. ELF의 약한 심볼(템플릿·인라인 인스턴스,
#     주로 std::)은 링커가 합치는 vague linkage라 규칙에서 뺀다.
#   - 어느 쪽에도 TestAccess가 없다.
cmake_minimum_required(VERSION 3.21)

foreach (requiredVariable IN ITEMS SERVERCORE_SYMBOLS_MODE SERVERCORE_SYMBOLS_TOOL_KIND
        SERVERCORE_SYMBOLS_SHARED)
    if (NOT DEFINED ${requiredVariable} OR "${${requiredVariable}}" STREQUAL "")
        message(FATAL_ERROR "${requiredVariable} was not provided to the library symbol test.")
    endif ()
endforeach ()
if (NOT DEFINED SERVERCORE_SYMBOLS_TOOL OR "${SERVERCORE_SYMBOLS_TOOL}" STREQUAL "" OR
        NOT EXISTS "${SERVERCORE_SYMBOLS_TOOL}")
    message(FATAL_ERROR "The symbol listing tool was not found: '${SERVERCORE_SYMBOLS_TOOL}'. "
            "Configure with a toolchain that provides nm or dumpbin.")
endif ()

# 정적 라이브러리는 아카이브가 정의하는 공개 심볼을, 공유 라이브러리는 동적 export 표를 읽는다.
# demangle이 참이면 nm이 C++ 이름을 풀어 준다(dumpbin /exports는 장식된 이름 그대로다).
function(ListSymbols output file demangle)
    if (SERVERCORE_SYMBOLS_TOOL_KIND STREQUAL "dumpbin")
        if (SERVERCORE_SYMBOLS_SHARED)
            set(arguments /nologo /exports)
        else ()
            set(arguments /nologo /linkermember:1)
        endif ()
    else ()
        if (SERVERCORE_SYMBOLS_SHARED)
            set(arguments -D --defined-only)
        else ()
            set(arguments -g --defined-only)
        endif ()
        if (demangle)
            list(APPEND arguments -C)
        endif ()
    endif ()
    execute_process(COMMAND "${SERVERCORE_SYMBOLS_TOOL}" ${arguments} "${file}"
            RESULT_VARIABLE listResult OUTPUT_VARIABLE listOutput ERROR_VARIABLE listError)
    if (NOT "${listResult}" STREQUAL "0")
        message(FATAL_ERROR "Listing symbols of ${file} failed (${listResult}).\n${listError}")
    endif ()
    set(${output} "${listOutput}" PARENT_SCOPE)
endfunction()

function(RequireNoTestHooks symbols file)
    string(REGEX MATCHALL "[^\r\n]*TestAccess[^\r\n]*" leakedHooks "${symbols}")
    if (leakedHooks)
        list(JOIN leakedHooks "\n" leakedHooksText)
        message(FATAL_ERROR "${file} contains test hooks:\n${leakedHooksText}")
    endif ()
endfunction()

if (SERVERCORE_SYMBOLS_MODE STREQUAL "hooks")
    foreach (requiredVariable IN ITEMS SERVERCORE_SYMBOLS_BINARY_DIR SERVERCORE_SYMBOLS_TEST_ROOT
            SERVERCORE_SYMBOLS_CONFIGURATION SERVERCORE_SYMBOLS_HOOKS_FILE
            SERVERCORE_SYMBOLS_INSTALLED_RELATIVE)
        if (NOT DEFINED ${requiredVariable} OR "${${requiredVariable}}" STREQUAL "")
            message(FATAL_ERROR "${requiredVariable} was not provided to the hook symbol test.")
        endif ()
    endforeach ()

    # 지울 수 있는 것은 이 시험의 전용 디렉터리뿐이다. 경로 정규화 뒤에도 build 디렉터리의 자식인지
    # 확인하고 나서만 재귀 삭제한다.
    cmake_path(ABSOLUTE_PATH SERVERCORE_SYMBOLS_BINARY_DIR NORMALIZE
            OUTPUT_VARIABLE symbolsBinaryDirectory)
    cmake_path(ABSOLUTE_PATH SERVERCORE_SYMBOLS_TEST_ROOT NORMALIZE OUTPUT_VARIABLE symbolsTestRoot)
    cmake_path(IS_PREFIX symbolsBinaryDirectory "${symbolsTestRoot}" NORMALIZE symbolsRootInsideBuild)
    if (NOT symbolsRootInsideBuild OR symbolsBinaryDirectory STREQUAL symbolsTestRoot)
        message(FATAL_ERROR "The symbol test root must be a child of the build directory: ${symbolsTestRoot}")
    endif ()
    file(REMOVE_RECURSE "${symbolsTestRoot}")
    file(MAKE_DIRECTORY "${symbolsTestRoot}")

    ListSymbols(hookSymbols "${SERVERCORE_SYMBOLS_HOOKS_FILE}" FALSE)
    string(FIND "${hookSymbols}" "TestAccess" hookPosition)
    if (hookPosition EQUAL -1)
        message(FATAL_ERROR "The hook build lists no TestAccess symbol: ${SERVERCORE_SYMBOLS_HOOKS_FILE}")
    endif ()

    set(installPrefix "${symbolsTestRoot}/prefix")
    execute_process(COMMAND "${CMAKE_COMMAND}" --install "${symbolsBinaryDirectory}"
            --config "${SERVERCORE_SYMBOLS_CONFIGURATION}" --prefix "${installPrefix}"
            RESULT_VARIABLE installResult OUTPUT_VARIABLE installOutput ERROR_VARIABLE installError)
    if (NOT "${installResult}" STREQUAL "0")
        message(FATAL_ERROR "Installing ServerCore failed (${installResult}).\n${installOutput}\n${installError}")
    endif ()

    set(installedLibrary "${installPrefix}/${SERVERCORE_SYMBOLS_INSTALLED_RELATIVE}")
    if (NOT EXISTS "${installedLibrary}")
        message(FATAL_ERROR "The installed library is missing: ${installedLibrary}")
    endif ()
    ListSymbols(installedSymbols "${installedLibrary}" FALSE)
    string(FIND "${installedSymbols}" "ServerCore" productPosition)
    if (productPosition EQUAL -1)
        message(FATAL_ERROR "The installed library lists no ServerCore symbol: ${installedLibrary}")
    endif ()
    RequireNoTestHooks("${installedSymbols}" "${installedLibrary}")
    message(STATUS "Installed ${installedLibrary} lists no TestAccess symbol; the hook build does.")
    return()
endif ()

if (NOT SERVERCORE_SYMBOLS_MODE STREQUAL "exports")
    message(FATAL_ERROR "Unknown SERVERCORE_SYMBOLS_MODE: ${SERVERCORE_SYMBOLS_MODE}")
endif ()
foreach (requiredVariable IN ITEMS SERVERCORE_SYMBOLS_PRODUCT_FILE SERVERCORE_SYMBOLS_C_API
        SERVERCORE_SYMBOLS_C_HEADER_DIR)
    if (NOT DEFINED ${requiredVariable} OR "${${requiredVariable}}" STREQUAL "")
        message(FATAL_ERROR "${requiredVariable} was not provided to the export set test.")
    endif ()
endforeach ()
if (NOT SERVERCORE_SYMBOLS_SHARED)
    message(FATAL_ERROR "The export set test applies only to shared builds.")
endif ()

# 헤더가 약속하는 C ABI 집합: SC_API 뒤 첫 '(' 바로 앞의 식별자 중 sc_로 시작하는 것.
set(declaredC "")
if (SERVERCORE_SYMBOLS_C_API)
    file(GLOB cHeaders "${SERVERCORE_SYMBOLS_C_HEADER_DIR}/*.h")
    foreach (header IN LISTS cHeaders)
        file(READ "${header}" headerText)
        string(REGEX MATCHALL "SC_API[^;(]*\\(" declarations "${headerText}")
        foreach (declaration IN LISTS declarations)
            # 두 번째 MATCHES가 CMAKE_MATCH_1을 덮으므로 이름을 먼저 옮겨 둔다.
            if (declaration MATCHES "([A-Za-z_][A-Za-z0-9_]*)[ \t\r\n]*\\($")
                set(declaredName "${CMAKE_MATCH_1}")
                if (declaredName MATCHES "^sc_")
                    list(APPEND declaredC "${declaredName}")
                endif ()
            endif ()
        endforeach ()
    endforeach ()
    list(REMOVE_DUPLICATES declaredC)
    if (NOT declaredC)
        message(FATAL_ERROR "No SC_API declaration was found under ${SERVERCORE_SYMBOLS_C_HEADER_DIR}.")
    endif ()
endif ()

ListSymbols(exportedSymbols "${SERVERCORE_SYMBOLS_PRODUCT_FILE}" TRUE)
RequireNoTestHooks("${exportedSymbols}" "${SERVERCORE_SYMBOLS_PRODUCT_FILE}")
string(REPLACE "\r" "" exportedSymbols "${exportedSymbols}")
# 목록으로 나누기 전에 ; 와 대괄호를 치운다. CMake 목록은 대괄호 안의 ; 를 구분자로 보지 않는다.
# 이름공간 규칙과 sc_ 이름은 이 문자들과 무관하다.
string(REPLACE ";" "," exportedSymbols "${exportedSymbols}")
string(REPLACE "[" "(" exportedSymbols "${exportedSymbols}")
string(REPLACE "]" ")" exportedSymbols "${exportedSymbols}")
string(REPLACE "\n" ";" exportedLines "${exportedSymbols}")

set(exportedC "")
set(unexpected "")
set(cxxCount 0)
foreach (line IN LISTS exportedLines)
    set(kind "")
    set(name "")
    if (SERVERCORE_SYMBOLS_TOOL_KIND STREQUAL "dumpbin")
        # "    ordinal hint RVA      name"
        if (line MATCHES "^[ \t]+[0-9]+[ \t]+[0-9A-Fa-f]+[ \t]+[0-9A-Fa-f]+[ \t]+([^ \t]+)")
            set(kind "T")
            set(name "${CMAKE_MATCH_1}")
        endif ()
    elseif (line MATCHES "^[0-9A-Fa-f]+ ([A-Za-z]) (.+)$")
        set(kind "${CMAKE_MATCH_1}")
        set(name "${CMAKE_MATCH_2}")
    endif ()
    if (name STREQUAL "")
        continue()
    endif ()
    if (name MATCHES "^sc_[A-Za-z0-9_]+$")
        list(APPEND exportedC "${name}")
    elseif (SERVERCORE_SYMBOLS_TOOL_KIND STREQUAL "dumpbin")
        # 장식된 이름에서 바깥 이름공간 ServerCore는 "ServerCore@@"로 끝난다(역참조면 "0ServerCore@@").
        if (name MATCHES "ServerCore@@" OR name MATCHES "^ServerCoreNativeAbi_")
            math(EXPR cxxCount "${cxxCount} + 1")
        else ()
            list(APPEND unexpected "${name}")
        endif ()
    elseif (kind MATCHES "^[WwVvu]$")
        # 약한 심볼: 템플릿·인라인 인스턴스. 링커가 DSO 사이에서 합치므로 이름공간 규칙을 두지 않는다.
    elseif (name MATCHES "^(vtable for |typeinfo for |typeinfo name for |VTT for |guard variable for |non-virtual thunk to |virtual thunk to )?ServerCore::")
        math(EXPR cxxCount "${cxxCount} + 1")
    elseif (name MATCHES "^(_init|_fini|_edata|_end|__bss_start)$")
    else ()
        list(APPEND unexpected "${kind} ${name}")
    endif ()
endforeach ()

if (unexpected)
    list(JOIN unexpected "\n" unexpectedText)
    message(FATAL_ERROR "${SERVERCORE_SYMBOLS_PRODUCT_FILE} exports symbols outside ServerCore and the C ABI:\n${unexpectedText}")
endif ()
if (cxxCount EQUAL 0)
    message(FATAL_ERROR "${SERVERCORE_SYMBOLS_PRODUCT_FILE} exports no ServerCore C++ symbol.")
endif ()

list(REMOVE_DUPLICATES exportedC)
set(missingC ${declaredC})
if (exportedC)
    list(REMOVE_ITEM missingC ${exportedC})
endif ()
set(extraC ${exportedC})
if (declaredC)
    list(REMOVE_ITEM extraC ${declaredC})
endif ()
if (missingC OR extraC)
    list(JOIN missingC "\n  " missingText)
    list(JOIN extraC "\n  " extraText)
    message(FATAL_ERROR "The exported C ABI differs from the SC_API declarations.\n"
            "Declared but not exported:\n  ${missingText}\nExported but not declared:\n  ${extraText}")
endif ()
list(LENGTH exportedC exportedCCount)
message(STATUS "${SERVERCORE_SYMBOLS_PRODUCT_FILE}: ${exportedCCount} C ABI exports match the headers; "
        "${cxxCount} strong C++ exports are in namespace ServerCore.")
