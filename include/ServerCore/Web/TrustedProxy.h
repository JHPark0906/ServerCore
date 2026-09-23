#pragma once
#include "ServerCore/Export.h"
#include "ServerCore/Core/Endpoint.h"
#include "ServerCore/Web/HttpServer.h"
#include <vector>

namespace ServerCore::Web
{
enum class ProxyHeaderMode
{
    Forwarded,
    XForwarded
};
struct TrustedProxyOptions
{
    std::vector<Core::IpNetwork> trustedNetworks;
    ProxyHeaderMode mode = ProxyHeaderMode::Forwarded;
    std::size_t maxHops = 32;
    std::size_t maxHeaderBytes = 8192;
    bool acceptProto = false;
    bool acceptHost = false;
    bool normalizeMappedIpv4 = true;
};
struct ProxyPeer
{
    Core::IpEndpoint transportPeer;
    Core::IpEndpoint client;
    std::size_t acceptedHops = 0;
    // Empty unless explicitly enabled and supplied by the selected trusted hop.
    std::string proto, host;
};
// Header parsing and trust are separate from socket metadata. No configuration
// means no trust. Walk right-to-left, stopping at the first untrusted/unknown
// node; never use a client-supplied leftmost value across that boundary.
class TrustedProxyPolicy
{
public:
    SERVERCORE_API static Core::Result<TrustedProxyPolicy> Create(TrustedProxyOptions options);
    SERVERCORE_API Core::Result<ProxyPeer> Resolve(const Core::IpEndpoint& peer, const HttpHeaders& headers) const;
    SERVERCORE_API Core::Result<ProxyPeer> Resolve(const HttpRequest& request) const;

private:
    explicit TrustedProxyPolicy(TrustedProxyOptions options)
        : mOptions(std::move(options))
    {
    }
    [[nodiscard]] SERVERCORE_API bool Trusted(const Core::IpAddress& address) const noexcept;
    TrustedProxyOptions mOptions;
};
}
