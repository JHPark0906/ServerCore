#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

/// <summary>
/// 길이 접두 프레이밍의 링크 독립 구현이다.
/// </summary>
/// <remarks>
/// 이 헤더는 게임 클라이언트가 ServerCore 정적 라이브러리를 링크하지 않고도 그대로 포함할 수
/// 있다. 따라서 운영체제 헤더와 ServerCore의 상태 타입을 포함하지 않고, 동적 할당이나 C++
/// 예외를 사용하지 않는다. 모든 공개 함수는 noexcept이며, 저장소는 호출자가 준다.
/// ServerCore의 Protocol::FrameReader는 이 스트리밍 구현을 감싸 Core::Status와 동적 보관으로
/// 바꿀 뿐, 별도 프레이밍을 만들지 않는다.
/// </remarks>
namespace ServerCore::Protocol::FrameCodec
{
inline constexpr std::size_t HeaderSize = 4;
inline constexpr std::uint32_t DefaultMaxBodySize = 64u * 1024u;

/// <summary>공유 프레이밍이 돌려주는 실패 종류다.</summary>
enum class Error
{
    None,
    WouldBlock,
    TooLarge,
    InvalidFormat,
    InvalidArgument,
    InsufficientStorage
};

/// <summary>입력 바이트를 얼마나 받아들였는지와 그 결과다.</summary>
/// <remarks>
/// error가 None이 아니면 consumed 뒤의 입력은 Reader가 보관하지 않았다. 호출자는 그 tail을
/// 잃지 않아야 한다. 형식 오류는 복구하지 않는 것이 와이어 계약이므로, 서버는 보통 연결을
/// 닫는다.
/// </remarks>
struct AppendResult
{
    Error error = Error::None;
    std::size_t consumed = 0;

    [[nodiscard]] bool IsOk() const noexcept
    {
        return error == Error::None;
    }
};

/// <summary>완결된 본문을 동기적으로 받는 함수다.</summary>
/// <remarks>
/// callback은 예외를 던지지 않아야 하며 같은 Reader에 재진입하면 안 된다. body span은 callback
/// 이 돌아올 때까지만 유효하다. callback이 본문을 보관하려면 그 안에서 호출자 자신의 저장소로
/// 복사한다.
/// </remarks>
using FrameCallback = void (*)(void* context, std::span<const std::byte> body) noexcept;

/// <summary>출력 슬롯에 프레임을 인코딩한 결과다.</summary>
struct EncodeResult
{
    Error error = Error::None;
    std::size_t required = 0;
    std::size_t written = 0;

    [[nodiscard]] bool IsOk() const noexcept
    {
        return error == Error::None;
    }
};

/// <summary>
/// TCP 바이트 흐름을 길이 접두 프레임으로 자른다.
/// </summary>
/// <remarks>
/// 연결 하나가 하나씩, 한 스레드에서만 사용한다. 생성자에 준 storage는 적어도
/// HeaderSize + maxBodySize 바이트여야 한다. Reader는 완결된 프레임을 대기열에 할당하지 않고
/// Append 중 callback으로 즉시 넘긴다. 그러므로 같은 Append에 여러 프레임이 들어와도 추가
/// 할당 없이 순서대로 처리한다.
/// </remarks>
class Reader
{
public:
    Reader(const std::span<std::byte> storage,
        const std::uint32_t maxBodySize = DefaultMaxBodySize) noexcept
        : mStorage(storage)
        , mMaxBodySize(maxBodySize)
    {
        const std::size_t required = RequiredStorageSize(maxBodySize);
        if (required == 0 || mStorage.size() < required)
        {
            mTerminalError = Error::InsufficientStorage;
        }
    }

    /// <summary>도착한 바이트를 보존하고, 완결된 본문을 callback으로 바로 준다.</summary>
    /// <remarks>
    /// bytes는 생성 때 넘긴 storage와 겹치지 않아야 한다. Reader는 미완성 바이트를 압축할 수
    /// 있으므로, 그 저장소를 입력으로 재사용하면 호출자가 기대한 입력 순서를 보장할 수 없다.
    /// </remarks>
    [[nodiscard]] AppendResult Append(
        std::span<const std::byte> bytes, void* const context, const FrameCallback callback) noexcept
    {
        if (mTerminalError != Error::None)
        {
            return AppendResult{ mTerminalError, 0 };
        }
        if (callback == nullptr)
        {
            return AppendResult{ Error::InvalidArgument, 0 };
        }
        if (mInvokingCallback)
        {
            mTerminalError = Error::InvalidFormat;
            return AppendResult{ mTerminalError, 0 };
        }

        std::size_t consumed = 0;
        for (;;)
        {
            const Error pumped = PumpCompletedFrames(context, callback);
            if (pumped != Error::None)
            {
                return AppendResult{ pumped, consumed };
            }

            if (bytes.empty())
            {
                return AppendResult{ Error::None, consumed };
            }

            if (mEnd == mStorage.size())
            {
                CompactUnreadBytes();
                if (mEnd == mStorage.size())
                {
                    mTerminalError = Error::InvalidFormat;
                    return AppendResult{ mTerminalError, consumed };
                }
            }

            const std::size_t writable = mStorage.size() - mEnd;
            const std::size_t count = writable < bytes.size() ? writable : bytes.size();
            std::memcpy(mStorage.data() + mEnd, bytes.data(), count);
            mEnd += count;
            consumed += count;
            bytes = bytes.subspan(count);
        }
    }

