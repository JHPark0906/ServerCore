#include "TestHarness.h"

#include "ServerCore/Core/Error.h"
#include "ServerCore/Protocol/DatagramCodec.h"
#include "ServerCore/Protocol/FrameCodec.h"
#include "ServerCore/Protocol/Framing.h"
#include "ServerCore/Protocol/Json.h"
#include "ServerCore/Protocol/Message.h"
#include "ServerCore/Session/Session.h"

#include "Protocol/MessageTestAccess.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
using ServerCore::Core::ErrorCode;
using ServerCore::Protocol::FrameReader;
using ServerCore::Protocol::JsonValue;
using ServerCore::Protocol::MessageFields;

struct FrameCollector
{
    std::array<std::array<std::byte, 16>, 2> bodies{};
    std::array<std::size_t, 2> lengths{};
    std::size_t count = 0;
    bool overflowed = false;
};

/// <summary>FrameReader가 body 복사 전에 admission을 묻는 순서와 상한을 검사한다.</summary>
struct FrameAdmissionBudget
{
    std::size_t remainingBytes = 0;
    std::size_t remainingFrames = 0;
    std::size_t admittedBytes = 0;
    std::size_t admittedFrames = 0;
    std::size_t rejectedFrames = 0;
};

[[nodiscard]] bool AdmitFrameBody(void* const context, const std::size_t bodySize) noexcept
{
    FrameAdmissionBudget* const budget = static_cast<FrameAdmissionBudget*>(context);
    if (budget == nullptr || budget->remainingFrames == 0 || bodySize > budget->remainingBytes)
    {
        if (budget != nullptr)
        {
            ++budget->rejectedFrames;
        }
        return false;
    }

    --budget->remainingFrames;
    budget->remainingBytes -= bodySize;
    budget->admittedBytes += bodySize;
    ++budget->admittedFrames;
    return true;
}

void CollectSharedFrame(void* const context, const std::span<const std::byte> body) noexcept
{
    FrameCollector* const collector = static_cast<FrameCollector*>(context);
    if (collector == nullptr || collector->count == collector->bodies.size() ||
        body.size() > collector->bodies[0].size())
    {
        if (collector != nullptr)
        {
            collector->overflowed = true;
        }
        return;
    }

    const std::size_t index = collector->count;
    for (std::size_t byte = 0; byte < body.size(); ++byte)
    {
        collector->bodies[index][byte] = body[byte];
    }
    collector->lengths[index] = body.size();
    ++collector->count;
}

[[nodiscard]] std::vector<std::byte> ToBytes(const std::string_view text)
{
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (const char character : text)
    {
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
    return bytes;
}

[[nodiscard]] std::string ToText(const std::span<const std::byte> bytes)
{
    std::string text;
    text.reserve(bytes.size());
    for (const std::byte value : bytes)
    {
        text.push_back(static_cast<char>(std::to_integer<unsigned char>(value)));
    }
    return text;
}

[[nodiscard]] std::vector<std::string> Split(
    const std::string_view text, const char delimiter, const bool retainEmpty)
{
    std::vector<std::string> values;
    std::size_t start = 0;
    while (start <= text.size())
    {
        const std::size_t end = text.find(delimiter, start);
        const std::size_t length =
            end == std::string_view::npos ? text.size() - start : end - start;
        if (retainEmpty || length != 0)
        {
            values.emplace_back(text.substr(start, length));
        }
        if (end == std::string_view::npos)
        {
            break;
        }
        start = end + 1;
    }
    return values;
}

[[nodiscard]] std::string ExpandBody(const std::string_view specification)
{
    if (specification == "@empty")
    {
        return {};
    }

    constexpr std::string_view RepeatPrefix = "@repeat:";
    if (!specification.starts_with(RepeatPrefix))
    {
        return std::string(specification);
    }

    const std::vector<std::string> fields =
        Split(specification.substr(RepeatPrefix.size()), ':', true);
    ServerCoreTest::ExpectEqual(std::size_t{ 2 }, fields.size(), "repeat vector field count");
    if (fields.size() != 2 || fields[0].size() != 1)
    {
        return {};
    }

    const std::size_t count = static_cast<std::size_t>(std::stoull(fields[1]));
    return std::string(count, fields[0][0]);
}

[[nodiscard]] std::vector<std::byte> MakeFramedStream(const std::vector<std::string>& bodies)
{
    std::vector<std::byte> stream;
    for (const std::string& body : bodies)
    {
        const std::vector<std::byte> bytes = ToBytes(body);
        std::vector<std::byte> encodedBytes(
            ServerCore::Protocol::FrameCodec::HeaderSize + bytes.size());
        const ServerCore::Protocol::FrameCodec::EncodeResult encoded =
            ServerCore::Protocol::FrameCodec::EncodeTo(bytes, encodedBytes);
        ServerCoreTest::ExpectTrue(encoded.IsOk(), "vector body encodes");
        stream.insert(stream.end(), encodedBytes.begin(), encodedBytes.end());
    }
    return stream;
}

[[nodiscard]] std::vector<std::byte> DecodeHexBytes(const std::string_view text)
{
    ServerCoreTest::ExpectTrue(text.size() % 2 == 0, "raw hexadecimal vector has complete bytes");
    if (text.size() % 2 != 0)
    {
        return {};
    }

    const auto hexValue = [](const char character) -> unsigned int
    {
        if (character >= '0' && character <= '9')
        {
            return static_cast<unsigned int>(character - '0');
        }
        if (character >= 'a' && character <= 'f')
        {
            return static_cast<unsigned int>(character - 'a' + 10);
        }
        if (character >= 'A' && character <= 'F')
        {
            return static_cast<unsigned int>(character - 'A' + 10);
        }
        return 16;
    };

    std::vector<std::byte> bytes;
    bytes.reserve(text.size() / 2);
    for (std::size_t index = 0; index < text.size(); index += 2)
    {
        const unsigned int high = hexValue(text[index]);
        const unsigned int low = hexValue(text[index + 1]);
        ServerCoreTest::ExpectTrue(
            high < 16 && low < 16, "raw hexadecimal vector contains only hex");
        if (high >= 16 || low >= 16)
        {
            return {};
        }
        bytes.push_back(static_cast<std::byte>((high << 4U) | low));
    }
    return bytes;
}

[[nodiscard]] std::vector<std::string> ExpandBodies(const std::string_view specification)
{
    if (specification == "@none")
    {
        return {};
    }

    const std::vector<std::string> parts = Split(specification, ';', true);
    std::vector<std::string> bodies;
    bodies.reserve(parts.size());
    for (const std::string& part : parts)
    {
        bodies.push_back(ExpandBody(part));
    }
    return bodies;
}

[[nodiscard]] std::vector<std::byte> ExpandWire(const std::string_view specification)
{
    constexpr std::string_view BodiesPrefix = "@bodies:";
    constexpr std::string_view RawHexPrefix = "@rawhex:";
    if (specification.starts_with(BodiesPrefix))
    {
        return MakeFramedStream(ExpandBodies(specification.substr(BodiesPrefix.size())));
    }
    if (specification.starts_with(RawHexPrefix))
    {
        return DecodeHexBytes(specification.substr(RawHexPrefix.size()));
    }

    ServerCoreTest::ExpectTrue(false, "framing vector uses a known wire specification");
    return {};
}

[[nodiscard]] ErrorCode ParseExpectedCode(const std::string_view text)
{
    if (text == "Ok")
    {
        return ErrorCode::Ok;
    }
    if (text == "TooLarge")
    {
        return ErrorCode::TooLarge;
    }
    if (text == "InvalidFormat")
    {
        return ErrorCode::InvalidFormat;
    }
    if (text == "WouldBlock")
    {
        return ErrorCode::WouldBlock;
    }

    ServerCoreTest::ExpectTrue(false, "framing vector names a known expected status");
    return ErrorCode::InvalidFormat;
}

void RunFramingVector(const std::vector<std::byte>& stream,
    const std::vector<std::string>& expectedBodies, const std::string_view chunkSpecification,
    const ErrorCode expectedAppendCode, const ErrorCode expectedFinishCode)
{
    FrameReader reader;
    std::size_t offset = 0;
    ServerCore::Core::Status lastAppend = ServerCore::Core::Status::Ok();

    const std::vector<std::string> chunks = Split(chunkSpecification, ',', false);
    for (const std::string& chunk : chunks)
    {
        const std::size_t requested =
            chunk == "all" ? stream.size() - offset : static_cast<std::size_t>(std::stoull(chunk));
        const std::size_t count = (std::min)(requested, stream.size() - offset);
        lastAppend = reader.Append(std::span<const std::byte>(stream).subspan(offset, count));
        offset += count;
        if (!lastAppend.IsOk())
        {
            break;
        }
    }

    ServerCoreTest::ExpectTrue(
        lastAppend.Code() == expectedAppendCode, "vector append result matches");
    ServerCoreTest::ExpectEqual(stream.size(), offset, "vector chunks consume the whole stream");

    if (expectedAppendCode == ErrorCode::Ok)
    {
        for (const std::string& expected : expectedBodies)
        {
            ServerCore::Core::Result<std::span<const std::byte>> frame = reader.NextFrame();
            ServerCoreTest::ExpectTrue(frame.IsOk(), "vector yields a frame");
            if (frame.IsOk())
            {
                ServerCoreTest::ExpectEqual(expected, ToText(frame.Value()), "vector body matches");
            }
        }
        ServerCore::Core::Result<std::span<const std::byte>> end = reader.NextFrame();
        ServerCoreTest::ExpectTrue(!end.IsOk() && end.GetStatus().Code() == ErrorCode::WouldBlock,
            "vector has no unexpected complete frames");
    }

    const ServerCore::Core::Status finished = reader.Finish();
    ServerCoreTest::ExpectTrue(
        finished.Code() == expectedFinishCode, "vector finish result matches");
}

void FramingConformanceVectors()
{
    std::ifstream file(SERVERCORE_PROTOCOL_VECTOR_PATH);
    ServerCoreTest::ExpectTrue(file.is_open(), "framing vector file opens");
    if (!file.is_open())
    {
        return;
    }

    std::string line;
    std::size_t executed = 0;
    while (std::getline(file, line))
    {
        // getline removes LF; retain the same vectors for a CRLF checkout on Linux.
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == '#')
        {
            continue;
        }

        const std::vector<std::string> fields = Split(line, '|', true);
        ServerCoreTest::ExpectEqual(std::size_t{ 6 }, fields.size(), "framing vector field count");
        if (fields.size() != 6)
        {
            continue;
        }

        RunFramingVector(ExpandWire(fields[1]), ExpandBodies(fields[3]), fields[2],
            ParseExpectedCode(fields[4]), ParseExpectedCode(fields[5]));
        ++executed;
    }
    ServerCoreTest::ExpectEqual(std::size_t{ 9 }, executed, "all framing vectors execute");
}

