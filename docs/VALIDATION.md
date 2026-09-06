# 검증 결과

2026-09-07에 공통 UDP 전송을 ServerCore로 옮긴 소스를 독립된 빌드 디렉터리에서 검증했습니다. 기존 공개 소스에 `Runtime::DatagramTransport`, 내부 Net 소켓과 회귀 검사를 추가한 입력이며, 게임·엔진 소스나 에셋을 요구하지 않습니다.

## 환경

| 항목 | 확인한 값 |
| --- | --- |
| 플랫폼 | Windows x64 |
| 컴파일러 | MSVC 19.50.35728.0 |
| Windows SDK | 10.0.26100.0 |
| CMake | 4.2.3-msvc3 |
| 생성기 | Visual Studio 18 2026, x64 |
| CRT | Debug `/MDd`, Release `/MD` |

라이브러리의 실제 생성된 컴파일 설정에서 `/W4 /WX`와 구성별 CRT를 확인했으며 두 구성의 빌드 경고는 0개입니다. Windows 시스템 라이브러리 `ws2_32`와 `bcrypt`를 링크하며 제3자 패키지 설치 단계는 없습니다.

## 빌드와 회귀 테스트

`cmake -S . -B build/vs -G "Visual Studio 18 2026" -A x64 -DSERVERCORE_BUILD_TESTS=ON`으로 구성했습니다. Debug·Release의 전체 빌드가 성공한 뒤 `ctest --test-dir build/vs -C <구성> --output-on-failure --no-tests=error`로 각 구성을 순서대로 검사하고 JUnit 결과를 확인했습니다.

| 구성 | 전체 빌드 | CTest | CTest 실행 시간 |
| --- | --- | --- | --- |
| Debug | 통과, 경고 0개 | 125/125 | 130.99초 |
| Release | 통과, 경고 0개 | 125/125 | 129.88초 |

총 **250/250 통과**이며 실패하거나 건너뛴 검사는 없습니다. 기존 115개에 다음 범위의 UDP 검사 10개를 추가했습니다.

- 임시 포트·배타적 바인딩, 반복 종료·재바인딩과 독립적인 Winsock 수명입니다.
- 세션 토큰 등록·폐기·재등록, 일반 JSON 봉투 왕복과 실제 송신 성공의 순번·바이트 지표입니다.
- 알 수 없는 토큰·재전송·손상 JSON·허용되지 않은 고순번 메시지의 거절과 정상 경로 회복입니다.
- endpoint 재바인딩, 콜백의 송신·등록 해제·종료 재진입, admission 중 등록 교체와 예외 경계입니다.
- 빈 datagram·잘린 패킷·초과 길이 입력과 수신 시도·바이트 예산입니다.

`ServerCore.CMake.InstallAndSourceTreeConsume`은 새 공개 API를 포함하는 별도 소비 프로젝트를 소스의 `add_subdirectory`와 이동한 설치 패키지의 `find_package`로 빌드·실행했습니다. CRT 불일치와 잘못된 CMP0091 부모 정책의 거절 검사도 포함합니다. 공개 전송 헤더는 새 UDP 검사와 패키지 소비 코드에서 첫 번째 헤더로 포함해 독립적인 선언 컴파일을 확인했습니다.

SummitServer도 같은 ServerCore 소스로 Debug·Release를 빌드하고 기존 8개 검사를 각각 통과했습니다. schema 6, UDP Hello/Ready·이동과 기존 TCP, 토큰 폐기·손상 패킷 거절·주소 재바인딩의 통신 계약을 유지했습니다.

## 결과의 범위

위 시간은 회귀 검사의 소요 시간이며 처리량이나 네트워크 지연 벤치마크가 아닙니다. 이번 변경에서는 별도의 부하 성능 실험을 실행하지 않았습니다. 소켓 버퍼 포화, CSPRNG 실패와 메모리 할당 실패를 강제로 주입한 검사는 포함하지 않습니다.

명령과 테스트 구분은 [빌드·테스트·배포](BUILD_TEST_DEPLOY.md), 지원 범위는 [지원 범위와 설정](SUPPORT_AND_LIMITS.md)을 참고합니다.
