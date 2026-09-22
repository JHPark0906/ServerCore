#include "C/Internal.h"
#include "Core/Utf8Internal.h"
#include "ServerCore/Observability/AsyncLogger.h"
#include <filesystem>

struct sc_logger { std::shared_ptr<ServerCore::Observability::AsyncLogger> value; };
struct sc_owned_text { std::string value; };

namespace ServerCore::CDetail {
std::shared_ptr<Core::ILogger> LoggerInstance(const sc_logger* logger) { return logger ? logger->value : nullptr; }
sc_status MakeOwnedText(std::string text, sc_owned_text** out) {
    if (!out) return SC_INVALID_ARGUMENT;
    *out = nullptr;
    return Protect([&]() -> sc_status { *out = new sc_owned_text{std::move(text)}; return SC_OK; });
}
sc_status MakeWait(const RegisterWait& registerWait, sc_wait** out) {
    if (!out) return SC_INVALID_ARGUMENT;
    *out = nullptr;
    return Protect([&] {
        auto result = std::make_unique<sc_wait>();
        result->state = std::make_shared<WaitState>();
        auto registered = registerWait([state = result->state](Core::Status status) { state->Complete(std::move(status)); });
        if (!registered.IsOk()) return Code(registered.GetStatus());
        result->subscription = std::move(registered.Value());
        *out = result.release(); return sc_status{SC_OK};
    });
}
}
extern "C" {
uint32_t sc_abi_version(void) { return SC_ABI_VERSION; }
uint32_t sc_capabilities(void) {
    return SC_CAP_WEB | SC_CAP_TCP | SC_CAP_WEB_EXTENSIONS | SC_CAP_OBSERVABILITY;
}
const char* sc_status_name(sc_status value) {
    static const char* names[] = {"Ok", "InvalidArgument", "InvalidFormat", "TooLarge", "NotFound", "AlreadyExists",
        "Closed", "WouldBlock", "PlatformError", "Unimplemented", "UnknownType", "Timeout", "Cancelled"};
    return value >= 0 && value <= SC_CANCELLED ? names[value] : "UnknownStatus";
}
sc_status sc_wait_result(const sc_wait* wait) {
    if (!wait) return SC_INVALID_ARGUMENT;
    return ServerCore::CDetail::Protect([&] {
        std::lock_guard lock(wait->state->mutex); return wait->state->result;
    });
}
sc_status sc_wait_wait(sc_wait* wait, uint32_t timeout) {
    if (!wait) return SC_INVALID_ARGUMENT;
    return ServerCore::CDetail::Protect([&] {
        auto& state = *wait->state;
        std::unique_lock lock(state.mutex);
        const auto ready = [&] { return state.result != SC_WOULD_BLOCK; };
        if (!ready()) {
            if (timeout == 0) return sc_status{SC_WOULD_BLOCK};
            if (timeout == UINT32_MAX) state.wake.wait(lock, ready);
            else if (!state.wake.wait_for(lock, std::chrono::milliseconds(timeout), ready)) return sc_status{SC_TIMEOUT};
        }
        return state.result;
    });
}
void sc_wait_cancel(sc_wait* wait) { if (wait) (void)wait->subscription.Cancel(); }
void sc_wait_destroy(sc_wait* wait) { delete wait; }
sc_status sc_logger_options_init(sc_logger_options* options, size_t size) {
    if (!options || size < sizeof(*options)) return SC_INVALID_ARGUMENT;
    *options = {}; options->abi_version = SC_ABI_VERSION; options->struct_size = sizeof(*options);
    options->minimum_level = SC_LOG_INFO; options->console = 1;
    options->max_queued_messages = 1024; options->max_retained_bytes = 1024 * 1024;
    options->max_message_bytes = 4096; options->max_file_bytes = 10 * 1024 * 1024; options->retained_files = 3;
    return SC_OK;
}
sc_status sc_logger_create(const sc_logger_options* options, sc_logger** out) {
    if (!out) return SC_INVALID_ARGUMENT;
    *out = nullptr;
    namespace C = ServerCore::CDetail;
    if (!C::Version(options) || !C::Valid(options->file) || options->file.len > 32768 ||
        options->minimum_level > SC_LOG_ERROR || options->console > 1 ||
        !ServerCore::Core::Detail::IsValidUtf8(C::Text(options->file)) || C::Text(options->file).find('\0') != std::string_view::npos)
        return SC_INVALID_ARGUMENT;
    return C::Protect([&]() -> sc_status {
        ServerCore::Observability::LoggerOptions decoded;
        decoded.minimumLevel = static_cast<ServerCore::Core::LogLevel>(options->minimum_level);
        decoded.console = options->console != 0;
        if (options->file.len) decoded.file = std::filesystem::path(std::u8string(
            reinterpret_cast<const char8_t*>(options->file.data), options->file.len));
        decoded.maxQueuedMessages = options->max_queued_messages; decoded.maxRetainedBytes = options->max_retained_bytes;
        decoded.maxMessageBytes = options->max_message_bytes; decoded.maxFileBytes = options->max_file_bytes;
        decoded.retainedFiles = options->retained_files;
        auto result = std::make_unique<sc_logger>(); result->value = std::make_shared<ServerCore::Observability::AsyncLogger>();
        auto status = result->value->Start(decoded); if (!status.IsOk()) return C::Code(status);
        *out = result.release(); return SC_OK;
    });
}
sc_status sc_logger_try_write(sc_logger* logger, uint32_t level, sc_bytes message) {
    if (!logger || level > SC_LOG_ERROR || !ServerCore::CDetail::Valid(message)) return SC_INVALID_ARGUMENT;
    return ServerCore::CDetail::Code(logger->value->TryWrite(static_cast<ServerCore::Core::LogLevel>(level), ServerCore::CDetail::Text(message)));
}
sc_status sc_logger_set_minimum_level(sc_logger* logger, uint32_t level) {
    if (!logger || level > SC_LOG_ERROR) return SC_INVALID_ARGUMENT;
    return ServerCore::CDetail::Code(logger->value->SetMinimumLevel(static_cast<ServerCore::Core::LogLevel>(level)));
}
sc_status sc_logger_get_metrics(const sc_logger* logger, sc_logger_metrics* out) {
    if (!logger || !ServerCore::CDetail::Version(out)) return SC_INVALID_ARGUMENT;
    const auto metrics = logger->value->GetMetrics();
    out->pending_messages = metrics.pendingMessages; out->retained_bytes = metrics.retainedBytes;
    out->accepted_messages = metrics.acceptedMessages; out->written_messages = metrics.writtenMessages;
    out->filtered_messages = metrics.filteredMessages; out->dropped_messages = metrics.droppedMessages;
    out->output_errors = metrics.outputErrors; return SC_OK;
}
void sc_logger_request_stop(sc_logger* logger) { if (logger) logger->value->RequestStop(); }
sc_status sc_logger_stop(sc_logger* logger) {
    return !logger ? SC_INVALID_ARGUMENT : ServerCore::CDetail::Protect([&] { return ServerCore::CDetail::Code(logger->value->Stop()); });
}
void sc_logger_destroy(sc_logger* logger) { delete logger; }
sc_bytes sc_owned_text_view(const sc_owned_text* text) { return text ? ServerCore::CDetail::View(text->value) : sc_bytes{}; }
void sc_owned_text_destroy(sc_owned_text* text) { delete text; }
}
