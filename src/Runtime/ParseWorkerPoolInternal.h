#pragma once

#include "ServerCore/Core/Error.h"

#include <functional>
#include <memory>

namespace ServerCore::Runtime
{
/// <summary>ServerHost 내부의 제한된 JSON 파싱 작업자 묶음이다.</summary>
/// <remarks>
/// 이 타입은 공개 ServerCore API가 아니다. 작업의 의미·예산·취소·세션 순서는 ServerHost가
/// 정하고, 이곳은 그 작업을 서로 다른 worker에서 실행하는 최소 기계만 제공한다. queue의 상한은
/// Host의 parse byte/task 예산이 보장하므로 별도의 독립 상한을 두지 않는다.
///
/// StopAndDiscard는 새 작업을 닫고 아직 시작하지 않은 작업을 버린 뒤, 이미 실행 중인 작업이
/// 끝날 때까지 기다린다. 버린 작업의 lambda 소멸자가 자기 parse reservation을 반납하는 것이
/// Host 종료에서 메모리 예산을 남기지 않는 수단이다.
/// </remarks>
class ParseWorkerPool final
{
public:
    ParseWorkerPool();
    ~ParseWorkerPool();

    ParseWorkerPool(const ParseWorkerPool&) = delete;
    ParseWorkerPool& operator=(const ParseWorkerPool&) = delete;
    ParseWorkerPool(ParseWorkerPool&&) = delete;
    ParseWorkerPool& operator=(ParseWorkerPool&&) = delete;

    /// <summary>workerThreadCount개의 파싱 worker를 한 번 시작한다.</summary>
    [[nodiscard]] Core::Status Start(int workerThreadCount);

    /// <summary>입력 큐에 작업 하나를 넣는다.</summary>
    /// <returns>StopAndDiscard 뒤에는 Closed, 빈 작업은 InvalidArgument다.</returns>
    [[nodiscard]] Core::Status Post(std::function<void()> job);

    /// <summary>새 작업을 닫고 대기 작업을 버린 뒤 실행 중인 worker를 모두 join한다.</summary>
    void StopAndDiscard();

    /// <summary>현재 스레드가 이 묶음의 worker인지 답한다.</summary>
    [[nodiscard]] bool IsCurrentThread() const noexcept;

private:
    class State;
    std::unique_ptr<State> mState;
};
}