    /// <summary>스트림이 끝났을 때 남은 미완성 바이트가 있는지 판정한다.</summary>
    [[nodiscard]] Error Finish() const noexcept
    {
        if (mTerminalError != Error::None)
        {
            return mTerminalError;
        }
        return mStart == mEnd ? Error::None : Error::InvalidFormat;
    }

    [[nodiscard]] bool HasIncompleteFrame() const noexcept
    {
        return mStart != mEnd;
    }

private:
    [[nodiscard]] static constexpr std::size_t RequiredStorageSize(
        const std::uint32_t maxBodySize) noexcept
    {
        const std::size_t bodySize = static_cast<std::size_t>(maxBodySize);
        constexpr std::size_t MaximumSize = (std::numeric_limits<std::size_t>::max)();
        return bodySize > MaximumSize - HeaderSize ? 0 : HeaderSize + bodySize;
    }

    [[nodiscard]] std::uint32_t ReadLittleEndianLength() const noexcept
    {
        const std::byte* const bytes = mStorage.data() + mStart;
        return static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[0])) |
            (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[1])) << 8U) |
            (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[2])) << 16U) |
            (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[3])) << 24U);
    }

    // 길이 머리는 본문을 다 받기 전에 검증한다. 0바이트 프레임도 여기서는 완결 프레임이며,
    // 빈 JSON 본문인지 거절하는 책임은 프레이밍 위의 메시지 파서에 있다.
    [[nodiscard]] Error PumpCompletedFrames(
        void* const context, const FrameCallback callback) noexcept
    {
        while (mEnd - mStart >= HeaderSize)
        {
            const std::uint32_t bodyLength = ReadLittleEndianLength();
            if (bodyLength > mMaxBodySize)
            {
                mTerminalError = Error::TooLarge;
                return mTerminalError;
            }

            const std::size_t frameSize = HeaderSize + static_cast<std::size_t>(bodyLength);
            if (mEnd - mStart < frameSize)
            {
                return Error::None;
            }

            mInvokingCallback = true;
            callback(context, std::span<const std::byte>(
                                  mStorage.data() + mStart + HeaderSize, bodyLength));
            mInvokingCallback = false;
            if (mTerminalError != Error::None)
            {
                return mTerminalError;
            }

            mStart += frameSize;
        }

        return Error::None;
    }

    void CompactUnreadBytes() noexcept
    {
        if (mStart == 0)
        {
            return;
        }

        const std::size_t unread = mEnd - mStart;
        if (unread != 0)
        {
            std::memmove(mStorage.data(), mStorage.data() + mStart, unread);
        }
        mStart = 0;
        mEnd = unread;
    }

    std::span<std::byte> mStorage;
    std::uint32_t mMaxBodySize;
    std::size_t mStart = 0;
    std::size_t mEnd = 0;
    Error mTerminalError = Error::None;
    bool mInvokingCallback = false;
};

/// <summary>본문 앞에 리틀 엔디언 길이 머리를 써 넣는다.</summary>
[[nodiscard]] inline EncodeResult EncodeTo(const std::span<const std::byte> body,
    const std::span<std::byte> output,
    const std::uint32_t maxBodySize = DefaultMaxBodySize) noexcept
{
    if (body.size() > static_cast<std::size_t>(maxBodySize))
    {
        return EncodeResult{ Error::TooLarge, 0, 0 };
    }

    constexpr std::size_t MaximumSize = (std::numeric_limits<std::size_t>::max)();
    if (body.size() > MaximumSize - HeaderSize)
    {
        return EncodeResult{ Error::TooLarge, 0, 0 };
    }

    const std::size_t required = HeaderSize + body.size();
    if (output.size() < required)
    {
        return EncodeResult{ Error::InsufficientStorage, required, 0 };
    }

    const std::uint32_t length = static_cast<std::uint32_t>(body.size());
    // body가 output의 일부를 가리킬 수 있다. 특히 호출자가 같은 저장소의 뒷부분에 본문을
    // 조립해 두고 머리를 붙이는 경우가 그렇다. 머리를 먼저 쓰면 그 본문을 덮을 수 있고,
    // memcpy는 겹치는 범위에서 정의되지 않는다. 본문을 memmove로 제자리까지 먼저 옮긴 뒤
    // 머리를 쓰면 겹침 여부와 관계없이 원래 본문을 보존한다.
    if (!body.empty())
    {
        std::memmove(output.data() + HeaderSize, body.data(), body.size());
    }
    output[0] = static_cast<std::byte>(length & 0xFFU);
    output[1] = static_cast<std::byte>((length >> 8U) & 0xFFU);
    output[2] = static_cast<std::byte>((length >> 16U) & 0xFFU);
    output[3] = static_cast<std::byte>((length >> 24U) & 0xFFU);
    return EncodeResult{ Error::None, required, required };
}
}
