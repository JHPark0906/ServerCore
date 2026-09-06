#pragma once

#include "ServerCore/Net/Acceptor.h"

#include <memory>

namespace ServerCore::Net
{
class SendBudget;

/// <summary>실행 환경 층이 Acceptor가 만들 Connection에 내부 송신 예산을 주입하는 통로다.</summary>
/// <remarks>
/// 공개 Acceptor는 ServerHost의 합산 메모리 정책을 알지 않는다. 이 접근자는 ServerHost 조립
/// 지점에서만 쓰며, 소비자가 Acceptor를 직접 사용할 때의 공개 표면을 늘리지 않는다.
/// </remarks>
class AcceptorAccess
{
public:
    static void SetSendBudget(Acceptor& acceptor, std::shared_ptr<SendBudget> sendBudget);
};
}
