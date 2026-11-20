#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include "LocalIPv6.h"
#include <limits>
#include <memory>
#include <tuple>
#include <utility>

#pragma comment(lib, "Iphlpapi.lib")
#pragma comment(lib, "Ws2_32.lib")

namespace LANScreenCast::casting {
bool IsPublicIPv6Prefix(unsigned char firstByte) {
    return (firstByte & 0xe0) == 0x20;
}

std::optional<std::wstring> SelectLocalIPv6(IP_ADAPTER_ADDRESSES const* adapters) {
    // Prefer an interface with an IPv6 gateway, then its interface metric and the address lifetime.
    using Rank = std::tuple<bool, ULONG, ULONG, std::wstring>;
    std::optional<Rank> best;
    for (auto adapter = adapters; adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

        bool hasGateway = false;
        for (auto gateway = adapter->FirstGatewayAddress; gateway; gateway = gateway->Next) {
            if (gateway->Address.lpSockaddr && gateway->Address.iSockaddrLength >= static_cast<int>(sizeof(SOCKADDR_IN6)) &&
                gateway->Address.lpSockaddr->sa_family == AF_INET6) {
                hasGateway = true;
                break;
            }
        }

        for (auto entry = adapter->FirstUnicastAddress; entry; entry = entry->Next) {
            auto socket = entry->Address.lpSockaddr;
            if (!socket || entry->Address.iSockaddrLength < static_cast<int>(sizeof(SOCKADDR_IN6)) ||
                socket->sa_family != AF_INET6 || entry->DadState != IpDadStatePreferred ||
                entry->ValidLifetime == 0 || entry->PreferredLifetime == 0) continue;

            auto const& address = reinterpret_cast<SOCKADDR_IN6 const*>(socket)->sin6_addr;
            if (!IsPublicIPv6Prefix(address.u.Byte[0])) continue;

            wchar_t formatted[INET6_ADDRSTRLEN]{};
            if (!InetNtopW(AF_INET6, &address, formatted, INET6_ADDRSTRLEN)) continue;

            Rank candidate{ !hasGateway, adapter->Ipv6Metric,
                std::numeric_limits<ULONG>::max() - entry->PreferredLifetime, formatted };
            if (!best || candidate < *best) best = std::move(candidate);
        }
    }
    if (best) return std::get<3>(*best);
    return std::nullopt;
}

std::optional<std::wstring> DetectLocalIPv6() {
    ULONG size = 15 * 1024;
    constexpr ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
        GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS;
    for (int attempt = 0; attempt < 3; ++attempt) {
        auto buffer = std::make_unique_for_overwrite<unsigned char[]>(size);
        auto adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.get());
        auto result = GetAdaptersAddresses(AF_INET6, flags, nullptr, adapters, &size);
        if (result == ERROR_SUCCESS) return SelectLocalIPv6(adapters);
        if (result != ERROR_BUFFER_OVERFLOW) break;
    }
    return std::nullopt;
}
}
