#include <ServerCore/C/Types.h>
#include <ServerCore/C/Web.h>
#include <ServerCore/C/Net.h>
#include <ServerCore/C/Observability.h>
#include <string.h>

int main(void)
{
    sc_web_options web;
    sc_tcp_options tcp;
    sc_response_head response;
    sc_web_server* server = NULL;
    sc_tcp_server* listener = NULL;
    if (sc_abi_version() != SC_ABI_VERSION || (sc_capabilities() & (SC_CAP_WEB | SC_CAP_TCP)) != (SC_CAP_WEB | SC_CAP_TCP)) return 1;
    if (strcmp(sc_status_name(SC_CANCELLED), "Cancelled") != 0) return 2;
    if (sc_web_options_init(&web, sizeof(web)) != SC_OK ||
        sc_tcp_options_init(&tcp, sizeof(tcp)) != SC_OK ||
        sc_response_head_init(&response, sizeof(response)) != SC_OK) return 3;
    if (sc_web_server_create(&web, &server) != SC_OK || sc_tcp_server_create(&tcp, &listener) != SC_OK) return 4;
    if (sc_web_server_stop(server) != SC_OK || sc_tcp_server_stop(listener) != SC_OK) return 5;
    sc_web_server_destroy(server);
    sc_tcp_server_destroy(listener);
    return 0;
}