void FramingRejectsInvalidLengths()
{
    const std::vector<std::byte> twoBytes = ToBytes("{}");
    std::vector<std::byte> encodedBytes(
        ServerCore::Protocol::FrameCodec::HeaderSize + twoBytes.size());
    const ServerCore::Protocol::FrameCodec::EncodeResult encoded =
        ServerCore::Protocol::FrameCodec::EncodeTo(twoBytes, encodedBytes);
    ServerCoreTest::ExpectTrue(encoded.IsOk() && encodedBytes.size() == 6 &&
                                   encodedBytes[0] == static_cast<std::byte>(2) &&
                                   encodedBytes[1] == static_cast<std::byte>(0) &&
                                   encodedBytes[2] == static_cast<std::byte>(0) &&
                                   encodedBytes[3] == static_cast<std::byte>(0),
        "the shared codec writes little-endian lengths");

    // EncodeTo는 caller가 output 안에 미리 조립해 둔 본문도 안전하게 머리를 붙일 수 있어야 한다.
    // 본문 시작이 머리 영역과 겹치는 위치라, 머리를 먼저 쓰거나 memcpy를 쓰면 원래 바이트를
    // 잃거나 정의되지 않은 겹침 복사가 된다.
    std::array<std::byte, ServerCore::Protocol::FrameCodec::HeaderSize + 3> overlapping{
        static_cast<std::byte>('x'), static_cast<std::byte>('y'), static_cast<std::byte>('a'),
        static_cast<std::byte>('b'), static_cast<std::byte>('c'), static_cast<std::byte>('u'),
        static_cast<std::byte>('v')
    };
    const ServerCore::Protocol::FrameCodec::EncodeResult overlapEncoded =
        ServerCore::Protocol::FrameCodec::EncodeTo(
            std::span<const std::byte>(overlapping).subspan(2, 3), overlapping);
    ServerCoreTest::ExpectTrue(
        overlapEncoded.IsOk() && overlapping[0] == static_cast<std::byte>(3) &&
            overlapping[1] == static_cast<std::byte>(0) &&
            overlapping[2] == static_cast<std::byte>(0) &&
            overlapping[3] == static_cast<std::byte>(0) &&
            ToText(std::span<const std::byte>(overlapping)
                    .subspan(ServerCore::Protocol::FrameCodec::HeaderSize)) == "abc",
        "the shared codec preserves an input body that overlaps output storage");
}

/// <summary>
/// 비동기 파서가 FrameReader 밖으로 본문을 가져갈 때, span 수명에 기대지 않고 완료 큐의 vector를
/// 옮기는 경로와 관측 counter를 함께 고정한다.
/// </summary>
void FramingTransfersAndDiscardsCompletedBodies()
{
    const std::string first = "first";
    const std::string second = "second";
    const std::string third = "third";
    FrameReader reader;

    const std::vector<std::byte> initial =
        MakeFramedStream(std::vector<std::string>{ first, second });
    const ServerCore::Core::Status appended = reader.Append(initial);
    ServerCoreTest::ExpectTrue(appended.IsOk(), "the owned-frame test input appends");
    if (!appended.IsOk())
    {
        return;
    }

    ServerCoreTest::ExpectEqual(std::size_t{ 2 }, reader.CompletedFrameCount(),
        "two complete bodies are observable before either is removed");
    ServerCoreTest::ExpectEqual(first.size() + second.size(), reader.CompletedBodyBytes(),
        "completed body bytes sum the queued body sizes");

    const ServerCore::Core::Result<std::span<const std::byte>> firstView = reader.NextFrame();
    ServerCoreTest::ExpectTrue(firstView.IsOk(), "the legacy span API remains available");
    if (!firstView.IsOk())
    {
        return;
    }
    ServerCoreTest::ExpectEqual(
        first, ToText(firstView.Value()), "the legacy span body is unchanged");
    ServerCoreTest::ExpectEqual(std::size_t{ 1 }, reader.CompletedFrameCount(),
        "NextFrame removes one body from the completed counter");
    ServerCoreTest::ExpectEqual(second.size(), reader.CompletedBodyBytes(),
        "NextFrame removes that body's bytes from the completed counter");

    ServerCore::Core::Result<std::vector<std::byte>> owned = reader.TakeNextFrame();
    ServerCoreTest::ExpectTrue(owned.IsOk(), "TakeNextFrame transfers the remaining body");
    if (!owned.IsOk())
    {
        return;
    }
    ServerCoreTest::ExpectEqual(
        second, ToText(owned.Value()), "the transferred body matches its frame");
    ServerCoreTest::ExpectEqual(std::size_t{ 0 }, reader.CompletedFrameCount(),
        "TakeNextFrame removes its body from the completed count");
    ServerCoreTest::ExpectEqual(std::size_t{ 0 }, reader.CompletedBodyBytes(),
        "TakeNextFrame removes its body bytes from the completed count");

    const std::vector<std::byte> later = MakeFramedStream(std::vector<std::string>{ third });
    const ServerCore::Core::Status laterAppended = reader.Append(later);
    ServerCoreTest::ExpectTrue(
        laterAppended.IsOk(), "another frame appends after ownership transfer");
    ServerCoreTest::ExpectEqual(second, ToText(owned.Value()),
        "an owned body remains valid after later FrameReader activity");
    ServerCoreTest::ExpectEqual(std::size_t{ 1 }, reader.CompletedFrameCount(),
        "a later complete frame enters the completed count");
    ServerCoreTest::ExpectEqual(third.size(), reader.CompletedBodyBytes(),
        "a later complete frame enters the completed byte count");
    reader.DiscardCompletedFrames();
    ServerCoreTest::ExpectEqual(second, ToText(owned.Value()),
        "an owned body remains valid after FrameReader discards later bodies");
    ServerCoreTest::ExpectEqual(std::size_t{ 0 }, reader.CompletedFrameCount(),
        "discarding later frames clears the completed count");
    ServerCoreTest::ExpectEqual(std::size_t{ 0 }, reader.CompletedBodyBytes(),
        "discarding later frames clears the completed byte count");

    const ServerCore::Core::Result<std::vector<std::byte>> empty = reader.TakeNextFrame();
    ServerCoreTest::ExpectTrue(!empty.IsOk() && empty.GetStatus().Code() == ErrorCode::WouldBlock,
        "TakeNextFrame reports WouldBlock when no complete body remains");
    ServerCoreTest::ExpectEqual(std::size_t{ 0 }, reader.CompletedFrameCount(),
        "TakeNextFrame WouldBlock does not change the completed count");
    ServerCoreTest::ExpectEqual(std::size_t{ 0 }, reader.CompletedBodyBytes(),
        "TakeNextFrame WouldBlock does not change the completed byte count");

    const std::string legacyCurrent = "legacy-current";
    const std::string legacyDiscarded = "legacy-discarded";
    FrameReader legacyReader;
    const std::vector<std::byte> legacyStream =
        MakeFramedStream(std::vector<std::string>{ legacyCurrent, legacyDiscarded });
    const ServerCore::Core::Status legacyAppended = legacyReader.Append(legacyStream);
    ServerCoreTest::ExpectTrue(legacyAppended.IsOk(), "legacy span discard input appends");
    if (!legacyAppended.IsOk())
    {
        return;
    }
    const ServerCore::Core::Result<std::span<const std::byte>> legacyView =
        legacyReader.NextFrame();
    ServerCoreTest::ExpectTrue(
        legacyView.IsOk(), "legacy span is available before discarding queued frames");
    if (!legacyView.IsOk())
    {
        return;
    }
    legacyReader.DiscardCompletedFrames();
    ServerCoreTest::ExpectEqual(legacyCurrent, ToText(legacyView.Value()),
        "DiscardCompletedFrames leaves an already-issued legacy span intact");
    ServerCoreTest::ExpectEqual(std::size_t{ 0 }, legacyReader.CompletedFrameCount(),
        "discarding after a legacy span clears only queued bodies");

    const std::string discard = "discard";
    const std::string preserveIncomplete = "preserve-incomplete";
    FrameReader discardReader;
    const std::vector<std::byte> discardStream =
        MakeFramedStream(std::vector<std::string>{ discard, preserveIncomplete });
    const std::size_t firstFrameBytes = ServerCore::Protocol::HeaderSize + discard.size();
    const std::size_t secondPrefixBytes = ServerCore::Protocol::HeaderSize + std::size_t{ 2 };
    const std::size_t prefixBytes = firstFrameBytes + secondPrefixBytes;
    const ServerCore::Core::Status prefixAppended =
        discardReader.Append(std::span<const std::byte>(discardStream).first(prefixBytes));
    ServerCoreTest::ExpectTrue(
        prefixAppended.IsOk(), "one complete and one incomplete body append before discard");
    if (!prefixAppended.IsOk())
    {
        return;
    }

    ServerCoreTest::ExpectEqual(std::size_t{ 1 }, discardReader.CompletedFrameCount(),
        "discard reader exposes only the completed body");
    ServerCoreTest::ExpectEqual(discard.size(), discardReader.CompletedBodyBytes(),
        "discard reader excludes incomplete body bytes from its counter");
    discardReader.DiscardCompletedFrames();
    ServerCoreTest::ExpectEqual(std::size_t{ 0 }, discardReader.CompletedFrameCount(),
        "DiscardCompletedFrames clears the completed count");
    ServerCoreTest::ExpectEqual(std::size_t{ 0 }, discardReader.CompletedBodyBytes(),
        "DiscardCompletedFrames clears the completed byte count");

    const ServerCore::Core::Status suffixAppended =
        discardReader.Append(std::span<const std::byte>(discardStream).subspan(prefixBytes));
    ServerCoreTest::ExpectTrue(suffixAppended.IsOk(),
        "discarding completed frames leaves an incomplete frame available to finish");
    if (!suffixAppended.IsOk())
    {
        return;
    }

    const ServerCore::Core::Result<std::vector<std::byte>> preserved =
        discardReader.TakeNextFrame();
    ServerCoreTest::ExpectTrue(preserved.IsOk(), "the incomplete frame completes after discard");
    if (preserved.IsOk())
    {
        ServerCoreTest::ExpectEqual(preserveIncomplete, ToText(preserved.Value()),
            "DiscardCompletedFrames does not discard an incomplete frame");
    }
    ServerCoreTest::ExpectTrue(discardReader.Finish().IsOk(),
        "the discard reader has no incomplete bytes after its preserved frame is taken");

    // admission은 FrameReader가 vector를 만들기 전에 호출된다. 따라서 세 번째 body는 큐에
    // 잠깐이라도 들어가지 않고, 동일 Append 안의 뒤 body도 더 복사하지 않는다.
    FrameReader admissionReader;
    FrameAdmissionBudget budget{ .remainingBytes = first.size() + second.size(),
        .remainingFrames = 2 };
    const std::vector<std::byte> admissionStream =
        MakeFramedStream(std::vector<std::string>{ first, second, third });
    const ServerCore::Core::Status admissionAppended =
        admissionReader.Append(admissionStream, &budget, &AdmitFrameBody);
    ServerCoreTest::ExpectEqual(static_cast<int>(ErrorCode::TooLarge),
        static_cast<int>(admissionAppended.Code()),
        "a completed-frame admission rejection is reported as TooLarge");
    ServerCoreTest::ExpectEqual(std::size_t{ 2 }, budget.admittedFrames,
        "admission permits only the configured number of bodies before copying");
    ServerCoreTest::ExpectEqual(first.size() + second.size(), budget.admittedBytes,
        "admission accounts only bodies copied into the completed queue");
    ServerCoreTest::ExpectEqual(std::size_t{ 1 }, budget.rejectedFrames,
        "admission sees the first body that does not fit its budget");
    ServerCoreTest::ExpectEqual(std::size_t{ 2 }, admissionReader.CompletedFrameCount(),
        "a rejected body is never added to the completed frame queue");
    ServerCoreTest::ExpectEqual(first.size() + second.size(), admissionReader.CompletedBodyBytes(),
        "a rejected body is never added to completed byte accounting");
    const ServerCore::Core::Status postRejectionAppend = admissionReader.Append({});
    ServerCoreTest::ExpectEqual(static_cast<int>(ErrorCode::TooLarge),
        static_cast<int>(postRejectionAppend.Code()),
        "admission rejection makes later Append calls terminal too");
    ServerCoreTest::ExpectEqual(static_cast<int>(ErrorCode::TooLarge),
        static_cast<int>(admissionReader.Finish().Code()),
        "admission rejection makes Finish report the terminal limit error");
    const ServerCore::Core::Result<std::vector<std::byte>> admittedFirst =
        admissionReader.TakeNextFrame();
    const ServerCore::Core::Result<std::vector<std::byte>> admittedSecond =
        admissionReader.TakeNextFrame();
    ServerCoreTest::ExpectTrue(admittedFirst.IsOk() && admittedSecond.IsOk(),
        "bodies admitted before a later rejection remain available in order");
    if (admittedFirst.IsOk() && admittedSecond.IsOk())
    {
        ServerCoreTest::ExpectEqual(first, ToText(admittedFirst.Value()),
            "the first admitted body retains its content after rejection");
        ServerCoreTest::ExpectEqual(second, ToText(admittedSecond.Value()),
            "the second admitted body retains its content after rejection");
    }

    // body byte가 0이어도 task 하나는 예약해야 한다. 그렇지 않으면 빈 frame만으로 deque/task가
    // 상한 없이 늘 수 있다.
    FrameReader zeroBodyReader;
    FrameAdmissionBudget zeroBodyBudget{ .remainingBytes = 0, .remainingFrames = 1 };
    const std::vector<std::byte> zeroBodyStream =
        MakeFramedStream(std::vector<std::string>{ {}, {} });
    const ServerCore::Core::Status zeroBodyAppended =
        zeroBodyReader.Append(zeroBodyStream, &zeroBodyBudget, &AdmitFrameBody);
    ServerCoreTest::ExpectEqual(static_cast<int>(ErrorCode::TooLarge),
        static_cast<int>(zeroBodyAppended.Code()),
        "an empty body still consumes one admission task");
    ServerCoreTest::ExpectEqual(std::size_t{ 1 }, zeroBodyBudget.admittedFrames,
        "the empty first body consumed the only admission task");
    ServerCoreTest::ExpectEqual(std::size_t{ 0 }, zeroBodyBudget.admittedBytes,
        "empty bodies do not consume byte admission budget");
    ServerCoreTest::ExpectEqual(std::size_t{ 1 }, zeroBodyReader.CompletedFrameCount(),
        "the second empty body is rejected by task count before queue copy");
    ServerCoreTest::ExpectEqual(std::size_t{ 0 }, zeroBodyReader.CompletedBodyBytes(),
        "empty body queue accounting remains zero bytes while task count stays bounded");
}

