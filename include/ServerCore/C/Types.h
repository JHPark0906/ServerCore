#ifndef SERVERCORE_C_TYPES_H
#define SERVERCORE_C_TYPES_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(SC_CABI_SHARED)
#if defined(SC_CABI_BUILD)
#define SC_API __declspec(dllexport)
#else
#define SC_API __declspec(dllimport)
#endif
#elif defined(SC_CABI_SHARED) && defined(__GNUC__)
/* The shared library is built with hidden default visibility, so the C ABI is
 * exported explicitly. ServerCore.CMake.SharedLibraryExportSet compares the
 * exported sc_* set with these SC_API declarations. */
#define SC_API __attribute__((visibility("default")))
#else
#define SC_API
#endif

#ifdef __cplusplus
extern "C"
{
#endif

/* Every SC_API function is declared SC_NOEXCEPT. In C++11 and later that is
 * noexcept, so the compiler turns an exception escaping an implementation
 * into std::terminate instead of unwinding into a C, Rust or other foreign
 * caller. CAbiPackageTest.cmake rejects an installed SC_API declaration that
 * lacks it. C and pre-C++11 consumers see nothing. */
#if defined(__cplusplus) &&                                                                        \
    (__cplusplus >= 201103L || (defined(_MSVC_LANG) && _MSVC_LANG >= 201103L))
#define SC_NOEXCEPT noexcept
#else
#define SC_NOEXCEPT
#endif

/* A structure layout never changes within one ABI version; a layout change
 * raises it once released. ABI 2 (0.3.0) grew sc_tcp_options, sc_web_options,
 * sc_udp_options, sc_udp_metrics and sc_outbound_metrics and put an abi_version/struct_size header on
 * sc_tick_info, sc_tick_metrics, sc_outbound_metrics and sc_udp_packet_view,
 * so an ABI 1 caller's buffers no longer match (an ABI 1 sc_outbound_metrics
 * is 16 bytes shorter). Descriptors carrying
 * abi_version 1 are rejected; a binding must also compare sc_abi_version()
 * before passing a structure without that header. */
#define SC_ABI_VERSION 2u
    typedef int32_t sc_status;
    enum
    {
        SC_OK = 0,
        SC_INVALID_ARGUMENT = 1,
        SC_INVALID_FORMAT = 2,
        SC_TOO_LARGE = 3,
        SC_NOT_FOUND = 4,
        SC_ALREADY_EXISTS = 5,
        SC_CLOSED = 6,
        SC_WOULD_BLOCK = 7,
        SC_PLATFORM_ERROR = 8,
        SC_UNIMPLEMENTED = 9,
        SC_UNKNOWN_TYPE = 10,
        SC_TIMEOUT = 11,
        SC_CANCELLED = 12
    };
    typedef struct sc_bytes
    {
        const uint8_t* data;
        size_t len;
    } sc_bytes;
    typedef struct sc_header
    {
        sc_bytes name;
        sc_bytes value;
    } sc_header;
    /* Capability bit 4 is retired and must not be reused. */
    enum
    {
        SC_CAP_WEB = 1u,
        SC_CAP_TCP = 2u,
        SC_CAP_WEB_EXTENSIONS = 8u,
        SC_CAP_OBSERVABILITY = 16u,
        SC_CAP_READINESS = 32u,
        SC_CAP_CHANNEL = 64u,
        SC_CAP_RUNTIME = 128u,
        SC_CAP_ENDPOINT = 256u,
        SC_CAP_REQUEST_LIMITER = 512u,
        SC_CAP_WEB_POLICIES = 1024u,
        SC_CAP_WEB_DATA = 2048u,
        SC_CAP_WS_EXTENSIONS = 4096u,
        SC_CAP_GAME_EXECUTION = 8192u,
        SC_CAP_BINARY_IO = 16384u,
        SC_CAP_DATAGRAM = 32768u,
        SC_CAP_ATOMIC_FILE = 65536u,
        SC_CAP_OPERATIONS = 131072u
    };

    /* Native addresses must be valid for the advertised lengths. NULL is valid for
 * empty slices and destroy functions only, unless stated otherwise. All input
 * slices are borrowed for a call and copied before asynchronous use. No C++
 * exception crosses this ABI: SC_NOEXCEPT (above) terminates instead. Failures,
 * including allocation failure, are reported as sc_status; allocation failure
 * injection covers the completion subscriptions in CAbi.LogicalTicksOwnership
 * and CAbi.OutboundOwnershipAndBatch. Text inputs follow the native API's validation;
 * received HTTP header values and message bodies are arbitrary bytes.
 * A versioned input descriptor may carry a struct_size larger than this
 * header's only if every byte past it is zero and struct_size <= 4096;
 * otherwise the call returns InvalidArgument, so an option added by a newer
 * header is rejected rather than ignored. Output structures ignore extra
 * bytes (CAbi.TcpOwnershipAndTerminal covers both).
 * An owned handle must be destroyed exactly once. Calls on a handle may
 * be concurrent, but destruction must not overlap another call on that same
 * handle. A retained/cloned handle has an independent destruction lifetime. */
    SC_API uint32_t sc_abi_version(void) SC_NOEXCEPT;
    SC_API uint32_t sc_capabilities(void) SC_NOEXCEPT;
    SC_API const char* sc_status_name(sc_status status) SC_NOEXCEPT;

    /* The C headers compile as ISO C90, C99 and C11 without extensions, given a
 * <stdint.h>. C90 and C99 allow each typedef once per translation unit, so a
 * handle named by more than one header is declared here and nowhere else.
 * CAbiConsumer compiles every header together per standard, with
 * -pedantic-errors under GCC and Clang (MSVC does not diagnose this). */
    typedef struct sc_executor sc_executor;
    typedef struct sc_tcp_server sc_tcp_server;
    typedef struct sc_web_server sc_web_server;

    typedef struct sc_notifier sc_notifier;
    typedef struct sc_subscription sc_subscription;
    /* Bounded, coalescing readiness mailbox; no foreign-language callback executes
 * on a native worker. Keys must be nonzero and unique among live subscriptions.
 * capacity is 1..4096; event readiness sources also cap live registrations at
 * 4096 across all mailboxes. Completion/channel APIs have their own limits;
 * cancellation-token subscriptions use mailbox limits. A subscribe call that
 * finds its mailbox, source or observer limit full returns TooLarge, not
 * WouldBlock: WouldBlock means "subscribe, then wait", but nothing signals when
 * a registration slot frees (CAbi.ReadinessLifetimeAndRaces).
 * Subscribe before the nonblocking
 * operation; readiness is advisory and never reserves data or send capacity.
 * Registrations can notify inline before subscribe returns. Destroy suppresses
 * future notifications; a previously queued key may already have been read.
 * Consumers must ignore keys whose registration has been removed. */
    SC_API sc_status sc_notifier_create(size_t capacity, sc_notifier** out) SC_NOEXCEPT;
    /* 0 is nonblocking, UINT32_MAX blocks. Explicit interrupt returns WouldBlock
 * even for a positive/infinite timeout, allowing a bridge to change deadlines. */
    SC_API sc_status sc_notifier_next(
        sc_notifier* notifier, uint32_t timeout_ms, uint64_t* key) SC_NOEXCEPT;
    SC_API void sc_notifier_interrupt(sc_notifier* notifier) SC_NOEXCEPT;
    SC_API void sc_notifier_close(sc_notifier* notifier) SC_NOEXCEPT;
    SC_API void sc_notifier_destroy(sc_notifier* notifier) SC_NOEXCEPT;
    SC_API void sc_subscription_destroy(sc_subscription* subscription) SC_NOEXCEPT;

    typedef struct sc_wait sc_wait;
    /* Capacity waits are advisory, not reservations. timeout_ms=0 is nonblocking;
 * UINT32_MAX waits until completion. Timeout never cancels the operation. */
    SC_API sc_status sc_wait_result(const sc_wait* wait) SC_NOEXCEPT;
    SC_API sc_status sc_wait_wait(sc_wait* wait, uint32_t timeout_ms) SC_NOEXCEPT;
    SC_API sc_status sc_wait_subscribe(
        sc_wait* wait, sc_notifier* notifier, uint64_t key, sc_subscription** out) SC_NOEXCEPT;
    SC_API void sc_wait_cancel(sc_wait* wait) SC_NOEXCEPT;
    /* Cancels/suppresses and joins any native notification before returning. */
    SC_API void sc_wait_destroy(sc_wait* wait) SC_NOEXCEPT;

#ifdef __cplusplus
}
#endif
#endif
