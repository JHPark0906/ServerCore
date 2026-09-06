#pragma once

#include "ServerCore/Core/Error.h"
#include "ServerCore/Protocol/FrameCodec.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <utility>
#include <vector>

/// <summary>
/// 프로토콜 층. 바이트 흐름을 메시지로 자르고 되돌리는 일만 한다.
/// 소켓도 세션도 모르므로 이 층은 바이트 배열만으로 전부 시험할 수 있다.
/// </summary>
namespace ServerCore::Protocol
{
/// <summary>
/// 프레임 머리의 크기다. 본문 길이를 담는 uint32 하나뿐이다.
/// </summary>
inline constexpr std::size_t HeaderSize = FrameCodec::HeaderSize;

/// <summary>
/// 본문 크기의 기본 상한이다. 설정으로 바꿀 수 있다.
/// </summary>
/// <remarks>
/// 상한이 규격의 일부인 이유:
/// 길이 필드만 믿으면 악의적인 길이 하나로 서버의 메모리를 터뜨릴 수 있다. 상한이 있으면
/// 본문을 받기 전에, 머리 4바이트만 읽고 끊을 수 있다.
/// </remarks>
inline constexpr std::uint32_t DefaultMaxBodySize = FrameCodec::DefaultMaxBodySize;

/// <summary>
/// 도착한 바이트를 모아 두었다가 완결된 프레임 본문을 하나씩 잘라 준다.
/// </summary>
/// <remarks>
/// 와이어 규격:
/// [uint32 본문 길이(리틀 엔디언)][UTF-8 JSON 본문]
/// 길이 필드는 본문의 바이트 수이며 머리 4바이트를 포함하지 않는다. 서버에서 클라이언트로
/// 가는 방향도 같은 형식이다.
///
/// 호출 계약:
/// - 연결 하나당 하나씩 둔다.
/// - 스레드 안전성: 단일 스레드 전용.
/// - Append로 넣고 NextFrame() 또는 TakeNextFrame()이 WouldBlock을 줄 때까지 반복해서 꺼낸다.
///   한 번의 Append가 여러 프레임을 완성시킬 수 있기 때문이다.
///
/// 약속하지 않는 것:
/// - NextFrame이 돌려주는 뷰는 다음 Append, NextFrame, TakeNextFrame 호출 전까지만 유효하다.
/// - 본문이 올바른 JSON인지 보지 않는다. 자르기만 한다. 파싱은 ParseMessage의 일이다.
/// - 프레이밍이 한 번 어긋나면 되돌릴 방법이 없다. 그래서 형식 오류에 대한 유일한 대응은
///   그 연결을 끊는 것이다.
/// </remarks>
class FrameReader
{
public:
    /// <summary>완결 본문을 내부 큐에 보관하기 전에 호출하는 비할당 admission 함수다.</summary>
    /// <remarks>
    /// true면 본문 복사를 계속하고, false면 그 본문과 같은 Append 안의 뒤 본문을 보관하지
    /// 않는다. callback은 예외를 던지거나 FrameReader에 재진입해서는 안 된다. 연결별·Host 전체
    /// byte/task 예산처럼, 복사 전에 결정해야 실제 메모리 상한이 되는 정책에 쓴다.
    /// </remarks>
    using CompletedFrameAdmission = bool (*)(void* context, std::size_t bodySize) noexcept;

    /// <summary>본문 상한을 정해서 만든다.</summary>
    explicit FrameReader(std::uint32_t maxBodySize = DefaultMaxBodySize);

    /// <summary>
    /// 내부 FrameCodec이 이 객체의 저장소를 span으로 가리키므로 복사·이동을 막는다.
    /// </summary>
    /// <remarks>
    /// 연결 하나에 하나씩 두는 단일 스레드 상태다. 값을 옮겨야 하는 이점보다, 옮긴 뒤 span을
    /// 새 저장소에 다시 묶지 않아 생길 수 있는 수명 오류를 구조적으로 막는 이점이 크다.
    /// </remarks>
    FrameReader(const FrameReader&) = delete;
    FrameReader& operator=(const FrameReader&) = delete;
    FrameReader(FrameReader&&) = delete;
    FrameReader& operator=(FrameReader&&) = delete;

    /// <summary>도착한 바이트를 뒤에 붙인다.</summary>
    Core::Status Append(std::span<const std::byte> bytes);

    /// <summary>완결 본문마다 admission을 거쳐 도착한 바이트를 뒤에 붙인다.</summary>
    /// <remarks>
    /// admission이 false면 ErrorCode::TooLarge를 돌린다. 이미 허용되어 내부 완료 큐에 들어간
    /// 본문은 남으므로, 호출자는 실패 시 자신의 reservation을 그 본문 수와 맞춰 반납하거나
    /// DiscardCompletedFrames()로 비워야 한다. allocation 실패와 달리 거부된 본문은 복사하지
    /// 않으므로, 한 Append에 많은 frame이 들어와도 admission 정책의 메모리 상한을 넘지 않는다.
    /// 거부는 이 Reader를 terminal 상태로 만든다. 그 뒤 Append와 Finish는 TooLarge를 돌리며,
    /// 남아 있는 완료 본문을 처리할지 버릴지는 호출자의 종료 정책이 정한다.
    /// </remarks>
    Core::Status Append(std::span<const std::byte> bytes, void* admissionContext,
        CompletedFrameAdmission admission);

