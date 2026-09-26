#include "ServerCore/Web/TrustedProxy.h"
#include "Web/PolicyInternal.h"
#include <optional>
#include <set>

namespace ServerCore::Web
{
namespace
{
using Core::ErrorCode;
using Core::Status;
template <class T> Core::Result<T> Fail(ErrorCode code = ErrorCode::InvalidFormat)
{
    return Core::Result<T>::FromStatus(Status::FailWithoutMessage(code));
}
struct Hop
{
    std::string node, proto, host;
};
Core::Result<std::vector<Hop>> ParseForwarded(std::string_view value, std::size_t maxHops)
{
    std::vector<Hop> hops;
    while (!value.empty())
    {
        if (hops.size() >= maxHops)
            return Fail<std::vector<Hop>>(ErrorCode::TooLarge);
        Hop hop;
        std::set<std::string> names;
        bool hasPair = false;
        while (true)
        {
            value = Detail::Trim(value);
            auto equal = value.find('=');
            if (equal == std::string_view::npos)
                return Fail<std::vector<Hop>>();
            auto name = Detail::Lower(Detail::Trim(value.substr(0, equal)));
            if (!Detail::Token(name) || !names.insert(name).second)
                return Fail<std::vector<Hop>>();
            value.remove_prefix(equal + 1);
            value = Detail::Trim(value);
            std::string parsed;
            if (!value.empty() && value.front() == '"')
            {
                value.remove_prefix(1);
                bool ended = false;
                while (!value.empty())
                {
                    char c = value.front();
                    value.remove_prefix(1);
                    if (c == '"')
                    {
                        ended = true;
                        break;
                    }
                    if (c == '\\')
                    {
                        if (value.empty())
                            return Fail<std::vector<Hop>>();
                        c = value.front();
                        value.remove_prefix(1);
                    }
                    if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) >= 127)
                        return Fail<std::vector<Hop>>();
                    parsed.push_back(c);
                }
                if (!ended)
                    return Fail<std::vector<Hop>>();
            }
            else
            {
                auto end = value.find_first_of(";, \t");
                auto token = value.substr(0, end);
                if (!Detail::Token(token))
                    return Fail<std::vector<Hop>>();
                parsed.assign(token);
                value.remove_prefix(token.size());
            }
            if (name == "for")
                hop.node = std::move(parsed);
            else if (name == "proto")
                hop.proto = std::move(parsed);
            else if (name == "host")
                hop.host = std::move(parsed);
            hasPair = true;
            value = Detail::Trim(value);
            if (value.empty())
                break;
            const char delimiter = value.front();
            value.remove_prefix(1);
            if (delimiter == ',')
            {
                if (Detail::Trim(value).empty())
                    return Fail<std::vector<Hop>>();
                break;
            }
            if (delimiter != ';' || value.empty())
                return Fail<std::vector<Hop>>();
        }
        if (!hasPair)
            return Fail<std::vector<Hop>>();
        hops.push_back(std::move(hop));
    }
    return Core::Result<std::vector<Hop>>::FromValue(std::move(hops));
}
Core::Result<std::vector<std::string>> List(std::string_view text, std::size_t max)
{
    std::vector<std::string> list;
    if (text.empty())
        return Core::Result<std::vector<std::string>>::FromValue(std::move(list));
    while (true)
    {
        auto comma = text.find(',');
        auto part = Detail::Trim(text.substr(0, comma));
        if (part.empty())
            return Fail<std::vector<std::string>>();
        if (list.size() >= max)
            return Fail<std::vector<std::string>>(ErrorCode::TooLarge);
        list.emplace_back(part);
        if (comma == std::string_view::npos)
            break;
        text.remove_prefix(comma + 1);
    }
    return Core::Result<std::vector<std::string>>::FromValue(std::move(list));
}
Core::Result<std::optional<Core::IpEndpoint>> Node(std::string_view text, bool forwarded)
{
    if (text.empty() || Detail::Same(text, "unknown") || text.front() == '_')
        return Core::Result<std::optional<Core::IpEndpoint>>::FromValue(std::nullopt);
    std::string_view address = text;
    std::uint16_t port = 0;
    if (text.front() == '[')
    {
        auto close = text.find(']');
        if (close == std::string_view::npos)
            return Fail<std::optional<Core::IpEndpoint>>();
        address = text.substr(1, close - 1);
        if (close + 1 < text.size() &&
            (text[close + 1] != ':' || !Detail::Port(text.substr(close + 2), port)))
            return Fail<std::optional<Core::IpEndpoint>>();
    }
    else if (forwarded)
    {
        const auto colon = text.find(':');
        if (colon != std::string_view::npos)
        {
            address = text.substr(0, colon);
            if (!Detail::Port(text.substr(colon + 1), port))
                return Fail<std::optional<Core::IpEndpoint>>();
        }
    }
    auto parsed = Core::IpAddress::Parse(address);
    if (!parsed.IsOk() || parsed.Value().ScopeId() != 0)
        return Fail<std::optional<Core::IpEndpoint>>();
    if (forwarded && parsed.Value().Family() == Core::IpFamily::V6 && text.front() != '[')
        return Fail<std::optional<Core::IpEndpoint>>();
    return Core::Result<std::optional<Core::IpEndpoint>>::FromValue(
        Core::IpEndpoint{ parsed.Value(), port });
}
}
Core::Result<TrustedProxyPolicy> TrustedProxyPolicy::Create(TrustedProxyOptions options)
{
    if (!options.maxHops || options.maxHops > 256 || !options.maxHeaderBytes ||
        options.maxHeaderBytes > 1024 * 1024 || options.trustedNetworks.size() > 256 ||
        (options.mode != ProxyHeaderMode::Forwarded && options.mode != ProxyHeaderMode::XForwarded))
        return Fail<TrustedProxyPolicy>(ErrorCode::InvalidArgument);
    return Core::Result<TrustedProxyPolicy>::FromValue(TrustedProxyPolicy(std::move(options)));
}
bool TrustedProxyPolicy::Trusted(const Core::IpAddress& input) const noexcept
{
    const auto address = mOptions.normalizeMappedIpv4 ? input.Normalized() : input;
    for (const auto& range : mOptions.trustedNetworks)
        if (range.Contains(address))
            return true;
    return false;
}
Core::Result<ProxyPeer> TrustedProxyPolicy::Resolve(const HttpRequest& request) const
{
    return Resolve(request.remoteEndpoint, request.headers);
}
Core::Result<ProxyPeer> TrustedProxyPolicy::Resolve(
    const Core::IpEndpoint& peer, const HttpHeaders& headers) const
{
    if (peer.address.Family() == Core::IpFamily::Unspecified)
        return Fail<ProxyPeer>(ErrorCode::InvalidArgument);
    try
    {
        ProxyPeer result{ peer, peer, 0, {}, {} };
        // Do not parse attacker-controlled headers from an untrusted socket peer.
        if (!Trusted(peer.address))
            return Core::Result<ProxyPeer>::FromValue(std::move(result));
        std::string forwarded, xff, proto, host;
        std::size_t bytes = 0;
        for (const auto& [name, value] : headers)
        {
            std::string* destination = nullptr;
            if (mOptions.mode == ProxyHeaderMode::Forwarded && Detail::Same(name, "forwarded"))
                destination = &forwarded;
            if (mOptions.mode == ProxyHeaderMode::XForwarded &&
                Detail::Same(name, "x-forwarded-for"))
                destination = &xff;
            if (mOptions.mode == ProxyHeaderMode::XForwarded && mOptions.acceptProto &&
                Detail::Same(name, "x-forwarded-proto"))
                destination = &proto;
            if (mOptions.mode == ProxyHeaderMode::XForwarded && mOptions.acceptHost &&
                Detail::Same(name, "x-forwarded-host"))
                destination = &host;
            if (!destination)
                continue;
            if (value.size() >= mOptions.maxHeaderBytes - bytes)
                return Fail<ProxyPeer>(ErrorCode::TooLarge);
            bytes += value.size() + 1;
            if (!destination->empty())
                destination->push_back(',');
            destination->append(value);
        }
        std::vector<Hop> hops;
        if (mOptions.mode == ProxyHeaderMode::Forwarded)
        {
            auto parsed = ParseForwarded(forwarded, mOptions.maxHops);
            if (!parsed.IsOk())
                return Fail<ProxyPeer>(parsed.GetStatus().Code());
            hops = std::move(parsed.Value());
        }
        else
        {
            auto nodes = List(xff, mOptions.maxHops), protocols = List(proto, mOptions.maxHops),
                 hosts = List(host, mOptions.maxHops);
            if (!nodes.IsOk())
                return Fail<ProxyPeer>(nodes.GetStatus().Code());
            if (!protocols.IsOk())
                return Fail<ProxyPeer>(protocols.GetStatus().Code());
            if (!hosts.IsOk())
                return Fail<ProxyPeer>(hosts.GetStatus().Code());
            // nginx·ALB는 X-Forwarded-For에 덧붙이고 Proto·Host는 한 값으로 덮어쓴다. 길이가 다르면 오른쪽
            // (가까운 hop)부터 짝을 짓고, 짝이 없는 먼 hop은 비워 둔다. 남는 먼 쪽 값은 버린다(WEB-6).
            const auto aligned = [&nodes](std::vector<std::string>& values, std::size_t index)
            {
                const auto count = nodes.Value().size();
                return index + values.size() < count
                           ? std::string{}
                           : std::move(values[index + values.size() - count]);
            };
            for (std::size_t i = 0; i < nodes.Value().size(); ++i)
                hops.push_back({ std::move(nodes.Value()[i]), aligned(protocols.Value(), i),
                    aligned(hosts.Value(), i) });
        }
        for (auto it = hops.rbegin(); it != hops.rend() && Trusted(result.client.address); ++it)
        {
            auto node = Node(it->node, mOptions.mode == ProxyHeaderMode::Forwarded);
            if (!node.IsOk())
                return Fail<ProxyPeer>();
            if (!node.Value())
                break;
            if (mOptions.acceptProto && !it->proto.empty() && it->proto != "http" &&
                it->proto != "https")
                return Fail<ProxyPeer>();
            if (mOptions.acceptHost && !it->host.empty() && !Detail::Authority(it->host))
                return Fail<ProxyPeer>();
            result.client = *node.Value();
            ++result.acceptedHops;
            result.proto = mOptions.acceptProto ? it->proto : "";
            result.host = mOptions.acceptHost ? it->host : "";
        }
        return Core::Result<ProxyPeer>::FromValue(std::move(result));
    }
    catch (...)
    {
        return Fail<ProxyPeer>(ErrorCode::PlatformError);
    }
}
}
