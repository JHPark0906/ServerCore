#include "ServerCore/Web/Multipart.h"
#include "Core/Utf8Internal.h"
#include "Web/WebProtocol.h"
#include <algorithm>
#include <atomic>
#include <limits>

namespace ServerCore::Web
{
namespace
{
using Core::ErrorCode;
using Core::Status;
template<class T> Core::Result<T> Fail(ErrorCode code)
{ return Core::Result<T>::FromStatus(Status::FailWithoutMessage(code)); }
std::string_view Trim(std::string_view value)
{
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    return value;
}
std::string Lower(std::string_view value)
{
    std::string result(value);
    for (auto& ch : result) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    return result;
}
bool Metadata(std::string_view value)
{
    for (const unsigned char ch : value) if (ch < 0x20 || ch == 0x7F) return false;
    return Core::Detail::IsValidUtf8(value);
}
struct Parameterized { std::string main; FormFields parameters; };
Core::Result<Parameterized> Parameters(std::string_view input)
{
    Parameterized result;
    const auto semicolon = input.find(';'); result.main = Lower(Trim(input.substr(0, semicolon)));
    input = semicolon == input.npos ? std::string_view{} : input.substr(semicolon + 1);
    while (!input.empty())
    {
        input = Trim(input);
        const auto equal = input.find('=');
        if (equal == input.npos) return Fail<Parameterized>(ErrorCode::InvalidFormat);
        auto name = Lower(Trim(input.substr(0, equal)));
        if (!Detail::IsToken(name)) return Fail<Parameterized>(ErrorCode::InvalidFormat);
        for (const auto& entry : result.parameters)
            if (entry.first == name) return Fail<Parameterized>(ErrorCode::InvalidFormat);
        input.remove_prefix(equal + 1); input = Trim(input);
        std::string value;
        if (!input.empty() && input.front() == '"')
        {
            input.remove_prefix(1); bool closed = false;
            while (!input.empty())
            {
                char ch = input.front(); input.remove_prefix(1);
                if (ch == '"') { closed = true; break; }
                if (ch == '\\')
                {
                    if (input.empty()) return Fail<Parameterized>(ErrorCode::InvalidFormat);
                    ch = input.front(); input.remove_prefix(1);
                }
                value.push_back(ch);
            }
            if (!closed) return Fail<Parameterized>(ErrorCode::InvalidFormat);
            input = Trim(input);
            if (!input.empty())
            {
                if (input.front() != ';') return Fail<Parameterized>(ErrorCode::InvalidFormat);
                input.remove_prefix(1);
                if (Trim(input).empty()) return Fail<Parameterized>(ErrorCode::InvalidFormat);
            }
        }
        else
        {
            const auto next = input.find(';'); const auto token = Trim(input.substr(0, next));
            if (!Detail::IsToken(token)) return Fail<Parameterized>(ErrorCode::InvalidFormat);
            value = token;
            input = next == input.npos ? std::string_view{} : input.substr(next + 1);
            if (next != input.npos && Trim(input).empty()) return Fail<Parameterized>(ErrorCode::InvalidFormat);
        }
        if (!Metadata(value)) return Fail<Parameterized>(ErrorCode::InvalidFormat);
        result.parameters.emplace_back(std::move(name), std::move(value));
    }
    return Core::Result<Parameterized>::FromValue(std::move(result));
}
bool Boundary(std::string_view value)
{
    if (value.empty() || value.size() > 70 || value.back() == ' ') return false;
    for (const unsigned char ch : value)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
              std::string_view("'()+_,-./:=? ").find(static_cast<char>(ch)) != std::string_view::npos)) return false;
    return true;
}
}
class MultipartParser::State
{
public:
    struct Credit { std::atomic<std::size_t> bytes{0}; };
    enum class Phase { Initial, Headers, Body, Epilogue, Done };
    enum class Suffix { More, Invalid, Normal, Final };
    State(std::string value, MultipartOptions limits)
        : options(limits), marker("--" + value), delimiter("\r\n" + marker), credit(std::make_shared<Credit>()) {}
    MultipartOptions options;
    std::string marker, delimiter, buffer;
    std::shared_ptr<Credit> credit;
    Phase phase = Phase::Initial;
    ErrorCode error = ErrorCode::Ok;
    bool eof = false, initialStart = true, file = false;
    std::uint64_t total = 0, partBytes = 0, part = 0;
    std::size_t files = 0;
    std::size_t Retained() const noexcept { return buffer.size() + credit->bytes.load(); }
    void FailWith(ErrorCode code) noexcept
    { error = code; std::string empty; buffer.swap(empty); }
    Suffix DelimiterSuffix(std::size_t start, std::size_t& end) const
    {
        auto pos = start; bool final = false;
        if (pos < buffer.size() && buffer[pos] == '-')
        {
            if (buffer.size() - pos < 2) return Suffix::More;
            if (buffer[pos + 1] != '-') return Suffix::Invalid;
            final = true; pos += 2;
        }
        while (pos < buffer.size() && (buffer[pos] == ' ' || buffer[pos] == '\t')) ++pos;
        if (pos - start > options.maxHeaderBytes) return Suffix::Invalid;
        if (pos == buffer.size())
        {
            if (final && eof) { end = pos; return Suffix::Final; }
            return Suffix::More;
        }
        if (buffer[pos] != '\r') return Suffix::Invalid;
        if (buffer.size() - pos < 2) return Suffix::More;
        if (buffer[pos + 1] != '\n') return Suffix::Invalid;
        end = pos + 2; return final ? Suffix::Final : Suffix::Normal;
    }
    Core::Result<std::shared_ptr<const MultipartEvent>> Event(MultipartEvent event, std::size_t consumed)
    {
        std::size_t bytes = event.data.size() + event.name.size() + event.contentType.size();
        if (event.filename) bytes += event.filename->size();
        for (const auto& [name, value] : event.headers) bytes += name.size() + value.size();
        const auto held = credit->bytes.load();
        if (bytes > options.maxRetainedBytes) { FailWith(ErrorCode::TooLarge); return Fail<std::shared_ptr<const MultipartEvent>>(error); }
        if (bytes > options.maxRetainedBytes - held - (buffer.size() - consumed))
            return Fail<std::shared_ptr<const MultipartEvent>>(ErrorCode::WouldBlock);
        auto owned = std::make_unique<MultipartEvent>(std::move(event));
        credit->bytes.fetch_add(bytes);
        std::shared_ptr<const MultipartEvent> result(owned.release(), [account = credit, bytes](const MultipartEvent* value) {
            delete value; account->bytes.fetch_sub(bytes);
        });
        buffer.erase(0, consumed);
        return Core::Result<std::shared_ptr<const MultipartEvent>>::FromValue(std::move(result));
    }
    Core::Result<std::shared_ptr<const MultipartEvent>> Data(std::size_t count)
    {
        count = (std::min)(count, options.maxEventBytes);
        const auto limit = file ? options.maxFileBytes : options.maxPartBytes;
        if (count > limit - partBytes) { FailWith(ErrorCode::TooLarge); return Fail<std::shared_ptr<const MultipartEvent>>(error); }
        MultipartEvent event; event.kind = MultipartEventKind::Data; event.partIndex = part;
        event.data.assign(reinterpret_cast<const std::byte*>(buffer.data()), reinterpret_cast<const std::byte*>(buffer.data()) + count);
        auto result = Event(std::move(event), count);
        if (result.IsOk()) partBytes += count;
        return result;
    }
    Core::Result<MultipartEvent> Headers(std::string_view block)
    {
        MultipartEvent event; event.partIndex = part + 1;
        bool disposition = false, contentType = false;
        while (!block.empty())
        {
            const auto lineEnd = block.find("\r\n"); const auto line = block.substr(0, lineEnd);
            block = lineEnd == block.npos ? std::string_view{} : block.substr(lineEnd + 2);
            const auto colon = line.find(':');
            if (colon == line.npos || !Detail::IsToken(line.substr(0, colon))) return Fail<MultipartEvent>(ErrorCode::InvalidFormat);
            auto name = Lower(line.substr(0, colon)); const auto value = Trim(line.substr(colon + 1));
            if (!Detail::ValidHeaderValue(value)) return Fail<MultipartEvent>(ErrorCode::InvalidFormat);
            if (event.headers.size() == options.maxHeaders) return Fail<MultipartEvent>(ErrorCode::TooLarge);
            if (name == "content-disposition")
            {
                if (disposition) return Fail<MultipartEvent>(ErrorCode::InvalidFormat);
                disposition = true;
                auto parameters = Parameters(value);
                if (!parameters.IsOk()) return Core::Result<MultipartEvent>::FromStatus(std::move(parameters).TakeStatus());
                if (parameters.Value().main != "form-data") return Fail<MultipartEvent>(ErrorCode::InvalidFormat);
                bool named = false;
                for (const auto& [key, text] : parameters.Value().parameters)
                {
                    if (key == "name") { event.name = text; named = true; }
                    else if (key == "filename") event.filename = text;
                    else if (key == "filename*") return Fail<MultipartEvent>(ErrorCode::InvalidFormat);
                }
                if (!named) return Fail<MultipartEvent>(ErrorCode::InvalidFormat);
            }
            else if (name == "content-type")
            {
                if (contentType) return Fail<MultipartEvent>(ErrorCode::InvalidFormat);
                contentType = true; event.contentType = value;
            }
            else if (name == "content-transfer-encoding") return Fail<MultipartEvent>(ErrorCode::InvalidFormat);
            event.headers.emplace_back(std::move(name), value);
        }
        if (!disposition) return Fail<MultipartEvent>(ErrorCode::InvalidFormat);
        return Core::Result<MultipartEvent>::FromValue(std::move(event));
    }
    Core::Result<std::shared_ptr<const MultipartEvent>> Read()
    {
        using Result = Core::Result<std::shared_ptr<const MultipartEvent>>;
        if (error != ErrorCode::Ok) return Fail<std::shared_ptr<const MultipartEvent>>(error);
        for (;;)
        {
            if (phase == Phase::Done) return Result::FromValue({});
            if (phase == Phase::Epilogue)
            {
                buffer.clear();
                if (eof) { phase = Phase::Done; return Result::FromValue({}); }
                return Fail<std::shared_ptr<const MultipartEvent>>(ErrorCode::WouldBlock);
            }
            if (phase == Phase::Headers)
            {
                const auto end = buffer.find("\r\n\r\n");
                if ((end == buffer.npos && buffer.size() > options.maxHeaderBytes) ||
                    (end != buffer.npos && end + 4 > options.maxHeaderBytes))
                { FailWith(ErrorCode::TooLarge); return Fail<std::shared_ptr<const MultipartEvent>>(error); }
                if (end == buffer.npos) break;
                if (part == options.maxParts) { FailWith(ErrorCode::TooLarge); return Fail<std::shared_ptr<const MultipartEvent>>(error); }
                auto parsed = Headers(std::string_view(buffer).substr(0, end));
                if (!parsed.IsOk()) { FailWith(parsed.GetStatus().Code()); return Result::FromStatus(std::move(parsed).TakeStatus()); }
                const bool nextFile = parsed.Value().filename.has_value();
                if (nextFile && files == options.maxFiles) { FailWith(ErrorCode::TooLarge); return Fail<std::shared_ptr<const MultipartEvent>>(error); }
                auto event = Event(std::move(parsed.Value()), end + 4);
                if (event.IsOk()) { ++part; file = nextFile; if (file) ++files; partBytes = 0; phase = Phase::Body; }
                return event;
            }
            const bool initial = phase == Phase::Initial;
            std::size_t search = 0;
            for (;;)
            {
                std::size_t at;
                std::size_t markerSize;
                if (initial && initialStart && buffer.size() < marker.size() && marker.starts_with(buffer)) break;
                if (initial && initialStart && buffer.starts_with(marker)) { at = 0; markerSize = marker.size(); }
                else { at = buffer.find(delimiter, search); markerSize = delimiter.size(); }
                if (at == buffer.npos)
                {
                    const auto keep = delimiter.size() + 2;
                    if (buffer.size() > keep)
                    {
                        const auto discard = buffer.size() - keep;
                        if (!initial) return Data(discard);
                        buffer.erase(0, discard); initialStart = false;
                    }
                    break;
                }
                std::size_t end = 0; const auto suffix = DelimiterSuffix(at + markerSize, end);
                if (suffix == Suffix::Invalid)
                {
                    if (initial && initialStart && at == 0) initialStart = false;
                    search = at + 1; continue;
                }
                if (at != 0)
                {
                    if (!initial) return Data(at);
                    buffer.erase(0, at); initialStart = false;
                    continue;
                }
                if (suffix == Suffix::More) break;
                if (initial)
                {
                    buffer.erase(0, end); initialStart = false;
                    phase = suffix == Suffix::Final ? Phase::Epilogue : Phase::Headers;
                    break;
                }
                MultipartEvent event; event.kind = MultipartEventKind::PartEnd; event.partIndex = part;
                auto result = Event(std::move(event), end);
                if (result.IsOk()) phase = suffix == Suffix::Final ? Phase::Epilogue : Phase::Headers;
                return result;
            }
            if ((initial && phase != Phase::Initial)) continue;
            break;
        }
        if (eof) { FailWith(ErrorCode::InvalidFormat); return Fail<std::shared_ptr<const MultipartEvent>>(error); }
        return Fail<std::shared_ptr<const MultipartEvent>>(ErrorCode::WouldBlock);
    }
};
MultipartParser::MultipartParser(std::unique_ptr<State> state) noexcept : mState(std::move(state)) {}
MultipartParser::~MultipartParser() = default;
Core::Result<std::unique_ptr<MultipartParser>> MultipartParser::Create(std::string_view contentType, const MultipartOptions& options)
{
    using Result = Core::Result<std::unique_ptr<MultipartParser>>;
    if (!options.maxParts || !options.maxFiles || !options.maxHeaders || !options.maxHeaderBytes ||
        !options.maxPartBytes || !options.maxFileBytes || !options.maxTotalBytes || !options.maxEventBytes ||
        options.maxEventBytes > options.maxRetainedBytes || options.maxHeaderBytes > ((std::numeric_limits<std::size_t>::max)() - 80) / 2 ||
        options.maxRetainedBytes < options.maxHeaderBytes * 2 + 80)
        return Fail<std::unique_ptr<MultipartParser>>(ErrorCode::InvalidArgument);
    if (contentType.size() > options.maxHeaderBytes) return Fail<std::unique_ptr<MultipartParser>>(ErrorCode::TooLarge);
    try
    {
        auto parsed = Parameters(contentType);
        if (!parsed.IsOk()) return Result::FromStatus(std::move(parsed).TakeStatus());
        if (parsed.Value().main != "multipart/form-data") return Fail<std::unique_ptr<MultipartParser>>(ErrorCode::InvalidFormat);
        std::string boundary;
        for (const auto& [name, value] : parsed.Value().parameters) if (name == "boundary") boundary = value;
        if (!Boundary(boundary)) return Fail<std::unique_ptr<MultipartParser>>(ErrorCode::InvalidFormat);
        return Result::FromValue(std::unique_ptr<MultipartParser>(new MultipartParser(std::make_unique<State>(std::move(boundary), options))));
    }
    catch (...) { return Result::FromStatus(Status::AllocationFailure()); }
}
Core::Result<std::size_t> MultipartParser::Feed(std::span<const std::byte> bytes)
{
    auto& state = *mState;
    if (state.error != ErrorCode::Ok) return Fail<std::size_t>(state.error);
    if (state.eof) return Fail<std::size_t>(ErrorCode::Closed);
    if (bytes.empty()) return Core::Result<std::size_t>::FromValue(0);
    if (bytes.size() > state.options.maxTotalBytes - state.total)
    { state.FailWith(ErrorCode::TooLarge); return Fail<std::size_t>(state.error); }
    // Header events own both raw fields and selected metadata. Reserve room for
    // that bounded duplication so a full input buffer cannot prevent its first
    // PartBegin event from being read when no caller holds any prior events.
    const auto inputLimit = state.options.maxRetainedBytes - state.options.maxHeaderBytes;
    const auto count = (std::min)({bytes.size(), state.options.maxRetainedBytes - state.Retained(),
        inputLimit - state.buffer.size()});
    if (!count) return Fail<std::size_t>(ErrorCode::WouldBlock);
    try { state.buffer.append(reinterpret_cast<const char*>(bytes.data()), count); state.total += count; }
    catch (...) { state.FailWith(ErrorCode::PlatformError); return Core::Result<std::size_t>::FromStatus(Status::AllocationFailure()); }
    return Core::Result<std::size_t>::FromValue(count);
}
Core::Result<std::shared_ptr<const MultipartEvent>> MultipartParser::Read()
{
    try { return mState->Read(); }
    catch (...) { mState->FailWith(ErrorCode::PlatformError); return Core::Result<std::shared_ptr<const MultipartEvent>>::FromStatus(Status::AllocationFailure()); }
}
Core::Status MultipartParser::Finish() noexcept
{
    if (mState->error != ErrorCode::Ok) return Status::FailWithoutMessage(mState->error);
    mState->eof = true; return Status::Ok();
}
void MultipartParser::Cancel() noexcept
{
    if (mState->phase != State::Phase::Done && mState->error == ErrorCode::Ok)
        mState->FailWith(ErrorCode::Cancelled);
}
std::size_t MultipartParser::RetainedBytes() const noexcept { return mState->Retained(); }
}
