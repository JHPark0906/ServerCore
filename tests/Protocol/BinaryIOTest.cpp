#include "ServerCore/Protocol/BinaryIO.h"
#include "TestHarness.h"
#include <algorithm>
#include <array>
#include <bit>
#include <limits>
namespace
{
using namespace ServerCore;
using ServerCoreTest::ExpectTrue;
void BinaryWireAndBounds()
{
    for (auto order : { Protocol::ByteOrder::BigEndian, Protocol::ByteOrder::LittleEndian })
    {
        auto writer = Protocol::BinaryWriter::Create(128, order).Value();
        ExpectTrue(writer.WriteUnsigned(0x1234, 2).IsOk(), "write ordered integer");
        const std::array expected = order == Protocol::ByteOrder::BigEndian
                                        ? std::array{ std::byte{ 0x12 }, std::byte{ 0x34 } }
                                        : std::array{ std::byte{ 0x34 }, std::byte{ 0x12 } };
        ExpectTrue(std::ranges::equal(writer.Bytes(), expected),
            "wire order independent from host alignment");
        ExpectTrue(writer.WriteSigned(INT64_MIN, 8).IsOk() && writer.WriteSigned(-128, 1).IsOk() &&
                       writer.WriteFloat32(-0.0f).IsOk() && writer.WriteFloat64(1.25).IsOk() &&
                       writer.WriteBool(true).IsOk() && writer.WriteUtf8("한글", 6).IsOk(),
            "supported scalar and bounded UTF8 encoding");
        auto reader = Protocol::BinaryReader::Create(writer.Bytes(), order).Value();
        ExpectTrue(reader.ReadUnsigned(2).Value() == 0x1234 &&
                       reader.ReadSigned(8).Value() == INT64_MIN &&
                       reader.ReadSigned(1).Value() == -128,
            "signed range and unaligned decode");
        ExpectTrue(std::bit_cast<std::uint32_t>(reader.ReadFloat32().Value()) == 0x80000000 &&
                       reader.ReadFloat64().Value() == 1.25 && reader.ReadBool().Value(),
            "float bit patterns and canonical bool");
        auto saved = reader.Position();
        ExpectTrue(!reader.ReadUtf8(5).IsOk() && reader.Position() == saved,
            "field length rejection does not consume prefix");
        ExpectTrue(reader.ReadUtf8(6).Value() == "한글" && reader.Remaining() == 0,
            "retry with explicit adequate field cap");
        auto before = std::vector<std::byte>(writer.Bytes().begin(), writer.Bytes().end());
        ExpectTrue(!writer.WriteSigned(128, 1).IsOk() && !writer.WriteUnsigned(256, 1).IsOk() &&
                       !writer.WriteUnsigned(1, 3).IsOk() &&
                       std::ranges::equal(writer.Bytes(), before),
            "narrowing and invalid width preserve output");
        const auto size = writer.Bytes().size();
        ExpectTrue(writer.WriteBytes(writer.Bytes()).IsOk() && writer.Bytes().size() == size * 2 &&
                       std::ranges::equal(writer.Bytes().first(size), writer.Bytes().subspan(size)),
            "self append survives vector growth");
    }
    const std::array<std::byte, 6> blob{ std::byte{ 0 }, std::byte{ 0 }, std::byte{ 0 },
        std::byte{ 2 }, std::byte{ 0xC0 }, std::byte{ 0xAF } };
    for (std::size_t length = 0; length < blob.size(); ++length)
    {
        auto reader = Protocol::BinaryReader::Create(std::span(blob).first(length)).Value();
        ExpectTrue(!reader.ReadLengthPrefixed(10).IsOk() && reader.Position() == 0,
            "truncated prefix or payload consumes nothing");
    }
    auto reader = Protocol::BinaryReader::Create(blob).Value();
    ExpectTrue(
        !reader.ReadUtf8(2).IsOk() && reader.Position() == 0 && reader.ReadLengthPrefixed(2).IsOk(),
        "UTF8 validation separate from opaque binary");
    auto writer = Protocol::BinaryWriter::Create(4).Value();
    ExpectTrue(!writer.WriteLengthPrefixed(blob, 6).IsOk() && writer.Bytes().empty(),
        "length-prefixed write atomically obeys total cap");
    const std::array badBool{ std::byte{ 2 } };
    reader = Protocol::BinaryReader::Create(badBool).Value();
    ExpectTrue(!reader.ReadBool().IsOk() && reader.Position() == 0,
        "boolean rejects ambiguous wire values");
}
void BinaryVersionNegotiation()
{
    const std::array server{ Protocol::ProtocolOffer{ 4, 7 }, Protocol::ProtocolOffer{ 3, 3 } };
    const std::array peer{ Protocol::ProtocolOffer{ 3, 3 }, Protocol::ProtocolOffer{ 4, 1 } };
    auto selected = Protocol::NegotiateProtocol(server, peer, 2);
    ExpectTrue(selected.IsOk() && selected.Value().version == 3 && selected.Value().features == 3,
        "required features gate version selection");
    ExpectTrue(Protocol::NegotiateProtocol(server, peer).Value().version == 4,
        "server preference wins without implicit numeric policy");
    ExpectTrue(Protocol::NegotiateProtocol(server, peer, 8).GetStatus().Code() ==
                   Core::ErrorCode::Unimplemented,
        "no compatible version is explicit");
    const std::array duplicate{ server[0], server[0] };
    ExpectTrue(!Protocol::NegotiateProtocol(duplicate, peer).IsOk() &&
                   !Protocol::NegotiateProtocol({}, peer).IsOk(),
        "ambiguous or empty offer rejected");
}
ServerCoreTest::CheckRegistration a("Protocol.BinaryWireAndBounds", BinaryWireAndBounds);
ServerCoreTest::CheckRegistration b("Protocol.BinaryVersionNegotiation", BinaryVersionNegotiation);
}
