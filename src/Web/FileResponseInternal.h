#pragma once
#include "Web/WebProtocol.h"
#include <algorithm>
#include <charconv>
#include <ctime>
#include <iomanip>
#include <locale>
#include <sstream>

namespace ServerCore::Web::Detail
{
inline std::string_view FileTrim(std::string_view text) noexcept
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}
inline bool ValidEntityTag(std::string_view tag) noexcept
{
    if (tag.starts_with("W/")) tag.remove_prefix(2);
    if (tag.size() < 2 || tag.front() != '"' || tag.back() != '"') return false;
    tag.remove_prefix(1); tag.remove_suffix(1);
    for (const unsigned char byte : tag) if (byte < 0x21 || byte == 0x22 || byte == 0x7f) return false;
    return true;
}
inline bool EntityTagMatches(std::string_view list, std::string_view actual, bool weak) noexcept
{
    list = FileTrim(list);
    if (list == "*") return true;
    bool matched = false;
    while (!list.empty())
    {
        list = FileTrim(list);
        const auto prefix = list.starts_with("W/") ? std::size_t{2} : 0;
        if (list.size() <= prefix || list[prefix] != '"') return false;
        const auto end = list.find('"', prefix + 1);
        if (end == std::string_view::npos) return false;
        auto tag = list.substr(0, end + 1);
        if (!ValidEntityTag(tag)) return false;
        auto target = actual;
        if (weak)
        {
            if (tag.starts_with("W/")) tag.remove_prefix(2);
            if (target.starts_with("W/")) target.remove_prefix(2);
        }
        if (tag == target && (weak || (!tag.starts_with("W/") && !target.starts_with("W/")))) matched = true;
        list = FileTrim(list.substr(end + 1));
        if (list.empty()) break;
        if (list.front() != ',') return false;
        list.remove_prefix(1);
    }
    return matched;
}
inline std::optional<std::chrono::sys_seconds> ParseFileDate(std::string_view value)
{
    for (const auto* format : {"%a, %d %b %Y %H:%M:%S GMT", "%A, %d-%b-%y %H:%M:%S GMT", "%a %b %d %H:%M:%S %Y"})
    {
        std::tm parsed{};
        std::istringstream input{std::string(value)};
        input.imbue(std::locale::classic());
        input >> std::get_time(&parsed, format);
        if (input.fail() || input.peek() != std::char_traits<char>::eof() || parsed.tm_sec > 59) continue;
        int year = parsed.tm_year + 1900;
        if (std::string_view(format).find("%y") != std::string_view::npos)
        {
            const auto today = std::chrono::year_month_day(std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now()));
            const auto current = static_cast<int>(today.year());
            year = current / 100 * 100 + year % 100;
            if (year > current + 50) year -= 100;
        }
        const std::chrono::year_month_day date{std::chrono::year(year),
            std::chrono::month(static_cast<unsigned>(parsed.tm_mon + 1)), std::chrono::day(static_cast<unsigned>(parsed.tm_mday))};
        if (year < 1601 || year > 9999 || !date.ok() || parsed.tm_hour < 0 || parsed.tm_hour > 23 ||
            parsed.tm_min < 0 || parsed.tm_min > 59 || parsed.tm_sec < 0) continue;
        return std::chrono::sys_days(date) + std::chrono::hours(parsed.tm_hour) +
            std::chrono::minutes(parsed.tm_min) + std::chrono::seconds(parsed.tm_sec);
    }
    return {};
}
struct FileResponsePlan
{
    unsigned status = 200;
    std::uint64_t offset = 0, length = 0;
    std::string contentRange;
};
inline FileResponsePlan PlanFileResponse(const HttpRequest& request, std::uint64_t size,
    std::string_view entityTag, std::chrono::system_clock::time_point modified)
{
    FileResponsePlan plan{200, 0, size, {}};
    const auto field = [&](std::string_view name) {
        std::optional<std::string> joined;
        for (const auto& [key, value] : request.headers)
            if (EqualInsensitive(key, name)) { if (joined) *joined += ","; else joined.emplace(); *joined += value; }
        return joined;
    };
    const bool retrieval = request.method == "GET" || request.method == "HEAD";
    const auto lastModified = std::chrono::floor<std::chrono::seconds>(modified);
    const auto match = field("if-match");
    if (match && !EntityTagMatches(*match, entityTag, false)) plan.status = 412;
    if (!match)
        if (const auto value = field("if-unmodified-since"))
            if (const auto date = ParseFileDate(*value); date && lastModified > *date) plan.status = 412;
    const auto noneMatch = field("if-none-match");
    if (plan.status == 200 && noneMatch && EntityTagMatches(*noneMatch, entityTag, true)) plan.status = retrieval ? 304u : 412u;
    if (plan.status == 200 && !noneMatch && retrieval)
        if (const auto value = field("if-modified-since"))
            if (const auto date = ParseFileDate(*value); date && lastModified <= *date) plan.status = 304;
    if (plan.status != 200) { plan.length = 0; return plan; }
    if (request.method != "GET") return plan; // Range has GET semantics only.
    const auto range = field("range");
    if (!range) return plan;
    if (const auto condition = field("if-range"))
    {
        // Filesystem timestamps are not guaranteed strong validators. A date or
        // weak tag therefore safely selects the full representation.
        if (!ValidEntityTag(*condition) || condition->starts_with("W/") || entityTag.starts_with("W/") || *condition != entityTag) return plan;
    }
    auto value = FileTrim(*range);
    if (value.size() < 6 || !EqualInsensitive(value.substr(0, 6), "bytes=")) return plan;
    value.remove_prefix(6);
    if (value.find(',') != std::string_view::npos) return plan; // Multipart is deliberately ignored.
    const auto dash = value.find('-');
    if (dash == std::string_view::npos) return plan;
    const auto integer = [](std::string_view text, std::uint64_t& number) {
        if (text.empty()) return false;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
        return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
    };
    std::uint64_t first = 0, last = 0;
    const auto left = value.substr(0, dash), right = value.substr(dash + 1);
    if (left.empty())
    {
        if (!integer(right, last)) return plan;
        if (last == 0 || size == 0) plan.status = 416;
        else { plan.offset = size - (std::min)(size, last); plan.length = size - plan.offset; plan.status = 206; }
    }
    else
    {
        if (!integer(left, first) || (!right.empty() && !integer(right, last))) return plan;
        if (!right.empty() && first > last) return plan;
        if (first >= size) plan.status = 416;
        else
        {
            plan.offset = first;
            plan.length = (right.empty() ? size - 1 : (std::min)(last, size - 1)) - first + 1;
            plan.status = 206;
        }
    }
    if (plan.status == 416) { plan.length = 0; plan.contentRange = "bytes */" + std::to_string(size); }
    if (plan.status == 206) plan.contentRange = "bytes " + std::to_string(plan.offset) + "-" +
        std::to_string(plan.offset + plan.length - 1) + "/" + std::to_string(size);
    return plan;
}
}
