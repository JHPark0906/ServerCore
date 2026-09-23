#pragma once
#include "ServerCore/Web/HttpServer.h"
#include <algorithm>
#include <charconv>
#include <string_view>
namespace ServerCore::Web::Detail
{
inline char LowerAscii(char c) noexcept
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}
inline std::string Lower(std::string_view text)
{
    std::string out(text);
    for (auto& c : out)
        c = LowerAscii(c);
    return out;
}
inline bool Same(std::string_view a, std::string_view b) noexcept
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y)
                                       { return LowerAscii(x) == LowerAscii(y); });
}
inline std::string_view Trim(std::string_view s) noexcept
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.remove_suffix(1);
    return s;
}
inline bool Token(std::string_view text) noexcept
{
    if (text.empty())
        return false;
    for (unsigned char c : text)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                std::string_view("!#$%&'*+-.^_`|~").find(static_cast<char>(c)) !=
                    std::string_view::npos))
            return false;
    return true;
}
inline bool HeaderValue(std::string_view text) noexcept
{
    for (unsigned char c : text)
        if ((c < 32 && c != 9) || c == 127)
            return false;
    return true;
}
inline bool Port(std::string_view text, std::uint16_t& value) noexcept
{
    unsigned int parsed = 0;
    const auto r = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (text.empty() || r.ec != std::errc{} || r.ptr != text.data() + text.size() || parsed > 65535)
        return false;
    value = static_cast<std::uint16_t>(parsed);
    return true;
}
inline bool Authority(std::string_view text) noexcept
{
    if (text.empty() || text.size() > 255)
        return false;
    for (unsigned char c : text)
        if (c <= 32 || c >= 127 ||
            std::string_view("/@\\?#,;\"").find(static_cast<char>(c)) != std::string_view::npos)
            return false;
    std::string_view host = text;
    if (text.front() == '[')
    {
        auto end = text.find(']');
        if (end == std::string_view::npos || end < 2)
            return false;
        host = text.substr(1, end - 1);
        auto ip = Core::IpAddress::Parse(host);
        if (!ip.IsOk() || ip.Value().Family() != Core::IpFamily::V6 || ip.Value().ScopeId() != 0)
            return false;
        if (end + 1 == text.size())
            return true;
        std::uint16_t port = 0;
        return text[end + 1] == ':' && Port(text.substr(end + 2), port);
    }
    auto colon = text.find(':');
    if (colon != std::string_view::npos)
    {
        host = text.substr(0, colon);
        std::uint16_t port = 0;
        if (!Port(text.substr(colon + 1), port))
            return false;
    }
    if (host.empty())
        return false;
    for (char c : host)
        if (!(c == '.' || c == '-' || (c >= '0' && c <= '9') ||
                (LowerAscii(c) >= 'a' && LowerAscii(c) <= 'z')))
            return false;
    return true;
}
inline void SetHeader(HttpHeaders& headers, std::string name, std::string value)
{
    if (!Same(name, "set-cookie"))
        std::erase_if(headers, [&](const auto& item) { return Same(item.first, name); });
    headers.emplace_back(std::move(name), std::move(value));
}
}
