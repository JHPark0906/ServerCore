#include "ServerCore/C/Endpoint.h"
#include "C/EndpointInternal.h"
#include "C/Internal.h"
#include <cstring>
extern "C" {
sc_status sc_ip_endpoint_parse(sc_bytes address,uint16_t port,sc_ip_endpoint* out)
{
    if (!out || !ServerCore::CDetail::Valid(address)) return SC_INVALID_ARGUMENT;
    auto result=ServerCore::Core::IpEndpoint::Parse(ServerCore::CDetail::Text(address),port);
    if (!result.IsOk()) return ServerCore::CDetail::Code(result.GetStatus());
    *out=ServerCore::CDetail::FromEndpoint(result.Value());
    return SC_OK;
}
sc_status sc_ip_endpoint_format(const sc_ip_endpoint* endpoint,char* buffer,size_t capacity,size_t* written)
{
    if (!endpoint || !written || (capacity!=0&&!buffer)) return SC_INVALID_ARGUMENT;
    *written=0;
    return ServerCore::CDetail::Protect([&]() -> sc_status {
        ServerCore::Core::IpEndpoint value;
        if (!ServerCore::CDetail::ToEndpoint(*endpoint,value)) return SC_INVALID_ARGUMENT;
        const auto text=value.ToString();
        *written=text.size();
        if (capacity<=text.size()) return SC_TOO_LARGE;
        std::memcpy(buffer,text.c_str(),text.size()+1);
        return SC_OK;
    });
}
}
