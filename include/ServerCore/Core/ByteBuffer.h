#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace ServerCore::Core
{
/// <summary>
/// 전송 층이 조각으로 주는 바이트를, 프로토콜 층이 한 프레임씩 잘라 갈 때까지 담아 두는 통이다.
/// </summary>
/// <remarks>
/// 왜 이 자리에 있는가:
/// TCP는 메시지 경계를 보존하지 않는다. 한 프레임이 두 번에 나뉘어 오거나 두 프레임이 한 번에
/// 붙어 온다. 그 사이를 메우는 것이 이 통이고, 소켓도 프레임도 모르므로 기반 층에 있다.
///
/// 왜 링이 아닌가 - 이 이름이 ByteBuffer인 이유다:
/// 링은 끝에서 앞으로 감기므로 담긴 바이트가 감김 지점에서 두 조각으로 갈린다. 그런데 Peek()은
/// 뷰를 하나만 돌려주므로 두 번째 조각을 줄 방법이 없다. 그러면 프레임 머리 4바이트가 감김
/// 지점에 걸치는 순간 프로토콜 층이 길이 필드를 읽지 못한다.
/// 그래서 이 통은 감기지 않는다. 담긴 것은 항상 연속이고, 뒤 공간이 모자랄 때 남은 것을 앞으로
/// 당겨 자리를 만든다. 프로토콜 층이 그 연속성에 기대고 있으므로, 감기지 않는 것을 결함으로
/// 보고 진짜 링으로 바꾸면 그 층이 조용히 깨진다.
///
/// 소유한다: 자기 저장소 하나. 만들 때 한 번 잡고 그 뒤로 늘리거나 줄이지 않는다.
///
/// 호출 계약:
/// - 연결 하나당 하나씩 둔다.
/// - 스레드 안전성: 단일 스레드 전용. 여러 스레드에서 쓰려면 호출자가 직렬화한다.
///
/// 약속하지 않는 것:
/// - Peek()이 돌려주는 뷰는 다음 Write()나 Consume() 호출 전까지만 유효하다.
///   보관하려면 받는 쪽이 복사한다.
/// - 담긴 바이트가 무엇인지 모른다. 프레임도 JSON도 모른다.
/// - 무한히 자라지 않는다. 용량이 정해져 있고, 넘치면 Write가 요청보다 적게 담는다.
///   그 상황을 어떻게 다룰지는 호출자가 정한다.
/// - 용량이 쓰임새에 충분한지 검사하지 않는다. 이 통은 프레이밍을 모르므로 검사할 수 없다.
///   머리와 본문을 함께 담아야 하는 쓰임새라면 그 합을 용량으로 줘야 한다. 본문 상한만큼만
///   주면 최대 크기 프레임이 영원히 완성되지 않는데, 그때 나는 증상이 나쁜 쪽이다.
///   Write가 계속 요청보다 적게 담고 프레임은 계속 "아직"인 채로 남아, 크래시도 오류도
///   기록도 없이 그 연결만 조용히 멈춘다. 작은 프레임은 전부 잘 돌기 때문에 시험도 통과한다.
///   다만 이 통을 소유하는 쪽이 하나라면 용량을 정하는 자리도 그 안 한 줄뿐이므로,
///   틀릴 수 있는 자리는 하나로 줄어든다.
/// </remarks>
class ByteBuffer
{
public:
    /// <summary>담을 수 있는 최대 바이트 수를 정해서 만든다.</summary>
    /// <param name="capacity">
    /// 담을 수 있는 최대 바이트 수. 0을 넘기는 것은 계약 위반이다. 0이면 아무것도 담을 수 없는데,
    /// Write가 0을 돌려주는 것은 "가득 찼다"와 구분되지 않으므로 진행 불가 상태가 정상 반환으로
    /// 위장된다.
    /// </param>
    explicit ByteBuffer(std::size_t capacity);

    /// <summary>바이트를 뒤에 붙인다.</summary>
    /// <returns>실제로 담은 바이트 수. 공간이 모자라면 요청보다 작을 수 있다.</returns>
    std::size_t Write(std::span<const std::byte> bytes);

    /// <summary>담긴 바이트를 복사 없이 들여다본다.</summary>
    [[nodiscard]] std::span<const std::byte> Peek() const;

    /// <summary>앞에서부터 지정한 만큼을 버린다. 담긴 것보다 많이 버리려 하면 계약 위반이다.</summary>
    void Consume(std::size_t byteCount);

    [[nodiscard]] std::size_t Size() const noexcept;

    [[nodiscard]] bool Empty() const noexcept;

    /// <summary>만들 때 정해진 최대 바이트 수다. 살아 있는 동안 바뀌지 않는다.</summary>
    [[nodiscard]] std::size_t Capacity() const noexcept;

private:
    /// <summary>
    /// 담긴 바이트는 항상 [mReadPos, mWritePos) 구간에 연속으로 있다.
    /// Consume은 mReadPos만 올리고 옮기지 않는다. 뒤 공간이 모자랄 때 Write가 앞으로 당긴다.
    /// </summary>
    std::vector<std::byte> mStorage;
    std::size_t mReadPos = 0;
    std::size_t mWritePos = 0;
};
}