void FrameReaderReleaseStorageResetsReader()
{
    const std::string completed = "completed";
    const std::string incomplete = "incomplete";
    const std::vector<std::byte> stream =
        MakeFramedStream(std::vector<std::string>{ completed, incomplete });
    const std::size_t prefixSize = ServerCore::Protocol::HeaderSize + completed.size() +
                                   ServerCore::Protocol::HeaderSize + std::size_t{ 2 };

    FrameReader reader;
    const ServerCore::Core::Status appended =
        reader.Append(std::span<const std::byte>(stream).first(prefixSize));
    ServerCoreTest::ExpectTrue(
        appended.IsOk(), "complete and incomplete frames append before storage release");
    if (!appended.IsOk())
    {
        return;
    }

    ServerCoreTest::ExpectEqual(std::size_t{ 1 }, reader.CompletedFrameCount(),
        "the completed body is queued before storage release");
    ServerCoreTest::ExpectTrue(
        reader.HasIncompleteFrame(), "the partial body is retained before storage release");

    reader.ReleaseStorage();

    ServerCoreTest::ExpectEqual(
        std::size_t{ 0 }, reader.CompletedFrameCount(), "ReleaseStorage() clears completed bodies");
    ServerCoreTest::ExpectEqual(std::size_t{ 0 }, reader.CompletedBodyBytes(),
        "ReleaseStorage() clears completed body accounting");
    ServerCoreTest::ExpectTrue(
        !reader.HasIncompleteFrame(), "ReleaseStorage() clears an incomplete body");
    ServerCoreTest::ExpectTrue(
        reader.Finish().IsOk(), "ReleaseStorage() leaves an empty finished reader");

    const std::string reusedBody = "reused";
    const std::vector<std::byte> reusedStream =
        MakeFramedStream(std::vector<std::string>{ reusedBody });
    const ServerCore::Core::Status reused = reader.Append(reusedStream);
    ServerCoreTest::ExpectTrue(
        reused.IsOk(), "Append() prepares storage again after ReleaseStorage()");
    if (!reused.IsOk())
    {
        return;
    }

    const ServerCore::Core::Result<std::vector<std::byte>> body = reader.TakeNextFrame();
    ServerCoreTest::ExpectTrue(body.IsOk(), "the reused reader returns its complete body");
    if (body.IsOk())
    {
        ServerCoreTest::ExpectEqual(
            reusedBody, ToText(body.Value()), "the reused reader preserves the new body");
    }
}

void FrameCodecSharedApiIsNoThrow()
{
    using SharedReader = ServerCore::Protocol::FrameCodec::Reader;
    using SharedCallback = ServerCore::Protocol::FrameCodec::FrameCallback;
    static_assert(!std::is_copy_constructible_v<FrameReader>);
    static_assert(!std::is_copy_assignable_v<FrameReader>);
    static_assert(!std::is_move_constructible_v<FrameReader>);
    static_assert(!std::is_move_assignable_v<FrameReader>);
    static_assert(noexcept(SharedReader(std::declval<std::span<std::byte>>(),
        ServerCore::Protocol::FrameCodec::DefaultMaxBodySize)));
    static_assert(
        noexcept(std::declval<SharedReader&>().Append(std::declval<std::span<const std::byte>>(),
            nullptr, static_cast<SharedCallback>(nullptr))));
    static_assert(noexcept(ServerCore::Protocol::FrameCodec::EncodeTo(
        std::declval<std::span<const std::byte>>(), std::declval<std::span<std::byte>>(),
        ServerCore::Protocol::FrameCodec::DefaultMaxBodySize)));

    std::array<std::byte, ServerCore::Protocol::FrameCodec::HeaderSize + 16> storage{};
    SharedReader reader(storage, 16);
    FrameCollector collector;

    const std::array<std::byte, 1> firstBody{ static_cast<std::byte>('a') };
    const std::array<std::byte, 2> secondBody{ static_cast<std::byte>('b'),
        static_cast<std::byte>('c') };
    std::array<std::byte, ServerCore::Protocol::FrameCodec::HeaderSize + firstBody.size()> first{};
    std::array<std::byte, ServerCore::Protocol::FrameCodec::HeaderSize + secondBody.size()>
        second{};
    const ServerCore::Protocol::FrameCodec::EncodeResult encodedFirst =
        ServerCore::Protocol::FrameCodec::EncodeTo(firstBody, first);
    const ServerCore::Protocol::FrameCodec::EncodeResult encodedSecond =
        ServerCore::Protocol::FrameCodec::EncodeTo(secondBody, second);
    ServerCoreTest::ExpectTrue(encodedFirst.IsOk() && encodedSecond.IsOk(),
        "the shared no-throw codec encodes into caller storage");

    std::array<std::byte, first.size() + second.size()> stream{};
    for (std::size_t index = 0; index < first.size(); ++index)
    {
        stream[index] = first[index];
    }
    for (std::size_t index = 0; index < second.size(); ++index)
    {
        stream[first.size() + index] = second[index];
    }

    const ServerCore::Protocol::FrameCodec::AppendResult head =
        reader.Append(std::span<const std::byte>(stream).first(2), &collector, &CollectSharedFrame);
    const ServerCore::Protocol::FrameCodec::AppendResult tail = reader.Append(
        std::span<const std::byte>(stream).subspan(2), &collector, &CollectSharedFrame);
    ServerCoreTest::ExpectTrue(
        head.IsOk() && head.consumed == 2, "the shared codec preserves a split length header");
    ServerCoreTest::ExpectTrue(tail.IsOk() && tail.consumed == stream.size() - 2,
        "the shared codec consumes coalesced frame bytes");
    ServerCoreTest::ExpectTrue(!collector.overflowed && collector.count == 2,
        "the shared codec invokes the callback once per complete frame");
    ServerCoreTest::ExpectEqual(std::string("a"),
        ToText(std::span<const std::byte>(collector.bodies[0].data(), collector.lengths[0])),
        "the first shared callback body matches");
    ServerCoreTest::ExpectEqual(std::string("bc"),
        ToText(std::span<const std::byte>(collector.bodies[1].data(), collector.lengths[1])),
        "the second shared callback body matches");
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Protocol::FrameCodec::Error::None),
        static_cast<int>(reader.Finish()), "the shared codec leaves no incomplete bytes");

    std::array<std::byte, ServerCore::Protocol::FrameCodec::HeaderSize> tooSmall{};
    const ServerCore::Protocol::FrameCodec::EncodeResult shortOutput =
        ServerCore::Protocol::FrameCodec::EncodeTo(firstBody, tooSmall);
    ServerCoreTest::ExpectTrue(
        !shortOutput.IsOk() &&
            shortOutput.error == ServerCore::Protocol::FrameCodec::Error::InsufficientStorage &&
            shortOutput.required == first.size(),
        "the shared codec reports caller output storage that is too small");
}

