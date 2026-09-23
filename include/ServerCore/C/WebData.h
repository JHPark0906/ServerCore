#ifndef SERVERCORE_C_WEB_DATA_H
#define SERVERCORE_C_WEB_DATA_H
#include "ServerCore/C/Types.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct sc_web_fields sc_web_fields;
typedef struct sc_web_data_text sc_web_data_text;
typedef struct sc_multipart sc_multipart;
typedef struct sc_multipart_event sc_multipart_event;
enum { SC_FIELDS_QUERY = 0, SC_FIELDS_FORM = 1, SC_FIELDS_COOKIE = 2 };
enum { SC_COOKIE_UNSPECIFIED = 0, SC_COOKIE_LAX = 1, SC_COOKIE_STRICT = 2, SC_COOKIE_NONE = 3 };
enum { SC_MULTIPART_PART_BEGIN = 0, SC_MULTIPART_DATA = 1, SC_MULTIPART_PART_END = 2 };
typedef struct sc_web_field_limits {
    uint32_t abi_version, struct_size;
    size_t max_bytes, max_fields, max_name_bytes, max_value_bytes;
} sc_web_field_limits;
typedef struct sc_web_cookie_options {
    uint32_t abi_version, struct_size;
    sc_bytes domain, path;
    int64_t max_age_seconds;
    uint32_t has_max_age, same_site, secure, http_only;
    size_t max_bytes;
} sc_web_cookie_options;
typedef struct sc_multipart_options {
    uint32_t abi_version, struct_size;
    size_t max_parts, max_files, max_headers, max_header_bytes;
    uint64_t max_part_bytes, max_file_bytes, max_total_bytes;
    size_t max_retained_bytes, max_event_bytes;
} sc_multipart_options;
typedef struct sc_multipart_event_view {
    uint32_t abi_version, struct_size;
    uint32_t kind;
    uint64_t part_index;
    sc_bytes name, filename, content_type, data;
    uint32_t has_filename;
} sc_multipart_event_view;
SC_API sc_status sc_web_field_limits_init(sc_web_field_limits*, size_t);
SC_API sc_status sc_web_cookie_options_init(sc_web_cookie_options*, size_t);
SC_API sc_status sc_multipart_options_init(sc_multipart_options*, size_t);
/* Query input is a request target; form/cookie input is their field/body value.
 * Ordered duplicate names remain separate. All successful output is owned.
 * Query '+' is literal; form '+' means space. URL UTF-8/NUL/escape errors are
 * InvalidFormat. Cookie values use ASCII cookie-octet, no percent decoding. */
SC_API sc_status sc_web_parse_fields(uint32_t kind, sc_bytes input,
    const sc_web_field_limits*, sc_web_fields** out);
SC_API size_t sc_web_fields_count(const sc_web_fields*);
SC_API sc_status sc_web_fields_at(const sc_web_fields*, size_t index, sc_header* out);
SC_API void sc_web_fields_destroy(sc_web_fields*);
SC_API sc_status sc_web_percent_decode(sc_bytes input, uint32_t plus_as_space,
    size_t max_bytes, sc_web_data_text** out);
SC_API sc_status sc_web_percent_encode(sc_bytes input, uint32_t form_mode,
    size_t max_bytes, sc_web_data_text** out);
SC_API sc_status sc_web_set_cookie(sc_bytes name, sc_bytes value,
    const sc_web_cookie_options*, sc_web_data_text** out);
SC_API sc_status sc_web_data_text_view(const sc_web_data_text*, sc_bytes* out);
SC_API void sc_web_data_text_destroy(sc_web_data_text*);
/* Feed accepts a bounded prefix. Success writes consumed; WouldBlock accepts
 * none. Drain read/release held events before retrying remaining input. EOF is
 * Finish followed by read returning SC_OK + NULL, never just a PartEnd event.
 * Missing final boundary/early EOF => sticky InvalidFormat; limits => TooLarge.
 * Cancellation => sticky Cancelled. No file writes; filename is metadata only.
 * Input and outstanding event payload share retained-byte capacity. Event data
 * is immutable and remains readable after parser destroy. Copies are caller
 * storage outside the budget. Container/scratch capacity is not exact RSS.
 * Parser methods are synchronized; destroy must not race use of its handle. */
SC_API sc_status sc_multipart_create(sc_bytes content_type, const sc_multipart_options*, sc_multipart** out);
SC_API sc_status sc_multipart_feed(sc_multipart*, sc_bytes input, size_t* consumed);
SC_API sc_status sc_multipart_read(sc_multipart*, sc_multipart_event** out);
SC_API sc_status sc_multipart_finish(sc_multipart*);
SC_API void sc_multipart_cancel(sc_multipart*);
SC_API size_t sc_multipart_retained_bytes(const sc_multipart*);
SC_API void sc_multipart_destroy(sc_multipart*);
SC_API sc_status sc_multipart_event_get(const sc_multipart_event*, sc_multipart_event_view* out);
SC_API size_t sc_multipart_event_header_count(const sc_multipart_event*);
SC_API sc_status sc_multipart_event_header_at(const sc_multipart_event*, size_t index, sc_header* out);
SC_API void sc_multipart_event_destroy(sc_multipart_event*);
#ifdef __cplusplus
}
#endif
#endif
