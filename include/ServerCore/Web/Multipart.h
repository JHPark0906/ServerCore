#pragma once
#include "ServerCore/Export.h"
#include "ServerCore/Web/RequestData.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace ServerCore::Web
{
struct MultipartOptions
{
    std::size_t maxParts = 128;
    std::size_t maxFiles = 32;
    std::size_t maxHeaders = 32;
    std::size_t maxHeaderBytes = 16 * 1024;
    std::uint64_t maxPartBytes = 1024 * 1024; // Parts without filename.
    std::uint64_t maxFileBytes = 64 * 1024 * 1024;
    std::uint64_t maxTotalBytes = 128 * 1024 * 1024; // Entire encoded input.
    std::size_t maxRetainedBytes = 256 * 1024; // Input + outstanding event payload.
    std::size_t maxEventBytes = 32 * 1024;
};
enum class MultipartEventKind { PartBegin, Data, PartEnd };
struct MultipartEvent
{
    MultipartEventKind kind = MultipartEventKind::PartBegin;
    std::uint64_t partIndex = 0; // 1-based, stable across the part's events.
    // Metadata only on PartBegin. Names/filenames are UTF-8; filenames are
    // never decoded, normalized or used as paths. Repeated form names remain.
    std::string name;
    std::optional<std::string> filename{};
    std::string contentType;
    FormFields headers; // Lowercase names, owned values.
    std::vector<std::byte> data; // Data events only; opaque, possibly non-UTF8.
};
// Serialized feed/pull parser, no worker or foreign callbacks. Concurrent calls
// on one parser require external serialization. Events may outlive the parser
// and are immutable; releasing them from another thread returns their charge.
// Feed accepts a bounded PREFIX and reports bytes consumed. Retry remaining
// input after reading/releasing events. WouldBlock consumes none. RetainedBytes
// counts live input and event string/data bytes; container capacity/metadata and
// temporary copies are bounded separately by these limits, not exact RSS.
// Finish marks input EOF (idempotent), then drain Read to null for validated EOF.
// Read returns WouldBlock when more input/released capacity is needed. Bad syntax
// or early EOF is sticky InvalidFormat; limits are TooLarge; Cancel is sticky
// Cancelled and releases buffered input, while previously returned events survive.
// No filesystem writes. Preamble/epilogue are ignored but count toward total.
// CRLF framing, quoted boundary (1..70 ASCII bytes), UTF-8 disposition metadata.
// Rejects duplicate disposition parameters, filename*, folded/duplicate framing
// headers and Content-Transfer-Encoding. Nested multipart stays opaque payload;
// no charset or transfer decoding. Commit application effects only after EOF.
class MultipartParser
{
public:
    SERVERCORE_API static Core::Result<std::unique_ptr<MultipartParser>> Create(
        std::string_view contentType, const MultipartOptions& options = {});
    SERVERCORE_API ~MultipartParser();
    MultipartParser(const MultipartParser&) = delete;
    MultipartParser& operator=(const MultipartParser&) = delete;
    SERVERCORE_API Core::Result<std::size_t> Feed(std::span<const std::byte> bytes);
    SERVERCORE_API Core::Result<std::shared_ptr<const MultipartEvent>> Read();
    SERVERCORE_API Core::Status Finish() noexcept;
    SERVERCORE_API void Cancel() noexcept;
    [[nodiscard]] SERVERCORE_API std::size_t RetainedBytes() const noexcept;
private:
    class State;
    SERVERCORE_API explicit MultipartParser(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> mState;
};
}
