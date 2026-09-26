#include "Web/RoutePattern.h"

#include "Core/Utf8Internal.h"
#include "ServerCore/Core/Assert.h"

#include <cstddef>
#include <unordered_set>
#include <utility>

namespace ServerCore::Web::Detail
{
namespace
{
int HexDigit(const unsigned char value) noexcept
{
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

bool DecodeSegment(const std::string_view raw, std::string& output)
{
    std::string decoded;
    decoded.reserve(raw.size());
    for (std::size_t index = 0; index < raw.size(); ++index)
    {
        unsigned char value = static_cast<unsigned char>(raw[index]);
        // A raw request target is visible ASCII; percent escapes can represent
        // UTF-8 and spaces, but never separators or control bytes.
        if (value < 33 || value >= 127 || value == '?' || value == '#')
            return false;
        if (value == '%')
        {
            if (raw.size() - index < 3)
                return false;
            const int high = HexDigit(static_cast<unsigned char>(raw[index + 1]));
            const int low = HexDigit(static_cast<unsigned char>(raw[index + 2]));
            if (high < 0 || low < 0)
                return false;
            value = static_cast<unsigned char>(high * 16 + low);
            index += 2;
        }
        if (value < 32 || value == 127 || value == '/' || value == '\\')
            return false;
        decoded.push_back(static_cast<char>(value));
    }
    if (decoded == "." || decoded == ".." || !Core::Detail::IsValidUtf8(decoded))
        return false;
    output = std::move(decoded);
    return true;
}

template <typename Visitor> bool VisitSegments(std::string_view path, Visitor&& visit)
{
    if (path.empty() || path.front() != '/')
        return false;
    path.remove_prefix(1);
    while (true)
    {
        const auto slash = path.find('/');
        if (!visit(path.substr(0, slash)))
            return false;
        if (slash == std::string_view::npos)
            return true;
        path.remove_prefix(slash + 1);
    }
}

bool IdentifierStart(const unsigned char value) noexcept
{
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_';
}

bool ValidParameterName(const std::string_view name) noexcept
{
    if (name.empty() || !IdentifierStart(static_cast<unsigned char>(name.front())))
        return false;
    for (const unsigned char value : name)
        if (!IdentifierStart(value) && (value < '0' || value > '9'))
            return false;
    return true;
}
}

bool CompileRoutePattern(const std::string_view pattern, RoutePattern& output)
{
    RoutePattern compiled;
    std::unordered_set<std::string_view> names;
    const bool valid = VisitSegments(pattern,
        [&](const std::string_view raw)
        {
            RouteSegment segment;
            if (raw.find_first_of("{}") != std::string_view::npos)
            {
                if (raw.size() < 3 || raw.front() != '{' || raw.back() != '}')
                    return false;
                const auto name = raw.substr(1, raw.size() - 2);
                if (!ValidParameterName(name) || !names.emplace(name).second)
                    return false;
                segment.value.assign(name);
                segment.parameter = true;
            }
            else if (!DecodeSegment(raw, segment.value))
                return false;
            compiled.segments.push_back(std::move(segment));
            return true;
        });
    if (!valid)
        return false;
    output = std::move(compiled);
    return true;
}

bool DecodeRoutePath(const std::string_view rawPath, RoutePath& output)
{
    RoutePath decoded;
    const bool valid = VisitSegments(rawPath,
        [&](const std::string_view raw)
        {
            std::string segment;
            if (!DecodeSegment(raw, segment))
                return false;
            decoded.push_back(std::move(segment));
            return true;
        });
    if (!valid)
        return false;
    output = std::move(decoded);
    return true;
}

bool MatchesRoutePattern(const RoutePattern& pattern, const RoutePath& path) noexcept
{
    if (pattern.segments.size() != path.size())
        return false;
    for (std::size_t index = 0; index < path.size(); ++index)
    {
        const auto& segment = pattern.segments[index];
        if (segment.parameter ? path[index].empty() : segment.value != path[index])
            return false;
    }
    return true;
}

bool SameRouteShape(const RoutePattern& left, const RoutePattern& right) noexcept
{
    if (left.segments.size() != right.segments.size())
        return false;
    for (std::size_t index = 0; index < left.segments.size(); ++index)
    {
        const auto& leftSegment = left.segments[index];
        const auto& rightSegment = right.segments[index];
        if (leftSegment.parameter != rightSegment.parameter ||
            (!leftSegment.parameter && leftSegment.value != rightSegment.value))
            return false;
    }
    return true;
}

bool MoreSpecificRoutePattern(const RoutePattern& left, const RoutePattern& right) noexcept
{
    if (left.segments.size() != right.segments.size())
        return false;
    for (std::size_t index = 0; index < left.segments.size(); ++index)
        if (left.segments[index].parameter != right.segments[index].parameter)
            return !left.segments[index].parameter;
    return false;
}

void CaptureRouteParameters(const RoutePattern& pattern, const RoutePath& path,
    std::vector<std::pair<std::string, std::string>>& output)
{
    SERVERCORE_ASSERT(
        MatchesRoutePattern(pattern, path), "route parameters require a matching path");
    std::vector<std::pair<std::string, std::string>> captured;
    for (std::size_t index = 0; index < pattern.segments.size(); ++index)
        if (pattern.segments[index].parameter)
            captured.emplace_back(pattern.segments[index].value, path[index]);
    output = std::move(captured);
}
}
