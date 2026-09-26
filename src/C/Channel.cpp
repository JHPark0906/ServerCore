#include "ServerCore/C/Channel.h"
#include "C/Internal.h"
#include "ServerCore/Runtime/Channel.h"

#include <vector>

namespace
{
using Payload = std::vector<uint8_t>;
namespace C = ServerCore::CDetail;
std::shared_ptr<const Payload> Copy(sc_bytes bytes)
{
    if (!bytes.len)
        return std::make_shared<const Payload>();
    return std::make_shared<const Payload>(bytes.data, bytes.data + bytes.len);
}
}
struct sc_channel
{
    ServerCore::Runtime::BoundedChannel<Payload> value;
    size_t maxBytes;
};
struct sc_latest_value
{
    ServerCore::Runtime::LatestValue<Payload> value;
    size_t maxBytes;
};
struct sc_channel_value
{
    std::shared_ptr<const Payload> bytes;
    uint64_t version = 0;
};

extern "C"
{
    sc_status sc_channel_create(const sc_channel_options* options, sc_channel** out) noexcept
    {
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        if (!C::Version(options))
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]() -> sc_status
            {
                auto created = ServerCore::Runtime::BoundedChannel<Payload>::Create(
                    { options->max_messages, options->max_retained_bytes });
                if (!created.IsOk())
                    return C::Code(created.GetStatus());
                *out = new sc_channel{ std::move(created.Value()), options->max_retained_bytes };
                return SC_OK;
            });
    }
    sc_status sc_channel_retain(const sc_channel* channel, sc_channel** out) noexcept
    {
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        if (!channel)
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]
            {
                *out = new sc_channel(*channel);
                return SC_OK;
            });
    }
    void sc_channel_destroy(sc_channel* channel) noexcept
    {
        delete channel;
    }
    void sc_channel_close(sc_channel* channel) noexcept
    {
        if (channel)
            channel->value.Close();
    }
    sc_status sc_channel_try_send(sc_channel* channel, sc_bytes bytes) noexcept
    {
        if (!channel || !C::Valid(bytes))
            return SC_INVALID_ARGUMENT;
        if (bytes.len > channel->maxBytes)
            return SC_TOO_LARGE;
        return C::Protect([&] { return C::Code(channel->value.TrySend(Copy(bytes), bytes.len)); });
    }
    sc_status sc_channel_try_receive(sc_channel* channel, sc_channel_value** out) noexcept
    {
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        if (!channel)
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]() -> sc_status
            {
                // Allocate the foreign owner before consuming a queue entry.
                auto result = std::make_unique<sc_channel_value>();
                auto received = channel->value.TryReceive();
                if (!received.IsOk())
                    return C::Code(received.GetStatus());
                result->bytes = std::move(received.Value());
                *out = result.release();
                return SC_OK;
            });
    }
    size_t sc_channel_size(const sc_channel* channel) noexcept
    {
        return channel ? channel->value.Size() : 0;
    }
    size_t sc_channel_retained_bytes(const sc_channel* channel) noexcept
    {
        return channel ? channel->value.RetainedBytes() : 0;
    }
    sc_status sc_channel_subscribe_read(
        sc_channel* channel, sc_notifier* notifier, uint64_t key, sc_subscription** out) noexcept
    {
        if (out)
            *out = nullptr;
        if (!channel)
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]
            {
                return C::SubscribeCompletion([&](auto callback)
                    { return channel->value.WaitForReadReady(std::move(callback)); }, notifier, key,
                    out);
            });
    }
    sc_status sc_channel_subscribe_write(sc_channel* channel, size_t requiredBytes,
        sc_notifier* notifier, uint64_t key, sc_subscription** out) noexcept
    {
        if (out)
            *out = nullptr;
        if (!channel)
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]
            {
                return C::SubscribeCompletion(
                    [&](auto callback)
                    {
                        return channel->value.WaitForWriteReady(requiredBytes, std::move(callback));
                    },
                    notifier, key, out);
            });
    }
    sc_status sc_latest_value_create(size_t maxBytes, sc_latest_value** out) noexcept
    {
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        return C::Protect(
            [&]() -> sc_status
            {
                auto created = ServerCore::Runtime::LatestValue<Payload>::Create(maxBytes);
                if (!created.IsOk())
                    return C::Code(created.GetStatus());
                *out = new sc_latest_value{ std::move(created.Value()), maxBytes };
                return SC_OK;
            });
    }
    sc_status sc_latest_value_retain(const sc_latest_value* value, sc_latest_value** out) noexcept
    {
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        if (!value)
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]
            {
                *out = new sc_latest_value(*value);
                return SC_OK;
            });
    }
    void sc_latest_value_destroy(sc_latest_value* value) noexcept
    {
        delete value;
    }
    void sc_latest_value_close(sc_latest_value* value) noexcept
    {
        if (value)
            value->value.Close();
    }
    sc_status sc_latest_value_publish(sc_latest_value* value, sc_bytes bytes) noexcept
    {
        if (!value || !C::Valid(bytes))
            return SC_INVALID_ARGUMENT;
        if (bytes.len > value->maxBytes)
            return SC_TOO_LARGE;
        return C::Protect([&] { return C::Code(value->value.Publish(Copy(bytes), bytes.len)); });
    }
    sc_status sc_latest_value_read_after(
        sc_latest_value* value, uint64_t version, sc_channel_value** out) noexcept
    {
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        if (!value)
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]() -> sc_status
            {
                auto result = std::make_unique<sc_channel_value>();
                auto received = value->value.ReadAfter(version);
                if (!received.IsOk())
                    return C::Code(received.GetStatus());
                result->bytes = std::move(received.Value().value);
                result->version = received.Value().version;
                *out = result.release();
                return SC_OK;
            });
    }
    size_t sc_latest_value_retained_bytes(const sc_latest_value* value) noexcept
    {
        return value ? value->value.RetainedBytes() : 0;
    }
    sc_status sc_latest_value_subscribe(sc_latest_value* value, uint64_t version,
        sc_notifier* notifier, uint64_t key, sc_subscription** out) noexcept
    {
        if (out)
            *out = nullptr;
        if (!value)
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]
            {
                return C::SubscribeCompletion([&](auto callback)
                    { return value->value.WaitForChange(version, std::move(callback)); }, notifier,
                    key, out);
            });
    }
    sc_status sc_channel_value_view(const sc_channel_value* value, sc_bytes* out) noexcept
    {
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = {};
        if (!value || !value->bytes)
            return SC_INVALID_ARGUMENT;
        *out = { value->bytes->data(), value->bytes->size() };
        return SC_OK;
    }
    uint64_t sc_channel_value_version(const sc_channel_value* value) noexcept
    {
        return value ? value->version : 0;
    }
    void sc_channel_value_destroy(sc_channel_value* value) noexcept
    {
        delete value;
    }
}
