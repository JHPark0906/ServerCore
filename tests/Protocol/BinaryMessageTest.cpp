#include "ServerCore/Protocol/BinaryMessage.h"
#include "TestHarness.h"

#include <array>

namespace
{
void BinaryMessageEnvelope()
{
    using namespace ServerCore;
    const std::array payload{std::byte{0xff}, std::byte{0}, std::byte{0x80}};
    auto encoded = Protocol::EncodeBinaryMessage(0x04030201, payload, 7);
    ServerCoreTest::ExpectTrue(encoded.IsOk(), "binary envelope accepts uninterpreted bytes");
    if (!encoded.IsOk()) return;
    ServerCoreTest::ExpectTrue(encoded.Value()[0] == std::byte{1} && encoded.Value()[3] == std::byte{4},
        "type has a stable little-endian wire representation");
    auto decoded = Protocol::DecodeBinaryMessage(encoded.Value());
    ServerCoreTest::ExpectTrue(decoded.IsOk() && decoded.Value().type == 0x04030201 &&
        std::equal(decoded.Value().payload.begin(), decoded.Value().payload.end(), payload.begin(), payload.end()),
        "type and binary payload round trip");
    ServerCoreTest::ExpectTrue(!Protocol::EncodeBinaryMessage(0, payload, 7).IsOk(), "zero type is reserved");
    ServerCoreTest::ExpectTrue(Protocol::EncodeBinaryMessage(1, payload, 6).GetStatus().Code() ==
        Core::ErrorCode::TooLarge, "type header counts toward the payload envelope limit");
    ServerCoreTest::ExpectTrue(!Protocol::DecodeBinaryMessage(std::span(encoded.Value()).first(3)).IsOk(),
        "truncated binary envelope is rejected");
    const std::array<std::byte, 4> zero{};
    ServerCoreTest::ExpectTrue(!Protocol::DecodeBinaryMessage(zero).IsOk(), "zero wire type is rejected");
    ServerCoreTest::ExpectTrue(Protocol::EncodeBinaryMessage(1, {}, 4).IsOk(), "empty application payload is valid");
}
const ServerCoreTest::CheckRegistration BinaryEnvelopeCheck("Protocol.BinaryMessageEnvelope", BinaryMessageEnvelope);
}
