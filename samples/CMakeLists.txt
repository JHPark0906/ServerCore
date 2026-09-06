# 샘플 실행 파일들. 라이브러리가 시험 밖에서도 실제로 호출되는 자리다.
add_executable(HelloServerCore HelloServerCore/main.cpp)
target_link_libraries(HelloServerCore PRIVATE ServerCore::ServerCore)

if (MSVC)
    # 샘플도 설치 패키지 소비자와 같은 /MD[d] 계약으로 ServerCore를 링크한다.
    set_property(TARGET HelloServerCore PROPERTY
            MSVC_RUNTIME_LIBRARY "${ServerCoreMsvcRuntimeLibrary}")
    target_compile_options(HelloServerCore PRIVATE /W4 /permissive- /utf-8 /EHsc)
endif ()
