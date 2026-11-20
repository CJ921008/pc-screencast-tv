#pragma once

#include <optional>
#include <string>

struct _IP_ADAPTER_ADDRESSES_LH;

namespace LANScreenCast::casting {
bool IsPublicIPv6Prefix(unsigned char firstByte);
std::optional<std::wstring> SelectLocalIPv6(_IP_ADAPTER_ADDRESSES_LH const* adapters);
std::optional<std::wstring> DetectLocalIPv6();
}
