#include "Core/Utf8Internal.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ServerCore::Core::Detail
{
bool IsValidUtf8(const std::string_view text) noexcept
{
    constexpr std::uint32_t MaximumCodePoint = 0x10FFFF;
    constexpr std::uint32_t FirstSurrogate = 0xD800;
    constexpr std::uint32_t LastSurrogate = 0xDFFF;

    for (std::size_t index = 0; index < text.size();)
    {
        const auto lead = static_cast<unsigned char>(text[index]);
        std::size_t continuationCount = 0;
        std::uint32_t codePoint = 0;
        std::uint32_t smallest = 0;
        if (lead < 0x80)
        {
            codePoint = lead;
        }
        else if ((lead & 0xE0U) == 0xC0U)
        {
            continuationCount = 1;
            codePoint = lead & 0x1FU;
            smallest = 0x80;
        }
        else if ((lead & 0xF0U) == 0xE0U)
        {
            continuationCount = 2;
            codePoint = lead & 0x0FU;
            smallest = 0x800;
        }
        else if ((lead & 0xF8U) == 0xF0U)
        {
            continuationCount = 3;
            codePoint = lead & 0x07U;
            smallest = 0x10000;
        }
        else
        {
            return false;
        }

        if (continuationCount > 0 && index + continuationCount >= text.size())
        {
            return false;
        }
        for (std::size_t offset = 1; offset <= continuationCount; ++offset)
        {
            const auto continuation = static_cast<unsigned char>(text[index + offset]);
            if ((continuation & 0xC0U) != 0x80U)
            {
                return false;
            }
            codePoint = (codePoint << 6U) | (continuation & 0x3FU);
        }
        index += continuationCount + 1;

        // 연속 바이트 모양만으로는 충분하지 않다. overlong 표기와 UTF-16 surrogate를 거절해
        // 서로 다른 바이트열이 같은 문자를 가장하거나 Unicode scalar 범위를 벗어나지 못하게 한다.
        if (codePoint < smallest || (codePoint >= FirstSurrogate && codePoint <= LastSurrogate) ||
            codePoint > MaximumCodePoint)
        {
            return false;
        }
    }
    return true;
}
}
