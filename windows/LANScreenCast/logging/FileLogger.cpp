#include "pch.h"
#include "FileLogger.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace LANScreenCast::logging
{
    namespace
    {
        std::mutex logMutex;

        std::wstring Clean(std::wstring_view text)
        {
            std::wstring result(text);
            for (auto& ch : result)
                if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
            return result;
        }
    }

    void FileLogger::Write(std::wstring_view level, std::wstring_view module,
        std::wstring_view event, std::wstring_view message)
    {
        try
        {
            std::lock_guard lock(logMutex);
            auto path = std::filesystem::path(
                winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path().c_str()) / L"logs";
            std::filesystem::create_directories(path);

            SYSTEMTIME time{};
            GetSystemTime(&time);
            std::wostringstream line;
            line << std::setfill(L'0') << std::setw(4) << time.wYear << L'-'
                << std::setw(2) << time.wMonth << L'-' << std::setw(2) << time.wDay << L'T'
                << std::setw(2) << time.wHour << L':' << std::setw(2) << time.wMinute << L':'
                << std::setw(2) << time.wSecond << L'.' << std::setw(3) << time.wMilliseconds
                << L"Z\t" << Clean(level) << L'\t' << Clean(module) << L'\t'
                << Clean(event) << L'\t' << Clean(message) << L'\n';

            auto text = line.str();
            int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            std::string utf8(size, 0);
            WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), size, nullptr, nullptr);
            std::ofstream file(path / L"lanscreencast.log", std::ios::app | std::ios::binary);
            file.write(utf8.data(), utf8.size());
        }
        catch (...)
        {
            OutputDebugStringW(L"LAN ScreenCast: failed to write log\n");
        }
    }
}
