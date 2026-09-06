#include "ServerCore/Runtime/DatagramTransport.h"

#include "ServerCore/Core/Error.h"
#include "ServerCore/Core/Version.h"
#include "ServerCore/Net/Acceptor.h"
#include "ServerCore/Runtime/ServerHost.h"

#include <concepts>

static_assert(std::same_as<int, int>);

int main()
{
    ServerCore::Runtime::DatagramTransport datagrams;
    const ServerCore::Core::Status invalidDatagramEndpoint = datagrams.Bind("not-an-ipv4", 0);
    if (invalidDatagramEndpoint.Code() != ServerCore::Core::ErrorCode::InvalidArgument ||
        datagrams.Port() != 0)
    {
        return 1;
    }
    datagrams.Close();

    ServerCore::Net::Acceptor acceptor;
    const ServerCore::Core::Status invalidEndpoint = acceptor.Listen("not-an-ipv4", 1, 1);

    if (invalidEndpoint.Code() != ServerCore::Core::ErrorCode::InvalidArgument)
    {
        return 1;
    }

    return ServerCore::GetVersionString().empty() ? 1 : 0;
}
