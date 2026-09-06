#include "ServerCore/Net/IoContext.h"

#include "Net/IoContextInternal.h"
#include "Net/IoOperationInternal.h"
#include "Net/WinsockInternal.h"
#include "ServerCore/Core/Assert.h"

#include <atomic>
#include <cstddef>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace ServerCore::Net
{
namespace
{
/// <summary>요청과 짝이 없는 종료 신호에 붙이는 완료 키다.</summary>
/// <remarks>
/// 소켓을 붙일 때 쓰는 키는 0이므로 이 값과 겹치지 않는다. 다만 완료 루프는 키를 보기 전에
/// OVERLAPPED가 널인지부터 보므로, 겹치더라도 소켓 완료가 종료 신호로 오해되지는 않는다.
/// </remarks>
constexpr ULONG_PTR StopCompletionKey = 1;

/// <summary>지금 이 스레드가 완료 루프를 돌고 있다면 그 주인. 아니면 널이다.</summary>
/// <remarks>
/// 왜 스레드 지역인가:
/// "이 함수를 I/O 스레드에서 부르지 마라"라는 약속이 여럿 있는데, 그것을 판정하려면 지금
/// 스레드가 누구의 완료 스레드인지 알아야 한다. 스레드 목록을 뒤져 비교할 수도 있지만 그때는
/// 잠금이 필요하고, 그 잠금을 이미 쥔 자리에서도 판정해야 해서 얽힌다. 스레드 지역 포인터는
/// 잠금 없이 답한다.
/// </remarks>
thread_local const IoContext* gCurrentIoContext = nullptr;

/// <summary>완료 포트에서 완료를 하나씩 꺼내 그 요청의 주인에게 넘긴다.</summary>
/// <param name="completionPort">이 스레드가 볼 완료 포트.</param>
/// <param name="owner">이 스레드를 띄운 IoContext. 스레드 지역 표시에만 쓴다.</param>
void RunCompletionLoop(HANDLE completionPort, const IoContext* owner)
{
    gCurrentIoContext = owner;

    for (;;)
    {
        DWORD bytesTransferred = 0;
        ULONG_PTR completionKey = 0;
        LPOVERLAPPED overlappedPointer = nullptr;

        const BOOL succeeded = ::GetQueuedCompletionStatus(
            completionPort, &bytesTransferred, &completionKey, &overlappedPointer, INFINITE);

        // 실패했더라도 OVERLAPPED가 널이 아니면 "요청 하나가 실패로 끝났다"는 뜻이다.
        // 그 경우는 주인에게 넘겨야 한다. 넘기지 않으면 그 요청이 든 참조가 영영 안 풀린다.
        const unsigned long errorCode = succeeded == FALSE ? ::GetLastError() : 0UL;

        if (overlappedPointer == nullptr)
        {
            const bool portIsGone = succeeded == FALSE;
            if (portIsGone || completionKey == StopCompletionKey)
            {
                break;
            }

            // 우리가 보내지 않은 통지다. 할 일이 없다.
            continue;
        }

        // OVERLAPPED는 요청 구조체의 첫 멤버이므로 그 주소가 곧 요청의 주소다.
        IoOperation* const operation = reinterpret_cast<IoOperation*>(overlappedPointer);
        SERVERCORE_ASSERT(
            operation->target != nullptr, "a completed I/O operation carries no target");

        try
        {
            operation->target->OnIoCompleted(*operation, bytesTransferred, errorCode);
        }
        catch (...)
        {
            Core::ReportAssertFailure("I/O completion target did not throw", __FILE__, __LINE__,
                "an I/O completion target threw across the worker boundary");
        }
    }

    gCurrentIoContext = nullptr;
}
}

/// <summary>IoContext의 구현 상태다.</summary>
/// <remarks>
/// 데이터 멤버에 m 접두를 붙이지 않은 이유: 이것은 캡슐화된 상태를 스스로 지키는 타입이
/// 아니라 IoContext가 감춰 둔 자리를 담는 통이다. 캡슐화의 경계는 IoContext 쪽에 있고 이
/// 타입은 그 안쪽이다. 코드 규약이 말하는 "경계는 접근 지정자가 아니라 뜻이다"가 이 자리다.
/// </remarks>
class IoContext::State
{
public:
    /// <summary>스레드를 멈추고 포트를 닫는다. lifecycleMutex를 쥔 채 부른다.</summary>
    void ShutDownLocked();

    std::mutex lifecycleMutex;
    HANDLE completionPort = nullptr;
    std::vector<std::thread> workerThreads;
    std::atomic<bool> running{ false };
};

void IoContext::State::ShutDownLocked()
{
    if (completionPort == nullptr)
    {
        return;
    }

    // 깨움 신호는 하나에 스레드 하나만 깨운다. 그래서 스레드 수만큼 보낸다.
    bool everySignalWasPosted = true;
    for (std::size_t index = 0; index < workerThreads.size(); ++index)
    {
        if (::PostQueuedCompletionStatus(completionPort, 0, StopCompletionKey, nullptr) == FALSE)
        {
            everySignalWasPosted = false;
            break;
        }
    }

    if (!everySignalWasPosted)
    {
        // 신호를 못 보내면 대기 중인 스레드를 깨울 남은 방법은 포트를 닫는 것뿐이다. 닫으면
        // 대기가 실패로 풀리고 루프가 빠져나온다. join 전에 닫아야 한다. 뒤에 닫으면 깨어나지
        // 않은 스레드를 기다리며 여기서 멈춘다.
        ::CloseHandle(completionPort);
        completionPort = nullptr;
    }

    for (std::thread& worker : workerThreads)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }
    workerThreads.clear();

    if (completionPort != nullptr)
    {
        ::CloseHandle(completionPort);
        completionPort = nullptr;
    }

    running.store(false, std::memory_order_release);
}

