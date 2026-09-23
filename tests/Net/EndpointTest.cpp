#include "SocketTestSupport.h"
#include "TestHarness.h"
#include "Net/DatagramSocket.h"
#include "ServerCore/Net/Acceptor.h"
#include "ServerCore/Net/IoContext.h"
#include "ServerCore/Runtime/DatagramTransport.h"
#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
using namespace ServerCore;
using ServerCoreTest::ExpectTrue;
using ServerCoreTest::ExpectEqual;
template<class Predicate> bool Await(Predicate predicate) {
    const auto limit=std::chrono::steady_clock::now()+3s;
    do { if (predicate()) return true; std::this_thread::sleep_for(1ms); }
    while (std::chrono::steady_clock::now()<limit);
    return false;
}
struct RawAddress {
    sockaddr_storage bytes{};
    ServerCoreTest::SocketLength length=0;
    RawAddress(int family,uint16_t port) {
        if (family==AF_INET6) {
            sockaddr_in6 value{}; value.sin6_family=AF_INET6; value.sin6_port=htons(port);
            (void)::inet_pton(AF_INET6,"::1",&value.sin6_addr);
            std::memcpy(&bytes,&value,sizeof value); length=sizeof value;
        } else {
            sockaddr_in value{}; value.sin_family=AF_INET; value.sin_port=htons(port);
            value.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
            std::memcpy(&bytes,&value,sizeof value); length=sizeof value;
        }
    }
    const sockaddr* Get() const { return reinterpret_cast<const sockaddr*>(&bytes); }
};
struct RawSocket {
    ServerCoreTest::Socket value=ServerCoreTest::InvalidSocket;
    int family;
    RawSocket(int af,int kind) : value(::socket(af,kind,kind==SOCK_STREAM?IPPROTO_TCP:IPPROTO_UDP)),family(af) {}
    ~RawSocket() { if (value!=ServerCoreTest::InvalidSocket) ServerCoreTest::CloseSocket(value); }
    bool Bind() { const RawAddress address(family,0); return ::bind(value,address.Get(),address.length)==0; }
    bool Connect(uint16_t port) { const RawAddress address(family,port); return ::connect(value,address.Get(),address.length)==0; }
    uint16_t Port() const {
        sockaddr_storage address{}; ServerCoreTest::SocketLength length=sizeof address;
        if (::getsockname(value,reinterpret_cast<sockaddr*>(&address),&length)!=0) return 0;
        if (address.ss_family==AF_INET6) { sockaddr_in6 typed{}; std::memcpy(&typed,&address,sizeof typed); return ntohs(typed.sin6_port); }
        sockaddr_in typed{}; std::memcpy(&typed,&address,sizeof typed); return ntohs(typed.sin_port);
    }
    bool SendDatagram(uint16_t port,std::span<const std::byte> data) {
        const RawAddress address(family,port);
        return ServerCoreTest::SendTo(value,reinterpret_cast<const char*>(data.data()),static_cast<int>(data.size()),0,
            address.Get(),address.length)==static_cast<int>(data.size());
    }
};
uint16_t FreePort() {
    RawSocket socket(AF_INET6,SOCK_STREAM);
    if (!socket.Bind()) return 0;
    return socket.Port();
}
struct Observer final : Net::IConnectionObserver {
    std::mutex mutex;
    std::vector<std::shared_ptr<Net::Connection>> connections;
    unsigned disconnected=0;
    void OnBytesReceived(std::span<const std::byte>) override {}
    void OnDisconnected(Core::Status) override { const std::lock_guard guard(mutex); ++disconnected; }
    std::shared_ptr<Net::Connection> At(size_t index) {
        const std::lock_guard guard(mutex); return index<connections.size()?connections[index]:nullptr;
    }
};
struct Listener {
    Net::IoContext io;
    Net::Acceptor acceptor;
    std::shared_ptr<Observer> observer=std::make_shared<Observer>();
    ~Listener() {
        acceptor.Stop();
        std::vector<std::shared_ptr<Net::Connection>> connections;
        { const std::lock_guard guard(observer->mutex); connections=observer->connections; }
        for (const auto& connection:connections) connection->Close();
        ExpectTrue(Await([&] { const std::lock_guard guard(observer->mutex); return observer->disconnected==connections.size(); }),
            "endpoint fixture drains every disconnect before stopping I/O");
        io.Stop();
    }
    bool Start(bool only=true) {
        const auto port=FreePort();
        if (!port) return false;
        auto endpoint=Core::IpEndpoint::Parse("::",port);
        if (!endpoint.IsOk()||!acceptor.Listen(endpoint.Value(),16,only).IsOk()) return false;
        acceptor.SetConnectionHandler([state=observer](std::shared_ptr<Net::Connection> connection) {
            connection->SetObserver(state);
            const std::lock_guard guard(state->mutex); state->connections.push_back(std::move(connection));
        });
        return io.Start(2).IsOk()&&acceptor.Start(io).IsOk();
    }
};
void Ipv6ConnectionEndpoints() {
    ServerCoreTest::SocketRuntime sockets;
    Listener listener;
    const bool started=listener.Start();
    ExpectTrue(started,"IPv6 wildcard TCP listener starts");
    if (!started) return;
    ExpectEqual(std::string("::"),listener.acceptor.LocalEndpoint().address.ToString(),"listener reports its wildcard bind address");
    RawSocket peer(AF_INET6,SOCK_STREAM);
    ExpectTrue(peer.Connect(listener.acceptor.Port()),"raw IPv6 peer connects");
    ExpectTrue(Await([&] { return listener.observer->At(0)!=nullptr; }),"IPv6 accept is handed off");
    const auto connection=listener.observer->At(0);
    if (!connection) return;
    const auto local=connection->LocalEndpoint(),remote=connection->RemoteEndpoint();
    ExpectTrue(local.address.ToString()=="::1"&&local.port==listener.acceptor.Port(),"accepted local address is actual interface rather than wildcard");
    ExpectTrue(remote.address.ToString()=="::1"&&remote.port==peer.Port()&&remote.port!=0,"accepted remote endpoint preserves actual ephemeral port");
    connection->Close();
    ExpectTrue(connection->LocalEndpoint()==local&&connection->RemoteEndpoint()==remote,"endpoint snapshots survive Close");
}
void Ipv6OnlyAndDualStack() {
    ServerCoreTest::SocketRuntime sockets;
    {
        Listener listener;
        const bool started=listener.Start();
        ExpectTrue(started,"IPv6-only listener starts");
        if (!started) return;
        RawSocket v4(AF_INET,SOCK_STREAM);
        ExpectTrue(!v4.Connect(listener.acceptor.Port()),"default IPv6-only listener rejects IPv4");
    }
    Listener dual;
    const bool started=dual.Start(false);
    ExpectTrue(started,"explicit dual-stack listener starts");
    if (!started) return;
    RawSocket v4(AF_INET,SOCK_STREAM);
    ExpectTrue(v4.Connect(dual.acceptor.Port()),"explicit dual-stack listener accepts IPv4");
    ExpectTrue(Await([&] { return dual.observer->At(0)!=nullptr; }),"mapped IPv4 accept is handed off");
    const auto mapped=dual.observer->At(0);
    if (mapped) {
        ExpectTrue(mapped->RemoteEndpoint().address.IsV4Mapped(),"native mapped address family is preserved");
        ExpectTrue(mapped->RemoteEndpoint().address.Normalized().ToString()=="127.0.0.1"&&mapped->RemoteEndpoint().port==v4.Port(),"explicit mapped normalization preserves peer identity");
    }
    RawSocket v6(AF_INET6,SOCK_STREAM);
    ExpectTrue(v6.Connect(dual.acceptor.Port()),"same dual-stack socket accepts IPv6");
    ExpectTrue(Await([&] { return dual.observer->At(1)!=nullptr; }),"second IPv6 accept is handed off");
    const auto native=dual.observer->At(1);
    if (native) ExpectTrue(native->RemoteEndpoint().address.ToString()=="::1","native IPv6 remains distinct from mapped peer");
}
void Ipv6DatagramEndpoints() {
    ServerCoreTest::SocketRuntime sockets;
    Net::DatagramSocket socket;
    const auto binding=socket.Bind("::1",0);
    ExpectTrue(binding.IsOk(),"IPv6 UDP binds ephemeral loopback");
    if (!binding.IsOk()) return;
    ExpectTrue(socket.LocalEndpoint().address.ToString()=="::1"&&socket.LocalEndpoint().port==socket.Port(),"UDP preserves actual local endpoint");
    RawSocket peer(AF_INET6,SOCK_DGRAM);
    ExpectTrue(peer.Bind()&&ServerCoreTest::SetSocketNonblocking(peer.value),"raw IPv6 UDP peer binds");
    const std::array payload{std::byte{'a'},std::byte{'b'},std::byte{'c'}};
    ExpectTrue(peer.SendDatagram(socket.Port(),payload),"oversized IPv6 datagram sent");
    std::array<std::byte,2> undersizedBuffer{};
    Net::DatagramReceiveResult received{Core::Status::Ok()};
    ExpectTrue(Await([&] { received=socket.Receive(undersizedBuffer); return received.status.Code()!=Core::ErrorCode::WouldBlock; }),"IPv6 truncated datagram is consumed");
    ExpectTrue(received.status.Code()==Core::ErrorCode::TooLarge,"IPv6 truncation cannot be accepted as a prefix");
    ExpectTrue(peer.SendDatagram(socket.Port(),payload),"complete IPv6 datagram sent");
    std::array<std::byte,32> buffer{};
    ExpectTrue(Await([&] { received=socket.Receive(buffer); return received.status.Code()!=Core::ErrorCode::WouldBlock; }),"IPv6 datagram received");
    ExpectTrue(received.status.IsOk()&&received.bytes==payload.size()&&received.endpoint.port==peer.Port()&&received.endpoint.address.ToString()=="::1","IPv6 datagram preserves remote endpoint");
    ExpectTrue(socket.Send(received.endpoint,payload).IsOk(),"UDP replies using retained IPv6 endpoint");
    ExpectTrue(Await([&] { return ServerCoreTest::ReceiveFrom(peer.value,reinterpret_cast<char*>(buffer.data()),static_cast<int>(buffer.size()),0,nullptr,nullptr)==3; }),"raw peer receives IPv6 reply");
    socket.Close();
    ExpectTrue(!socket.LocalEndpoint().IsValid(),"UDP close clears local binding metadata");

    const auto wildcard=Core::IpEndpoint::Parse("::",0).Value();
    ExpectTrue(socket.Bind(wildcard,false).IsOk(),"explicit UDP dual-stack bind");
    RawSocket v4(AF_INET,SOCK_DGRAM);
    ExpectTrue(v4.Bind()&&ServerCoreTest::SetSocketNonblocking(v4.value),"raw IPv4 UDP peer binds");
    ExpectTrue(v4.SendDatagram(socket.Port(),payload),"IPv4 peer sends to dual-stack UDP");
    ExpectTrue(Await([&] { received=socket.Receive(buffer); return received.status.Code()!=Core::ErrorCode::WouldBlock; }),"dual-stack UDP receives IPv4");
    ExpectTrue(received.status.IsOk()&&received.endpoint.address.IsV4Mapped(),"UDP keeps mapped source address");
    ExpectTrue(socket.Send(received.endpoint,payload).IsOk(),"dual-stack UDP can reply to mapped peer");
    ExpectTrue(Await([&] { return ServerCoreTest::ReceiveFrom(v4.value,reinterpret_cast<char*>(buffer.data()),static_cast<int>(buffer.size()),0,nullptr,nullptr)==3; }),"raw IPv4 peer receives dual-stack reply");
}
void Ipv6DatagramSessionEndpoints() {
    ServerCoreTest::SocketRuntime sockets;
    Runtime::DatagramTransport transport;
    const auto bound=transport.Bind("::1",0);
    ExpectTrue(bound.IsOk(),"session datagram adapter binds IPv6");
    if (!bound.IsOk()) return;
    const auto id=static_cast<Session::SessionId>(7);
    const auto registered=transport.RegisterSession(id);
    ExpectTrue(registered.IsOk(),"IPv6 datagram session registers token");
    if (!registered.IsOk()) return;
    ExpectTrue(transport.RemoteEndpoint(id).GetStatus().Code()==Core::ErrorCode::WouldBlock,"unadmitted datagram session has no fabricated remote address");
    RawSocket peer(AF_INET6,SOCK_DGRAM);
    ExpectTrue(peer.Bind()&&ServerCoreTest::SetSocketNonblocking(peer.value),"session datagram peer starts");
    constexpr std::string_view text=R"({"type":"Hello","body":{}})";
    const auto payload=std::as_bytes(std::span(text.data(),text.size()));
    std::array<std::byte,Protocol::DatagramCodec::MaximumDatagramBytes> bytes{};
    const auto length=Protocol::DatagramCodec::Encode(bytes,registered.Value(),1,payload);
    ExpectTrue(peer.SendDatagram(transport.Port(),std::span(bytes).first(length)),"token-bearing IPv6 packet sent");
    bool delivered=false;
    ExpectTrue(Await([&] { transport.Poll([](auto,const auto&) { return true; },[&](auto actual,const auto&) { delivered=actual==id; }); return delivered; }),"IPv6 packet traverses existing protocol admission pipeline");
    const auto remote=transport.RemoteEndpoint(id);
    ExpectTrue(remote.IsOk()&&remote.Value().address.ToString()=="::1"&&remote.Value().port==peer.Port(),"session endpoint is committed only with admitted packet");
    ExpectTrue(transport.SendSerialized(id,payload).IsOk(),"session adapter sends IPv6 framed reply");
    int received=-1;
    ExpectTrue(Await([&] { received=ServerCoreTest::ReceiveFrom(peer.value,reinterpret_cast<char*>(bytes.data()),static_cast<int>(bytes.size()),0,nullptr,nullptr); return received>0; }),"raw IPv6 peer receives adapter reply");
    if (received>0) ExpectTrue(Protocol::DatagramCodec::Decode(std::span(bytes).first(static_cast<size_t>(received))).has_value(),"existing datagram codec remains intact");
    transport.UnregisterSession(id);
    ExpectTrue(transport.RemoteEndpoint(id).GetStatus().Code()==Core::ErrorCode::NotFound,"session removal removes endpoint metadata");
}
const ServerCoreTest::CheckRegistration tcp("Transport.Ipv6ConnectionEndpoints",Ipv6ConnectionEndpoints);
const ServerCoreTest::CheckRegistration dual("Transport.Ipv6OnlyAndDualStack",Ipv6OnlyAndDualStack);
const ServerCoreTest::CheckRegistration udp("Transport.Ipv6DatagramEndpoints",Ipv6DatagramEndpoints);
const ServerCoreTest::CheckRegistration sessions("Runtime.Ipv6DatagramSessionEndpoints",Ipv6DatagramSessionEndpoints);
}
