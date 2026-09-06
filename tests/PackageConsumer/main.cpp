#include "ServerCore/Core/Error.h"
#include "ServerCore/Core/Version.h"
#include "ServerCore/Net/Acceptor.h"
#include "ServerCore/Runtime/ServerHost.h"

#include <concepts>

static_assert(std::same_as<int, int>);

int main()
{
    ServerCore::Net::Acceptor acceptor;
    const ServerCore::Core::Status invalidEndpoint = acceptor.Listen("not-an-ipv4", 1, 1);

    if (invalidEndpoint.Code() != ServerCore::Core::ErrorCode::InvalidArgument)
    {
        return 1;
    }

    return ServerCore::GetVersionString().empty() ? 1 : 0;
}
