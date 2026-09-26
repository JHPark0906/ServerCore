#pragma once
#include "ServerCore/Export.h"

#include "ServerCore/Net/Connection.h"
#include "ServerCore/Web/HttpServer.h"
#include "Web/ResponseEncoding.h"

#include <atomic>
#include <mutex>

namespace ServerCore::Web::Detail
{
class RequestObservation;
SERVERCORE_TEST_API std::int64_t WebNow() noexcept;

/// <summary>정책이 준 헤더를 응답 헤더에 더한다.</summary>
/// <remarks>
/// 정책 헤더는 기본값이라 같은 이름이 응답에 있으면 뺀다. 다만 목록인 Vary와 줄마다 따로인
/// Set-Cookie는 응답의 값과 함께 보낸다. Vary: Origin이 빠지면 공유 캐시가 다른 origin에 같은 응답을
/// 준다(WEB-7, Web.PolicyHeadersKeepListFields). 같은 값의 Vary 줄은 두 번 넣지 않는다.
/// </remarks>
void MergePolicyHeaders(HttpHeaders& headers, const HttpHeaders& policy);

// Shared by HTTP and WebSocket sends on one transport. Monotonic admission
// accounting lets the maintenance thread distinguish draining from refilling.
class TrackedSender
{
public:
    explicit TrackedSender(std::shared_ptr<Net::Connection> value)
        : connection(std::move(value))
        , flow(Net::GetConnectionFlowControl(connection))
    {
    }
    SERVERCORE_TEST_API Core::Status Send(std::span<const std::byte> bytes);
    const std::shared_ptr<Net::Connection> connection;
    const std::shared_ptr<Net::ConnectionFlowControl> flow;
    std::atomic<std::uint64_t> admitted{ 0 };
    std::atomic<std::int64_t> lastWrite{ 0 };
};

class HttpResponseState final : public HttpResponseWriter
{
public:
    /// <param name="closeDelimited">
    /// 참이면(HTTP/1.0 요청) 길이 모르는 본문을 청크 대신 연결 종료로 끝내고, close 인자와 관계없이
    /// 응답 뒤에 연결을 닫는다. 생성자가 이 값을 close에 합친다.
    /// </param>
    /// <param name="finished">
    /// 응답이 끝까지 큐에 들고 IsFinished가 참이 된 직후, 잠금 밖에서 한 번 부른다. 비어 있어도 된다.
    /// 뒤에 줄 선 요청과 멈춘 수신을 다시 진행시킨다(WEB-5).
    /// </param>
    SERVERCORE_TEST_API HttpResponseState(std::shared_ptr<TrackedSender> sender,
        const HttpServerOptions& options, bool head, bool close, bool connect,
        const void* callbackOwner, std::shared_ptr<void> reservation,
        std::shared_ptr<RequestObservation> observation = {}, HttpHeaders policyHeaders = {},
        bool closeDelimited = false, std::function<void()> finished = {});
    SERVERCORE_TEST_API Core::Status Start(const HttpResponseHead& head) override;
    SERVERCORE_TEST_API Core::Status Write(std::span<const std::byte> bytes) override;
    SERVERCORE_TEST_API Core::Status Finish() override;
    SERVERCORE_TEST_API Core::Status Complete(const HttpResponse& response) override;
    SERVERCORE_TEST_API void Abort() noexcept override;
    SERVERCORE_TEST_API std::stop_token GetCancellationToken() const noexcept override;
    bool IsHeadRequest() const noexcept override { return mHead; }
    SERVERCORE_TEST_API std::size_t MaxWriteBytes() const noexcept override;
    SERVERCORE_TEST_API std::size_t RetainedSendBytes() const noexcept override;
    SERVERCORE_TEST_API Core::Result<Net::SendCapacitySubscription> WaitForWriteCapacity(
        std::size_t bytes, std::function<void(Core::Status)> callback) override;
    SERVERCORE_TEST_API void Cancel(Core::ErrorCode reason = Core::ErrorCode::Cancelled) noexcept;
    SERVERCORE_TEST_API bool IsFinished() const noexcept;
    SERVERCORE_TEST_API bool IsAborted() const noexcept;
    SERVERCORE_TEST_API bool ShouldClose() const noexcept;
    SERVERCORE_TEST_API bool HasStarted() const noexcept;
    /// <param name="handlerDeadline">
    /// 거짓이면 머리를 보내기 전의 핸들러 기한을 보지 않는다. 스트리밍 업로드의 본문을 받는 동안에 쓴다(WEB-10).
    /// </param>
    SERVERCORE_TEST_API bool TimedOut(std::int64_t now, bool handlerDeadline = true) const noexcept;
    /// <summary>핸들러 기한을 지금부터 다시 센다. 스트리밍 업로드의 본문이 끝났을 때 부른다.</summary>
    SERVERCORE_TEST_API void RestartHandlerDeadline() noexcept;

private:
    enum class Phase
    {
        New,
        Streaming,
        Finished,
        Aborted
    };
    SERVERCORE_TEST_API bool MarkCancelled() noexcept;
    SERVERCORE_TEST_API void Report(Core::ErrorCode reason) noexcept;
    SERVERCORE_TEST_API void AbortWithReason(Core::ErrorCode reason) noexcept;
    SERVERCORE_TEST_API Core::Status SendTransaction(std::string wire, Phase next,
        ResponseBodyMode mode, std::uint64_t length, std::uint64_t written, bool close,
        unsigned status);
    const std::shared_ptr<TrackedSender> mSender;
    const HttpServerOptions mOptions;
    const bool mHead;
    const bool mRequestClose;
    const bool mConnect;
    const bool mCloseDelimited;
    const void* const mCallbackOwner;
    const std::int64_t mCreated;
    std::stop_source mCancellation;
    std::stop_source mWaitCancellation;
    mutable std::mutex mMutex;
    Phase mPhase = Phase::New;
    ResponseBodyMode mMode = ResponseBodyMode::None;
    std::uint64_t mLength = 0;
    std::uint64_t mWritten = 0;
    std::int64_t mLastWrite;
    std::int64_t mHandlerStarted;
    bool mClose = false;
    bool mSending = false;
    bool mWaitsRetired = false;
    std::size_t mBlockedWireBytes = 0;
    std::shared_ptr<void> mReservation;
    const std::shared_ptr<RequestObservation> mObservation;
    const HttpHeaders mPolicyHeaders;
    const std::function<void()> mFinished;
    unsigned mStatus = 0;
};
}
