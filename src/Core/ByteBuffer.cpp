#include "ServerCore/Core/ByteBuffer.h"

#include "ServerCore/Core/Assert.h"

#include <algorithm>
#include <cstring>

namespace ServerCore::Core
{
ByteBuffer::ByteBuffer(std::size_t capacity)
    : mStorage(capacity)
{
    SERVERCORE_ASSERT(capacity != 0, "ByteBuffer was constructed with a capacity of zero");
}

std::size_t ByteBuffer::Write(std::span<const std::byte> bytes)
{
    if (bytes.empty())
    {
        return 0;
    }

    // 뒤 공간이 모자랄 때만 앞으로 당긴다. Consume마다 당기지 않는 이유는, 살아 있는 구간이
    // 감기지 않아 어차피 항상 연속이므로 Peek()의 계약이 압축 시점과 무관하게 지켜지기
    // 때문이다. 프레임을 꺼낼 때마다 남은 것을 옮기면 그만큼이 헛일이 된다.
    if (mStorage.size() - mWritePos < bytes.size() && mReadPos != 0)
    {
        const std::size_t liveCount = mWritePos - mReadPos;
        if (liveCount != 0)
        {
            std::memmove(mStorage.data(), mStorage.data() + mReadPos, liveCount);
        }
        mReadPos = 0;
        mWritePos = liveCount;
    }

    const std::size_t writableCount = std::min(bytes.size(), mStorage.size() - mWritePos);
    if (writableCount == 0)
    {
        return 0;
    }

    std::memcpy(mStorage.data() + mWritePos, bytes.data(), writableCount);
    mWritePos += writableCount;
    return writableCount;
}

std::span<const std::byte> ByteBuffer::Peek() const
{
    return std::span<const std::byte>(mStorage.data() + mReadPos, mWritePos - mReadPos);
}

void ByteBuffer::Consume(std::size_t byteCount)
{
    SERVERCORE_ASSERT(byteCount <= Size(), "Consume() was asked for more bytes than are held");

    mReadPos += byteCount;

    // 다 비었으면 두 자리를 앞으로 돌려놓는다. 그래야 다음 Write가 옮기지 않고 담을 수 있고,
    // 앞쪽에 쓰이지 않는 공간이 쌓이지 않는다.
    if (mReadPos == mWritePos)
    {
        mReadPos = 0;
        mWritePos = 0;
    }
}

std::size_t ByteBuffer::Size() const noexcept
{
    return mWritePos - mReadPos;
}

bool ByteBuffer::Empty() const noexcept
{
    return mWritePos == mReadPos;
}

std::size_t ByteBuffer::Capacity() const noexcept
{
    return mStorage.size();
}
}
