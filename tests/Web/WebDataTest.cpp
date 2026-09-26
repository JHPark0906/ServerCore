#include "ServerCore/Web/Multipart.h"
#include "TestHarness.h"
#include <algorithm>
#include <string>
#include <vector>

namespace
{
using namespace ServerCore::Web;
using ServerCore::Core::ErrorCode;
using ServerCore::Core::Status;
using ServerCoreTest::ExpectEqual;
using ServerCoreTest::ExpectTrue;
std::span<const std::byte> Bytes(std::string_view text)
{
    return { reinterpret_cast<const std::byte*>(text.data()), text.size() };
}
void Code(ErrorCode expected, const Status& actual, std::string_view label)
{
    ExpectEqual(static_cast<int>(expected), static_cast<int>(actual.Code()), label);
}
void RequestFieldsAndEncoding()
{
    auto query = ParseQueryParameters("/a?x=a+b&x=%252F&%EA%B0%80=%E2%98%83&empty&=v&&");
    ExpectTrue(query.IsOk(), "query decodes UTF-8 with duplicate and empty fields");
    if (!query.IsOk())
        return;
    ExpectEqual(
        std::size_t{ 5 }, query.Value().size(), "empty ampersand segments do not create fields");
    ExpectEqual(std::string("a+b"), query.Value()[0].second, "query plus is literal");
    ExpectEqual(
        std::string("%2F"), query.Value()[1].second, "percent decoding occurs exactly once");
    ExpectEqual(std::string("\xEA\xB0\x80"), query.Value()[2].first, "names decode as UTF-8");
    ExpectEqual(std::string(), query.Value()[3].second, "missing equals produces empty value");
    auto form = ParseFormUrlEncoded("x=a+b%2Bc&x=%26%3D");
    ExpectTrue(form.IsOk(), "form splits before decoding delimiters");
    if (form.IsOk())
    {
        ExpectEqual(
            std::string("a b+c"), form.Value()[0].second, "form plus rule differs from query");
        ExpectEqual(
            std::string("&="), form.Value()[1].second, "escaped separators remain field data");
    }
    auto encoded = PercentEncodeComponent("a +/\xEA\xB0\x80", true);
    ExpectTrue(encoded.IsOk(), "form encoder accepts UTF-8");
    if (encoded.IsOk())
        ExpectEqual(std::string("a+%2B%2F%EA%B0%80"), encoded.Value(),
            "encoder emits unambiguous uppercase escapes");
    auto formSet = PercentEncodeComponent("*~", true);
    auto uriSet = PercentEncodeComponent("*~", false);
    ExpectTrue(
        formSet.IsOk() && formSet.Value() == "*%7E", "HTML form encoder uses its own literal set");
    ExpectTrue(uriSet.IsOk() && uriSet.Value() == "%2A~",
        "URI component encoder uses RFC 3986 unreserved set");
    for (const auto invalid : { "%", "%0", "%XZ", "%00", "%C0%AF", "%ED%A0%80", "%F4%90%80%80" })
        Code(ErrorCode::InvalidFormat, PercentDecodeComponent(invalid).GetStatus(),
            "invalid encodings never normalize silently");
    auto line = PercentDecodeComponent("%0D%0A");
    ExpectTrue(line.IsOk() && line.Value() == "\r\n",
        "component decoder leaves context-specific control validation to callers");
    FieldLimits limits;
    limits.maxFields = 1;
    Code(ErrorCode::TooLarge, ParseFormUrlEncoded("a=1&a=2", limits).GetStatus(),
        "duplicates consume field count capacity");
    limits = {};
    limits.maxValueBytes = 2;
    Code(ErrorCode::TooLarge, ParseFormUrlEncoded("a=123", limits).GetStatus(),
        "decoded value bound is enforced");
    limits = {};
    limits.maxBytes = 3;
    Code(ErrorCode::TooLarge, ParseQueryParameters("/?a=12", limits).GetStatus(),
        "encoded query bound is enforced");
    Code(ErrorCode::InvalidFormat, ParseQueryParameters("/?x=1#fragment").GetStatus(),
        "request targets cannot contain fragments");
    Code(ErrorCode::TooLarge, PercentEncodeComponent(" ", false, 2).GetStatus(),
        "encoded output has an independent bound");
}
void CookiesValidateAndPreserveDuplicates()
{
    auto fields = ParseCookieHeader("sid=one; sid=\"two\"; raw=%2F; empty=");
    ExpectTrue(fields.IsOk(), "strict cookies accept quoted values and duplicates");
    if (!fields.IsOk())
        return;
    ExpectEqual(std::size_t{ 4 }, fields.Value().size(), "cookie duplicates preserve order");
    ExpectEqual(std::string("two"), fields.Value()[1].second, "outer cookie quotes are removed");
    ExpectEqual(
        std::string("%2F"), fields.Value()[2].second, "cookie values are not percent decoded");
    for (const auto value :
        { "bad", "a=white space", "a=\"unterminated", "a=x\r\nB:y", "a=comma,value" })
        Code(ErrorCode::InvalidFormat, ParseCookieHeader(value).GetStatus(),
            "invalid cookie grammar is rejected");
    CookieOptions options;
    options.secure = true;
    options.maxAgeSeconds = -1;
    auto cookie = SerializeSetCookie("__Host-sid", "abc", options);
    ExpectTrue(cookie.IsOk(), "secure host cookie can be serialized");
    if (cookie.IsOk())
    {
        ExpectTrue(cookie.Value().find("Max-Age=0") != std::string::npos,
            "nonpositive max age deletes the cookie");
        ExpectTrue(cookie.Value().find("Secure") != std::string::npos &&
                       cookie.Value().find("HttpOnly") != std::string::npos,
            "cookie security attributes are explicit");
    }
    options.domain = "example.com";
    Code(ErrorCode::InvalidArgument, SerializeSetCookie("__Host-sid", "abc", options).GetStatus(),
        "host cookie cannot set Domain");
    options = {};
    options.sameSite = CookieSameSite::None;
    Code(ErrorCode::InvalidArgument, SerializeSetCookie("sid", "abc", options).GetStatus(),
        "SameSite None requires Secure");
    options = {};
    options.path = "/; injected=x";
    Code(ErrorCode::InvalidArgument, SerializeSetCookie("sid", "abc", options).GetStatus(),
        "path cannot inject another attribute");
    Code(ErrorCode::InvalidArgument, SerializeSetCookie("sid", "a\r\nb").GetStatus(),
        "cookie value cannot inject a header");
    options = {};
    options.maxBytes = 2;
    Code(ErrorCode::TooLarge, SerializeSetCookie("sid", "abc", options).GetStatus(),
        "serialized cookie is bounded");
}

struct Parsed
{
    std::vector<std::string> names, filenames, payloads;
    std::size_t ends = 0;
    bool eof = false;
};
bool Drain(MultipartParser& parser, Parsed& output, ErrorCode expected = ErrorCode::WouldBlock)
{
    for (std::size_t iteration = 0; iteration < 10000; ++iteration)
    {
        auto read = parser.Read();
        if (!read.IsOk())
        {
            Code(expected, read.GetStatus(), "parser reaches expected input boundary");
            return read.GetStatus().Code() == expected;
        }
        if (!read.Value())
        {
            output.eof = true;
            return true;
        }
        const auto& event = *read.Value();
        if (event.kind == MultipartEventKind::PartBegin)
        {
            output.names.push_back(event.name);
            output.filenames.push_back(event.filename.value_or(""));
            output.payloads.emplace_back();
        }
        else if (event.kind == MultipartEventKind::Data)
        {
            if (output.payloads.empty())
            {
                ExpectTrue(false, "data follows a PartBegin");
                return false;
            }
            output.payloads.back().append(
                reinterpret_cast<const char*>(event.data.data()), event.data.size());
        }
        else
            ++output.ends;
    }
    ExpectTrue(false, "parser output is finite");
    return false;
}
std::string Message(
    std::string_view data, std::string_view disposition = "form-data; name=\"field\"")
{
    return "--test\r\nContent-Disposition: " + std::string(disposition) + "\r\n\r\n" +
           std::string(data) + "\r\n--test--\r\n";
}
void MultipartArbitraryChunkBoundaries()
{
    const std::string file = std::string("A\0B", 3) + "\r\n--testX\r\n--tes";
    const std::string message =
        "preamble\r\n--test\r\nContent-Disposition: form-data; name=\"same\"\r\n\r\nvalue\r\n"
        "--test\r\nContent-Disposition: form-data; name=\"same\"; filename=\"../upload.bin\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\n" +
        file + "\r\n--test-- \t\r\nepilogue";
    for (std::size_t split = 0; split <= message.size(); ++split)
    {
        auto created = MultipartParser::Create("multipart/form-data; boundary=\"test\"");
        ExpectTrue(created.IsOk(), "quoted multipart boundary accepted");
        if (!created.IsOk())
            return;
        auto& parser = *created.Value();
        Parsed output;
        for (const auto piece :
            { std::string_view(message).substr(0, split), std::string_view(message).substr(split) })
        {
            auto fed = parser.Feed(Bytes(piece));
            ExpectTrue(fed.IsOk() && fed.Value() == piece.size(), "chunk copied before return");
            if (!fed.IsOk() || !Drain(parser, output))
                return;
        }
        ExpectTrue(parser.Finish().IsOk() && Drain(parser, output) && output.eof,
            "final delimiter and EOF validated across every split");
        ExpectTrue(output.names == std::vector<std::string>{ "same", "same" },
            "multipart duplicate names preserve order");
        ExpectTrue(output.payloads == std::vector<std::string>{ "value", file },
            "binary data and boundary lookalikes survive unchanged");
        if (output.filenames.size() == 2)
            ExpectEqual(std::string("../upload.bin"), output.filenames[1],
                "filename remains uninterpreted metadata");
        ExpectEqual(std::size_t{ 2 }, output.ends, "every part emits one terminal event");
    }
    auto created = MultipartParser::Create("multipart/form-data; boundary=test");
    if (!created.IsOk())
    {
        ExpectTrue(false, "one-byte fixture created");
        return;
    }
    Parsed output;
    for (const char byte : Message("abc"))
    {
        const std::string_view one(&byte, 1);
        auto fed = created.Value()->Feed(Bytes(one));
        ExpectTrue(fed.IsOk(), "one-byte input accepted");
        if (!fed.IsOk() || !Drain(*created.Value(), output))
            return;
    }
    ExpectTrue(created.Value()->Finish().IsOk() && Drain(*created.Value(), output) && output.eof,
        "one-byte input completes");
}
void MultipartBoundsAndOwnedEvents()
{
    MultipartOptions options;
    options.maxHeaderBytes = 128;
    options.maxRetainedBytes = 336;
    options.maxEventBytes = 64;
    const auto wire = Message(std::string(900, 'x'));
    auto created = MultipartParser::Create("multipart/form-data; boundary=test", options);
    ExpectTrue(created.IsOk(), "small bounded parser created");
    if (!created.IsOk())
        return;
    auto& parser = *created.Value();
    std::size_t offset = 0, received = 0;
    std::vector<std::shared_ptr<const MultipartEvent>> held;
    bool blocked = false;
    for (std::size_t step = 0; step < 1000 && offset < wire.size(); ++step)
    {
        auto feed = parser.Feed(Bytes(std::string_view(wire).substr(offset)));
        if (feed.IsOk())
            offset += feed.Value();
        else if (feed.GetStatus().Code() == ErrorCode::WouldBlock)
            blocked = true;
        else
        {
            ExpectTrue(false, "bounded feed only backpressures");
            return;
        }
        for (;;)
        {
            auto event = parser.Read();
            if (!event.IsOk())
            {
                Code(
                    ErrorCode::WouldBlock, event.GetStatus(), "held-event pressure is recoverable");
                break;
            }
            if (!event.Value())
                break;
            if (event.Value()->kind == MultipartEventKind::Data)
                received += event.Value()->data.size();
            held.push_back(std::move(event.Value()));
        }
        ExpectTrue(parser.RetainedBytes() <= options.maxRetainedBytes,
            "buffer and outstanding output share one byte bound");
        if (blocked)
            break;
    }
    ExpectTrue(
        blocked && !held.empty(), "retaining immutable output eventually backpressures new input");
    const auto before = parser.RetainedBytes();
    held.clear();
    ExpectTrue(parser.RetainedBytes() < before, "dropping output immediately returns byte credit");
    for (std::size_t step = 0; step < 1000 && offset < wire.size(); ++step)
    {
        auto feed = parser.Feed(Bytes(std::string_view(wire).substr(offset)));
        if (feed.IsOk())
            offset += feed.Value();
        else
        {
            Code(ErrorCode::WouldBlock, feed.GetStatus(), "full input requires draining");
        }
        for (;;)
        {
            auto event = parser.Read();
            if (!event.IsOk())
            {
                Code(ErrorCode::WouldBlock, event.GetStatus(), "drain waits for more bytes");
                break;
            }
            if (!event.Value())
                break;
            if (event.Value()->kind == MultipartEventKind::Data)
            {
                received += event.Value()->data.size();
                held = { event.Value() };
            }
        }
        held.clear();
    }
    ExpectEqual(wire.size(), offset, "all remaining input progresses after release");
    ExpectTrue(parser.Finish().IsOk(), "bounded transfer EOF accepted");
    for (;;)
    {
        auto event = parser.Read();
        ExpectTrue(event.IsOk(), "bounded transfer finishes without early EOF");
        if (!event.IsOk() || !event.Value())
            break;
        if (event.Value()->kind == MultipartEventKind::Data)
            received += event.Value()->data.size();
        held = { event.Value() };
    }
    ExpectEqual(std::size_t{ 900 }, received, "large payload never requires whole-part buffering");
    auto lifetime = MultipartParser::Create("multipart/form-data; boundary=test");
    if (!lifetime.IsOk())
    {
        ExpectTrue(false, "lifetime fixture created");
        return;
    }
    ExpectTrue(lifetime.Value()->Feed(Bytes(Message("owned"))).IsOk(), "lifetime fixture fed");
    auto begin = lifetime.Value()->Read();
    ExpectTrue(begin.IsOk() && begin.Value(), "lifetime event returned");
    lifetime.Value().reset();
    if (begin.IsOk() && begin.Value())
        ExpectEqual(
            std::string("field"), begin.Value()->name, "owned event survives parser destruction");
}
void MultipartRejectsMalformedAndLimits()
{
    auto check = [](std::string_view wire, ErrorCode expected, MultipartOptions options = {})
    {
        auto created = MultipartParser::Create("multipart/form-data; boundary=test", options);
        ExpectTrue(created.IsOk(), "failure fixture created");
        if (!created.IsOk())
            return;
        auto feed = created.Value()->Feed(Bytes(wire));
        if (!feed.IsOk())
        {
            Code(expected, feed.GetStatus(), "input-wide limit rejects atomically");
            return;
        }
        ExpectEqual(wire.size(), feed.Value(), "small failure fixture fits input capacity");
        ExpectTrue(created.Value()->Finish().IsOk(), "EOF marked before validation");
        for (std::size_t step = 0; step < 100; ++step)
        {
            auto event = created.Value()->Read();
            if (!event.IsOk())
            {
                Code(
                    expected, event.GetStatus(), "malformed multipart has precise terminal status");
                created.Value()->Cancel();
                Code(expected, created.Value()->Read().GetStatus(),
                    "first failure remains sticky after cancel");
                return;
            }
            if (!event.Value())
                break;
        }
        ExpectTrue(false, "malformed multipart cannot complete successfully");
    };
    check(
        "--test\r\nContent-Disposition: form-data; name=x\r\n\r\nearly", ErrorCode::InvalidFormat);
    check(Message("x", "form-data; name=x; name=y"), ErrorCode::InvalidFormat);
    check(Message("x", "form-data; name=x; filename*=UTF-8''file"), ErrorCode::InvalidFormat);
    check("--test\r\nContent-Disposition: form-data; name=x\r\nContent-Transfer-Encoding: "
          "base64\r\n\r\neA==\r\n--test--\r\n",
        ErrorCode::InvalidFormat);
    MultipartOptions options;
    options.maxPartBytes = 2;
    check(Message("123"), ErrorCode::TooLarge, options);
    options = {};
    options.maxFileBytes = 2;
    check(Message("123", "form-data; name=x; filename=x"), ErrorCode::TooLarge, options);
    options = {};
    options.maxTotalBytes = 10;
    check(Message("a"), ErrorCode::TooLarge, options);
    options = {};
    options.maxHeaderBytes = 40;
    check(Message("a"), ErrorCode::TooLarge, options);
    const auto two = Message("a");
    const auto suffix = two.find("--test--");
    const auto repeated = two.substr(0, suffix) + Message("b");
    options = {};
    options.maxParts = 1;
    check(repeated, ErrorCode::TooLarge, options);
    const auto firstFile = Message("a", "form-data; name=x; filename=a");
    const auto twoFiles = firstFile.substr(0, firstFile.find("--test--")) +
                          Message("b", "form-data; name=x; filename=b");
    options = {};
    options.maxFiles = 1;
    check(twoFiles, ErrorCode::TooLarge, options);
    options = {};
    options.maxHeaders = 1;
    check("--test\r\nContent-Disposition: form-data; name=x\r\nContent-Type: "
          "text/plain\r\n\r\nx\r\n--test--\r\n",
        ErrorCode::TooLarge, options);
    Code(ErrorCode::InvalidFormat,
        MultipartParser::Create("multipart/form-data; boundary=x; boundary=y").GetStatus(),
        "duplicate boundary rejected");
    Code(ErrorCode::InvalidFormat,
        MultipartParser::Create("multipart/form-data; boundary=\"bad \"").GetStatus(),
        "boundary cannot end in space");
    options = {};
    options.maxRetainedBytes = 64;
    Code(ErrorCode::InvalidArgument,
        MultipartParser::Create("multipart/form-data; boundary=x", options).GetStatus(),
        "budget must fit bounded header duplication");
    auto created = MultipartParser::Create("multipart/form-data; boundary=x");
    if (created.IsOk())
    {
        created.Value()->Cancel();
        Code(ErrorCode::Cancelled, created.Value()->Read().GetStatus(),
            "cancel wakes pull consumers with terminal status");
        Code(ErrorCode::Cancelled, created.Value()->Feed(Bytes("x")).GetStatus(),
            "cancel rejects later input");
    }
}
const ServerCoreTest::CheckRegistration fields(
    "Web.RequestFieldsAndEncoding", RequestFieldsAndEncoding);
const ServerCoreTest::CheckRegistration cookies(
    "Web.CookiesValidateAndPreserveDuplicates", CookiesValidateAndPreserveDuplicates);
const ServerCoreTest::CheckRegistration chunks(
    "Web.MultipartArbitraryChunkBoundaries", MultipartArbitraryChunkBoundaries);
const ServerCoreTest::CheckRegistration bounds(
    "Web.MultipartBoundsAndOwnedEvents", MultipartBoundsAndOwnedEvents);
const ServerCoreTest::CheckRegistration malformed(
    "Web.MultipartRejectsMalformedAndLimits", MultipartRejectsMalformedAndLimits);
}
