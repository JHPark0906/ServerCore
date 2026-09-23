#include "ServerCore/Core/Endpoint.h"
#include "TestHarness.h"
#include <array>
namespace
{
using namespace ServerCore::Core;
using ServerCoreTest::ExpectTrue;
void NumericEndpointBoundaries()
{
    for (const auto text : { "0.0.0.0", "255.255.255.255", "127.0.0.1", "::", "::1",
             "2001:db8::12:abcd", "1:2:3:4:5:6:7:8", "::ffff:192.0.2.10", "fe80::1234%4294967295" })
    {
        auto parsed = IpAddress::Parse(text);
        ExpectTrue(parsed.IsOk(), "numeric endpoint accepts valid literal");
        if (!parsed.IsOk())
            continue;
        auto roundtrip = IpAddress::Parse(parsed.Value().ToString());
        ExpectTrue(roundtrip.IsOk() && roundtrip.Value() == parsed.Value(),
            "canonical address round trips bytes and scope");
    }
    for (const auto text :
        { "", "localhost", "[::1]", "127.1", "127.00.0.1", "256.0.0.1", "+1.2.3.4", "1.2.3.4.",
            "1:2:3", "1:2:3:4:5:6:7:8:9", "1:2:3:4:5:6:7:8::", ":::1", "1::2::3", "::1:", "::gg",
            "1.2.3.4::", "::1%eth0", "::1%4294967296", "::1%", "127.0.0.1%1", " ::1" })
        ExpectTrue(
            !IpAddress::Parse(text).IsOk(), "invalid, ambiguous and nonnumeric literals rejected");
    ExpectTrue(!IpAddress::Parse(std::string_view("::1\0x", 5)).IsOk(), "embedded NUL rejected");
    auto scoped = IpEndpoint::Parse("fe80::1%12", 443);
    ExpectTrue(scoped.IsOk() && scoped.Value().ToString() == "[fe80::1%12]:443",
        "endpoint brackets IPv6 and retains numeric scope");
    auto mapped = IpAddress::Parse("::ffff:192.0.2.10").Value();
    ExpectTrue(mapped.Family() == IpFamily::V6 && mapped.IsV4Mapped() &&
                   mapped.Normalized().ToString() == "192.0.2.10",
        "mapped normalization is explicit");
    ExpectTrue(
        IpAddress::Parse("::ffff:192.0.2.10%1").Value().Normalized().Family() == IpFamily::V6,
        "scoped mapped address keeps scope and family");
    std::array<std::uint8_t, 4> bytes{ 127, 0, 0, 1 };
    ExpectTrue(!IpAddress::FromBytes(IpFamily::V4, bytes, 1).IsOk() &&
                   !IpAddress::FromBytes(IpFamily::V6, bytes).IsOk(),
        "binary constructors validate family size and scope");
}
void CidrMasksAndFamilies()
{
    auto v4 = IpNetwork::Parse("192.0.2.123/24").Value();
    ExpectTrue(v4.Address().ToString() == "192.0.2.0" &&
                   v4.Contains(IpAddress::Parse("192.0.2.255").Value()),
        "CIDR masks host bits");
    ExpectTrue(!v4.Contains(IpAddress::Parse("192.0.3.0").Value()) &&
                   !v4.Contains(IpAddress::Parse("::ffff:192.0.2.1").Value()),
        "CIDR compares exact family and prefix");
    auto v6 = IpNetwork::Parse("2001:db8:abcd:1234::ff/65").Value();
    ExpectTrue(v6.Contains(IpAddress::Parse("2001:db8:abcd:1234:7fff::1").Value()) &&
                   !v6.Contains(IpAddress::Parse("2001:db8:abcd:1234:8000::1").Value()),
        "non-byte-aligned IPv6 prefix");
    ExpectTrue(IpNetwork::Parse("::/0").Value().Contains(IpAddress::Parse("ffff::1").Value()) &&
                   !IpNetwork::Parse("::/0").Value().Contains(IpAddress::Parse("1.2.3.4").Value()),
        "zero IPv6 prefix stays IPv6");
    ExpectTrue(
        IpNetwork::Parse("1.2.3.4/32").Value().Contains(IpAddress::Parse("1.2.3.4").Value()) &&
            !IpNetwork::Parse("1.2.3.4/32").Value().Contains(IpAddress::Parse("1.2.3.5").Value()),
        "full IPv4 prefix exact");
    ExpectTrue(IpNetwork::Parse("::1/128").Value().Contains(IpAddress::Parse("::1").Value()),
        "full IPv6 prefix exact");
    for (auto text : { "::1/129", "1.2.3.4/33", "1.2.3.4/-1", "1.2.3.4/+1", "1.2.3.4",
             "fe80::1%1/64", "::1/64/1" })
        ExpectTrue(!IpNetwork::Parse(text).IsOk(), "malformed CIDR rejected");
}
ServerCoreTest::CheckRegistration a("Core.NumericEndpointBoundaries", NumericEndpointBoundaries);
ServerCoreTest::CheckRegistration b("Core.CidrMasksAndFamilies", CidrMasksAndFamilies);
}
