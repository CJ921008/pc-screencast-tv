#pragma once
#include <string_view>

namespace LANScreenCast::logging
{
    struct FileLogger final
    {
        static void Write(std::wstring_view level, std::wstring_view module,
            std::wstring_view event, std::wstring_view message);
    };
}

