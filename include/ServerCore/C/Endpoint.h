#ifndef SERVERCORE_C_ENDPOINT_H
#define SERVERCORE_C_ENDPOINT_H
#include "ServerCore/C/Types.h"
#ifdef __cplusplus
extern "C"
{
#endif
    enum
    {
        SC_IP_UNSPECIFIED = 0,
        SC_IP_V4 = 4,
        SC_IP_V6 = 6
    };
    /* Address bytes use network order (first four for IPv4; remaining bytes zero).
 * IPv6 mapped addresses remain IPv6. Scope is a numeric interface index, zero
 * when unscoped. Unspecified is used for unavailable metadata, never for bind.
 * This fixed ABI-1 value has no pointers; reserved must be zero. */
    typedef struct sc_ip_endpoint
    {
        uint32_t family, scope_id;
        uint8_t address[16];
        uint16_t port, reserved;
    } sc_ip_endpoint;
    /* Numeric literal only, optional IPv6 %decimal scope; no DNS or brackets.
 * Output changes only on success. Port zero is valid for this value parser. */
    SC_API sc_status sc_ip_endpoint_parse(
        sc_bytes address, uint16_t port, sc_ip_endpoint* out) SC_NOEXCEPT;
    /* Formats address:port or [IPv6%scope]:port. written excludes trailing NUL.
 * Capacity must include that NUL. TooLarge reports required written; NULL/0 is
 * a size query. No partial output is written. */
    SC_API sc_status sc_ip_endpoint_format(
        const sc_ip_endpoint*, char* buffer, size_t capacity, size_t* written) SC_NOEXCEPT;
#ifdef __cplusplus
}
#endif
#endif
