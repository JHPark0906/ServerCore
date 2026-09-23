#include "ServerCore/Core/Endpoint.h"
#include <algorithm>
#include <charconv>
#include <limits>

namespace ServerCore::Core
{
namespace
{
template<class T> bool Decimal(std::string_view text,T& result) noexcept
{
    if (text.empty()) return false;
    for (const char value:text) if (value<'0'||value>'9') return false;
    const auto parsed=std::from_chars(text.data(),text.data()+text.size(),result);
    return parsed.ec==std::errc{} && parsed.ptr==text.data()+text.size();
}
bool V4(std::string_view text,std::uint8_t* bytes) noexcept
{
    for (std::size_t index=0;index<4;++index)
    {
        const auto dot=text.find('.');
        const auto part=text.substr(0,dot);
        unsigned value=0;
        if (part.size()>3 || (part.size()>1&&part.front()=='0') || !Decimal(part,value) || value>255)
            return false;
        bytes[index]=static_cast<std::uint8_t>(value);
        if (index==3) return dot==std::string_view::npos;
        if (dot==std::string_view::npos) return false;
        text.remove_prefix(dot+1);
    }
    return false;
}
bool Words(std::string_view text,bool allowV4,std::array<std::uint16_t,8>& words,std::size_t& count) noexcept
{
    if (text.empty()) return true;
    while (!text.empty())
    {
        const auto colon=text.find(':');
        const auto part=text.substr(0,colon);
        if (part.empty()||count==8) return false;
        if (part.find('.')!=std::string_view::npos)
        {
            std::array<std::uint8_t,4> bytes{};
            if (!allowV4 || colon!=std::string_view::npos || count>6 || !V4(part,bytes.data())) return false;
            words[count++]=static_cast<std::uint16_t>((unsigned(bytes[0])<<8)|bytes[1]);
            words[count++]=static_cast<std::uint16_t>((unsigned(bytes[2])<<8)|bytes[3]);
            return true;
        }
        if (part.size()>4) return false;
        unsigned value=0;
        for (const char digit:part)
        {
            unsigned hex;
            if (digit>='0'&&digit<='9') hex=static_cast<unsigned>(digit-'0');
            else if (digit>='a'&&digit<='f') hex=static_cast<unsigned>(digit-'a'+10);
            else if (digit>='A'&&digit<='F') hex=static_cast<unsigned>(digit-'A'+10);
            else return false;
            value=(value<<4)|hex;
        }
        words[count++]=static_cast<std::uint16_t>(value);
        if (colon==std::string_view::npos) return true;
        text.remove_prefix(colon+1);
        if (text.empty()) return false;
    }
    return true;
}
template<class T> Result<T> Invalid() noexcept
{ return Result<T>::FromStatus(Status::FailWithoutMessage(ErrorCode::InvalidArgument)); }
}
Result<IpAddress> IpAddress::FromBytes(IpFamily family,std::span<const std::uint8_t> bytes,std::uint32_t scope) noexcept
{
    if ((family!=IpFamily::V4&&family!=IpFamily::V6) ||
        bytes.size()!=(family==IpFamily::V4?4u:16u) || (family==IpFamily::V4&&scope!=0)) return Invalid<IpAddress>();
    IpAddress value;
    value.mFamily=family; value.mScopeId=scope;
    std::copy(bytes.begin(),bytes.end(),value.mBytes.begin());
    return Result<IpAddress>::FromValue(value);
}
Result<IpAddress> IpAddress::Parse(std::string_view text) noexcept
{
    if (text.empty() || text.size()>64) return Invalid<IpAddress>();
    IpAddress value;
    if (text.find(':')==std::string_view::npos)
    {
        value.mFamily=IpFamily::V4;
        if (!V4(text,value.mBytes.data())) return Invalid<IpAddress>();
        return Result<IpAddress>::FromValue(value);
    }
    value.mFamily=IpFamily::V6;
    const auto zone=text.find('%');
    if (zone!=std::string_view::npos)
    {
        if (!Decimal(text.substr(zone+1),value.mScopeId)) return Invalid<IpAddress>();
        text=text.substr(0,zone);
    }
    std::array<std::uint16_t,8> left{},right{};
    std::size_t leftCount=0,rightCount=0;
    const auto compressed=text.find("::");
    if (compressed==std::string_view::npos)
    {
        if (!Words(text,true,left,leftCount)||leftCount!=8) return Invalid<IpAddress>();
    }
    else
    {
        if (text.find("::",compressed+2)!=std::string_view::npos ||
            !Words(text.substr(0,compressed),false,left,leftCount) ||
            !Words(text.substr(compressed+2),true,right,rightCount) || leftCount+rightCount>=8)
            return Invalid<IpAddress>();
        std::copy_n(right.begin(),rightCount,left.begin()+static_cast<std::ptrdiff_t>(8-rightCount));
    }
    for (std::size_t index=0;index<8;++index)
    {
        value.mBytes[index*2]=static_cast<std::uint8_t>(left[index]>>8);
        value.mBytes[index*2+1]=static_cast<std::uint8_t>(left[index]&255);
    }
    return Result<IpAddress>::FromValue(value);
}
bool IpAddress::IsV4Mapped() const noexcept
{
    if (mFamily!=IpFamily::V6) return false;
    for (std::size_t index=0;index<10;++index) if (mBytes[index]!=0) return false;
    return mBytes[10]==255&&mBytes[11]==255;
}
IpAddress IpAddress::Normalized() const noexcept
{
    if (!IsV4Mapped() || mScopeId!=0) return *this;
    return FromBytes(IpFamily::V4,{mBytes.data()+12,4}).Value();
}
std::string IpAddress::ToString() const
{
    if (mFamily==IpFamily::Unspecified) return {};
    if (mFamily==IpFamily::V4)
        return std::to_string(mBytes[0])+"."+std::to_string(mBytes[1])+"."+
            std::to_string(mBytes[2])+"."+std::to_string(mBytes[3]);
    std::array<std::uint16_t,8> words{};
    for (std::size_t index=0;index<8;++index) words[index]=static_cast<std::uint16_t>(
        (unsigned(mBytes[index*2])<<8)|mBytes[index*2+1]);
    std::size_t best=8,length=0;
    for (std::size_t index=0;index<8;)
    {
        if (words[index]!=0) { ++index; continue; }
        const auto start=index;
        while (index<8&&words[index]==0) ++index;
        if (index-start>=2&&index-start>length) { best=start; length=index-start; }
    }
    std::string result;
    for (std::size_t index=0;index<8;)
    {
        if (index==best) { result+="::"; index+=length; continue; }
        if (!result.empty()&&result.back()!=':') result+=':';
        std::array<char,4> digits{};
        const auto converted=std::to_chars(digits.data(),digits.data()+digits.size(),words[index++],16);
        result.append(digits.data(),converted.ptr);
    }
    if (mScopeId!=0) { result+='%'; result+=std::to_string(mScopeId); }
    return result;
}
Result<IpEndpoint> IpEndpoint::Parse(std::string_view text,std::uint16_t port) noexcept
{
    auto address=IpAddress::Parse(text);
    if (!address.IsOk()) return Result<IpEndpoint>::FromStatus(std::move(address).TakeStatus());
    return Result<IpEndpoint>::FromValue({address.Value(),port});
}
std::string IpEndpoint::ToString() const
{
    if (!address.IsValid()) return {};
    return (address.Family()==IpFamily::V6 ? "["+address.ToString()+"]" : address.ToString())+
        ":"+std::to_string(port);
}
Result<IpNetwork> IpNetwork::Parse(std::string_view text) noexcept
{
    const auto slash=text.find('/');
    if (slash==std::string_view::npos || text.find('%')!=std::string_view::npos) return Invalid<IpNetwork>();
    auto address=IpAddress::Parse(text.substr(0,slash));
    unsigned prefix=0;
    if (!address.IsOk() || !Decimal(text.substr(slash+1),prefix) ||
        prefix>(address.Value().Family()==IpFamily::V4?32u:128u)) return Invalid<IpNetwork>();
    IpNetwork value;
    value.mAddress=address.Value(); value.mPrefix=static_cast<std::uint8_t>(prefix);
    for (std::size_t index=0;index<value.mAddress.Bytes().size();++index)
    {
        const unsigned remaining=prefix>index*8 ? prefix-static_cast<unsigned>(index*8) : 0;
        const auto mask=remaining>=8 ? 255u : remaining==0 ? 0u : (255u<<(8-remaining))&255u;
        value.mAddress.mBytes[index]&=static_cast<std::uint8_t>(mask);
    }
    return Result<IpNetwork>::FromValue(value);
}
bool IpNetwork::Contains(const IpAddress& address) const noexcept
{
    if (!mAddress.IsValid() || address.Family()!=mAddress.Family()) return false;
    for (std::size_t index=0;index<address.Bytes().size();++index)
    {
        const unsigned remaining=mPrefix>index*8 ? mPrefix-static_cast<unsigned>(index*8) : 0;
        const auto mask=remaining>=8 ? 255u : remaining==0 ? 0u : (255u<<(8-remaining))&255u;
        if ((address.mBytes[index]&mask)!=(mAddress.mBytes[index]&mask)) return false;
    }
    return true;
}
}
