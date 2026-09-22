# 의존성 정책

ServerCore는 웹·게임 백엔드의 서버 통신과 실행 기반을 제공하는 라이브러리입니다. 코어, C ABI와 Rust 래퍼는 C++·Rust 표준 라이브러리, 컴파일러 런타임과 운영체제 API만 사용하며 서드파티 라이브러리에 의존하지 않습니다. CMake 같은 빌드 도구는 실행 라이브러리 의존성과 구분합니다.

## 기본 원칙

- 공통 실행·네트워크·직렬화·로그 기능은 표준 라이브러리와 OS 기능을 사용합니다. Rust 래퍼도 실행기나 네트워크 크레이트를 요구하지 않습니다.
- 외부 서비스로 보내는 HTTP 요청은 소비 애플리케이션이 선택한 구현으로 처리합니다. 백엔드에 필요한 외부 호출을 금지하는 것이 아니라 ServerCore의 제공 범위와 애플리케이션의 의존성을 구분합니다.
- 공개 헤더·C ABI에 외부 라이브러리의 타입을 노출하지 않습니다. 비동기 응답 문맥과 취소 기능은 애플리케이션이 선택한 외부 작업과 연결할 수 있습니다.
- 직접·전이 의존성을 함께 관리합니다. 정적 링크, 헤더 전용 라이브러리와 저장소에 복사한 외부 코드도 의존성으로 취급합니다.
- 빌드 중 외부 패키지를 자동 다운로드하지 않습니다. 의존성 추가가 필요한 확장은 이 정책과 서버 계층의 책임을 먼저 검토합니다.
- TLS와 암호화는 의존성을 없애기 위해 직접 구현하지 않습니다. 현재 서버 TLS는 배포 계층에서 종료하며, 안전한 난수는 OS 기능을 사용합니다.

## 현재 구성

| 구성 | 의존성 |
| --- | --- |
| TCP·UDP, HTTP 서버·WebSocket, JSON, 실행·로그·메트릭 | C++20 표준 라이브러리, 컴파일러 런타임, OS API |
| Windows 플랫폼 | Winsock/IOCP의 `ws2_32`, 안전한 난수의 `bcrypt` |
| Linux 플랫폼 | libc·스레드, POSIX 소켓·epoll·getrandom |
| C ABI와 Rust | 네이티브 ServerCore와 위 런타임. Rust는 저장소 내부 두 크레이트만 사용 |
| 검사 | 저장소의 C++ 검사 하네스와 Rust 기본 검사 도구 |

정적 ServerCore나 C ABI를 링크해도 최종 실행 파일 전체가 정적으로 링크되는 것은 아닙니다. 플랫폼·툴체인에 맞는 C++·OS 런타임을 사용해야 합니다. 공유 C ABI에는 코어가 포함되며 소비자가 C++ 헤더를 설치할 필요는 없습니다.

## 빌드와 검증

```sh
cmake -S . -B build/minimal -DSERVERCORE_BUILD_C_API=ON
cmake --build build/minimal --config Release
```

Linux의 단일 구성 생성기로 Release를 빌드할 때는 구성 명령에 `-DCMAKE_BUILD_TYPE=Release`를 추가합니다. 선택적 C ABI 빌드 여부와 관계없이 외부 패키지를 설치하지 않습니다.

Rust 소스 소비는 `rust/`에서 `cargo test --workspace --all-targets --locked --offline`으로 검사합니다. `SERVERCORE_CABI_DIR`가 설정되어 있으면 새 네이티브 빌드 대신 그 경로의 바이너리를 사용합니다. CI도 네이티브 빌드와 C/C++ 설치 소비, Rust 소스 소비를 검사합니다. 실제 실행 결과와 원격 CI 실행 여부는 [VALIDATION.md](VALIDATION.md)에 구분하여 기록합니다.

## 이전 HTTP 클라이언트에서의 이전

libcurl 기반 HTTP 클라이언트와 관련 C++·C·Rust API, Cargo feature, CMake 타깃을 제거했습니다. 이전 클라이언트 포함 바이너리는 그 의존성을 그대로 가지므로 새 서버 전용 바이너리로 교체합니다. 제거한 API를 사용하는 애플리케이션은 외부 요청 구현을 별도로 선택하고 [API_MIGRATION.md](API_MIGRATION.md)에 따라 이전합니다. HTTP 서버·WebSocket·TCP/UDP 및 요청·응답 스트리밍은 유지합니다.
