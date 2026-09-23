#include "ServerCore/Core/Version.h"

int main()
{
    // Calling an out-of-line API verifies that the positive case imports and
    // loads the supplied native DLL, rather than only compiling its headers.
    return ServerCore::GetVersionString().empty() ? 1 : 0;
}
