#pragma once
#include "ServerCore/Export.h"

#include "ServerCore/Runtime/TaskExecutor.h"
#include "ServerCore/Web/HttpServer.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace ServerCore::Web
{
struct SseEvent
{
    std::string data;
    std::string event;
    // An absent id leaves the client's last event id unchanged; an empty id
    // explicitly resets it. CR/LF and NUL are not valid in this field.
    std::optional<std::string> id{};
    std::optional<std::uint64_t> retryMilliseconds{};
};

// UTF-8 event framing only; start a chunked text/event-stream response separately.
// data accepts CR/LF/CRLF and emits one data field per line. Metadata cannot
// inject additional lines. The encoded event, including framing, must fit maxBytes.
SERVERCORE_API Core::Result<std::string> EncodeSseEvent(const SseEvent& event, std::size_t maxBytes);
// One bounded write, with no internal retry. On WouldBlock, wait for write
// capacity then retry this event. Success means queued, not received by a client.
SERVERCORE_API Core::Status WriteSseEvent(HttpResponseWriter& writer, const SseEvent& event);

// Serve a regular file through a caller-owned bounded executor.
// Call before starting the response, and let the helper own its writes/finish.
// For status 200: evaluates conditional fields before a single GET byte range;
// emits 206/304/412/416 as applicable. HEAD ignores Range and reads no content.
// Unsupported/multipart/invalid ranges select the full representation. A weak
// size/mtime ETag is generated unless head supplies a valid ETag. Strong If-Range
// matching requires a caller-supplied strong representation tag; date If-Range
// falls back to full content because filesystem timestamps are not strong.
// File size is taken from the opened file, replacing head.contentLength.
// File changes that
// shorten the declared representation abort the response; growth is not sent.
//
// One worker performs blocking file reads and capacity waits. It retains a
// buffer of min(64 KiB, MaxWriteBytes()), plus compact owned path/header metadata;
// both are declared in the executor's byte budget. Request context storage keeps
// its separate server request-budget charge while the task retains the context.
// Cancellation interrupts capacity waits; blocking filesystem calls themselves
// are not forcibly interrupted. The writer token is the task's parent token.
//
// Submission failure leaves the response untouched. An admitted task sends an
// empty 404 for a missing/nonregular file or 500 for other pre-header I/O errors.
// If that error response cannot queue, or a failure occurs after Start, it aborts.
// The TaskHandle reports file/transport failure or cancellation independently of
// the HTTP error response. Success means the complete response was queued locally.
// Cancelling an admitted queued task also aborts its response before releasing it.
SERVERCORE_API Core::Result<Runtime::TaskHandle> SendFile(Runtime::TaskExecutor& executor,
    std::shared_ptr<const HttpRequestContext> context, std::filesystem::path path,
    HttpResponseHead head = {});
}