void MessagePreservesEnvelopeFields()
{
    const std::string source = R"({"type":"Move","body": { "x": 1 },"seq":["request",2]})";
    const ServerCore::Core::Result<ServerCore::Protocol::Message> parsed =
        ServerCore::Protocol::ParseMessage(ToBytes(source));
    ServerCoreTest::ExpectTrue(parsed.IsOk(), "message with body and seq parses");
    if (!parsed.IsOk())
    {
        return;
    }

    const ServerCore::Protocol::Message& message = parsed.Value();
    ServerCoreTest::ExpectEqual(
        std::string("Move"), std::string(message.Type()), "message type is preserved");
    ServerCoreTest::ExpectTrue(message.Body() != nullptr, "message body exists");
    ServerCoreTest::ExpectTrue(message.Sequence() != nullptr && message.Sequence()->IsArray(),
        "message sequence keeps its JSON type");
    ServerCoreTest::ExpectEqual(std::string("{ \"x\": 1 }").size(), message.RawBodySize(),
        "raw body size keeps wire whitespace");

    JsonValue::Object errorObject;
    errorObject.emplace("code", JsonValue(std::string("TooLarge")));
    errorObject.emplace("message", JsonValue(std::string("too large")));
    const JsonValue error(std::move(errorObject));
    const JsonValue sequence(std::string("same-token"));
    const ServerCore::Core::Result<std::vector<std::byte>> serialized =
        ServerCore::Protocol::SerializeMessage(
            MessageFields{ "Rejected", nullptr, &sequence, &error });
    ServerCoreTest::ExpectTrue(serialized.IsOk(), "error-only envelope serializes");
    if (!serialized.IsOk())
    {
        return;
    }

    const ServerCore::Core::Result<ServerCore::Protocol::Message> reparsed =
        ServerCore::Protocol::ParseMessage(serialized.Value());
    ServerCoreTest::ExpectTrue(reparsed.IsOk(), "error-only envelope parses");
    if (reparsed.IsOk())
    {
        ServerCoreTest::ExpectTrue(
            reparsed.Value().Body() == nullptr, "error-only envelope omits body");
        ServerCoreTest::ExpectTrue(
            reparsed.Value().Error() != nullptr && reparsed.Value().Error()->IsObject(),
            "error-only envelope preserves error");
        ServerCoreTest::ExpectTrue(reparsed.Value().Sequence() != nullptr &&
                                       reparsed.Value().Sequence()->TryString() != nullptr &&
                                       *reparsed.Value().Sequence()->TryString() == "same-token",
            "sequence is copied without a numeric restriction");
    }
}

void MessagePreserves64BitIntegers()
{
    constexpr std::int64_t MinimumSigned = (std::numeric_limits<std::int64_t>::min)();
    constexpr std::uint64_t MaximumUnsigned = (std::numeric_limits<std::uint64_t>::max)();
    constexpr std::uint64_t Sequence = 9007199254740993ULL;
    const std::string source =
        R"({"type":"ExactNumbers","body":{"signed":-9223372036854775808,"unsigned":18446744073709551615},"seq":9007199254740993})";

    const ServerCore::Core::Result<ServerCore::Protocol::Message> parsed =
        ServerCore::Protocol::ParseMessage(ToBytes(source));
    ServerCoreTest::ExpectTrue(parsed.IsOk(), "a message with 64-bit integers parses");
    if (!parsed.IsOk())
    {
        return;
    }

    const ServerCore::Protocol::Message& message = parsed.Value();
    const JsonValue* const body = message.Body();
    const JsonValue* const signedValue = body == nullptr ? nullptr : body->Find("signed");
    const JsonValue* const unsignedValue = body == nullptr ? nullptr : body->Find("unsigned");
    const JsonValue* const sequence = message.Sequence();
    ServerCoreTest::ExpectTrue(
        signedValue != nullptr && signedValue->IsNumber() && signedValue->TryInt64() != nullptr,
        "the smallest signed 64-bit body value remains an integer");
    if (signedValue != nullptr && signedValue->TryInt64() != nullptr)
    {
        ServerCoreTest::ExpectEqual(
            MinimumSigned, *signedValue->TryInt64(), "the signed body integer is exact");
    }
    ServerCoreTest::ExpectTrue(unsignedValue != nullptr && unsignedValue->IsNumber() &&
                                   unsignedValue->TryUInt64() != nullptr,
        "the largest unsigned 64-bit body value remains an integer");
    if (unsignedValue != nullptr && unsignedValue->TryUInt64() != nullptr)
    {
        ServerCoreTest::ExpectEqual(
            MaximumUnsigned, *unsignedValue->TryUInt64(), "the unsigned body integer is exact");
    }
    ServerCoreTest::ExpectTrue(
        sequence != nullptr && sequence->TryUInt64() != nullptr && sequence->TryNumber() != nullptr,
        "a large integer sequence remains exact while retaining the double accessor");
    if (sequence != nullptr && sequence->TryUInt64() != nullptr)
    {
        ServerCoreTest::ExpectEqual(
            Sequence, *sequence->TryUInt64(), "the sequence integer is exact");
    }

    const ServerCore::Core::Result<std::vector<std::byte>> serialized =
        ServerCore::Protocol::SerializeMessage(
            MessageFields{ message.Type(), body, sequence, message.Error() });
    ServerCoreTest::ExpectTrue(serialized.IsOk(), "64-bit integer fields serialize");
    if (!serialized.IsOk())
    {
        return;
    }

    const ServerCore::Core::Result<ServerCore::Protocol::Message> reparsed =
        ServerCore::Protocol::ParseMessage(serialized.Value());
    ServerCoreTest::ExpectTrue(reparsed.IsOk(), "serialized 64-bit integer fields parse again");
    if (!reparsed.IsOk())
    {
        return;
    }

    const JsonValue* const reparsedBody = reparsed.Value().Body();
    const JsonValue* const reparsedSigned =
        reparsedBody == nullptr ? nullptr : reparsedBody->Find("signed");
    const JsonValue* const reparsedUnsigned =
        reparsedBody == nullptr ? nullptr : reparsedBody->Find("unsigned");
    const JsonValue* const reparsedSequence = reparsed.Value().Sequence();
    ServerCoreTest::ExpectTrue(reparsedSigned != nullptr && reparsedSigned->TryInt64() != nullptr &&
                                   *reparsedSigned->TryInt64() == MinimumSigned,
        "the signed body integer survives a message round trip");
    ServerCoreTest::ExpectTrue(reparsedUnsigned != nullptr &&
                                   reparsedUnsigned->TryUInt64() != nullptr &&
                                   *reparsedUnsigned->TryUInt64() == MaximumUnsigned,
        "the unsigned body integer survives a message round trip");
    ServerCoreTest::ExpectTrue(reparsedSequence != nullptr &&
                                   reparsedSequence->TryUInt64() != nullptr &&
                                   *reparsedSequence->TryUInt64() == Sequence,
        "the sequence integer survives a message round trip");
}

void JsonRejectsUnrepresentableNumbers()
{
    constexpr std::array<std::string_view, 3> invalidNumbers{ "18446744073709551616",
        "-9223372036854775809", "1e999" };
    for (const std::string_view text : invalidNumbers)
    {
        const ServerCore::Core::Result<JsonValue> parsed = JsonValue::Parse(text);
        ServerCoreTest::ExpectTrue(
            !parsed.IsOk() && parsed.GetStatus().Code() == ErrorCode::InvalidFormat,
            "a number outside the supported exact integer or finite double range is rejected");
    }
}

/// <summary>값 수 상한이 스칼라·컨테이너·객체 멤버 값을 모두 세고, 넘으면 TooLarge인지 본다.</summary>
void JsonParseLimitsValueCount()
{
    using ServerCore::Protocol::JsonParseLimits;
    const auto code = [](std::string_view text, std::size_t maxValues)
    {
        return static_cast<int>(
            JsonValue::Parse(text, JsonParseLimits{ maxValues }).GetStatus().Code());
    };
    const int ok = static_cast<int>(ServerCore::Core::ErrorCode::Ok);
    const int tooLarge = static_cast<int>(ServerCore::Core::ErrorCode::TooLarge);
    // [0,0,0]은 배열 하나와 숫자 셋, {"a":1,"b":[2]}는 객체·1·배열·2의 넷이다.
    ServerCoreTest::ExpectEqual(
        ok, code("[0,0,0]", 4), "an array and its three items fit four values");
    ServerCoreTest::ExpectEqual(
        tooLarge, code("[0,0,0]", 3), "an array and its three items exceed three values");
    ServerCoreTest::ExpectEqual(
        ok, code(R"({"a":1,"b":[2]})", 4), "object member values count, keys do not");
    ServerCoreTest::ExpectEqual(
        tooLarge, code(R"({"a":1,"b":[2]})", 3), "nested member values count toward the limit");
    ServerCoreTest::ExpectEqual(ok, code("[0,0,0]", (std::numeric_limits<std::size_t>::max)()),
        "the default limit is unlimited");
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::InvalidFormat),
        code("[0,", 1000), "malformed input under the limit keeps InvalidFormat");

    const std::string envelope = R"({"type":"t","body":{"a":[1,2,3]}})";
    const auto bytes = std::as_bytes(std::span(envelope.data(), envelope.size()));
    // 최상위 봉투 객체는 세지 않는다. type 값, body 객체, 배열, 숫자 셋으로 여섯이다.
    ServerCoreTest::ExpectTrue(
        ServerCore::Protocol::ParseMessage(bytes, JsonParseLimits{ 6 }).IsOk(),
        "a message within its value limit parses");
    ServerCoreTest::ExpectEqual(tooLarge,
        static_cast<int>(
            ServerCore::Protocol::ParseMessage(bytes, JsonParseLimits{ 5 }).GetStatus().Code()),
        "a message over its value limit is TooLarge");
}

