//! Numeric peer/local addresses. IPv6 scope IDs and mapped IPv4 addresses are
//! preserved; this module performs no DNS or interface-name lookup.
use crate::{bytes, check, sys, Error, Result};
use std::{fmt, net::{IpAddr, Ipv4Addr, Ipv6Addr, SocketAddr, SocketAddrV6}};

#[derive(Clone, Copy, Debug, Eq, PartialEq, Hash)]
pub struct Endpoint {
    pub address: IpAddr,
    pub port: u16,
    pub scope_id: u32,
}
impl Endpoint {
    /// Numeric address only, including an optional decimal IPv6 `%scope` suffix.
    /// Port zero is valid as a value; TCP listener options still reject zero.
    pub fn parse(address: &str, port: u16) -> Result<Self> {
        let mut raw = sys::sc_ip_endpoint::default();
        check(unsafe { sys::sc_ip_endpoint_parse(bytes(address.as_bytes()), port, &mut raw) })?;
        Self::from_raw(raw).ok_or(Error::PLATFORM)
    }
    pub(crate) fn from_raw(raw: sys::sc_ip_endpoint) -> Option<Self> {
        if raw.reserved != 0 { return None; }
        let address = match raw.family {
            sys::SC_IP_V4 if raw.scope_id == 0 && raw.address[4..].iter().all(|&byte| byte == 0) =>
                IpAddr::V4(Ipv4Addr::new(raw.address[0], raw.address[1], raw.address[2], raw.address[3])),
            sys::SC_IP_V6 => IpAddr::V6(Ipv6Addr::from(raw.address)),
            _ => return None,
        };
        Some(Self { address, port: raw.port, scope_id: raw.scope_id })
    }
    pub(crate) fn to_raw(self) -> Result<sys::sc_ip_endpoint> {
        let mut raw = sys::sc_ip_endpoint { port: self.port, scope_id: self.scope_id, ..Default::default() };
        match self.address {
            IpAddr::V4(address) => {
                if self.scope_id != 0 { return Err(Error::INVALID_ARGUMENT); }
                raw.family = sys::SC_IP_V4;
                raw.address[..4].copy_from_slice(&address.octets());
            }
            IpAddr::V6(address) => { raw.family = sys::SC_IP_V6; raw.address = address.octets(); }
        }
        Ok(raw)
    }
    pub fn socket_addr(self) -> SocketAddr {
        match self.address {
            IpAddr::V4(address) => SocketAddr::from((address, self.port)),
            IpAddr::V6(address) => SocketAddr::V6(SocketAddrV6::new(address, self.port, 0, self.scope_id)),
        }
    }
    /// Explicitly normalize IPv4-mapped IPv6 only when unscoped, for policies
    /// which intentionally treat those addresses as the same peer family.
    pub fn normalized(self) -> Self {
        if self.scope_id == 0 {
            if let IpAddr::V6(ip) = self.address {
                if let Some(v4) = ip.to_ipv4_mapped() {
                    return Self { address: IpAddr::V4(v4), ..self };
                }
            }
        }
        self
    }
}
impl From<SocketAddr> for Endpoint {
    fn from(value: SocketAddr) -> Self {
        Self { address: value.ip(), port: value.port(), scope_id: match value {
            SocketAddr::V6(v6) => v6.scope_id(), SocketAddr::V4(_) => 0,
        } }
    }
}
impl fmt::Display for Endpoint {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result { self.socket_addr().fmt(formatter) }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn numeric_endpoint_scope_and_mapping() {
        let scoped = Endpoint::parse("fe80::1234%42", 9876).unwrap();
        assert_eq!(scoped.scope_id, 42);
        assert_eq!(scoped.to_string(), "[fe80::1234%42]:9876");
        assert_eq!(Endpoint::from_raw(scoped.to_raw().unwrap()), Some(scoped));
        assert_eq!(Endpoint { address: IpAddr::V4(Ipv4Addr::LOCALHOST), port: 1, scope_id: 7 }.to_raw().unwrap_err(), Error::INVALID_ARGUMENT);
        let mapped = Endpoint::parse("::ffff:127.0.0.1", 9).unwrap();
        assert!(mapped.address.is_ipv6());
        assert_eq!(mapped.normalized().to_string(), "127.0.0.1:9");
        for invalid in ["localhost", "127.00.0.1", "[::1]", "::1%eth0", "::1\0ignored"] {
            assert_eq!(Endpoint::parse(invalid, 1), Err(Error::INVALID_ARGUMENT));
        }
    }
}