IoContext::IoContext()
    : mState(std::make_unique<State>())
{
}

IoContext::~IoContext()
{
    // 헤더가 약속한 "소멸 전에 반드시 Stop()"을 지키는 자리가 여기다.
    // Stop() 안의 단언도 그대로 돈다. I/O 스레드에서 이 객체를 소멸시키는 것 역시 같은
    // 이유로 계약 위반이기 때문이다.
    Stop();
}

Core::Status IoContext::Start(int workerThreadCount)
{
    SERVERCORE_ASSERT(
        workerThreadCount > 0, "Start() was given a worker thread count that is not positive");

    const std::lock_guard<std::mutex> guard(mState->lifecycleMutex);

    if (mState->completionPort != nullptr)
    {
        return Core::Status::Fail(
            Core::ErrorCode::AlreadyExists, "IoContext::Start() was called while already running");
    }

    // 첫 인자가 INVALID_HANDLE_VALUE면 어떤 파일에도 붙지 않은 빈 완료 포트를 만든다.
    // 마지막 인자는 동시에 깨어 있을 스레드 수의 상한이며, 우리는 띄운 만큼을 그대로 준다.
    HANDLE port = ::CreateIoCompletionPort(
        INVALID_HANDLE_VALUE, nullptr, 0, static_cast<DWORD>(workerThreadCount));
    if (port == nullptr)
    {
        return MakeWin32Failure("CreateIoCompletionPort", ::GetLastError());
    }

    mState->completionPort = port;
    mState->running.store(true, std::memory_order_release);
    try
    {
        mState->workerThreads.reserve(static_cast<std::size_t>(workerThreadCount));
    }
    catch (const std::bad_alloc&)
    {
        ::CloseHandle(mState->completionPort);
        mState->completionPort = nullptr;
        mState->running.store(false, std::memory_order_release);
        return Core::Status::AllocationFailure();
    }

    for (int index = 0; index < workerThreadCount; ++index)
    {
        try
        {
            mState->workerThreads.emplace_back(RunCompletionLoop, port, this);
        }
        catch (const std::system_error& error)
        {
            // 이 층은 예외를 밖으로 내보내지 않는다. 스레드를 못 만드는 것은 계약 위반이
            // 아니라 자원 고갈이므로 Status로 바꾼다. 이미 띄운 것은 여기서 거둔다.
            const int errorCode = error.code().value();
            mState->ShutDownLocked();
            try
            {
                std::string message = "std::thread creation failed, std::error_code::value()=";
                message.append(std::to_string(errorCode));
                return Core::Status::Fail(Core::ErrorCode::PlatformError, std::move(message));
            }
            catch (...)
            {
                return Core::Status::AllocationFailure();
            }
        }
        catch (const std::bad_alloc&)
        {
            mState->ShutDownLocked();
            return Core::Status::AllocationFailure();
        }
    }

    return Core::Status::Ok();
}

void IoContext::Stop()
{
    SERVERCORE_ASSERT(
        !IsCurrentThreadIoThread(), "Stop() was called from an I/O thread of this IoContext");

    const std::lock_guard<std::mutex> guard(mState->lifecycleMutex);
    mState->ShutDownLocked();
}

bool IoContext::IsRunning() const noexcept
{
    return mState->running.load(std::memory_order_acquire);
}

bool IoContext::IsCurrentThreadIoThread() const noexcept
{
    return gCurrentIoContext == this;
}

Core::Status IoContextAccess::AssociateSocket(IoContext& context, SOCKET socket)
{
    IoContext::State& state = *context.mState;
    const std::lock_guard<std::mutex> guard(state.lifecycleMutex);

    if (state.completionPort == nullptr)
    {
        return Core::Status::Fail(Core::ErrorCode::Closed,
            "the socket was not attached because IoContext is not running");
    }

    const HANDLE attached =
        ::CreateIoCompletionPort(reinterpret_cast<HANDLE>(socket), state.completionPort, 0, 0);
    if (attached == nullptr)
    {
        return MakeWin32Failure("CreateIoCompletionPort(socket)", ::GetLastError());
    }

    return Core::Status::Ok();
}
}