void JsonNumbersUnderflowToSignedZero()
{
    struct Underflow
    {
        std::string_view text;
        bool negative;
    };
    constexpr std::array<Underflow, 6> underflows{ { { "1e-400", false }, { "-1e-400", true },
        { "123e-400", false }, { "0.000001e-320", false }, { "1000000000000000000000e-500", false },
        { "-1e-99999999999999999999", true } } };
    for (const Underflow& underflow : underflows)
    {
        const auto parsed = JsonValue::Parse(underflow.text);
        const double* const number = parsed.IsOk() ? parsed.Value().TryNumber() : nullptr;
        ServerCoreTest::ExpectTrue(
            number != nullptr && *number == 0.0 && std::signbit(*number) == underflow.negative,
            "a number too small for a double parses as zero with its sign");
    }

    const auto subnormal = JsonValue::Parse("4.9e-324");
    ServerCoreTest::ExpectTrue(
        subnormal.IsOk() && subnormal.Value().TryNumber() != nullptr &&
            *subnormal.Value().TryNumber() == std::numeric_limits<double>::denorm_min(),
        "the smallest subnormal double keeps its value");

    constexpr std::array<std::string_view, 4> overflows{ "1e400", "-1e400", "0.00001e320",
        "1e99999999999999999999" };
    for (const std::string_view text : overflows)
    {
        const auto parsed = JsonValue::Parse(text);
        ServerCoreTest::ExpectTrue(
            !parsed.IsOk() && parsed.GetStatus().Code() == ErrorCode::InvalidFormat,
            "a number too large for a finite double is still rejected");
    }
}

void MessageRejectsInvalidUtf8()
{
    const std::vector<std::byte> invalidUtf8{ static_cast<std::byte>('{'),
        static_cast<std::byte>('"'), static_cast<std::byte>('t'), static_cast<std::byte>('y'),
        static_cast<std::byte>('p'), static_cast<std::byte>('e'), static_cast<std::byte>('"'),
        static_cast<std::byte>(':'), static_cast<std::byte>('"'), static_cast<std::byte>(0xC3),
        static_cast<std::byte>('"'), static_cast<std::byte>('}') };
    const ServerCore::Core::Result<ServerCore::Protocol::Message> parsed =
        ServerCore::Protocol::ParseMessage(invalidUtf8);
    ServerCoreTest::ExpectTrue(
        !parsed.IsOk() && parsed.GetStatus().Code() == ErrorCode::InvalidFormat,
        "invalid UTF-8 does not escape as an exception");
}

void JsonEntryPointsShareValidation()
{
    const std::array<std::string, 8> invalidDocuments{ R"({"type":"Probe","body":{})",
        R"({"type":"Probe","body":{}} false)", R"({"type":"Probe","body":{},"body":{}})",
        R"({"type":"Probe","body":{"value":1,}})", R"({"type":"Probe","body":{"value":"\uD800"}})",
        R"({"type":"Probe","body":{"value":18446744073709551616}})",
        std::string(R"({"type":"Probe","body":{"value":")") + "\xC3\"}}",
        std::string(R"({"type":"Probe","body":{"value":)") + std::string(256, '[') + "0" +
            std::string(256, ']') + "}}" };
    for (const auto& text : invalidDocuments)
    {
        const auto json = JsonValue::Parse(text);
        const auto bytes = ToBytes(text);
        const auto byteJson = JsonValue::ParseBytes(bytes);
        const auto envelope = ServerCore::Protocol::ParseMessage(bytes);
        ServerCoreTest::ExpectTrue(!json.IsOk() && !byteJson.IsOk() && !envelope.IsOk() &&
                                       json.GetStatus().Code() == ErrorCode::InvalidFormat &&
                                       byteJson.GetStatus().Code() == ErrorCode::InvalidFormat &&
                                       envelope.GetStatus().Code() == ErrorCode::InvalidFormat,
            "JSON and message entry points reject the same malformed document");
        ServerCoreTest::ExpectEqual(json.GetStatus().Message(), envelope.GetStatus().Message(),
            "both entry points preserve the same JSON error location and cause");
        ServerCoreTest::ExpectEqual(json.GetStatus().Message(), byteJson.GetStatus().Message(),
            "the byte-oriented JSON entry point preserves the same diagnostic");
    }

    const std::string text = "\xEF\xBB\xBF \n{\"type\":\"Probe\",\"body\":{ \"x\" : 1 }}\r\n";
    const auto json = JsonValue::Parse(text);
    const auto envelope = ServerCore::Protocol::ParseMessage(ToBytes(text));
    ServerCoreTest::ExpectTrue(json.IsOk() && envelope.IsOk(),
        "both entry points accept BOM and surrounding JSON whitespace");
    if (envelope.IsOk())
        ServerCoreTest::ExpectEqual(std::size_t{ 11 }, envelope.Value().RawBodySize(),
            "shared document parsing retains original body bytes including whitespace");
}

void MessageEnvelopeValidationIsSymmetric()
{
    const JsonValue object(JsonValue::Object{});
    const JsonValue scalar(1.0);
    const JsonValue nullValue(nullptr);
    const JsonValue error(JsonValue::Object{ { "code", JsonValue(std::string("test")) } });
    struct Example
    {
        MessageFields fields;
        bool valid;
    };
    const std::array<Example, 8> examples{ { { { "Probe", &object, nullptr, nullptr }, true },
        { { "Probe", nullptr, &scalar, &error }, true },
        { { "Probe", &object, &nullValue, &error }, true },
        { { "", &object, nullptr, nullptr }, false },
        { { "Probe", nullptr, nullptr, nullptr }, false },
        { { "Probe", &scalar, nullptr, &error }, false },
        { { "Probe", &object, nullptr, &object }, false },
        { { "Probe", &object, nullptr, &nullValue }, false } } };
    for (const auto& example : examples)
    {
        const auto& fields = example.fields;
        JsonValue::Object raw{ { "type", JsonValue(std::string(fields.type)) } };
        if (fields.body)
            raw.emplace("body", *fields.body);
        if (fields.sequence)
            raw.emplace("seq", *fields.sequence);
        if (fields.error)
            raw.emplace("error", *fields.error);
        const auto wire = JsonValue(std::move(raw)).Dump();
        ServerCoreTest::ExpectTrue(wire.IsOk(), "envelope examples contain valid JSON values");
        if (!wire.IsOk())
            continue;
        const auto incoming = ServerCore::Protocol::ParseMessage(ToBytes(wire.Value()));
        const auto outgoing = ServerCore::Protocol::SerializeMessage(fields);
        ServerCoreTest::ExpectTrue(
            incoming.IsOk() == example.valid && outgoing.IsOk() == example.valid,
            "incoming and outgoing envelopes enforce the same field rules");
        if (!example.valid)
            ServerCoreTest::ExpectTrue(
                incoming.GetStatus().Code() == ErrorCode::InvalidFormat &&
                    outgoing.GetStatus().Code() == ErrorCode::InvalidArgument,
                "wire format errors remain distinct from invalid caller arguments");
    }
}

void JsonDumpRejectsUnsafeValues()
{
    JsonValue::Object validObject;
    validObject.emplace("message", JsonValue(std::string("line one\nline two")));
    validObject.emplace("answer", JsonValue(42.0));
    const ServerCore::Core::Result<std::string> validDump =
        JsonValue(std::move(validObject)).Dump();
    ServerCoreTest::ExpectTrue(validDump.IsOk(), "Dump() writes ordinary values");
    if (validDump.IsOk())
    {
        const ServerCore::Core::Result<JsonValue> reparsed = JsonValue::Parse(validDump.Value());
        ServerCoreTest::ExpectTrue(
            reparsed.IsOk(), "a successful Dump() result parses as UTF-8 JSON");
    }

    const double unsignedBoundary = std::ldexp(1.0, 64);
    const double signedBoundary = -std::ldexp(1.0, 63);
    const std::array<double, 18> finiteNumbers{ 0.0, -0.0, 42.0, -42.0, 12.5, -12.5, 1.0 / 3.0,
        std::nextafter(unsignedBoundary, 0.0), unsignedBoundary,
        std::nextafter(unsignedBoundary, std::numeric_limits<double>::infinity()),
        std::nextafter(signedBoundary, 0.0), signedBoundary,
        std::nextafter(signedBoundary, -std::numeric_limits<double>::infinity()),
        (std::numeric_limits<double>::max)(), -(std::numeric_limits<double>::max)(),
        (std::numeric_limits<double>::min)(), std::numeric_limits<double>::denorm_min(),
        -std::numeric_limits<double>::denorm_min() };
    for (const double number : finiteNumbers)
    {
        const ServerCore::Core::Result<std::string> dumped = JsonValue(number).Dump();
        ServerCoreTest::ExpectTrue(dumped.IsOk(), "every finite double can be dumped");
        if (!dumped.IsOk())
        {
            continue;
        }
        const ServerCore::Core::Result<JsonValue> reparsed = JsonValue::Parse(dumped.Value());
        ServerCoreTest::ExpectTrue(reparsed.IsOk(),
            "dumped doubles parse even at and beyond signed/unsigned integer boundaries");
        if (reparsed.IsOk())
        {
            const double* const restored = reparsed.Value().TryNumber();
            ServerCoreTest::ExpectTrue(restored != nullptr && *restored == number &&
                                           std::signbit(*restored) == std::signbit(number),
                "double round trips preserve the exact value, including negative zero");
        }
    }

    // Exercise received exponent tokens through the reply path, not only directly built values.
    const std::string source =
        R"({"type":"Echo","body":{"positive":1.8446744073709552e19,"negative":-9.223372036854778e18}})";
    const ServerCore::Core::Result<ServerCore::Protocol::Message> incoming =
        ServerCore::Protocol::ParseMessage(ToBytes(source));
    ServerCoreTest::ExpectTrue(incoming.IsOk(), "finite doubles beyond integer bounds arrive");
    if (incoming.IsOk())
    {
        const ServerCore::Core::Result<std::vector<std::byte>> reply =
            ServerCore::Protocol::SerializeMessage(
                incoming.Value().Type(), *incoming.Value().Body());
        ServerCoreTest::ExpectTrue(reply.IsOk(), "received out-of-integer-range doubles serialize");
        if (reply.IsOk())
        {
            const ServerCore::Core::Result<ServerCore::Protocol::Message> reparsed =
                ServerCore::Protocol::ParseMessage(reply.Value());
            ServerCoreTest::ExpectTrue(reparsed.IsOk(),
                "a peer using the same protocol can parse the echoed finite doubles");
        }
    }

    const JsonValue invalidUtf8(std::string(1, static_cast<char>(0xC3)));
    const ServerCore::Core::Result<std::string> invalidUtf8Dump = invalidUtf8.Dump();
    ServerCoreTest::ExpectTrue(
        !invalidUtf8Dump.IsOk() && invalidUtf8Dump.GetStatus().Code() == ErrorCode::InvalidArgument,
        "Dump() rejects a manually constructed invalid UTF-8 string");

    const JsonValue nonFinite(std::numeric_limits<double>::quiet_NaN());
    const ServerCore::Core::Result<std::string> nonFiniteDump = nonFinite.Dump();
    ServerCoreTest::ExpectTrue(
        !nonFiniteDump.IsOk() && nonFiniteDump.GetStatus().Code() == ErrorCode::InvalidArgument,
        "Dump() rejects a non-finite number instead of changing its value");

    JsonValue::Object invalidKeyObject;
    invalidKeyObject.emplace(std::string(1, static_cast<char>(0xC3)), JsonValue(nullptr));
    const ServerCore::Core::Result<std::string> invalidKeyDump =
        JsonValue(std::move(invalidKeyObject)).Dump();
    ServerCoreTest::ExpectTrue(
        !invalidKeyDump.IsOk() && invalidKeyDump.GetStatus().Code() == ErrorCode::InvalidArgument,
        "Dump() rejects an invalid UTF-8 object key");

    const JsonValue nestedInvalid(
        JsonValue::Array{ JsonValue(std::string(1, static_cast<char>(0xC3))) });
    const ServerCore::Core::Result<std::string> nestedDump = nestedInvalid.Dump();
    ServerCoreTest::ExpectTrue(
        !nestedDump.IsOk() && nestedDump.GetStatus().Code() == ErrorCode::InvalidArgument,
        "Dump() checks strings recursively");

    JsonValue deeplyNested(nullptr);
    for (std::size_t depth = 0; depth <= 256; ++depth)
    {
        JsonValue::Array parent;
        parent.emplace_back(std::move(deeplyNested));
        deeplyNested = JsonValue(std::move(parent));
    }
    const ServerCore::Core::Result<std::string> deepDump = deeplyNested.Dump();
    ServerCoreTest::ExpectTrue(
        !deepDump.IsOk() && deepDump.GetStatus().Code() == ErrorCode::InvalidArgument,
        "Dump() enforces the parser's nesting limit for directly constructed JSON values");
}

