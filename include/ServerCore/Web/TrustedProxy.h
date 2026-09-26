#pragma once
#include "ServerCore/Core/Endpoint.h"
#include "ServerCore/Export.h"
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
// X-Forwarded 모드에서 Proto·Host 목록의 길이가 For와 다르면 오른쪽(가까운 hop)부터 짝을 짓는다.
// 짝이 없는 먼 hop의 proto·host는 비고, 남는 먼 쪽 값은 버린다(Web.TrustedProxyRightAlignsForwardedFields).
class TrustedProxyPolicy
{
public:
    SERVERCORE_API static Core::Result<TrustedProxyPolicy> Create(TrustedProxyOptions options);
    SERVERCORE_API Core::Result<ProxyPeer> Resolve(
        const Core::IpEndpoint& peer, const HttpHeaders& headers) const;
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
