/* Includes every public C ABI header in one translation unit. CMakeLists.txt
 * compiles it once per C standard; under GCC and Clang with -pedantic-errors
 * this rejects C11-only constructs such as a repeated typedef. */
#include <ServerCore/C/BinaryIO.h>
#include <ServerCore/C/Channel.h>
#include <ServerCore/C/Datagram.h>
#include <ServerCore/C/Endpoint.h>
#include <ServerCore/C/Files.h>
#include <ServerCore/C/GameExecution.h>
#include <ServerCore/C/Net.h>
#include <ServerCore/C/Observability.h>
#include <ServerCore/C/RequestLimiter.h>
#include <ServerCore/C/Runtime.h>
#include <ServerCore/C/Types.h>
#include <ServerCore/C/Web.h>
#include <ServerCore/C/WebData.h>
#include <ServerCore/C/WebPolicy.h>