void MessageRejectsUnsafeOutboundJson()
{
    std::string invalidString;
    invalidString.push_back(static_cast<char>(0xC3));
    const JsonValue invalidUtf8Value(std::move(invalidString));
    const ServerCore::Core::Result<std::vector<std::byte>> invalidUtf8 =
        ServerCore::Protocol::SerializeMessage(
            MessageFields{ "Reply", &invalidUtf8Value, nullptr, nullptr });
    ServerCoreTest::ExpectTrue(
        !invalidUtf8.IsOk() && invalidUtf8.GetStatus().Code() == ErrorCode::InvalidArgument,
        "serialization rejects a manually constructed invalid UTF-8 string");

    const JsonValue nonFinite(std::numeric_limits<double>::infinity());
    const ServerCore::Core::Result<std::vector<std::byte>> invalidNumber =
        ServerCore::Protocol::SerializeMessage(
            MessageFields{ "Reply", &nonFinite, nullptr, nullptr });
    ServerCoreTest::ExpectTrue(
        !invalidNumber.IsOk() && invalidNumber.GetStatus().Code() == ErrorCode::InvalidArgument,
        "serialization rejects a manually constructed non-finite number");
}

void MessageRejectsNonObjectBodies()
{
    const std::array<std::string_view, 3> invalidIncoming{ R"({"type":"Probe","body":null})",
        R"({"type":"Probe","body":[1]})", R"({"type":"Probe","body":1})" };
    for (const std::string_view source : invalidIncoming)
    {
        const ServerCore::Core::Result<ServerCore::Protocol::Message> parsed =
            ServerCore::Protocol::ParseMessage(ToBytes(source));
        ServerCoreTest::ExpectTrue(
            !parsed.IsOk() && parsed.GetStatus().Code() == ErrorCode::InvalidFormat,
            "a non-object incoming message body is rejected at the envelope boundary");
    }

    const JsonValue scalar(1.0);
    const JsonValue nullValue(nullptr);
    const JsonValue array(JsonValue::Array{ JsonValue(1.0) });
    const std::array<const JsonValue*, 3> invalidOutgoing{ &scalar, &nullValue, &array };
    for (const JsonValue* const value : invalidOutgoing)
    {
        const ServerCore::Core::Result<std::vector<std::byte>> serialized =
            ServerCore::Protocol::SerializeMessage(
                MessageFields{ "Probe", value, nullptr, nullptr });
        ServerCoreTest::ExpectTrue(
            !serialized.IsOk() && serialized.GetStatus().Code() == ErrorCode::InvalidArgument,
            "a non-object outgoing message body is rejected at the envelope boundary");
    }
}

void JsonTextConstructorsKeepTheirType()
{
    // 문자 하나와 문자열이 아닌 포인터는 숫자나 bool로 조용히 바뀌지 않고 구성 자체가 막힌다.
    // 막힘이 풀리면 이 단언들이 이 파일의 컴파일을 멈춘다.
    static_assert(!std::is_constructible_v<JsonValue, char>);
    static_assert(!std::is_constructible_v<JsonValue, wchar_t>);
    static_assert(!std::is_constructible_v<JsonValue, char8_t>);
    static_assert(!std::is_constructible_v<JsonValue, char16_t>);
    static_assert(!std::is_constructible_v<JsonValue, char32_t>);
    static_assert(!std::is_constructible_v<JsonValue, const int*>);
    static_assert(!std::is_constructible_v<JsonValue, void*>);
    static_assert(!std::is_constructible_v<JsonValue, const char8_t*>);
    static_assert(!std::is_constructible_v<JsonValue, volatile char*>);
    // std::int8_t와 std::uint8_t는 문자가 아니라 정수로 남는다.
    static_assert(std::is_constructible_v<JsonValue, signed char>);
    static_assert(std::is_constructible_v<JsonValue, unsigned char>);
    static_assert(std::is_constructible_v<JsonValue, std::nullptr_t>);

    const JsonValue literal("ok");
    ServerCoreTest::ExpectTrue(literal.TryString() != nullptr && *literal.TryString() == "ok",
        "a string literal constructs a JSON string");

    char mutableText[] = "mutable";
    const JsonValue fromMutable(mutableText);
    ServerCoreTest::ExpectTrue(
        fromMutable.TryString() != nullptr && *fromMutable.TryString() == "mutable",
        "a mutable character array constructs a JSON string");

    const char* const pointer = "pointer";
    const JsonValue fromPointer(pointer);
    ServerCoreTest::ExpectTrue(
        fromPointer.TryString() != nullptr && *fromPointer.TryString() == "pointer",
        "a C string pointer constructs a JSON string");

    const auto dumped = JsonValue(JsonValue::Object{ { "status", JsonValue("ok") } }).Dump();
    ServerCoreTest::ExpectTrue(dumped.IsOk(), "a body built from a string literal dumps");
    if (dumped.IsOk())
        ServerCoreTest::ExpectEqual(std::string(R"({"status": "ok"})"), dumped.Value(),
            "a string literal reaches the wire as a JSON string");
}

/// <summary>
/// MSVC std::hash&lt;std::string&gt;(FNV-1a 64)의 하위 24비트가 모두 같은 키 2^stages개를 만든다.
/// </summary>
/// <remarks>
/// FNV-1a의 하위 k비트는 앞 상태의 하위 k비트에만 의존한다. 단계마다 하위 비트가 같아지는 서로 다른
/// 네 글자 블록 두 개를 찾아 이어 붙이면, 선택의 모든 조합이 같은 하위 비트를 갖는다. 난수 생성기의
/// 출력은 표준이 정하므로 어느 플랫폼에서나 같은 키가 나온다. 다른 해시를 쓰는 구현에서는 이 키들이
/// 충돌하지 않을 뿐이다.
/// </remarks>
std::vector<std::string> LowBitCollidingKeys(const unsigned stages)
{
    constexpr std::uint64_t OffsetBasis = 14695981039346656037ULL;
    constexpr std::uint64_t Prime = 1099511628211ULL;
    constexpr std::uint64_t LowBits = (std::uint64_t{ 1 } << 24) - 1;
    constexpr std::string_view Alphabet =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    const auto step = [](std::uint64_t state, const std::string_view block)
    {
        for (const unsigned char character : block)
        {
            state ^= character;
            state *= Prime;
        }
        return state;
    };
    std::mt19937_64 random(12345);
    std::uint64_t state = OffsetBasis;
    std::vector<std::pair<std::string, std::string>> choices;
    for (unsigned stage = 0; stage < stages; ++stage)
    {
        std::unordered_map<std::uint64_t, std::string> seen;
        for (;;)
        {
            std::string block(4, 'a');
            for (char& character : block)
                character = Alphabet[random() % Alphabet.size()];
            const auto [found, inserted] = seen.emplace(step(state, block) & LowBits, block);
            if (!inserted && found->second != block)
            {
                choices.emplace_back(found->second, block);
                state = step(state, block);
                break;
            }
        }
    }
    std::vector<std::string> keys;
    for (std::uint64_t mask = 0; mask < (std::uint64_t{ 1 } << stages); ++mask)
    {
        std::string key;
        for (unsigned stage = 0; stage < stages; ++stage)
            key += ((mask >> stage) & 1U) != 0 ? choices[stage].second : choices[stage].first;
        keys.push_back(std::move(key));
    }
    return keys;
}

/// <summary>해시 컨테이너이면 가장 긴 버킷의 원소 수를, 아니면 0을 준다.</summary>
template <typename Map> std::size_t LargestHashBucket(const Map& map)
{
    if constexpr (requires { map.bucket_count(); })
    {
        std::size_t largest = 0;
        for (std::size_t bucket = 0; bucket < map.bucket_count(); ++bucket)
            largest = (std::max)(largest, map.bucket_size(bucket));
        return largest;
    }
    else
    {
        return 0;
    }
}

