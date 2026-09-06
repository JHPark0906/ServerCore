#include "TestHarness.h"

#include "ServerCore/Core/ByteBuffer.h"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

/// <summary>
/// ByteBuffer가 담은 바이트를 순서대로, 그리고 한 조각으로 돌려주는지 고정하는 검사다.
/// </summary>
/// <remarks>
/// 여기서 가장 값이 큰 것은 HeldBytesStayContiguous다. 이 통이 감기지 않는다는 것이 설계이고,
/// 프로토콜 층이 그 연속성에 기대어 프레임 머리 4바이트를 읽는다. 누가 이것을 감기는 링으로
/// "고치면" 그 검사가 붉어진다.
///
/// 계약 위반 경로는 여기 없다. 담긴 것보다 많이 Consume하는 것과 용량 0으로 만드는 것은
/// 단언으로 프로세스를 끊으므로 시험 대상이 아니다.
/// </remarks>
namespace
{
/// <summary>정수 목록으로 바이트 묶음을 만든다.</summary>
std::vector<std::byte> MakeBytes(std::initializer_list<int> values)
{
    std::vector<std::byte> bytes;
    bytes.reserve(values.size());
    for (const int value : values)
    {
        bytes.push_back(static_cast<std::byte>(value));
    }
    return bytes;
}

/// <summary>바이트 묶음을 "1,2,3" 꼴로 적는다. 실패 메시지가 읽히게 하려는 것이다.</summary>
std::string Describe(std::span<const std::byte> bytes)
{
    std::string text;
    for (const std::byte value : bytes)
    {
        if (!text.empty())
        {
            text += ",";
        }
        text += std::to_string(std::to_integer<int>(value));
    }
    return text;
}

void NewBufferIsEmpty()
{
    const ServerCore::Core::ByteBuffer buffer(16);

    ServerCoreTest::ExpectTrue(buffer.Empty(), "a new buffer is empty");
    ServerCoreTest::ExpectEqual(std::size_t{ 0 }, buffer.Size(), "a new buffer holds nothing");
    ServerCoreTest::ExpectEqual(
        std::size_t{ 16 }, buffer.Capacity(), "a new buffer keeps the capacity it was given");
}

void WriteThenPeekReturnsSameBytes()
{
    ServerCore::Core::ByteBuffer buffer(16);
    const std::vector<std::byte> input = MakeBytes({ 1, 2, 3, 4 });

    const std::size_t written = buffer.Write(input);

    ServerCoreTest::ExpectEqual(
        std::size_t{ 4 }, written, "Write() reports how many bytes it took");
    ServerCoreTest::ExpectEqual(
        std::size_t{ 4 }, buffer.Size(), "the buffer holds what was written");
    ServerCoreTest::ExpectEqual(std::string("1,2,3,4"), Describe(buffer.Peek()),
        "Peek() returns what was written, in order");
}

void ConsumeRemovesFromTheFront()
{
    ServerCore::Core::ByteBuffer buffer(16);
    const std::vector<std::byte> input = MakeBytes({ 1, 2, 3, 4, 5 });
    const std::size_t written = buffer.Write(input);
    ServerCoreTest::ExpectEqual(std::size_t{ 5 }, written, "the setup write took every byte");

    buffer.Consume(2);

    ServerCoreTest::ExpectEqual(std::size_t{ 3 }, buffer.Size(), "Consume() reduces the size");
    ServerCoreTest::ExpectEqual(
        std::string("3,4,5"), Describe(buffer.Peek()), "Consume() drops from the front");
}

void ConsumeEverythingLeavesItEmpty()
{
    ServerCore::Core::ByteBuffer buffer(16);
    const std::vector<std::byte> input = MakeBytes({ 1, 2, 3 });
    const std::size_t written = buffer.Write(input);
    ServerCoreTest::ExpectEqual(std::size_t{ 3 }, written, "the setup write took every byte");

    buffer.Consume(3);

    ServerCoreTest::ExpectTrue(buffer.Empty(), "consuming everything leaves the buffer empty");
    ServerCoreTest::ExpectEqual(std::size_t{ 0 }, buffer.Size(), "an emptied buffer holds nothing");
    ServerCoreTest::ExpectEqual(
        std::string(), Describe(buffer.Peek()), "an emptied buffer shows nothing");
}

void WriteBeyondCapacityIsPartial()
{
    ServerCore::Core::ByteBuffer buffer(4);
    const std::vector<std::byte> input = MakeBytes({ 1, 2, 3, 4, 5, 6 });

    const std::size_t written = buffer.Write(input);

    ServerCoreTest::ExpectEqual(
        std::size_t{ 4 }, written, "Write() takes only what fits and reports that count");
    ServerCoreTest::ExpectEqual(std::string("1,2,3,4"), Describe(buffer.Peek()),
        "Write() takes the bytes from the front of the input");
}

void WriteToAFullBufferTakesNothing()
{
    ServerCore::Core::ByteBuffer buffer(4);
    const std::vector<std::byte> first = MakeBytes({ 1, 2, 3, 4 });
    const std::size_t writtenFirst = buffer.Write(first);
    ServerCoreTest::ExpectEqual(
        std::size_t{ 4 }, writtenFirst, "the setup write filled the buffer");

    const std::vector<std::byte> second = MakeBytes({ 5 });
    const std::size_t writtenSecond = buffer.Write(second);

    ServerCoreTest::ExpectEqual(
        std::size_t{ 0 }, writtenSecond, "a full buffer takes no more bytes");
    ServerCoreTest::ExpectEqual(std::string("1,2,3,4"), Describe(buffer.Peek()),
        "a refused write leaves the held bytes alone");
}

void HeldBytesStayContiguous()
{
    // 링이었다면 여기서 감긴다. 용량 8에 6바이트를 담고 앞 4를 버리면 뒤에 2바이트가 남고,
    // 이어서 5바이트를 더 담으면 링에서는 끝을 넘어 앞으로 돌아간다. 그때 Peek()은 두 조각
    // 중 하나만 줄 수 있으므로 아래 기대가 깨진다. 이 통은 감기지 않고 앞으로 당기므로
    // 일곱 바이트가 한 조각으로 나온다.
    ServerCore::Core::ByteBuffer buffer(8);

    const std::vector<std::byte> first = MakeBytes({ 1, 2, 3, 4, 5, 6 });
    const std::size_t writtenFirst = buffer.Write(first);
    ServerCoreTest::ExpectEqual(std::size_t{ 6 }, writtenFirst, "the setup write took every byte");

    buffer.Consume(4);
    ServerCoreTest::ExpectEqual(
        std::string("5,6"), Describe(buffer.Peek()), "the setup consume left the last two bytes");

    const std::vector<std::byte> second = MakeBytes({ 7, 8, 9, 10, 11 });
    const std::size_t writtenSecond = buffer.Write(second);

    ServerCoreTest::ExpectEqual(std::size_t{ 5 }, writtenSecond,
        "the second write fits after the held bytes move to the front");
    ServerCoreTest::ExpectEqual(std::size_t{ 7 }, buffer.Size(), "the buffer holds both writes");
    ServerCoreTest::ExpectEqual(std::string("5,6,7,8,9,10,11"), Describe(buffer.Peek()),
        "Peek() returns every held byte in one span, in order");
}

const ServerCoreTest::CheckRegistration gNewBufferIsEmpty{ "ByteBuffer.NewBufferIsEmpty",
    NewBufferIsEmpty };
const ServerCoreTest::CheckRegistration gWriteThenPeekReturnsSameBytes{
    "ByteBuffer.WriteThenPeekReturnsSameBytes", WriteThenPeekReturnsSameBytes
};
const ServerCoreTest::CheckRegistration gConsumeRemovesFromTheFront{
    "ByteBuffer.ConsumeRemovesFromTheFront", ConsumeRemovesFromTheFront
};
const ServerCoreTest::CheckRegistration gConsumeEverythingLeavesItEmpty{
    "ByteBuffer.ConsumeEverythingLeavesItEmpty", ConsumeEverythingLeavesItEmpty
};
const ServerCoreTest::CheckRegistration gWriteBeyondCapacityIsPartial{
    "ByteBuffer.WriteBeyondCapacityIsPartial", WriteBeyondCapacityIsPartial
};
const ServerCoreTest::CheckRegistration gWriteToAFullBufferTakesNothing{
    "ByteBuffer.WriteToAFullBufferTakesNothing", WriteToAFullBufferTakesNothing
};
const ServerCoreTest::CheckRegistration gHeldBytesStayContiguous{
    "ByteBuffer.HeldBytesStayContiguous", HeldBytesStayContiguous
};
}
