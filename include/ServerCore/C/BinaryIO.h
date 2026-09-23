#ifndef SERVERCORE_C_BINARY_IO_H
#define SERVERCORE_C_BINARY_IO_H
#include "ServerCore/C/Types.h"
#ifdef __cplusplus
extern "C"
{
#endif
    enum
    {
        SC_BIG_ENDIAN = 0,
        SC_LITTLE_ENDIAN = 1
    };
    typedef struct sc_binary_reader sc_binary_reader;
    typedef struct sc_binary_writer sc_binary_writer;
    /* Reader copies bounded input. Handles require serialized access; read views
 * borrow their reader until destruction. Failed operations change no position/output. */
    SC_API sc_status sc_binary_reader_create(
        sc_bytes input, uint32_t order, size_t limit, sc_binary_reader** out);
    SC_API void sc_binary_reader_destroy(sc_binary_reader*);
    SC_API size_t sc_binary_reader_position(const sc_binary_reader*);
    SC_API size_t sc_binary_reader_remaining(const sc_binary_reader*);
    SC_API sc_status sc_binary_read_unsigned(sc_binary_reader*, size_t width, uint64_t* out);
    SC_API sc_status sc_binary_read_signed(sc_binary_reader*, size_t width, int64_t* out);
    SC_API sc_status sc_binary_read_bool(sc_binary_reader*, uint32_t* out);
    SC_API sc_status sc_binary_read_f32(sc_binary_reader*, float* out);
    SC_API sc_status sc_binary_read_f64(sc_binary_reader*, double* out);
    SC_API sc_status sc_binary_read_bytes(sc_binary_reader*, size_t length, sc_bytes* out);
    SC_API sc_status sc_binary_read_blob(sc_binary_reader*, size_t maximum_length, sc_bytes* out);
    SC_API sc_status sc_binary_read_utf8(sc_binary_reader*, size_t maximum_length, sc_bytes* out);
    SC_API sc_status sc_binary_skip(sc_binary_reader*, size_t length);
    SC_API sc_status sc_binary_writer_create(size_t limit, uint32_t order, sc_binary_writer** out);
    SC_API void sc_binary_writer_destroy(sc_binary_writer*);
    SC_API void sc_binary_writer_clear(sc_binary_writer*);
    /* View is invalidated by successful mutation or destruction. */
    SC_API sc_status sc_binary_writer_view(const sc_binary_writer*, sc_bytes* out);
    SC_API sc_status sc_binary_write_unsigned(sc_binary_writer*, uint64_t value, size_t width);
    SC_API sc_status sc_binary_write_signed(sc_binary_writer*, int64_t value, size_t width);
    SC_API sc_status sc_binary_write_bool(sc_binary_writer*, uint32_t value);
    SC_API sc_status sc_binary_write_f32(sc_binary_writer*, float value);
    SC_API sc_status sc_binary_write_f64(sc_binary_writer*, double value);
    SC_API sc_status sc_binary_write_bytes(sc_binary_writer*, sc_bytes value);
    SC_API sc_status sc_binary_write_blob(sc_binary_writer*, sc_bytes value, size_t maximum_length);
    SC_API sc_status sc_binary_write_utf8(sc_binary_writer*, sc_bytes value, size_t maximum_length);
    typedef struct sc_protocol_offer
    {
        uint32_t version, reserved;
        uint64_t features;
    } sc_protocol_offer;
    SC_API sc_status sc_protocol_negotiate(const sc_protocol_offer* server, size_t server_count,
        const sc_protocol_offer* peer, size_t peer_count, uint64_t required_features,
        sc_protocol_offer* out);
#ifdef __cplusplus
}
#endif
#endif
