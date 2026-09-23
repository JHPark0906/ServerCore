#include "ServerCore/C/BinaryIO.h"
#include "C/Internal.h"
#include "ServerCore/Protocol/BinaryIO.h"
#include <array>
#include <optional>
namespace P = ServerCore::Protocol;
namespace C = ServerCore::CDetail;
struct sc_binary_reader
{
    std::vector<std::byte> storage;
    std::optional<P::BinaryReader> native;
};
struct sc_binary_writer
{
    P::BinaryWriter native;
};
extern "C"
{
    sc_status sc_binary_reader_create(
        sc_bytes input, uint32_t order, size_t limit, sc_binary_reader** out)
    {
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        if (!C::Valid(input))
            return SC_INVALID_ARGUMENT;
        auto checked =
            P::BinaryReader::Create(C::Bytes(input), static_cast<P::ByteOrder>(order), limit);
        if (!checked.IsOk())
            return C::Code(checked.GetStatus());
        return C::Protect(
            [&]
            {
                auto owner = std::make_unique<sc_binary_reader>();
                if (input.len)
                    owner->storage.assign(C::Bytes(input).begin(), C::Bytes(input).end());
                owner->native.emplace(
                    P::BinaryReader::Create(owner->storage, static_cast<P::ByteOrder>(order), limit)
                        .Value());
                *out = owner.release();
                return SC_OK;
            });
    }
    void sc_binary_reader_destroy(sc_binary_reader* value)
    {
        delete value;
    }
    size_t sc_binary_reader_position(const sc_binary_reader* value)
    {
        return value ? value->native->Position() : 0;
    }
    size_t sc_binary_reader_remaining(const sc_binary_reader* value)
    {
        return value ? value->native->Remaining() : 0;
    }
#define SC_READ_NUMBER(name, type, expression)                                                     \
    sc_status name(sc_binary_reader* value, type* out)                                             \
    {                                                                                              \
        if (!value || !out)                                                                        \
            return SC_INVALID_ARGUMENT;                                                            \
        auto result = value->native->expression;                                                   \
        if (!result.IsOk())                                                                        \
            return C::Code(result.GetStatus());                                                    \
        *out = result.Value();                                                                     \
        return SC_OK;                                                                              \
    }
    SC_READ_NUMBER(sc_binary_read_bool, uint32_t, ReadBool())
    SC_READ_NUMBER(sc_binary_read_f32, float, ReadFloat32())
    SC_READ_NUMBER(sc_binary_read_f64, double, ReadFloat64())
#undef SC_READ_NUMBER
    sc_status sc_binary_read_unsigned(sc_binary_reader* value, size_t width, uint64_t* out)
    {
        if (!value || !out)
            return SC_INVALID_ARGUMENT;
        auto result = value->native->ReadUnsigned(width);
        if (!result.IsOk())
            return C::Code(result.GetStatus());
        *out = result.Value();
        return SC_OK;
    }
    sc_status sc_binary_read_signed(sc_binary_reader* value, size_t width, int64_t* out)
    {
        if (!value || !out)
            return SC_INVALID_ARGUMENT;
        auto result = value->native->ReadSigned(width);
        if (!result.IsOk())
            return C::Code(result.GetStatus());
        *out = result.Value();
        return SC_OK;
    }
#define SC_READ_BYTES(name, expression)                                                            \
    sc_status name(sc_binary_reader* value, size_t length, sc_bytes* out)                          \
    {                                                                                              \
        if (!value || !out)                                                                        \
            return SC_INVALID_ARGUMENT;                                                            \
        auto result = value->native->expression;                                                   \
        if (!result.IsOk())                                                                        \
            return C::Code(result.GetStatus());                                                    \
        *out = { reinterpret_cast<const uint8_t*>(result.Value().data()), result.Value().size() }; \
        return SC_OK;                                                                              \
    }
    SC_READ_BYTES(sc_binary_read_bytes, ReadBytes(length))
    SC_READ_BYTES(sc_binary_read_blob, ReadLengthPrefixed(length))
    SC_READ_BYTES(sc_binary_read_utf8, ReadUtf8(length))
#undef SC_READ_BYTES
    sc_status sc_binary_skip(sc_binary_reader* value, size_t length)
    {
        return value ? C::Code(value->native->Skip(length)) : SC_INVALID_ARGUMENT;
    }
    sc_status sc_binary_writer_create(size_t limit, uint32_t order, sc_binary_writer** out)
    {
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        return C::Protect(
            [&]() -> sc_status
            {
                auto native = P::BinaryWriter::Create(limit, static_cast<P::ByteOrder>(order));
                if (!native.IsOk())
                    return C::Code(native.GetStatus());
                *out = new sc_binary_writer{ std::move(native.Value()) };
                return SC_OK;
            });
    }
    void sc_binary_writer_destroy(sc_binary_writer* value)
    {
        delete value;
    }
    void sc_binary_writer_clear(sc_binary_writer* value)
    {
        if (value)
            value->native.Clear();
    }
    sc_status sc_binary_writer_view(const sc_binary_writer* value, sc_bytes* out)
    {
        if (!value || !out)
            return SC_INVALID_ARGUMENT;
        auto bytes = value->native.Bytes();
        *out = { reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size() };
        return SC_OK;
    }
    sc_status sc_binary_write_unsigned(sc_binary_writer* value, uint64_t number, size_t width)
    {
        return value
                   ? C::Protect([&] { return C::Code(value->native.WriteUnsigned(number, width)); })
                   : SC_INVALID_ARGUMENT;
    }
    sc_status sc_binary_write_signed(sc_binary_writer* value, int64_t number, size_t width)
    {
        return value ? C::Protect([&] { return C::Code(value->native.WriteSigned(number, width)); })
                     : SC_INVALID_ARGUMENT;
    }
    sc_status sc_binary_write_bool(sc_binary_writer* value, uint32_t number)
    {
        return value && number < 2
                   ? C::Protect([&] { return C::Code(value->native.WriteBool(number != 0)); })
                   : SC_INVALID_ARGUMENT;
    }
    sc_status sc_binary_write_f32(sc_binary_writer* value, float number)
    {
        return value ? C::Protect([&] { return C::Code(value->native.WriteFloat32(number)); })
                     : SC_INVALID_ARGUMENT;
    }
    sc_status sc_binary_write_f64(sc_binary_writer* value, double number)
    {
        return value ? C::Protect([&] { return C::Code(value->native.WriteFloat64(number)); })
                     : SC_INVALID_ARGUMENT;
    }
    sc_status sc_binary_write_bytes(sc_binary_writer* value, sc_bytes bytes)
    {
        return value && C::Valid(bytes)
                   ? C::Protect([&] { return C::Code(value->native.WriteBytes(C::Bytes(bytes))); })
                   : SC_INVALID_ARGUMENT;
    }
    sc_status sc_binary_write_blob(sc_binary_writer* value, sc_bytes bytes, size_t limit)
    {
        return value && C::Valid(bytes) ? C::Protect(
                                              [&]
                                              {
                                                  return C::Code(value->native.WriteLengthPrefixed(
                                                      C::Bytes(bytes), limit));
                                              })
                                        : SC_INVALID_ARGUMENT;
    }
    sc_status sc_binary_write_utf8(sc_binary_writer* value, sc_bytes bytes, size_t limit)
    {
        return value && C::Valid(bytes)
                   ? C::Protect(
                         [&] { return C::Code(value->native.WriteUtf8(C::Text(bytes), limit)); })
                   : SC_INVALID_ARGUMENT;
    }
    sc_status sc_protocol_negotiate(const sc_protocol_offer* server, size_t serverCount,
        const sc_protocol_offer* peer, size_t peerCount, uint64_t required, sc_protocol_offer* out)
    {
        if (!server || !peer || !out || !serverCount || serverCount > 64 || !peerCount ||
            peerCount > 64)
            return SC_INVALID_ARGUMENT;
        std::array<P::ProtocolOffer, 64> local{}, remote{};
        for (size_t i = 0; i < serverCount; ++i)
        {
            if (server[i].reserved)
                return SC_INVALID_ARGUMENT;
            local[i] = { server[i].version, server[i].features };
        }
        for (size_t i = 0; i < peerCount; ++i)
        {
            if (peer[i].reserved)
                return SC_INVALID_ARGUMENT;
            remote[i] = { peer[i].version, peer[i].features };
        }
        auto result = P::NegotiateProtocol(
            std::span(local).first(serverCount), std::span(remote).first(peerCount), required);
        if (!result.IsOk())
            return C::Code(result.GetStatus());
        *out = { result.Value().version, 0, result.Value().features };
        return SC_OK;
    }
}
