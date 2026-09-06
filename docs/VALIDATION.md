# 검증 결과

2026-09-06에 다른 게임·엔진 저장소, 기존 빌드 트리와 Git 이력을 포함하지 않은 소스 묶음에서 확인했다. `include/`, `src/`, `tests/`, `samples/`, CMake 파일과 검증 스크립트만으로 구성·빌드·테스트할 수 있다.

## 환경

| 항목 | 확인한 값 |
| --- | --- |
| 운영체제 | Windows 11 x64 |
| 컴파일러 | MSVC 19.50.35728.0, Visual Studio 2026 |
| Windows SDK | 10.0.26100.0 |
| CMake | 4.1.2 |
| 생성기 | Ninja 1.13.2 |
| 구성 | x64 Debug, x64 Release |
| CRT | Debug `/MDd`, Release `/MD` |

CMake 최소 요구 버전은 3.21이며, 위 표는 이번에 실제 실행한 도구 버전이다. Windows SDK 외에 내려받거나 설치할 제3자 코드 라이브러리는 없다.

## 빌드와 회귀 테스트

공개 소스의 `scripts/VerifyBuild.ps1 -Configuration All`을 Windows PowerShell 5.1에서 실행했다. 각 구성의 전체 빌드가 성공한 뒤 해당 CTest를 실행했다.

| 구성 | 전체 빌드 | CTest | CTest 실행 시간 |
| --- | --- | --- | --- |
| Debug | 통과 | 115/115 | 137.83초 |
| Release | 통과 | 115/115 | 139.23초 |

총 **230/230 통과**, 실패하거나 건너뛴 단계는 없다. 기반 자료형·설정·JSON·프레이밍·세션·디스패치·실행기뿐 아니라 실제 loopback TCP 통신, 종료와 예산 회수 경로도 포함한다.

`ServerCore.CMake.InstallAndSourceTreeConsume`은 별도 소비 프로젝트를 구성·빌드·실행하여 다음을 확인한다.

- 소스의 `add_subdirectory`와 설치 패키지의 `find_package`가 같은 `ServerCore::ServerCore` 타깃을 제공한다.
- 위치를 옮긴 설치 패키지만으로 소비자를 빌드한다.
- MSVC CRT가 다른 소비자를 예상한 링크 진단으로 거절한다.
- CMP0091 정책이 맞지 않는 부모 프로젝트를 명확한 구성 오류로 거절한다.

설치 결과에는 `share/doc/ServerCore/LICENSE`의 MIT-0 원문이 포함된 것도 확인했다.

## 예제 실행

- `HelloServerCore`를 Debug·Release에서 각각 실행하여 `ServerCore 0.1.0` 출력과 정상 종료를 확인했다.
- README의 Echo 서버 C++ 예제를 수정 없이 추출하고, Release 설치 패키지를 사용하여 `/W4 /WX`로 컴파일했다.
- `127.0.0.1:17891`에 길이 머리를 붙인 Echo 요청을 보내 `EchoReply`의 본문과 `seq`가 일치하는지 확인했다. Enter 입력 후 프로세스는 종료 코드 0으로 정리됐다.

## 결과의 범위

위 실행 시간은 회귀 테스트의 소요 시간이며 서버 처리량이나 지연 벤치마크가 아니다. 설정상 연결 상한과 테스트 통과만으로 실제 게임의 동시 접속 수를 보장하지 않는다. 성능 검증에는 소비 서버의 메시지 크기·빈도·방송 범위·처리기 비용을 반영한 별도 부하가 필요하다.

명령과 테스트 구분은 [빌드·테스트·배포](BUILD_TEST_DEPLOY.md), 지원하지 않는 기능과 설정 상한은 [지원 범위와 설정](SUPPORT_AND_LIMITS.md)에 있다.
