#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include "../LANScreenCast/casting/LocalIPv6.h"
#include <iostream>
#include <stdexcept>

namespace {
using LANScreenCast::casting::SelectLocalIPv6;

void Require(bool condition, char const* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Unicast {
    SOCKADDR_IN6 socket{};
    IP_ADAPTER_UNICAST_ADDRESS entry{};

    Unicast(wchar_t const* text, ULONG lifetime = 100) {
        socket.sin6_family = AF_INET6;
        Require(InetPtonW(AF_INET6, text, &socket.sin6_addr) == 1, "Invalid test address");
        entry.Address = {reinterpret_cast<SOCKADDR*>(&socket), sizeof(socket)};
        entry.DadState = IpDadStatePreferred;
        entry.PreferredLifetime = entry.ValidLifetime = lifetime;
    }
};
}

int main() {
    try {
        IP_ADAPTER_ADDRESSES wired{}, wireless{};
        wired.OperStatus = wireless.OperStatus = IfOperStatusUp;
        wired.IfType = IF_TYPE_ETHERNET_CSMACD;
        wireless.IfType = IF_TYPE_IEEE80211;
        wired.Ipv6Metric = 20;
        wireless.Ipv6Metric = 30;
        wired.Next = &wireless;

        Unicast local(L"fe80::1234"), privateAddress(L"fd00::3"),
            wiredGlobal(L"2400::42"), temporary(L"2400::86", 50), wirelessGlobal(L"2401::99");
        local.entry.Next = &privateAddress.entry;
        wired.FirstUnicastAddress = &local.entry;
        Require(!SelectLocalIPv6(&wired), "Local-scope addresses must not be offered");
        privateAddress.entry.Next = &temporary.entry;
        temporary.entry.Next = &wiredGlobal.entry;
        Require(SelectLocalIPv6(&wired) == L"2400::42", "Prefer the longer-lived address on one interface");

        wireless.FirstUnicastAddress = &wirelessGlobal.entry;
        Require(SelectLocalIPv6(&wired) == L"2400::42", "Prefer the lower metric between equivalent interfaces");
        IP_ADAPTER_GATEWAY_ADDRESS_LH gateway{};
        gateway.Address = local.entry.Address;
        wireless.FirstGatewayAddress = &gateway;
        Require(SelectLocalIPv6(&wired) == L"2401::99", "Prefer an IPv6 gateway over a lower interface metric");
        wireless.OperStatus = IfOperStatusDown;
        Require(SelectLocalIPv6(&wired) == L"2400::42", "Skip disconnected interfaces");
        wiredGlobal.entry.DadState = IpDadStateDeprecated;
        temporary.entry.PreferredLifetime = 0;
        Require(!SelectLocalIPv6(&wired), "Deprecated and expired preferred addresses must be skipped");
        std::cout << "Local IPv6 selection checks passed\n";
        return 0;
    } catch (std::exception const& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
