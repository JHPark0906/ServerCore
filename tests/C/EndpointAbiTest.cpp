#include "ServerCore/C/Endpoint.h"
#include "ServerCore/C/Net.h"
#include "SocketTestSupport.h"
#include "TestHarness.h"
#include <array>
#include <cstring>
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
void EndpointValues()
{
    sc_ip_endpoint endpoint{};
    ExpectEqual(sc_status{ SC_OK },
        sc_ip_endpoint_parse(Bytes("fe80::1%4294967295"), 9876, &endpoint),
        "C endpoint parses full numeric IPv6 scope");
    ExpectTrue(
        endpoint.family == SC_IP_V6 && endpoint.scope_id == UINT32_MAX && endpoint.port == 9876,
        "C endpoint preserves family scope and host-order port");
    size_t length = 0;
    ExpectEqual(sc_status{ SC_TOO_LARGE }, sc_ip_endpoint_format(&endpoint, nullptr, 0, &length),
        "C endpoint format supports bounded size query");
    std::array<char, 96> output{};
    ExpectEqual(sc_status{ SC_OK },
        sc_ip_endpoint_format(&endpoint, output.data(), output.size(), &length),
        "C endpoint formatting writes complete terminated value");
    ExpectEqual(std::string("[fe80::1%4294967295]:9876"), std::string(output.data()),
        "C endpoint format brackets IPv6 and preserves scope");
    std::array<char, 3> shortBuffer{ 'x', 'x', 'x' };
    ExpectEqual(sc_status{ SC_TOO_LARGE },
        sc_ip_endpoint_format(&endpoint, shortBuffer.data(), shortBuffer.size(), &length),
        "C endpoint format rejects undersized output atomically");
    ExpectTrue(shortBuffer[0] == 'x' && shortBuffer[2] == 'x', "failed format writes no prefix");
    const auto previous = endpoint;
    ExpectEqual(sc_status{ SC_INVALID_ARGUMENT },
        sc_ip_endpoint_parse(Bytes("::1%eth0"), 1, &endpoint),
        "C endpoint rejects interface-name lookup");
    ExpectTrue(endpoint.family == previous.family && endpoint.scope_id == previous.scope_id &&
                   endpoint.port == previous.port,
        "failed parse preserves output value");
    ExpectEqual(sc_status{ SC_OK }, sc_ip_endpoint_parse(Bytes("127.0.0.1"), 80, &endpoint),
        "C IPv4 parser remains supported");
    endpoint.scope_id = 1;
    ExpectEqual(sc_status{ SC_INVALID_ARGUMENT },
        sc_ip_endpoint_format(&endpoint, output.data(), output.size(), &length),
        "C IPv4 values cannot carry IPv6 scope metadata");
}
struct Socket
{
    ServerCoreTest::Socket value;
    explicit Socket(int family)
        : value(::socket(family, SOCK_STREAM, IPPROTO_TCP))
    {
    }
    ~Socket()
    {
        if (value != ServerCoreTest::InvalidSocket)
            ServerCoreTest::CloseSocket(value);
    }
};
uint16_t FreePort()
{
    Socket probe(AF_INET6);
    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    (void)::inet_pton(AF_INET6, "::1", &address.sin6_addr);
    if (::bind(probe.value, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0)
        return 0;
    ServerCoreTest::SocketLength size = sizeof address;
    if (::getsockname(probe.value, reinterpret_cast<sockaddr*>(&address), &size) != 0)
        return 0;
    return ntohs(address.sin6_port);
}
void TcpIpv6Endpoints()
{
    if (!ServerCoreTest::Ipv6LoopbackAvailable())
        return ServerCoreTest::Skip("IPv6 loopback ::1 is unavailable", "SERVERCORE_REQUIRE_IPV6");
    ServerCoreTest::SocketRuntime runtime;
    for (const bool only : { true, false })
    {
        sc_tcp_options options{};
        ExpectEqual(sc_status{ SC_OK }, sc_tcp_options_init(&options, sizeof options),
            "old TCP options layout initializes");
        options.listen_address = Bytes(only ? "::1" : "::");
        options.port = FreePort();
        ExpectTrue(options.port != 0, "C IPv6 test chooses free TCP port");
        sc_tcp_server* raw = nullptr;
        ExpectEqual(sc_status{ SC_INVALID_ARGUMENT }, sc_tcp_server_create_ex(&options, 2, &raw),
            "C IPv6-only flag is validated");
        const auto status = only ? sc_tcp_server_create(&options, &raw)
                                 : sc_tcp_server_create_ex(&options, 0, &raw);
        ExpectEqual(
            sc_status{ SC_OK }, status, "old create and explicit dual-stack create accept IPv6");
        std::unique_ptr<sc_tcp_server, decltype(&sc_tcp_server_destroy)> server(
            raw, sc_tcp_server_destroy);
        if (!server)
            return;
        ExpectEqual(
            sc_status{ SC_OK }, sc_tcp_server_start(server.get()), "C IPv6 TCP listener starts");
        sc_ip_endpoint binding{};
        ExpectEqual(sc_status{ SC_OK }, sc_tcp_server_local_endpoint(server.get(), &binding),
            "C listener exposes local endpoint");
        ExpectTrue(binding.family == SC_IP_V6 && binding.port == options.port,
            "C listener endpoint preserves family and port");
        Socket client(only ? AF_INET6 : AF_INET);
        int connected = -1;
        uint16_t clientPort = 0;
        if (only)
        {
            sockaddr_in6 address{};
            address.sin6_family = AF_INET6;
            address.sin6_port = htons(options.port);
            (void)::inet_pton(AF_INET6, "::1", &address.sin6_addr);
            connected =
                ::connect(client.value, reinterpret_cast<sockaddr*>(&address), sizeof address);
            ServerCoreTest::SocketLength size = sizeof address;
            if (::getsockname(client.value, reinterpret_cast<sockaddr*>(&address), &size) == 0)
                clientPort = ntohs(address.sin6_port);
        }
        else
        {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(options.port);
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            connected =
                ::connect(client.value, reinterpret_cast<sockaddr*>(&address), sizeof address);
            ServerCoreTest::SocketLength size = sizeof address;
            if (::getsockname(client.value, reinterpret_cast<sockaddr*>(&address), &size) == 0)
                clientPort = ntohs(address.sin_port);
        }
        ExpectTrue(connected == 0, "raw client connects to C IPv6 listener");
        sc_tcp_connection* accepted = nullptr;
        ExpectEqual(sc_status{ SC_OK }, sc_tcp_server_accept(server.get(), 3000, &accepted),
            "C accepted connection arrives");
        std::unique_ptr<sc_tcp_connection, decltype(&sc_tcp_connection_destroy)> connection(
            accepted, sc_tcp_connection_destroy);
        if (!connection)
            continue;
        sc_ip_endpoint local{}, remote{};
        ExpectEqual(sc_status{ SC_OK },
            sc_tcp_connection_endpoints(connection.get(), &local, &remote),
            "C accepted endpoint snapshots available");
        ExpectTrue(local.family == SC_IP_V6 && remote.family == SC_IP_V6 &&
                       local.port == options.port && remote.port == clientPort,
            "C endpoint metadata matches actual OS sockets");
        if (!only)
            ExpectTrue(
                remote.address[10] == 255 && remote.address[11] == 255 && remote.address[12] == 127,
                "C mapped IPv4 remains IPv6");
        sc_tcp_connection_close(connection.get());
        ExpectEqual(sc_status{ SC_OK }, sc_tcp_server_stop(server.get()),
            "C server drains closed connection");
        sc_ip_endpoint closedLocal{}, closedRemote{};
        ExpectEqual(sc_status{ SC_OK },
            sc_tcp_connection_endpoints(connection.get(), &closedLocal, &closedRemote),
            "C metadata survives server shutdown");
        ExpectTrue(closedRemote.port == remote.port && closedRemote.family == remote.family,
            "closed C endpoint remains immutable");
    }
}
const ServerCoreTest::CheckRegistration values("CAbi.EndpointValues", EndpointValues);
const ServerCoreTest::CheckRegistration ipv6("CAbi.TcpIpv6Endpoints", TcpIpv6Endpoints);
}
