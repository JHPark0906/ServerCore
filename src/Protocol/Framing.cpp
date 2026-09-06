#include "ServerCore/Protocol/Framing.h"

#include "ServerCore/Core/Assert.h"

#include <exception>
#include <limits>
#include <new>
#include <utility>

namespace ServerCore::Protocol
{
namespace
{
[[nodiscard]] Core::Status ToStatus(const FrameCodec::Error error) noexcept
{
    try
    {
        switch (error)
        {
        case FrameCodec::Error::None:
            return Core::Status::Ok();
        case FrameCodec::Error::WouldBlock:
            return Core::Status::Fail(
                Core::ErrorCode::WouldBlock, "a complete frame is not available");
        case FrameCodec::Error::TooLarge:
            return Core::Status::Fail(
                Core::ErrorCode::TooLarge, "the frame body exceeds the configured maximum");
        case FrameCodec::Error::InvalidFormat:
            return Core::Status::Fail(
                Core::ErrorCode::InvalidFormat, "the frame stream is incomplete");
        case FrameCodec::Error::InvalidArgument:
            return Core::Status::Fail(
                Core::ErrorCode::InvalidArgument, "the frame codec was called incorrectly");
        case FrameCodec::Error::InsufficientStorage:
            return Core::Status::Fail(Core::ErrorCode::PlatformError,
                "the frame codec could not obtain its required storage");
        }

        return Core::Status::Fail(
            Core::ErrorCode::InvalidFormat, "the frame codec returned an unknown error");
    }
    catch (...)
    {
        return Core::Status::AllocationFailure();
    }
}

[[nodiscard]] Core::Status PlatformFailureFrom(const std::exception& error) noexcept
{
    try
    {
        return Core::Status::Fail(Core::ErrorCode::PlatformError, error.what());
    }
    catch (...)
    {
        return Core::Status::AllocationFailure();
    }
}
}

FrameReader::FrameReader(const std::uint32_t maxBodySize)
    : mMaxBodySize(maxBodySize)
{
}

Core::Status FrameReader::EnsureReader()
{
    if (mReader.has_value())
    {
        return Core::Status::Ok();
    }

    constexpr std::size_t MaximumSize = (std::numeric_limits<std::size_t>::max)();
    const std::size_t bodySize = static_cast<std::size_t>(mMaxBodySize);
    if (bodySize > MaximumSize - HeaderSize)
    {
        return Core::Status::Fail(Core::ErrorCode::TooLarge,
            "the frame body limit cannot fit in the platform address space");
    }

    try
    {
        mStorage.resize(HeaderSize + bodySize);
        mReader.emplace(std::span<std::byte>(mStorage), mMaxBodySize);
    }
    catch (const std::bad_alloc&)
    {
        return Core::Status::AllocationFailure();
    }
    catch (const std::exception& error)
    {
        try
        {
            return Core::Status::Fail(Core::ErrorCode::PlatformError, error.what());
        }
        catch (...)
        {
            return Core::Status::AllocationFailure();
        }
    }

    return ToStatus(mReader->Finish());
}

void FrameReader::CopyCompletedFrame(
    void* const context, const std::span<const std::byte> body) noexcept
{
    FrameReader* const reader = static_cast<FrameReader*>(context);
    if (reader == nullptr || reader->mCallbackAllocationFailed ||
        reader->mCallbackAdmissionRejected)
    {
        return;
    }

    if (body.size() > (std::numeric_limits<std::size_t>::max)() - reader->mCompletedBodyBytes)
    {
        // 이 경우를 실제로 만들려면 완료 body들의 실제 할당 합도 size_t를 넘겨야 한다. 그래도
        // 관측 counter가 조용히 되돌아가는 것보다 이 프레임을 실패로 끝내는 편이 낫다.
        reader->mCallbackAllocationFailed = true;
        return;
    }

    if (reader->mAppendAdmission != nullptr &&
        !reader->mAppendAdmission(reader->mAppendAdmissionContext, body.size()))
    {
        // 같은 Append의 뒤 frame도 복사하지 않는다. FrameCodec은 나머지 흐름을 소비할 수 있지만,
        // Host는 Append 직후 연결을 닫으므로 그 바이트를 보관할 이유가 없다.
        reader->mCallbackAdmissionRejected = true;
        return;
    }

    try
    {
        reader->mCompleted.emplace_back(body.begin(), body.end());
        reader->mCompletedBodyBytes += body.size();
    }
    catch (...)
    {
        // FrameCodec의 callback은 예외를 내보낼 수 없다. 이 연결은 Append가 돌아온 직후
        // PlatformError로 끝내므로, 여기서는 실패 표식만 남기면 된다.
        reader->mCallbackAllocationFailed = true;
    }
}

Core::Status FrameReader::Append(const std::span<const std::byte> bytes)
{
    return Append(bytes, nullptr, nullptr);
}

Core::Status FrameReader::Append(const std::span<const std::byte> bytes,
    void* const admissionContext, const CompletedFrameAdmission admission)
{
    if (mAdmissionTerminal)
    {
        return ToStatus(FrameCodec::Error::TooLarge);
    }

    Core::Status prepared = EnsureReader();
    if (!prepared.IsOk())
    {
        return std::move(prepared);
    }

    mCallbackAllocationFailed = false;
    mCallbackAdmissionRejected = false;
    mAppendAdmissionContext = admissionContext;
    mAppendAdmission = admission;
    const FrameCodec::AppendResult appended =
        mReader->Append(bytes, this, &FrameReader::CopyCompletedFrame);
    mAppendAdmission = nullptr;
    mAppendAdmissionContext = nullptr;
    if (mCallbackAdmissionRejected)
    {
        mAdmissionTerminal = true;
        return ToStatus(FrameCodec::Error::TooLarge);
    }
    if (mCallbackAllocationFailed)
    {
        return Core::Status::AllocationFailure();
    }
    return ToStatus(appended.error);
}

Core::Result<std::span<const std::byte>> FrameReader::NextFrame()
{
    mCurrent.clear();
    if (mCompleted.empty())
    {
        return Core::Result<std::span<const std::byte>>::FromStatus(
            Core::Status::FailWithoutMessage(Core::ErrorCode::WouldBlock));
    }

    const std::size_t bodySize = mCompleted.front().size();
    mCurrent = std::move(mCompleted.front());
    mCompleted.pop_front();
    SERVERCORE_ASSERT(mCompletedBodyBytes >= bodySize,
        "a FrameReader completed-body counter underflowed while returning a span");
    mCompletedBodyBytes -= bodySize;
    return Core::Result<std::span<const std::byte>>::FromValue(
        std::span<const std::byte>(mCurrent.data(), mCurrent.size()));
}

Core::Result<std::vector<std::byte>> FrameReader::TakeNextFrame()
{
    // span 기반 NextFrame()이 내준 이전 본문도 이 호출부터는 더는 유효하다고 약속하지 않는다.
    mCurrent.clear();
    if (mCompleted.empty())
    {
        return Core::Result<std::vector<std::byte>>::FromStatus(
            Core::Status::FailWithoutMessage(Core::ErrorCode::WouldBlock));
    }

    std::vector<std::byte> body = std::move(mCompleted.front());
    const std::size_t bodySize = body.size();
    mCompleted.pop_front();
    SERVERCORE_ASSERT(mCompletedBodyBytes >= bodySize,
        "a FrameReader completed-body counter underflowed while transferring a body");
    mCompletedBodyBytes -= bodySize;
    return Core::Result<std::vector<std::byte>>::FromValue(std::move(body));
}

std::size_t FrameReader::CompletedFrameCount() const noexcept
{
    return mCompleted.size();
}

std::size_t FrameReader::CompletedBodyBytes() const noexcept
{
    return mCompletedBodyBytes;
}

void FrameReader::DiscardCompletedFrames() noexcept
{
    // vector 원소의 소멸로 각 body 저장소는 바로 반납한다. deque의 작은 내부 map은 남을 수 있지만,
    // 그것은 body-byte 예산의 대상이 아니며 다음 수신에서 다시 쓸 수 있다.
    mCompleted.clear();
    mCompletedBodyBytes = 0;
}

void FrameReader::ReleaseStorage() noexcept
{
    // Reader가 mStorage span을 먼저 놓은 뒤 backing vector를 해제한다.
    mReader.reset();
    DiscardCompletedFrames();
    std::vector<std::byte>().swap(mCurrent);
    std::vector<std::byte>().swap(mStorage);
    mAppendAdmissionContext = nullptr;
    mAppendAdmission = nullptr;
    mCallbackAllocationFailed = false;
    mCallbackAdmissionRejected = false;
    mAdmissionTerminal = false;
}

Core::Status FrameReader::Finish() const
{
    if (mAdmissionTerminal)
    {
        return ToStatus(FrameCodec::Error::TooLarge);
    }
    return mReader.has_value() ? ToStatus(mReader->Finish()) : Core::Status::Ok();
}

bool FrameReader::HasIncompleteFrame() const noexcept
{
    return mReader.has_value() && mReader->HasIncompleteFrame();
}

Core::Result<std::vector<std::byte>> EncodeFrame(
    const std::span<const std::byte> jsonBody, const std::uint32_t maxBodySize)
{
    if (jsonBody.size() > static_cast<std::size_t>(maxBodySize))
    {
        return Core::Result<std::vector<std::byte>>::FromStatus(
            ToStatus(FrameCodec::Error::TooLarge));
    }

    constexpr std::size_t MaximumSize = (std::numeric_limits<std::size_t>::max)();
    if (jsonBody.size() > MaximumSize - HeaderSize)
    {
        return Core::Result<std::vector<std::byte>>::FromStatus(
            ToStatus(FrameCodec::Error::TooLarge));
    }

    try
    {
        std::vector<std::byte> framed(HeaderSize + jsonBody.size());
        const FrameCodec::EncodeResult encoded =
            FrameCodec::EncodeTo(jsonBody, framed, maxBodySize);
        if (!encoded.IsOk())
        {
            return Core::Result<std::vector<std::byte>>::FromStatus(ToStatus(encoded.error));
        }
        return Core::Result<std::vector<std::byte>>::FromValue(std::move(framed));
    }
    catch (const std::bad_alloc&)
    {
        return Core::Result<std::vector<std::byte>>::FromStatus(Core::Status::AllocationFailure());
    }
    catch (const std::exception& error)
    {
        return Core::Result<std::vector<std::byte>>::FromStatus(PlatformFailureFrom(error));
    }
}
}
