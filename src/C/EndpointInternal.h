#pragma once
#include "ServerCore/C/Endpoint.h"
#include "ServerCore/Core/Endpoint.h"
#include <algorithm>

namespace ServerCore::CDetail
{
inline sc_ip_endpoint FromEndpoint(const Core::IpEndpoint& value) noexcept
{
    sc_ip_endpoint result{};
    result.family=static_cast<uint32_t>(value.address.Family());
    result.scope_id=value.address.ScopeId(); result.port=value.port;
    std::copy(value.address.Bytes().begin(),value.address.Bytes().end(),result.address);
    return result;
}
inline bool ToEndpoint(const sc_ip_endpoint& value,Core::IpEndpoint& output) noexcept
{
    if (value.reserved!=0 || (value.family!=SC_IP_V4&&value.family!=SC_IP_V6)) return false;
    if (value.family==SC_IP_V4)
        for (size_t index=4;index<16;++index) if (value.address[index]!=0) return false;
    auto ip=Core::IpAddress::FromBytes(static_cast<Core::IpFamily>(value.family),
        {value.address,value.family==SC_IP_V4?4u:16u},value.scope_id);
    if (!ip.IsOk()) return false;
    output={ip.Value(),value.port};
    return true;
}
}