    /// <summary>완결된 프레임의 본문 하나를 소유 벡터로 꺼낸다.</summary>
    /// <remarks>
    /// 완료 큐가 이미 소유한 vector를 결과로 옮기므로, 본문 바이트를 한 번 더 복사하지 않는다.
    /// 반환 vector는 이 FrameReader가 다음 Append(), NextFrame(), TakeNextFrame(),
    /// DiscardCompletedFrames()를 불러도 유효하다. 비동기 작업으로 본문을 넘겨야 하면 span을
    /// 주는 NextFrame() 대신 이 함수를 쓴다.
    /// </remarks>
    /// <returns>
    /// 성공이면 본문을 소유한 vector. 아직 다 안 왔으면 ErrorCode::WouldBlock이며 이것은 실패가
    /// 아니다.
    /// </returns>
    Core::Result<std::vector<std::byte>> TakeNextFrame();

    /// <summary>완결된 프레임의 본문을 하나 꺼낸다.</summary>
    /// <returns>
    /// 성공이면 본문 뷰. 아직 다 안 왔으면 ErrorCode::WouldBlock이며 이것은 실패가 아니다.
    /// 길이가 상한을 넘으면 ErrorCode::TooLarge이고, 이때 호출자는 연결을 끊어야 한다.
    /// </returns>
    /// <remarks>
    /// 이 기존 편의 API의 반환 span은 다음 Append(), NextFrame(), TakeNextFrame() 호출 전까지만
    /// 유효하다. 본문을 현재 호출보다 오래 보관해야 하면 TakeNextFrame()을 쓴다.
    /// </remarks>
    Core::Result<std::span<const std::byte>> NextFrame();

    /// <summary>아직 꺼내지 않은 완결 본문의 수를 준다.</summary>
    /// <remarks>
    /// 반환값은 completed queue의 순간값이다. NextFrame() 또는 TakeNextFrame()으로 꺼낸 현재
    /// 본문과, 아직 완결되지 않은 FrameCodec 저장소는 세지 않는다.
    /// </remarks>
    [[nodiscard]] std::size_t CompletedFrameCount() const noexcept;

    /// <summary>아직 꺼내지 않은 완결 본문이 차지하는 바이트 합계를 준다.</summary>
    /// <remarks>
    /// 반환값은 completed queue 안 vector들의 size 합계다. FrameReader의 고정 저장소, 현재
    /// NextFrame() span의 본문, allocator의 부가 메모리는 포함하지 않는다. 호출자는 이 값을
    /// 수신/파싱 대기열의 논리적 body-byte 예산에 쓸 수 있다.
    /// </remarks>
    [[nodiscard]] std::size_t CompletedBodyBytes() const noexcept;

    /// <summary>아직 꺼내지 않은 완결 본문을 모두 버린다.</summary>
    /// <remarks>
    /// 미완성 프레임과 이미 NextFrame()이 돌려준 현재 본문은 건드리지 않는다. 연결 종료처럼
    /// 더 이상 이 completed body들을 처리하지 않을 때 쓴다.
    /// </remarks>
    void DiscardCompletedFrames() noexcept;

    /// <summary>미완성·완성·현재 본문과 내부 codec 저장소를 모두 놓는다.</summary>
    /// <remarks>
    /// 연결 종료 뒤 FrameReader 객체 자체가 외부 소유자 때문에 오래 남아도 maxBodySize 크기의
    /// 저장소를 계속 붙들지 않게 하는 수명 경계다. NextFrame()이 돌려준 span은 즉시 무효가 된다.
    /// 이후 Append()를 다시 부르면 빈 Reader처럼 저장소를 다시 준비한다.
    /// </remarks>
    void ReleaseStorage() noexcept;

    /// <summary>연결이 닫힐 때 남은 미완성 프레임이 있는지 판정한다.</summary>
    [[nodiscard]] Core::Status Finish() const;

    [[nodiscard]] bool HasIncompleteFrame() const noexcept;

private:
    static void CopyCompletedFrame(void* context, std::span<const std::byte> body) noexcept;

    Core::Status EnsureReader();

    std::uint32_t mMaxBodySize;
    std::vector<std::byte> mStorage;
    std::optional<FrameCodec::Reader> mReader;
    std::deque<std::vector<std::byte>> mCompleted;
    std::size_t mCompletedBodyBytes = 0;
    std::vector<std::byte> mCurrent;
    void* mAppendAdmissionContext = nullptr;
    CompletedFrameAdmission mAppendAdmission = nullptr;
    bool mCallbackAllocationFailed = false;
    bool mCallbackAdmissionRejected = false;
    bool mAdmissionTerminal = false;
};

/// <summary>
/// JSON 본문 앞에 머리를 붙여 보낼 수 있는 바이트로 만든다.
/// </summary>
/// <param name="jsonBody">UTF-8로 인코딩된 JSON 본문.</param>
/// <returns>본문이 상한을 넘으면 ErrorCode::TooLarge.</returns>
Core::Result<std::vector<std::byte>> EncodeFrame(
    std::span<const std::byte> jsonBody, std::uint32_t maxBodySize = DefaultMaxBodySize);
}
