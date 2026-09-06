#include "ServerCore/Core/Version.h"

#include <iostream>

/// <summary>
/// ServerCore를 실제로 링크해서 호출하는 최소 실행 파일이다.
/// 라이브러리가 시험 안에서만 쓰이는 상태가 되지 않도록, 프로덕션 쪽 호출 지점을 하나 둔다.
/// </summary>
int main()
{
    std::cout << "ServerCore " << ServerCore::GetVersionString() << std::endl;
    return 0;
}
