#ifndef SERVERCORE_C_TYPES_H
#define SERVERCORE_C_TYPES_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(SC_CABI_SHARED)
# if defined(SC_CABI_BUILD)
#  define SC_API __declspec(dllexport)
# else
#  define SC_API __declspec(dllimport)
# endif
#else
# define SC_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SC_ABI_VERSION 1u
typedef int32_t sc_status;
enum { SC_OK = 0, SC_INVALID_ARGUMENT = 1, SC_INVALID_FORMAT = 2,
    SC_TOO_LARGE = 3, SC_NOT_FOUND = 4, SC_ALREADY_EXISTS = 5,
    SC_CLOSED = 6, SC_WOULD_BLOCK = 7, SC_PLATFORM_ERROR = 8,
    SC_UNIMPLEMENTED = 9, SC_UNKNOWN_TYPE = 10, SC_TIMEOUT = 11, SC_CANCELLED = 12 };
typedef struct sc_bytes { const uint8_t* data; size_t len; } sc_bytes;
typedef struct sc_header { sc_bytes name; sc_bytes value; } sc_header;
/* Capability bit 4 is retired and must not be reused. */
enum { SC_CAP_WEB = 1u, SC_CAP_TCP = 2u,
    SC_CAP_WEB_EXTENSIONS = 8u, SC_CAP_OBSERVABILITY = 16u };

/* Native addresses must be valid for the advertised lengths. NULL is valid for
 * empty slices and destroy functions only, unless stated otherwise. All input
 * slices are borrowed for a call and copied before asynchronous use. No C++
 * exception crosses this ABI. Text inputs follow the native API's validation;
 * received HTTP header values and message bodies are arbitrary bytes.
 * An owned handle must be destroyed exactly once. Calls on a handle may
 * be concurrent, but destruction must not overlap another call on that same
 * handle. A retained/cloned handle has an independent destruction lifetime. */
SC_API uint32_t sc_abi_version(void);
SC_API uint32_t sc_capabilities(void);
SC_API const char* sc_status_name(sc_status status);

typedef struct sc_wait sc_wait;
/* Capacity waits are advisory, not reservations. timeout_ms=0 is nonblocking;
 * UINT32_MAX waits until completion. Timeout never cancels the operation. */
SC_API sc_status sc_wait_result(const sc_wait* wait);
SC_API sc_status sc_wait_wait(sc_wait* wait, uint32_t timeout_ms);
SC_API void sc_wait_cancel(sc_wait* wait);
/* Cancels/suppresses and joins any native notification before returning. */
SC_API void sc_wait_destroy(sc_wait* wait);

#ifdef __cplusplus
}
#endif
#endif
