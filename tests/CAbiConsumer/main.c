#include <ServerCore/C/BinaryIO.h>
#include <ServerCore/C/Channel.h>
#include <ServerCore/C/Datagram.h>
#include <ServerCore/C/Endpoint.h>
#include <ServerCore/C/Files.h>
#include <ServerCore/C/GameExecution.h>
#include <ServerCore/C/Net.h>
#include <ServerCore/C/Observability.h>
#include <ServerCore/C/RequestLimiter.h>
#include <ServerCore/C/Runtime.h>
#include <ServerCore/C/Types.h>
#include <ServerCore/C/Web.h>
#include <ServerCore/C/WebData.h>
#include <ServerCore/C/WebPolicy.h>
#include <string.h>

#if defined(__cplusplus)
#error The shared C ABI consumer must compile as C.
#endif
#if !defined(SC_CABI_SHARED) || defined(SC_CABI_BUILD)
#error The shared C ABI consumer must receive only the C ABI import definition.
#endif
#if defined(SERVERCORE_SHARED) || defined(SERVERCORE_BUILDING_LIBRARY) ||                          \
    defined(SERVERCORE_MSVC_DEBUG) || defined(SERVERCORE_MSVC_VERSION)
#error Native C++ ABI definitions must not leak through ServerCore::CAbi.
#endif

static sc_status run_task(void* context, const sc_runtime_token* token)
{
    int* result = (int*)context;
    if (sc_runtime_token_requested(token))
        return SC_CANCELLED;
    *result = 23;
    return SC_OK;
}

int main(void)
{
    sc_web_options web;
    sc_tcp_options tcp;
    sc_response_head response;
    sc_web_server* server = NULL;
    sc_tcp_server* listener = NULL;
    {
        sc_binary_writer* binary = NULL;
        sc_atomic_file_options file_options;
        if (sc_binary_writer_create(64, SC_BIG_ENDIAN, &binary) != SC_OK)
            return 14;
        if (sc_binary_write_signed(binary, -23, 2) != SC_OK)
            return 15;
        sc_binary_writer_destroy(binary);
        if (sc_atomic_file_options_init(&file_options, sizeof(file_options)) != SC_OK)
            return 16;
        if ((sc_capabilities() & (SC_CAP_GAME_EXECUTION | SC_CAP_BINARY_IO | SC_CAP_DATAGRAM |
                                     SC_CAP_ATOMIC_FILE | SC_CAP_OPERATIONS)) !=
            (SC_CAP_GAME_EXECUTION | SC_CAP_BINARY_IO | SC_CAP_DATAGRAM | SC_CAP_ATOMIC_FILE |
                SC_CAP_OPERATIONS))
            return 17;
    }
    {
        sc_ip_endpoint endpoint;
        sc_bytes address = { (const uint8_t*)"::1", 3 };
        sc_request_limiter_options options;
        sc_request_limiter* limiter = NULL;
        sc_request_permit* permit = NULL;
        sc_request_limit_decision decision = { SC_ABI_VERSION, sizeof(sc_request_limit_decision), 0,
            0 };
        sc_bytes key = { (const uint8_t*)"consumer", 8 };
        sc_http_policy_result* json = NULL;
        sc_bytes input = { (const uint8_t*)"{}", 2 };
        if (sc_ip_endpoint_parse(address, 80, &endpoint) != SC_OK || endpoint.family != SC_IP_V6)
            return 11;
        if (sc_request_limiter_options_init(&options, sizeof(options)) != SC_OK ||
            sc_request_limiter_create(&options, &limiter) != SC_OK ||
            sc_request_limiter_acquire(limiter, key, 1, &decision, &permit) != SC_OK || !permit)
            return 12;
        sc_request_limiter_destroy(limiter);
        sc_request_permit_destroy(permit);
        if (sc_http_json_response(input, 200, 1024, &json) != SC_OK)
            return 13;
        sc_http_policy_result_destroy(json);
    }
    if (sc_abi_version() != SC_ABI_VERSION ||
        (sc_capabilities() & (SC_CAP_WEB | SC_CAP_TCP)) != (SC_CAP_WEB | SC_CAP_TCP))
        return 1;
    if (strcmp(sc_status_name(SC_CANCELLED), "Cancelled") != 0)
        return 2;
    if (sc_web_options_init(&web, sizeof(web)) != SC_OK ||
        sc_tcp_options_init(&tcp, sizeof(tcp)) != SC_OK ||
        sc_response_head_init(&response, sizeof(response)) != SC_OK)
        return 3;
    if (sc_web_server_create(&web, &server) != SC_OK ||
        sc_tcp_server_create(&tcp, &listener) != SC_OK)
        return 4;
    if (sc_web_server_stop(server) != SC_OK || sc_tcp_server_stop(listener) != SC_OK)
        return 5;
    sc_web_server_destroy(server);
    sc_tcp_server_destroy(listener);
    {
        sc_channel_options options = { SC_ABI_VERSION, sizeof(sc_channel_options), 2, 16 };
        sc_channel* channel = NULL;
        sc_channel_value* value = NULL;
        sc_bytes input = { (const uint8_t*)"owned", 5 };
        sc_bytes output = { NULL, 0 };
        if (sc_channel_create(&options, &channel) != SC_OK ||
            sc_channel_try_send(channel, input) != SC_OK ||
            sc_channel_try_receive(channel, &value) != SC_OK)
            return 6;
        sc_channel_destroy(channel);
        if (sc_channel_value_view(value, &output) != SC_OK || output.len != 5 ||
            memcmp(output.data, input.data, input.len) != 0)
            return 7;
        sc_channel_value_destroy(value);
    }
    {
        sc_executor_options options;
        sc_task_options task_options;
        sc_executor* executor = NULL;
        sc_runtime_task* task = NULL;
        int result = 0;
        sc_runtime_work work = { &result, run_task, NULL };
        if (sc_executor_options_init(&options, sizeof(options)) != SC_OK ||
            sc_task_options_init(&task_options, sizeof(task_options)) != SC_OK ||
            sc_executor_create(&options, &executor) != SC_OK ||
            sc_executor_submit(executor, work, &task_options, &task) != SC_OK)
            return 8;
        if (sc_runtime_task_wait(task, 5000) != SC_OK || result != 23 ||
            sc_executor_stop(executor) != SC_OK)
            return 9;
        sc_runtime_task_destroy(task);
        sc_executor_destroy(executor);
    }
    if (sc_runtime_shutdown(5000) != SC_OK)
        return 10;
    return 0;
}