void JsonObjectKeysResistHashFlooding()
{
    const std::vector<std::string> keys = LowBitCollidingKeys(10);
    std::string text = "{";
    for (const std::string& key : keys)
    {
        if (text.size() > 1)
            text += ',';
        text += '"' + key + "\":0";
    }
    text += '}';
    const auto parsed = JsonValue::Parse(text);
    ServerCoreTest::ExpectTrue(parsed.IsOk(), "an object with crafted keys parses");
    if (!parsed.IsOk())
        return;
    const JsonValue::Object* const object = parsed.Value().TryObject();
    ServerCoreTest::ExpectTrue(object != nullptr && object->size() == keys.size(),
        "every crafted key is kept as a distinct member");
    // 무작위 키 1024개면 가장 긴 버킷이 10개 안팎이다. 64를 넘으면 키가 한 버킷으로 몰린 것이다.
    ServerCoreTest::ExpectTrue(object != nullptr && LargestHashBucket(*object) < 64,
        "crafted keys cannot pile into one bucket of the object's lookup structure");
    ServerCoreTest::ExpectTrue(parsed.Value().Find(keys.front()) != nullptr &&
                                   parsed.Value().Find(keys[keys.size() / 2]) != nullptr &&
                                   parsed.Value().Find(keys.back()) != nullptr,
        "crafted keys stay reachable by lookup");
}

const JsonValue* gObservedFirstElement = nullptr;

void RecordFirstListElement(const JsonValue& body) noexcept
{
    const JsonValue* const list = body.Find("list");
    gObservedFirstElement = list != nullptr ? list->At(0) : nullptr;
}

void ParseMessageMovesTheParsedBody()
{
    namespace TestAccess = ServerCore::Protocol::TestAccess;
    TestAccess::SetParsedBodyObserver(&RecordFirstListElement);
    const auto parsed = ServerCore::Protocol::ParseMessage(
        ToBytes(R"({"type":"Probe","body":{"list":[1,2,3]},"seq":7})"));
    TestAccess::SetParsedBodyObserver(nullptr);
    ServerCoreTest::ExpectTrue(parsed.IsOk() && gObservedFirstElement != nullptr,
        "the parsed body is observed before it reaches the message");
    if (!parsed.IsOk() || parsed.Value().Body() == nullptr)
        return;
    // 같은 원소 주소는 body의 저장소가 복사되지 않고 Message로 옮겨졌다는 뜻이다.
    const JsonValue* const list = parsed.Value().Body()->Find("list");
    ServerCoreTest::ExpectTrue(list != nullptr && list->At(0) == gObservedFirstElement,
        "the message owns the parsed body storage instead of a deep copy");
}

void PreparedMessagesOwnImmutableJsonAndEnvelope()
{
    namespace Protocol = ServerCore::Protocol;
    constexpr auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    JsonValue::Object fields;
    fields.emplace("unsigned", JsonValue(maximum));
    fields.emplace("signed", JsonValue((std::numeric_limits<std::int64_t>::min)()));
    fields.emplace("text",
        JsonValue(std::string("한글 \"quoted\" \\ newline\n nul") + std::string("\0tail", 5)));
    JsonValue source(std::move(fields));
    const JsonValue frozen = source;
    auto item = Protocol::PrepareJsonValue(source);
    auto nullItem = Protocol::PrepareJsonValue(JsonValue(nullptr));
    ServerCoreTest::ExpectTrue(item.IsOk() && nullItem.IsOk(),
        "object and JSON null can be prepared as reusable array values");
    if (!item.IsOk() || !nullItem.IsOk())
        return;
    const auto itemBytes = ToText(item.Value().Bytes());
    source = JsonValue(std::string("the original object no longer exists"));
    ServerCoreTest::ExpectEqual(itemBytes, ToText(item.Value().Bytes()),
        "prepared JSON owns its immutable source snapshot");
    ServerCoreTest::ExpectEqual(
        itemBytes.size(), item.Value().Size(), "prepared value size counts serialized UTF-8 bytes");

    const std::string type = "Batch\"\\\n한글";
    const std::string key = "states\"\\\n목록";
    const std::array<const Protocol::PreparedJsonValue*, 3> items{ &item.Value(), &nullItem.Value(),
        &item.Value() };
    auto prepared = Protocol::PrepareArrayMessage(type, key, items);
    JsonValue::Object expectedBody;
    expectedBody.emplace(key, JsonValue(JsonValue::Array{ frozen, JsonValue(nullptr), frozen }));
    const auto expected = Protocol::SerializeMessage(type, JsonValue(std::move(expectedBody)));
    ServerCoreTest::ExpectTrue(prepared.IsOk() && expected.IsOk(),
        "prepared array with escaped names and repeated items is valid");
    if (!prepared.IsOk() || !expected.IsOk())
        return;
    ServerCoreTest::ExpectEqual(prepared.Value().Bytes().size(), prepared.Value().Size(),
        "prepared envelope size counts actual bytes without the four-byte frame header");
    const auto parsed = Protocol::ParseMessage(prepared.Value().Bytes());
    ServerCoreTest::ExpectTrue(
        parsed.IsOk(), "a prepared array is an independently parseable message");
    if (!parsed.IsOk())
        return;
    const auto* parsedBody = parsed.Value().Body();
    const auto canonical = Protocol::SerializeMessage(parsed.Value().Type(), *parsedBody);
    ServerCoreTest::ExpectTrue(
        canonical.IsOk() && ToText(expected.Value()) == ToText(canonical.Value()),
        "compact prepared assembly preserves envelope values, escaping and item order "
        "independently of optional whitespace");
    const auto* arrayValue = parsedBody ? parsedBody->Find(key) : nullptr;
    const auto* array = arrayValue ? arrayValue->TryArray() : nullptr;
    ServerCoreTest::ExpectTrue(array && array->size() == 3 && (*array)[1].IsNull(),
        "JSON null is distinct from a null prepared-item pointer");
    if (array && array->size() == 3)
    {
        const auto* integer = (*array)[0].Find("unsigned");
        ServerCoreTest::ExpectTrue(
            integer && integer->TryUInt64() && *integer->TryUInt64() == maximum,
            "prepared array concatenation preserves uint64 maximum without double conversion");
        const auto* signedInteger = (*array)[2].Find("signed");
        ServerCoreTest::ExpectTrue(
            signedInteger && signedInteger->TryInt64() &&
                *signedInteger->TryInt64() == (std::numeric_limits<std::int64_t>::min)(),
            "prepared array concatenation preserves int64 minimum");
    }
    const auto empty = Protocol::PrepareArrayMessage("StateBatch", "states", {});
    ServerCoreTest::ExpectTrue(empty.IsOk(), "an empty prepared array is a valid envelope");
    if (empty.IsOk())
        ServerCoreTest::ExpectEqual(std::string(R"({"body":{"states":[]},"type":"StateBatch"})"),
            ToText(empty.Value().Bytes()),
            "empty arrays contain no trailing comma or extra envelope field");

    std::string mutableType = "PreparedReply";
    JsonValue body = frozen;
    JsonValue sequence(maximum);
    JsonValue error(JsonValue::Object{ { "code", JsonValue(std::string("retry")) } });
    const auto before = Protocol::SerializeMessage({ mutableType, &body, &sequence, &error });
    auto envelope = Protocol::PrepareMessage({ mutableType, &body, &sequence, &error });
    ServerCoreTest::ExpectTrue(
        before.IsOk() && envelope.IsOk(), "body, seq and error can be prepared together");
    if (!before.IsOk() || !envelope.IsOk())
        return;
    mutableType.assign("changed");
    body = JsonValue(nullptr);
    sequence = JsonValue(nullptr);
    error = JsonValue(nullptr);
    ServerCoreTest::ExpectEqual(ToText(before.Value()), ToText(envelope.Value().Bytes()),
        "prepared messages do not borrow any MessageFields strings or JSON values");
}

void PreparedMessagesRejectInvalidInputs()
{
    namespace Protocol = ServerCore::Protocol;
    const JsonValue validBody(JsonValue::Object{});
    const std::string invalidUtf8(1, static_cast<char>(0xc3));
    const JsonValue invalidString(invalidUtf8);
    const JsonValue nonFinite(std::numeric_limits<double>::quiet_NaN());
    const JsonValue nestedNan(JsonValue::Object{ { "value", nonFinite } });
    const JsonValue nestedInvalid(JsonValue::Object{ { invalidUtf8, JsonValue(true) } });
    for (const JsonValue* value : { &invalidString, &nonFinite, &nestedNan, &nestedInvalid })
    {
        const auto result = Protocol::PrepareJsonValue(*value);
        ServerCoreTest::ExpectTrue(
            !result.IsOk() && result.GetStatus().Code() == ErrorCode::InvalidArgument,
            "prepared values reject non-finite numbers and invalid UTF-8 recursively");
    }
    for (const MessageFields fields : { MessageFields{ "", &validBody, nullptr, nullptr },
             MessageFields{ invalidUtf8, &validBody, nullptr, nullptr },
             MessageFields{ "Unsafe", &nestedNan, nullptr, nullptr },
             MessageFields{ "Missing", nullptr, nullptr, nullptr } })
    {
        const auto result = Protocol::PrepareMessage(fields);
        ServerCoreTest::ExpectTrue(
            !result.IsOk() && result.GetStatus().Code() == ErrorCode::InvalidArgument,
            "PrepareMessage preserves the existing outbound validation contract");
    }
    auto item = Protocol::PrepareJsonValue(validBody);
    if (!item.IsOk())
    {
        ServerCoreTest::ExpectTrue(false, "valid prepared test item exists");
        return;
    }
    const std::array<const Protocol::PreparedJsonValue*, 1> nullPointer{ nullptr };
    const auto missing = Protocol::PrepareArrayMessage("Batch", "states", nullPointer);
    ServerCoreTest::ExpectTrue(
        !missing.IsOk() && missing.GetStatus().Code() == ErrorCode::InvalidArgument,
        "a null prepared-item pointer cannot become a silent JSON null element");
    auto owner = std::move(item.Value());
    const std::array<const Protocol::PreparedJsonValue*, 1> movedFrom{ &item.Value() };
    const auto emptyItem = Protocol::PrepareArrayMessage("Batch", "states", movedFrom);
    ServerCoreTest::ExpectTrue(owner.Size() != 0 && !emptyItem.IsOk() &&
                                   emptyItem.GetStatus().Code() == ErrorCode::InvalidArgument,
        "a moved-from prepared value is rejected rather than corrupting the array");
    for (const auto& names : std::array<std::pair<std::string, std::string>, 3>{
             std::pair{ std::string{}, std::string("states") }, { invalidUtf8, "states" },
             { "Batch", invalidUtf8 } })
    {
        const auto result = Protocol::PrepareArrayMessage(names.first, names.second, {});
        ServerCoreTest::ExpectTrue(
            !result.IsOk() && result.GetStatus().Code() == ErrorCode::InvalidArgument,
            "array assembly validates empty message types and UTF-8 in both envelope names");
    }
}

