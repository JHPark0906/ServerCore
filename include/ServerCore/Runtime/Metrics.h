#pragma once

#include "ServerCore/Session/Session.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ServerCore::Runtime
{
/// <summary>세션 하나의 아직 비워지지 않은 송신 큐 상태다.</summary>
/// <remarks>
/// queuedBytes는 Connection::Send가 성공한 뒤 아직 송신 완료가 오지 않은 바이트의 순간값이다.
/// 상대가 실제로 받았다는 뜻이 아니며, 스냅숏을 읽는 동안에도 I/O 완료로 바뀔 수 있다.
/// </remarks>
struct SessionSendQueueSnapshot
{
    Session::SessionId id = Session::SessionId::Invalid;
    std::size_t queuedBytes = 0;
};

/// <summary>ServerHost가 당겨 읽기로 한 번 내는 고정 운영 지표 묶음이다.</summary>
/// <remarks>
/// 이것은 일반 지표 등록기나 시계열 저장소가 아니다. ServerCore가 운영 판정을 위해 고정해 둔
/// 값만 담는다. counter는 Host 수명 동안 누적되고, gauge와 counter는 서로 다른 동기화 경계에서
/// 읽으므로 한 스냅숏 안의 필드들이 하나의 원자적 시점을 뜻하지는 않는다.
///
/// receivedFrameCount는 L2가 완결 프레임을 꺼낸 횟수다. JSON 파싱 실패 프레임도 이미 완결되어
/// 수신된 것으로 센다. queuedSendFrameCount는 L1 송신 큐가 프레임 전체를 받아들인 횟수이며,
/// 실제 전송 완료나 상대 수신 횟수가 아니다. errorCount는 Host가 처리한 I/O·프로토콜·디스패치
/// 실패 중 Closed·Timeout·WouldBlock을 뺀 횟수다. 게임 코드가 임의로 Disconnect한 정책 결정은
/// 세지 않는다.
///
/// pendingParseBytes와 pendingParseTaskCount는 parse worker가 켜진 경우, FrameReader에서 순서를
/// 기다리는 완결 본문과 worker에서 해석 중인 한 본문을 합친 reservation이다. 본문 vector를 복사
/// 하기 전에 잡으므로 이 두 값은 각 설정 상한을 넘지 않는다. I/O 수신 callback 복사본은
/// pendingReceiveBytes라는 별도 예산에 속한다.
///
/// sessionSendQueues는 SessionId 오름차순이다. activeSessionCount는 이 배열의 길이와 같다.
/// </remarks>
struct ServerMetricsSnapshot
{
    std::uint32_t configuredIoWorkerThreadCount = 0;
    std::uint32_t configuredParseWorkerThreadCount = 0;
    std::size_t activeSessionCount = 0;
    std::size_t pendingReceiveBytes = 0;
    std::size_t pendingJobCount = 0;
    std::size_t pendingParseBytes = 0;
    std::size_t pendingParseTaskCount = 0;
    std::uint64_t receivedFrameCount = 0;
    std::uint64_t queuedSendFrameCount = 0;
    std::uint64_t errorCount = 0;
    std::uint64_t skippedPeriodCount = 0;
    std::vector<SessionSendQueueSnapshot> sessionSendQueues;
};
}
