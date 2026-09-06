#pragma once

#include "Net/WinsockInternal.h"

#include <cstddef>
#include <type_traits>

namespace ServerCore::Net
{
class IIoCompletionTarget;

/// <summary>완료 포트에서 돌아온 겹침 요청이 어떤 요청이었는지 가른다.</summary>
enum class IoOperationKind
{
    Receive,
    Send,
    Accept
};

/// <summary>
/// 진행 중인 겹침 I/O 하나를 나타낸다. 완료 포트가 돌려주는 OVERLAPPED가 이것으로 되돌아온다.
/// </summary>
/// <remarks>
/// 왜 이 자리에 있는가:
/// GetQueuedCompletionStatus는 OVERLAPPED 포인터 하나만 돌려준다. 그 포인터로부터 "무슨
/// 요청이었고 누가 처리해야 하는가"를 되찾을 방법이 없으면 완료를 배분할 수 없다. 그 되찾음을
/// 가능하게 하는 것이 이 구조체다.
///
/// 왜 overlapped가 첫 멤버여야 하는가:
/// 완료 포트가 돌려주는 것은 우리가 넘긴 &operation.overlapped다. 그 주소에서 이 구조체로
/// 돌아오려면 overlapped가 오프셋 0에 있어야 한다. 그리고 이 구조체는 표준 레이아웃이어야
/// 그 되돌림이 정의된 동작이다. 그래서 여기에는 가상 함수도, 표준 레이아웃이 아닌 멤버도
/// 두지 않는다. 자기 몫의 shared_ptr 참조 같은 것은 이것을 상속한 쪽이 든다.
///
/// 소유한다: 아무것도 소유하지 않는다.
/// 소유하지 않는다: target. 비소유 관찰자이며, 완료가 돌아올 때까지 살아 있는 것은 이
/// 구조체를 상속한 쪽이 드는 참조가 보장한다.
///
/// 스레드 안전성: 단일 스레드 전용. 하나의 요청이 진행 중인 동안 그 요청 객체를 건드리는
/// 것은 요청을 건 쪽과 완료를 받은 쪽 하나뿐이고, 그 둘은 동시에 돌지 않는다.
///
/// 약속하지 않는 것:
/// - 요청이 끝났는지 스스로 알지 못한다. 완료를 받은 쪽이 안다.
/// </remarks>
struct IoOperation
{
    /// <summary>반드시 첫 멤버다. 그것을 지키는 것은 이 구조체 아래의 static_assert다.</summary>
    /// <remarks>
    /// 옮기면 완료 배분이 잘못된 주소를 이 구조체로 읽는다. 그때 나는 증상은 컴파일 오류가
    /// 아니라 엉뚱한 곳을 가리키는 포인터이므로, 옮기는 것을 컴파일 시간에 막아야 한다.
    /// </remarks>
    OVERLAPPED overlapped{};

    IoOperationKind kind = IoOperationKind::Receive;

    /// <summary>이 완료를 처리할 대상. 비소유다.</summary>
    IIoCompletionTarget* target = nullptr;
};

// 위 두 문장("표준 레이아웃이어야 한다"와 "overlapped가 반드시 첫 멤버다")을 지키는 것이
// 이 두 줄이다. 이것이 없으면 그 문장들은 약속이 아니라 희망이다. 멤버를 하나 앞에 끼워
// 넣거나 가상 함수를 더하면 여기서 빌드가 멈춘다.
static_assert(std::is_standard_layout_v<IoOperation>,
    "IoOperation must stay standard layout; the completion loop casts an OVERLAPPED address to it");
static_assert(offsetof(IoOperation, overlapped) == 0,
    "IoOperation::overlapped must stay at offset 0; the completion loop returns that address");

/// <summary>
/// 완료 포트에서 돌아온 요청을 받는 자리다. 전송 층 안에서만 구현한다.
/// </summary>
/// <remarks>
/// 왜 이 자리에 있는가:
/// 이것이 없으면 IoContext가 연결과 수락기의 구체 타입을 알아야 한다. 그러면 완료 포트를
/// 도는 코드가 이 층의 모든 타입에 묶인다.
///
/// 스레드 안전성: 구현이 보장한다. 이 함수는 I/O 스레드 여럿에서 동시에 불릴 수 있다.
/// </remarks>
class IIoCompletionTarget
{
public:
    virtual ~IIoCompletionTarget() = default;

    IIoCompletionTarget(const IIoCompletionTarget&) = delete;
    IIoCompletionTarget& operator=(const IIoCompletionTarget&) = delete;
    IIoCompletionTarget(IIoCompletionTarget&&) = delete;
    IIoCompletionTarget& operator=(IIoCompletionTarget&&) = delete;

    /// <param name="operation">완료된 요청. 이 호출 안에서 다시 걸어도 된다.</param>
    /// <param name="bytesTransferred">실제로 옮겨진 바이트 수.</param>
    /// <param name="errorCode">0이면 성공. 아니면 WSAGetLastError()나 GetLastError()의 값.</param>
    virtual void OnIoCompleted(
        IoOperation& operation, DWORD bytesTransferred, unsigned long errorCode) = 0;

protected:
    IIoCompletionTarget() = default;
};
}