void PreparedMessagesAdaptLegacySessions()
{
    class LegacySession final : public ServerCore::Session::Session
    {
    public:
        ServerCore::Session::SessionId Id() const noexcept override
        {
            return static_cast<ServerCore::Session::SessionId>(1);
        }
        ServerCore::Session::SessionState State() const noexcept override
        {
            return ServerCore::Session::SessionState::Connected;
        }
        ServerCore::Core::Status MarkAuthenticated() override
        {
            return ServerCore::Core::Status::Ok();
        }
        ServerCore::Core::Status Send(const MessageFields& fields) override
        {
            ++calls;
            auto serialized = ServerCore::Protocol::SerializeMessage(fields);
            if (!serialized.IsOk())
                return std::move(serialized).TakeStatus();
            received = std::move(serialized.Value());
            return ServerCore::Core::Status::FailWithoutMessage(ErrorCode::WouldBlock);
        }
        ServerCore::Core::Status SendAndDisconnect(
            const MessageFields&, ServerCore::Core::Status) override
        {
            return ServerCore::Core::Status::FailWithoutMessage(ErrorCode::Closed);
        }
        void Disconnect(ServerCore::Core::Status) override {}
        std::size_t calls = 0;
        std::vector<std::byte> received;
    } session;
    const JsonValue sequence((std::numeric_limits<std::uint64_t>::max)());
    const JsonValue error(JsonValue::Object{ { "code", JsonValue(std::string("준비\"실패")) } });
    auto prepared =
        ServerCore::Protocol::PrepareMessage({ "ErrorOnly", nullptr, &sequence, &error });
    ServerCoreTest::ExpectTrue(
        prepared.IsOk(), "an error-only envelope can be prepared for a legacy session");
    if (!prepared.IsOk())
        return;
    const auto result = session.SendPrepared(prepared.Value());
    ServerCoreTest::ExpectTrue(result.Code() == ErrorCode::WouldBlock && session.calls == 1,
        "the default prepared adapter calls existing Send once and preserves its status");
    ServerCoreTest::ExpectEqual(ToText(prepared.Value().Bytes()), ToText(session.received),
        "the legacy adapter preserves absent body, error, exact seq and escaped UTF-8");
    ServerCoreTest::ExpectEqual(std::size_t{ 0 }, session.QueuedSendBytes(),
        "legacy sessions have an explicitly unavailable queue metric");
    auto owner = std::move(prepared.Value());
    const auto empty = session.SendPrepared(prepared.Value());
    ServerCoreTest::ExpectTrue(owner.Size() != 0 && !empty.IsOk() && session.calls == 1,
        "a moved-from message fails before reaching a legacy session's Send");
}

void PreparedArrayReservesEnvelopeDepth()
{
    namespace Protocol = ServerCore::Protocol;
    JsonValue value(nullptr);
    for (int index = 0; index < 253; ++index)
        value = JsonValue(JsonValue::Array{ std::move(value) });
    auto prepared = Protocol::PrepareJsonValue(value);
    ServerCoreTest::ExpectTrue(
        prepared.IsOk(), "array item permits the maximum depth left by its envelope");
    if (prepared.IsOk())
    {
        const std::array<const Protocol::PreparedJsonValue*, 1> items{ &prepared.Value() };
        auto envelope = Protocol::PrepareArrayMessage("Batch", "items", items);
        ServerCoreTest::ExpectTrue(
            envelope.IsOk() && Protocol::ParseMessage(envelope.Value().Bytes()).IsOk(),
            "maximum-depth prepared array remains parseable after envelope assembly");
    }
    value = JsonValue(JsonValue::Array{ std::move(value) });
    ServerCoreTest::ExpectTrue(value.Dump().IsOk() && !Protocol::PrepareJsonValue(value).IsOk(),
        "standalone-valid JSON cannot exceed the prepared envelope's reserved depth");
}

void DatagramBoundariesAndWireOrder()
{
    namespace Codec = ServerCore::Protocol::DatagramCodec;
    const auto token = Codec::TokenFromHex("000102030405060708090a0b0c0d0e0f");
    ServerCoreTest::ExpectTrue(token.has_value() && !Codec::TokenFromHex("xyz") &&
                                   !Codec::TokenFromHex("gg0102030405060708090a0b0c0d0e0f"),
        "datagram token parsing is exact and bounded");
    if (!token)
        return;
    ServerCoreTest::ExpectEqual(std::string("000102030405060708090a0b0c0d0e0f"),
        Codec::TokenToHex(*token), "token text round-trips each byte");
    std::array<std::byte, Codec::MaximumDatagramBytes + 1> bytes{};
    std::array<std::byte, Codec::MaximumPayloadBytes + 1> payload{};
    const auto body = std::span(payload).first(Codec::MaximumPayloadBytes);
    const auto count = Codec::Encode(bytes, *token, 0x0102030405060708ull, body);
    const auto packet = Codec::Decode(std::span(bytes).first(count));
    ServerCoreTest::ExpectTrue(count == 1200 && packet && packet->token == *token &&
                                   packet->sequence == 0x0102030405060708ull &&
                                   packet->payload.size() == 1172 && bytes[20] == std::byte{ 1 } &&
                                   bytes[27] == std::byte{ 8 },
        "datagram wire format preserves exact big-endian sequence at MTU bound");
    ServerCoreTest::ExpectTrue(
        !Codec::Decode(std::span(bytes).first(28)) && !Codec::Decode(bytes) &&
            Codec::Encode(bytes, *token, 0, body) == 0 &&
            Codec::Encode(bytes, *token, 1, payload) == 0 &&
            Codec::Encode(std::span(bytes).first(100), *token, 1, body) == 0 &&
            Codec::Encode(bytes, *token, 1, {}) == 0,
        "truncation, oversize, zero sequence and empty payload are rejected");
    bytes[0] = std::byte{ 'X' };
    ServerCoreTest::ExpectTrue(
        !Codec::Decode(std::span(bytes).first(count)), "foreign magic is ignored");
}

ServerCoreTest::CheckRegistration gPreparedArrayReservesEnvelopeDepth(
    "Protocol.PreparedArrayReservesEnvelopeDepth", &PreparedArrayReservesEnvelopeDepth);
ServerCoreTest::CheckRegistration gDatagramBoundariesAndWireOrder(
    "Protocol.DatagramBoundariesAndWireOrder", &DatagramBoundariesAndWireOrder);

ServerCoreTest::CheckRegistration gPreparedMessagesOwnImmutableJsonAndEnvelope(
    "Protocol.PreparedMessagesOwnImmutableJsonAndEnvelope",
    &PreparedMessagesOwnImmutableJsonAndEnvelope);
ServerCoreTest::CheckRegistration gPreparedMessagesRejectInvalidInputs(
    "Protocol.PreparedMessagesRejectInvalidInputs", &PreparedMessagesRejectInvalidInputs);
ServerCoreTest::CheckRegistration gPreparedMessagesAdaptLegacySessions(
    "Protocol.PreparedMessagesAdaptLegacySessions", &PreparedMessagesAdaptLegacySessions);
ServerCoreTest::CheckRegistration gFramingConformanceVectors(
    "Protocol.FramingConformanceVectors", &FramingConformanceVectors);
ServerCoreTest::CheckRegistration gFramingRejectsInvalidLengths(
    "Protocol.FramingRejectsInvalidLengths", &FramingRejectsInvalidLengths);
ServerCoreTest::CheckRegistration gFramingTransfersAndDiscardsCompletedBodies(
    "Protocol.FramingTransfersAndDiscardsCompletedBodies",
    &FramingTransfersAndDiscardsCompletedBodies);
ServerCoreTest::CheckRegistration gFrameReaderReleaseStorageResetsReader(
    "Protocol.FrameReaderReleaseStorageResetsReader", &FrameReaderReleaseStorageResetsReader);
ServerCoreTest::CheckRegistration gFrameCodecSharedApiIsNoThrow(
    "Protocol.FrameCodecSharedApiIsNoThrow", &FrameCodecSharedApiIsNoThrow);
ServerCoreTest::CheckRegistration gMessagePreservesEnvelopeFields(
    "Protocol.MessagePreservesEnvelopeFields", &MessagePreservesEnvelopeFields);
ServerCoreTest::CheckRegistration gMessagePreserves64BitIntegers(
    "Protocol.MessagePreserves64BitIntegers", &MessagePreserves64BitIntegers);
ServerCoreTest::CheckRegistration gJsonRejectsUnrepresentableNumbers(
    "Protocol.JsonRejectsUnrepresentableNumbers", &JsonRejectsUnrepresentableNumbers);
ServerCoreTest::CheckRegistration gMessageRejectsInvalidUtf8(
    "Protocol.MessageRejectsInvalidUtf8", &MessageRejectsInvalidUtf8);
ServerCoreTest::CheckRegistration gJsonEntryPointsShareValidation(
    "Protocol.JsonEntryPointsShareValidation", &JsonEntryPointsShareValidation);
ServerCoreTest::CheckRegistration gMessageEnvelopeValidationIsSymmetric(
    "Protocol.MessageEnvelopeValidationIsSymmetric", &MessageEnvelopeValidationIsSymmetric);
ServerCoreTest::CheckRegistration gJsonDumpRejectsUnsafeValues(
    "Protocol.JsonDumpRejectsUnsafeValues", &JsonDumpRejectsUnsafeValues);
ServerCoreTest::CheckRegistration gMessageRejectsUnsafeOutboundJson(
    "Protocol.MessageRejectsUnsafeOutboundJson", &MessageRejectsUnsafeOutboundJson);
ServerCoreTest::CheckRegistration gMessageRejectsNonObjectBodies(
    "Protocol.MessageRejectsNonObjectBodies", &MessageRejectsNonObjectBodies);
ServerCoreTest::CheckRegistration gJsonTextConstructorsKeepTheirType(
    "Protocol.JsonTextConstructorsKeepTheirType", &JsonTextConstructorsKeepTheirType);
ServerCoreTest::CheckRegistration gParseMessageMovesTheParsedBody(
    "Protocol.ParseMessageMovesTheParsedBody", &ParseMessageMovesTheParsedBody);
ServerCoreTest::CheckRegistration gJsonObjectKeysResistHashFlooding(
    "Protocol.JsonObjectKeysResistHashFlooding", &JsonObjectKeysResistHashFlooding);
ServerCoreTest::CheckRegistration gJsonParseLimitsValueCount(
    "Protocol.JsonParseLimitsValueCount", &JsonParseLimitsValueCount);
ServerCoreTest::CheckRegistration gJsonNumbersUnderflowToSignedZero(
    "Protocol.JsonNumbersUnderflowToSignedZero", &JsonNumbersUnderflowToSignedZero);
}
