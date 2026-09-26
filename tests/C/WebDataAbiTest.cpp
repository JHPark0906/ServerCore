#include "ServerCore/C/WebData.h"
#include "TestHarness.h"
#include <memory>
#include <string>

namespace
{
using ServerCoreTest::ExpectEqual;
using ServerCoreTest::ExpectTrue;
sc_bytes Bytes(std::string_view value)
{
    return { reinterpret_cast<const uint8_t*>(value.data()), value.size() };
}
std::string Text(sc_bytes value)
{
    return value.len ? std::string(reinterpret_cast<const char*>(value.data), value.len)
                     : std::string{};
}
void RequestDataOwnedViews()
{
    sc_web_field_limits limits{};
    ExpectEqual(sc_status{ SC_OK }, sc_web_field_limits_init(&limits, sizeof limits),
        "C field options initialize");
    ExpectEqual(sc_status{ SC_INVALID_ARGUMENT },
        sc_web_field_limits_init(&limits, sizeof limits - 1),
        "C options reject undersized storage");
    sc_web_fields* raw = nullptr;
    std::string input = "/?a=first&a=%2Bsecond";
    ExpectEqual(sc_status{ SC_OK },
        sc_web_parse_fields(SC_FIELDS_QUERY, Bytes(input), &limits, &raw), "C query parsed");
    std::unique_ptr<sc_web_fields, decltype(&sc_web_fields_destroy)> fields(
        raw, sc_web_fields_destroy);
    input.assign(100, 'x');
    ExpectEqual(std::size_t{ 2 }, sc_web_fields_count(fields.get()),
        "C duplicate fields owned independently");
    sc_header view{};
    ExpectEqual(sc_status{ SC_OK }, sc_web_fields_at(fields.get(), 1, &view),
        "C field indexed view available");
    ExpectEqual(std::string("+second"), Text(view.value), "C input storage can change after parse");
    ExpectEqual(sc_status{ SC_INVALID_ARGUMENT }, sc_web_fields_at(fields.get(), 2, &view),
        "C view bounds validated");
    sc_web_data_text* text = nullptr;
    ExpectEqual(sc_status{ SC_OK }, sc_web_percent_decode(Bytes("%252F+"), 0, 32, &text),
        "C codec decodes exactly once");
    std::unique_ptr<sc_web_data_text, decltype(&sc_web_data_text_destroy)> owned(
        text, sc_web_data_text_destroy);
    sc_bytes decoded{};
    ExpectEqual(
        sc_status{ SC_OK }, sc_web_data_text_view(owned.get(), &decoded), "C text view available");
    ExpectEqual(std::string("%2F+"), Text(decoded), "C codec uses query plus rules by default");
    sc_web_cookie_options cookies{};
    ExpectEqual(sc_status{ SC_OK }, sc_web_cookie_options_init(&cookies, sizeof cookies),
        "C cookie options initialize");
    cookies.same_site = SC_COOKIE_NONE;
    ExpectEqual(sc_status{ SC_INVALID_ARGUMENT },
        sc_web_set_cookie(Bytes("sid"), Bytes("x"), &cookies, &text),
        "C cookie options enforce security combination");
    ExpectTrue(text == nullptr, "failed C output reset");
    cookies.secure = 1;
    ExpectEqual(sc_status{ SC_OK }, sc_web_set_cookie(Bytes("sid"), Bytes("x"), &cookies, &text),
        "C valid cookie emitted");
    sc_web_data_text_destroy(text);
}
void MultipartOwnedEventsAndFailure()
{
    sc_multipart_options options{};
    ExpectEqual(sc_status{ SC_OK }, sc_multipart_options_init(&options, sizeof options),
        "C multipart defaults initialize");
    sc_multipart* raw = nullptr;
    ExpectEqual(sc_status{ SC_OK },
        sc_multipart_create(Bytes("multipart/form-data; boundary=x"), &options, &raw),
        "C multipart parser created");
    std::unique_ptr<sc_multipart, decltype(&sc_multipart_destroy)> parser(
        raw, sc_multipart_destroy);
    std::string wire = "--x\r\nContent-Disposition: form-data; name=upload; "
                       "filename=\"../x.bin\"\r\n\r\nabc\r\n--x--\r\n";
    std::size_t consumed = 0;
    ExpectEqual(sc_status{ SC_OK }, sc_multipart_feed(parser.get(), Bytes(wire), &consumed),
        "C multipart input copied");
    ExpectEqual(wire.size(), consumed, "C accepted prefix reported");
    wire.assign(200, 'z');
    sc_multipart_event* event = nullptr;
    ExpectEqual(
        sc_status{ SC_OK }, sc_multipart_read(parser.get(), &event), "C PartBegin returned");
    std::unique_ptr<sc_multipart_event, decltype(&sc_multipart_event_destroy)> begin(
        event, sc_multipart_event_destroy);
    sc_multipart_event_view view{};
    view.abi_version = SC_ABI_VERSION;
    view.struct_size = sizeof view;
    ExpectEqual(
        sc_status{ SC_OK }, sc_multipart_event_get(begin.get(), &view), "C event view valid");
    ExpectEqual(std::string("../x.bin"), Text(view.filename), "C filename is metadata only");
    const auto retained = sc_multipart_retained_bytes(parser.get());
    begin.reset();
    ExpectTrue(sc_multipart_retained_bytes(parser.get()) < retained,
        "C event destroy returns retained metadata charge");
    ExpectEqual(
        sc_status{ SC_OK }, sc_multipart_read(parser.get(), &event), "C binary Data returned");
    std::unique_ptr<sc_multipart_event, decltype(&sc_multipart_event_destroy)> data(
        event, sc_multipart_event_destroy);
    parser.reset();
    ExpectEqual(sc_status{ SC_OK }, sc_multipart_event_get(data.get(), &view),
        "C event survives parser destruction");
    ExpectEqual(uint32_t{ SC_MULTIPART_DATA }, view.kind, "C event identifies opaque data");
    ExpectEqual(std::string("abc"), Text(view.data),
        "C owned payload outlives both caller input and parser");
    ExpectEqual(sc_status{ SC_OK },
        sc_multipart_create(Bytes("multipart/form-data; boundary=x"), &options, &raw),
        "C early EOF fixture created");
    parser.reset(raw);
    ExpectEqual(sc_status{ SC_OK }, sc_multipart_feed(parser.get(), Bytes("--x\r\n"), &consumed),
        "C partial framing accepted");
    ExpectEqual(
        sc_status{ SC_OK }, sc_multipart_finish(parser.get()), "C EOF declaration accepted");
    ExpectEqual(sc_status{ SC_INVALID_FORMAT }, sc_multipart_read(parser.get(), &event),
        "C early EOF fails when drained");
    ExpectTrue(event == nullptr, "C terminal error has no event ownership");
    sc_multipart_cancel(parser.get());
    ExpectEqual(sc_status{ SC_INVALID_FORMAT }, sc_multipart_read(parser.get(), &event),
        "C original terminal failure remains sticky");
}
const ServerCoreTest::CheckRegistration fields("CAbi.RequestDataOwnedViews", RequestDataOwnedViews);
const ServerCoreTest::CheckRegistration multipart(
    "CAbi.MultipartOwnedEventsAndFailure", MultipartOwnedEventsAndFailure);
}
