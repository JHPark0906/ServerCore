#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ServerCore::Web::Detail
{
struct RouteSegment
{
    std::string value;
    bool parameter = false;
};

struct RoutePattern
{
    std::vector<RouteSegment> segments;
};

// The leading slash is not a segment. Empty and trailing segments are retained:
// "/" is {""}, "/a/" is {"a", ""}, and "/a//b" is {"a", "", "b"}.
using RoutePath = std::vector<std::string>;

// Parameters occupy a whole raw segment: {name}. Percent-encoded braces and
// literal asterisks are static text. Names are unique ASCII identifiers.
// Both functions split before decoding percent escapes exactly once, and leave
// output unchanged on failure. Allocation failures propagate to the caller.
[[nodiscard]] bool CompileRoutePattern(std::string_view pattern, RoutePattern& output);
[[nodiscard]] bool DecodeRoutePath(std::string_view rawPath, RoutePath& output);

[[nodiscard]] bool MatchesRoutePattern(const RoutePattern& pattern, const RoutePath& path) noexcept;
[[nodiscard]] bool SameRouteShape(const RoutePattern& left, const RoutePattern& right) noexcept;
// Compare patterns already known to match the same path. A static segment wins
// at the first static/parameter difference; indistinguishable precedence ties.
[[nodiscard]] bool MoreSpecificRoutePattern(
    const RoutePattern& left, const RoutePattern& right) noexcept;

// Requires MatchesRoutePattern(pattern, path). Replaces output on success;
// allocation failures leave its previous contents intact.
void CaptureRouteParameters(const RoutePattern& pattern, const RoutePath& path,
    std::vector<std::pair<std::string, std::string>>& output);
}
