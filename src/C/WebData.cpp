#include "ServerCore/C/WebData.h"
#include "ServerCore/Web/Multipart.h"
#include "C/Internal.h"
#include <mutex>

namespace W = ServerCore::Web;
namespace C = ServerCore::CDetail;
struct sc_web_fields { W::FormFields fields; };
struct sc_web_data_text { std::string text; };
struct sc_multipart { std::unique_ptr<W::MultipartParser> parser; mutable std::mutex mutex; };
struct sc_multipart_event { std::shared_ptr<const W::MultipartEvent> event; };
namespace {
template<class T> sc_status Init(T* out, size_t size, T value) noexcept {
    if (!out || size < sizeof(T)) return SC_INVALID_ARGUMENT;
    value.abi_version = SC_ABI_VERSION; value.struct_size = sizeof(T); *out = value; return SC_OK;
}
template<class Operation> sc_status Text(sc_web_data_text** out, Operation operation) {
    if (!out) return SC_INVALID_ARGUMENT;
    *out = nullptr;
    return C::Protect([&]() -> sc_status {
        auto value = operation();
        if (!value.IsOk()) return C::Code(value.GetStatus());
        *out = new sc_web_data_text{std::move(value.Value())}; return SC_OK;
    });
}
W::MultipartOptions Options(const sc_multipart_options& value) {
    return {value.max_parts,value.max_files,value.max_headers,value.max_header_bytes,
        value.max_part_bytes,value.max_file_bytes,value.max_total_bytes,value.max_retained_bytes,value.max_event_bytes};
}
}
extern "C" {
sc_status sc_web_field_limits_init(sc_web_field_limits* out, size_t size) {
    return Init(out,size,{0,0,64*1024,128,1024,64*1024});
}
sc_status sc_web_cookie_options_init(sc_web_cookie_options* out, size_t size) {
    return Init(out,size,{0,0,{},C::View("/"),0,0,SC_COOKIE_LAX,0,1,4096});
}
sc_status sc_multipart_options_init(sc_multipart_options* out, size_t size) {
    return Init(out,size,{0,0,128,32,32,16*1024,1024*1024,64*1024*1024,128*1024*1024,256*1024,32*1024});
}
sc_status sc_web_parse_fields(uint32_t kind, sc_bytes input, const sc_web_field_limits* limits, sc_web_fields** out) {
    if (!out) return SC_INVALID_ARGUMENT;
    *out = nullptr;
    if (!C::Valid(input) || !C::Version(limits) || kind > SC_FIELDS_COOKIE) return SC_INVALID_ARGUMENT;
    return C::Protect([&]() -> sc_status {
        const W::FieldLimits options{limits->max_bytes,limits->max_fields,limits->max_name_bytes,limits->max_value_bytes};
        auto result = kind == SC_FIELDS_QUERY ? W::ParseQueryParameters(C::Text(input),options) :
            kind == SC_FIELDS_FORM ? W::ParseFormUrlEncoded(C::Text(input),options) : W::ParseCookieHeader(C::Text(input),options);
        if (!result.IsOk()) return C::Code(result.GetStatus());
        *out = new sc_web_fields{std::move(result.Value())}; return SC_OK;
    });
}
size_t sc_web_fields_count(const sc_web_fields* value) { return value ? value->fields.size() : 0; }
sc_status sc_web_fields_at(const sc_web_fields* value, size_t index, sc_header* out) {
    if (!out) return SC_INVALID_ARGUMENT;
    *out = {};
    if (!value || index >= value->fields.size()) return SC_INVALID_ARGUMENT;
    *out = {C::View(value->fields[index].first),C::View(value->fields[index].second)}; return SC_OK;
}
void sc_web_fields_destroy(sc_web_fields* value) { delete value; }
sc_status sc_web_percent_decode(sc_bytes input, uint32_t plus, size_t maxBytes, sc_web_data_text** out) {
    if (!C::Valid(input) || plus > 1) { if (out) *out = nullptr; return SC_INVALID_ARGUMENT; }
    return Text(out,[&] { return W::PercentDecodeComponent(C::Text(input),plus != 0,maxBytes); });
}
sc_status sc_web_percent_encode(sc_bytes input, uint32_t form, size_t maxBytes, sc_web_data_text** out) {
    if (!C::Valid(input) || form > 1) { if (out) *out = nullptr; return SC_INVALID_ARGUMENT; }
    return Text(out,[&] { return W::PercentEncodeComponent(C::Text(input),form != 0,maxBytes); });
}
sc_status sc_web_set_cookie(sc_bytes name, sc_bytes value, const sc_web_cookie_options* options, sc_web_data_text** out) {
    if (!out) return SC_INVALID_ARGUMENT;
    *out = nullptr;
    if (!C::Valid(name) || !C::Valid(value) || !C::Version(options) || !C::Valid(options->domain) || !C::Valid(options->path) ||
        options->has_max_age > 1 || options->secure > 1 || options->http_only > 1 || options->same_site > SC_COOKIE_NONE)
        return SC_INVALID_ARGUMENT;
    return Text(out,[&] {
        W::CookieOptions native;
        native.domain = C::Text(options->domain); native.path = C::Text(options->path);
        if (options->has_max_age) native.maxAgeSeconds = options->max_age_seconds;
        native.sameSite = static_cast<W::CookieSameSite>(options->same_site);
        native.secure = options->secure != 0; native.httpOnly = options->http_only != 0; native.maxBytes = options->max_bytes;
        return W::SerializeSetCookie(C::Text(name),C::Text(value),native);
    });
}
sc_status sc_web_data_text_view(const sc_web_data_text* value, sc_bytes* out) {
    if (!out) return SC_INVALID_ARGUMENT;
    *out = {};
    if (!value) return SC_INVALID_ARGUMENT;
    *out = C::View(value->text); return SC_OK;
}
void sc_web_data_text_destroy(sc_web_data_text* value) { delete value; }
sc_status sc_multipart_create(sc_bytes contentType, const sc_multipart_options* options, sc_multipart** out) {
    if (!out) return SC_INVALID_ARGUMENT;
    *out = nullptr;
    if (!C::Valid(contentType) || !C::Version(options)) return SC_INVALID_ARGUMENT;
    return C::Protect([&]() -> sc_status {
        auto parsed = W::MultipartParser::Create(C::Text(contentType),Options(*options));
        if (!parsed.IsOk()) return C::Code(parsed.GetStatus());
        auto result = std::make_unique<sc_multipart>(); result->parser = std::move(parsed.Value());
        *out = result.release(); return SC_OK;
    });
}
sc_status sc_multipart_feed(sc_multipart* value, sc_bytes input, size_t* consumed) {
    if (!consumed) return SC_INVALID_ARGUMENT;
    *consumed = 0;
    if (!value || !C::Valid(input)) return SC_INVALID_ARGUMENT;
    return C::Protect([&]() -> sc_status {
        const std::lock_guard guard(value->mutex);
        auto result = value->parser->Feed(C::Bytes(input));
        if (!result.IsOk()) return C::Code(result.GetStatus());
        *consumed = result.Value(); return SC_OK;
    });
}
sc_status sc_multipart_read(sc_multipart* value, sc_multipart_event** out) {
    if (!out) return SC_INVALID_ARGUMENT;
    *out = nullptr;
    if (!value) return SC_INVALID_ARGUMENT;
    return C::Protect([&]() -> sc_status {
        auto event = std::make_unique<sc_multipart_event>();
        const std::lock_guard guard(value->mutex);
        auto result = value->parser->Read();
        if (!result.IsOk()) return C::Code(result.GetStatus());
        if (!result.Value()) return SC_OK;
        event->event = std::move(result.Value()); *out = event.release(); return SC_OK;
    });
}
sc_status sc_multipart_finish(sc_multipart* value) {
    if (!value) return SC_INVALID_ARGUMENT;
    const std::lock_guard guard(value->mutex); return C::Code(value->parser->Finish());
}
void sc_multipart_cancel(sc_multipart* value) { if (value) { const std::lock_guard guard(value->mutex); value->parser->Cancel(); } }
size_t sc_multipart_retained_bytes(const sc_multipart* value) {
    if (!value) return 0;
    const std::lock_guard guard(value->mutex); return value->parser->RetainedBytes();
}
void sc_multipart_destroy(sc_multipart* value) { delete value; }
sc_status sc_multipart_event_get(const sc_multipart_event* value, sc_multipart_event_view* out) {
    if (!value || !C::Version(out)) return SC_INVALID_ARGUMENT;
    const auto& event = *value->event;
    out->kind = static_cast<uint32_t>(event.kind); out->part_index = event.partIndex;
    out->name = C::View(event.name); out->filename = event.filename ? C::View(*event.filename) : sc_bytes{};
    out->content_type = C::View(event.contentType); out->has_filename = event.filename.has_value() ? 1u : 0u;
    out->data = {reinterpret_cast<const uint8_t*>(event.data.data()),event.data.size()}; return SC_OK;
}
size_t sc_multipart_event_header_count(const sc_multipart_event* value) { return value ? value->event->headers.size() : 0; }
sc_status sc_multipart_event_header_at(const sc_multipart_event* value, size_t index, sc_header* out) {
    if (!out) return SC_INVALID_ARGUMENT;
    *out = {};
    if (!value || index >= value->event->headers.size()) return SC_INVALID_ARGUMENT;
    const auto& field = value->event->headers[index]; *out = {C::View(field.first),C::View(field.second)}; return SC_OK;
}
void sc_multipart_event_destroy(sc_multipart_event* value) { delete value; }
}
