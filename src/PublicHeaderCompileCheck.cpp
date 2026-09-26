/// <summary>
/// 공개 헤더 전부를 한 번역 단위에 함께 포함한다. 다른 일은 하지 않는다.
/// </summary>
/// <remarks>
/// 왜 이 파일이 있는가:
/// 헤더는 아무도 포함하지 않으면 컴파일되지 않는다. 이 파일은 라이브러리 target과 같은 경고
/// 설정(ServerCoreConfigureLibrary)으로 공개 헤더 전부를 함께 컴파일하므로, 모든 빌드에서
/// "공개 헤더가 함께 포함되어도 경고 없이 컴파일된다"의 증거가 된다.
///
/// 이 파일이 증명하지 않는 것: 헤더 하나하나가 스스로 필요한 것을 모두 포함하는지(자기완결).
/// 한 번역 단위 안에서는 뒤의 헤더가 앞에서 들어온 선언에 기대도 통과한다. 자기완결은 시험을
/// 빌드할 때 tests/CMakeLists.txt가 헤더마다 그 헤더 하나만 포함하는 번역 단위를 만들어
/// 컴파일하고, ServerCore.Architecture.PublicHeadersCompile이 그 번역 단위가 헤더마다 실제로
/// 컴파일되었는지 확인한다.
///
/// 새 공개 헤더를 추가하면 여기에도 한 줄을 추가한다. 빠뜨리면 같은 시험이 그 헤더 이름을 적어
/// 실패한다.
/// </remarks>

#include "ServerCore/C/BinaryIO.h"
#include "ServerCore/C/Channel.h"
#include "ServerCore/C/Datagram.h"
#include "ServerCore/C/Endpoint.h"
#include "ServerCore/C/Files.h"
#include "ServerCore/C/GameExecution.h"
#include "ServerCore/C/Net.h"
#include "ServerCore/C/Observability.h"
#include "ServerCore/C/RequestLimiter.h"
#include "ServerCore/C/Runtime.h"
#include "ServerCore/C/Types.h"
#include "ServerCore/C/Web.h"
#include "ServerCore/C/WebData.h"
#include "ServerCore/C/WebPolicy.h"
#include "ServerCore/Core/Assert.h"
#include "ServerCore/Core/AtomicFile.h"
#include "ServerCore/Core/ByteBuffer.h"
#include "ServerCore/Core/Clock.h"
#include "ServerCore/Core/CompletionSubscription.h"
#include "ServerCore/Core/Config.h"
#include "ServerCore/Core/Endpoint.h"
#include "ServerCore/Core/Error.h"
#include "ServerCore/Core/JobQueue.h"
#include "ServerCore/Core/Logging.h"
#include "ServerCore/Core/Version.h"
#include "ServerCore/Dispatch/Dispatcher.h"
#include "ServerCore/Export.h"
#include "ServerCore/Net/Acceptor.h"
#include "ServerCore/Net/Connection.h"
#include "ServerCore/Net/ConnectionFlowControl.h"
#include "ServerCore/Net/IoContext.h"
#include "ServerCore/Observability/AsyncLogger.h"
#include "ServerCore/Observability/Metrics.h"
#include "ServerCore/Observability/Observation.h"
#include "ServerCore/Observability/Prometheus.h"
#include "ServerCore/Observability/RequestTrace.h"
#include "ServerCore/Observability/ServerObservation.h"
#include "ServerCore/Protocol/BinaryIO.h"
#include "ServerCore/Protocol/BinaryMessage.h"
#include "ServerCore/Protocol/DatagramCodec.h"
#include "ServerCore/Protocol/FrameCodec.h"
#include "ServerCore/Protocol/Framing.h"
#include "ServerCore/Protocol/Json.h"
#include "ServerCore/Protocol/Message.h"
#include "ServerCore/Runtime/Channel.h"
#include "ServerCore/Runtime/DatagramTransport.h"
#include "ServerCore/Runtime/JobRunner.h"
#include "ServerCore/Runtime/KeyedExecutor.h"
#include "ServerCore/Runtime/Metrics.h"
#include "ServerCore/Runtime/OutboundQueue.h"
#include "ServerCore/Runtime/PeriodicRunner.h"
#include "ServerCore/Runtime/RequestLimiter.h"
#include "ServerCore/Runtime/ServerHost.h"
#include "ServerCore/Runtime/SessionTask.h"
#include "ServerCore/Runtime/TaskExecutor.h"
#include "ServerCore/Runtime/TaskGroup.h"
#include "ServerCore/Runtime/TickRunner.h"
#include "ServerCore/Runtime/TimerScheduler.h"
#include "ServerCore/Session/Session.h"
#include "ServerCore/Session/SessionRegistry.h"
#include "ServerCore/Web/HttpPolicy.h"
#include "ServerCore/Web/HttpServer.h"
#include "ServerCore/Web/HttpStreaming.h"
#include "ServerCore/Web/Multipart.h"
#include "ServerCore/Web/RequestData.h"
#include "ServerCore/Web/TrustedProxy.h"
