# ServerCore.Architecture.PublicHeadersCompile의 본체다.
#
# tests/CMakeLists.txt는 공개 헤더마다 그 헤더 하나만 포함하는 번역 단위를 만들어 빌드한다. 빌드가
# 통과했다는 것만으로는 그 번역 단위들이 헤더마다 실제로 컴파일되었다고 말할 수 없다. 목록이 비었거나
# 새 헤더가 구성에 반영되지 않았어도 빌드는 초록이기 때문이다. 그래서 헤더 목록을 구성 시점의 목록이
# 아니라 시험 시점의 소스 트리에서 다시 읽고, 헤더마다 그 번역 단위의 목적 파일이 있는지 본다.
#
# 입력:
#   SERVERCORE_HEADERS_SOURCE_DIR  저장소 루트
#   SERVERCORE_HEADERS_OBJECTS     빌드가 만든 목적 파일 목록(file(GENERATE)로 구성마다 하나)
#   SERVERCORE_HEADERS_C_API       C ABI 헤더를 C로도 컴파일했는지
cmake_minimum_required(VERSION 3.21)

foreach (variable IN ITEMS SERVERCORE_HEADERS_SOURCE_DIR SERVERCORE_HEADERS_OBJECTS
        SERVERCORE_HEADERS_C_API)
    if (NOT DEFINED ${variable})
        message(FATAL_ERROR "${variable} is not set.")
    endif ()
endforeach ()
if (NOT EXISTS "${SERVERCORE_HEADERS_OBJECTS}")
    message(FATAL_ERROR "Object list ${SERVERCORE_HEADERS_OBJECTS} does not exist.")
endif ()
include("${SERVERCORE_HEADERS_OBJECTS}")

file(GLOB_RECURSE headers RELATIVE "${SERVERCORE_HEADERS_SOURCE_DIR}/include"
        "${SERVERCORE_HEADERS_SOURCE_DIR}/include/ServerCore/*.h")
list(SORT headers)

# 헤더 수와 목적 파일 수가 같고, 헤더마다 이름이 "<헤더 경로의 /를 .으로 바꾼 것>."으로 시작하는
# 목적 파일이 실제로 있는지 본다. 생성기마다 목적 파일 이름의 꼬리(.cpp.obj, .obj, .c.o)가 달라서
# 앞부분만 맞춘다.
function(ServerCoreCheckCompiledHeaders kind headers objects)
    list(LENGTH headers headerCount)
    list(LENGTH objects objectCount)
    if (headerCount EQUAL 0)
        message(FATAL_ERROR "${kind}: no public headers to check.")
    endif ()
    if (NOT objectCount EQUAL headerCount)
        message(FATAL_ERROR
                "${kind}: ${headerCount} public headers, ${objectCount} compiled translation units.")
    endif ()
    set(names)
    foreach (object IN LISTS objects)
        if (NOT EXISTS "${object}")
            message(FATAL_ERROR "${kind}: object file ${object} does not exist.")
        endif ()
        get_filename_component(name "${object}" NAME)
        list(APPEND names "${name}")
    endforeach ()
    foreach (header IN LISTS headers)
        string(REPLACE "/" "." stem "${header}")
        set(found FALSE)
        foreach (name IN LISTS names)
            string(FIND "${name}" "${stem}." position)
            if (position EQUAL 0)
                set(found TRUE)
                break()
            endif ()
        endforeach ()
        if (NOT found)
            message(FATAL_ERROR "${kind}: no compiled translation unit for ${header}.")
        endif ()
    endforeach ()
    message(STATUS "${kind}: ${headerCount} public headers, each compiled alone.")
endfunction()

ServerCoreCheckCompiledHeaders("C++" "${headers}" "${SERVERCORE_HEADERS_CXX_OBJECTS}")
if (SERVERCORE_HEADERS_C_API)
    set(cHeaders ${headers})
    list(FILTER cHeaders INCLUDE REGEX "^ServerCore/C/")
    ServerCoreCheckCompiledHeaders("C" "${cHeaders}" "${SERVERCORE_HEADERS_C_OBJECTS}")
endif ()

# src/PublicHeaderCompileCheck.cpp는 공개 헤더 전부를 한 번역 단위에 함께 넣는다고 적고, 새 헤더를
# 더하면 그 파일에도 한 줄 더하라고 적는다. 빠진 헤더가 있으면 여기서 그 이름을 적어 실패한다.
set(togetherSource "${SERVERCORE_HEADERS_SOURCE_DIR}/src/PublicHeaderCompileCheck.cpp")
file(STRINGS "${togetherSource}" includeLines REGEX "^#include \"ServerCore/")
set(included)
foreach (line IN LISTS includeLines)
    if (line MATCHES "^#include \"([^\"]+)\"")
        list(APPEND included "${CMAKE_MATCH_1}")
    endif ()
endforeach ()
set(missing)
foreach (header IN LISTS headers)
    if (NOT header IN_LIST included)
        list(APPEND missing "${header}")
    endif ()
endforeach ()
if (missing)
    list(JOIN missing ", " missingText)
    message(FATAL_ERROR "src/PublicHeaderCompileCheck.cpp does not include: ${missingText}")
endif ()
