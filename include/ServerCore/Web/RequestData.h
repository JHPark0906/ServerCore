#pragma once
#include "ServerCore/Core/Error.h"
#include "ServerCore/Export.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ServerCore::Web
{
using FormFields = std::vector<std::pair<std::string, std::string>>;
struct FieldLimits
{
    std::size_t maxBytes = 64 * 1024;
    std::size_t maxFields = 128;
    std::size_t maxNameBytes = 1024;
    std::size_t maxValueBytes = 64 * 1024;
};
// UTF-8 components; decode exactly once. Malformed escapes, UTF-8 or NUL are
// InvalidFormat. Other decoded controls remain data (e.g. textarea newlines).
// A path/header consumer must impose its own context-specific restrictions.
SERVERCORE_API Core::Result<std::string> PercentDecodeComponent(
    std::string_view input, bool plusAsSpace = false, std::size_t maxBytes = 64 * 1024);
// Generic mode keeps RFC3986 unreserved bytes; HTML form mode keeps *-._ and
// alphanumerics and maps space to + (thus * and ~ differ between the modes).
// Invalid UTF-8/NUL input is InvalidArgument; maxBytes bounds encoded output.
SERVERCORE_API Core::Result<std::string> PercentEncodeComponent(
    std::string_view input, bool formMode = false, std::size_t maxBytes = 64 * 1024);
// Ordered, owned pairs preserve ALL duplicates; callers choose duplicate policy.
// Split '&' and the first '=' before decoding. Empty segments are ignored;
// missing '=' means an empty value. Query takes a complete request target and
// preserves '+'. Form takes a body and maps '+' to space. Fragments are invalid.
SERVERCORE_API Core::Result<FormFields> ParseQueryParameters(
    std::string_view requestTarget, const FieldLimits& limits = {});
SERVERCORE_API Core::Result<FormFields> ParseFormUrlEncoded(
    std::string_view body, const FieldLimits& limits = {});
// One Cookie field; combine repeated fields by appending these ordered results.
// Names are case-sensitive. Values use RFC6265 cookie-octet, optionally quoted;
// no percent decoding or backslash unescaping. Duplicates remain separate.
SERVERCORE_API Core::Result<FormFields> ParseCookieHeader(
    std::string_view header, const FieldLimits& limits = {});
enum class CookieSameSite
{
    Unspecified,
    Lax,
    Strict,
    None
};
struct CookieOptions
{
    std::string domain{};
    std::string path = "/";
    std::optional<std::int64_t> maxAgeSeconds{}; // <=0 emits Max-Age=0.
    CookieSameSite sameSite = CookieSameSite::Lax;
    bool secure = false;
    bool httpOnly = true;
    std::size_t maxBytes = 4096;
};
// Returns a Set-Cookie field VALUE, not a complete header. No automatic encoding.
// Rejects injection; SameSite=None and __Secure-/__Host- require Secure;
// __Host- additionally requires Path=/ and no Domain. No ambient cookie jar.
SERVERCORE_API Core::Result<std::string> SerializeSetCookie(
    std::string_view name, std::string_view value, const CookieOptions& options = {});
}
