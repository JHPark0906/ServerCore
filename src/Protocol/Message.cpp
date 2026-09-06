#include "ServerCore/Protocol/Message.h"

#include "Protocol/JsonInternal.h"

#include <cstring>
#include <exception>
#include <new>
#include <string>
#include <utility>

namespace ServerCore::Protocol
{
namespace
{
[[nodiscard]] Core::Status InvalidEnvelope(const std::string_view message)
{
    return Core::Status::Fail(Core::ErrorCode::InvalidFormat, std::string(message));
}

[[nodiscard]] Core::Status PlatformFailureFrom(const std::exception& failure) noexcept
{
    try
    {
        return Core::Status::Fail(Core::ErrorCode::PlatformError, failure.what());
    }
    catch (...)
    {
        return Core::Status::AllocationFailure();
    }
}

[[nodiscard]] bool IsErrorEnvelope(const JsonValue& value)
{
    const JsonValue::Object* const object = value.TryObject();
    if (object == nullptr)
    {
        return false;
    }

    const JsonValue* const code = value.Find("code");
    return code != nullptr && code->TryString() != nullptr;
}

[[nodiscard]] Core::Status ValidateFields(const MessageFields& fields)
{
    if (fields.type.empty())
    {
        return Core::Status::Fail(
            Core::ErrorCode::InvalidArgument, "a message type must not be empty");
    }
    if (fields.body == nullptr && fields.error == nullptr)
    {
        return Core::Status::Fail(
            Core::ErrorCode::InvalidArgument, "a message requires body or error");
    }
    if (fields.body != nullptr && !fields.body->IsObject())
    {
        return Core::Status::Fail(
            Core::ErrorCode::InvalidArgument, "a message body must be a JSON object");
    }
    if (fields.error != nullptr && !IsErrorEnvelope(*fields.error))
    {
        return Core::Status::Fail(Core::ErrorCode::InvalidArgument,
            "a message error requires an object with string code");
    }
    return Core::Status::Ok();
}
}

std::string_view Message::Type() const
{
    return mType;
}

const JsonValue* Message::Body() const noexcept
{
    return mBody ? &*mBody : nullptr;
}

const JsonValue* Message::Sequence() const noexcept
{
    return mSequence ? &*mSequence : nullptr;
}

const JsonValue* Message::Error() const noexcept
{
    return mError ? &*mError : nullptr;
}

std::size_t Message::RawBodySize() const noexcept
{
    return mRawBodySize;
}

Core::Result<Message> ParseMessage(const std::span<const std::byte> jsonBody)
{
    try
    {
        Core::Result<Detail::ParsedEnvelopeDocument> parsed =
            Detail::ParseEnvelopeDocument(jsonBody);
        if (!parsed.IsOk())
        {
            return Core::Result<Message>::FromStatus(std::move(parsed).TakeStatus());
        }

        const Detail::ParsedEnvelopeDocument& document = parsed.Value();
        if (!document.value.IsObject())
        {
            return Core::Result<Message>::FromStatus(
                InvalidEnvelope("the message envelope is not an object"));
        }

        const JsonValue* const type = document.value.Find("type");
        if (type == nullptr || type->TryString() == nullptr || type->TryString()->empty())
        {
            return Core::Result<Message>::FromStatus(
                InvalidEnvelope("the message envelope requires a non-empty string type"));
        }

        const JsonValue* const body = document.value.Find("body");
        const JsonValue* const error = document.value.Find("error");
        if (body == nullptr && error == nullptr)
        {
            return Core::Result<Message>::FromStatus(
                InvalidEnvelope("the message envelope requires body or error"));
        }
        if (body != nullptr && !body->IsObject())
        {
            return Core::Result<Message>::FromStatus(
                InvalidEnvelope("the message body must be a JSON object"));
        }
        if (error != nullptr && !IsErrorEnvelope(*error))
        {
            return Core::Result<Message>::FromStatus(
                InvalidEnvelope("the message error requires an object with string code"));
        }

        // 파서 문서와 수신 버퍼의 수명은 여기서 끝날 수 있다. 반환 메시지는 봉투 필드들을
        // 자체 소유하므로 parse worker에서 JobRunner로 넘어가도 입력 바이트를 빌리지 않는다.
        Message message;
        message.mType = *type->TryString();
        if (body != nullptr)
        {
            message.mBody = *body;
            message.mRawBodySize = document.rawBodySize;
        }
        if (const JsonValue* const sequence = document.value.Find("seq"))
        {
            message.mSequence = *sequence;
        }
        if (error != nullptr)
        {
            message.mError = *error;
        }
        return Core::Result<Message>::FromValue(std::move(message));
    }
    catch (const std::bad_alloc&)
    {
        return Core::Result<Message>::FromStatus(Core::Status::AllocationFailure());
    }
    catch (const std::exception& failure)
    {
        return Core::Result<Message>::FromStatus(PlatformFailureFrom(failure));
    }
}

Core::Result<std::vector<std::byte>> SerializeMessage(const MessageFields& fields)
{
    try
    {
        Core::Status fieldsStatus = ValidateFields(fields);
        if (!fieldsStatus.IsOk())
        {
            return Core::Result<std::vector<std::byte>>::FromStatus(std::move(fieldsStatus));
        }

        JsonValue::Object envelope;
        envelope.emplace("type", JsonValue(std::string(fields.type)));
        if (fields.body != nullptr)
        {
            envelope.emplace("body", *fields.body);
        }
        if (fields.sequence != nullptr)
        {
            envelope.emplace("seq", *fields.sequence);
        }
        if (fields.error != nullptr)
        {
            envelope.emplace("error", *fields.error);
        }

        Core::Result<std::string> dumped = JsonValue(std::move(envelope)).Dump();
        if (!dumped.IsOk())
        {
            return Core::Result<std::vector<std::byte>>::FromStatus(std::move(dumped).TakeStatus());
        }

        const std::string& text = dumped.Value();

        std::vector<std::byte> bytes(text.size());
        if (!text.empty())
        {
            std::memcpy(bytes.data(), text.data(), text.size());
        }
        return Core::Result<std::vector<std::byte>>::FromValue(std::move(bytes));
    }
    catch (const std::bad_alloc&)
    {
        return Core::Result<std::vector<std::byte>>::FromStatus(Core::Status::AllocationFailure());
    }
    catch (const std::exception& failure)
    {
        return Core::Result<std::vector<std::byte>>::FromStatus(PlatformFailureFrom(failure));
    }
}

Core::Result<std::vector<std::byte>> SerializeMessage(
    const std::string_view type, const JsonValue& body)
{
    return SerializeMessage(MessageFields{ type, &body, nullptr, nullptr });
}

Core::Result<PreparedJsonValue> PrepareJsonValue(const JsonValue& value)
{
    using Result = Core::Result<PreparedJsonValue>;
    try
    {
        // 봉투 → body → 배열의 세 컨테이너를 미리 센다. 각 조각이 단독으로 유효해도
        // 조립 후 깊이 상한을 넘는 JSON이 나갈 수 있으므로 검증 시점에 여유를 확보한다.
        auto dumped = Detail::DumpJsonAtDepth(value, 3);
        if (!dumped.IsOk()) return Result::FromStatus(std::move(dumped).TakeStatus());
        PreparedJsonValue prepared;
        prepared.mBytes.resize(dumped.Value().size());
        std::memcpy(prepared.mBytes.data(), dumped.Value().data(), prepared.mBytes.size());
        return Result::FromValue(std::move(prepared));
    }
    catch (const std::bad_alloc&) { return Result::FromStatus(Core::Status::AllocationFailure()); }
    catch (const std::exception& failure) { return Result::FromStatus(PlatformFailureFrom(failure)); }
}

Core::Result<PreparedMessage> PrepareMessage(const MessageFields& fields)
{
    auto bytes = SerializeMessage(fields);
    if (!bytes.IsOk()) return Core::Result<PreparedMessage>::FromStatus(std::move(bytes).TakeStatus());
    PreparedMessage prepared;
    prepared.mBytes = std::move(bytes.Value());
    return Core::Result<PreparedMessage>::FromValue(std::move(prepared));
}

Core::Result<PreparedMessage> PrepareArrayMessage(const std::string_view type,
    const std::string_view arrayKey, const std::span<const PreparedJsonValue* const> items)
{
    using Result = Core::Result<PreparedMessage>;
    try
    {
        if (type.empty()) return Result::FromStatus(Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument));
        auto quotedType = JsonValue(std::string(type)).Dump();
        auto quotedKey = JsonValue(std::string(arrayKey)).Dump();
        if (!quotedType.IsOk()) return Result::FromStatus(std::move(quotedType).TakeStatus());
        if (!quotedKey.IsOk()) return Result::FromStatus(std::move(quotedKey).TakeStatus());
        std::string text = "{\"body\":{" + quotedKey.Value() + ":[";
        bool first = true;
        for (const PreparedJsonValue* item : items)
        {
            if (!item || item->Size() == 0)
                return Result::FromStatus(Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument));
            if (!first) text.push_back(',');
            text.append(reinterpret_cast<const char*>(item->Bytes().data()), item->Size());
            first = false;
        }
        text += "]},\"type\":";
        text += quotedType.Value();
        text.push_back('}');
        PreparedMessage prepared;
        prepared.mBytes.resize(text.size());
        std::memcpy(prepared.mBytes.data(), text.data(), text.size());
        return Result::FromValue(std::move(prepared));
    }
    catch (const std::bad_alloc&) { return Result::FromStatus(Core::Status::AllocationFailure()); }
    catch (const std::exception& failure) { return Result::FromStatus(PlatformFailureFrom(failure)); }
}
}
