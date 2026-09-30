#pragma once
#include "pch.h"
#include "MainWindow.g.h"

namespace winrt::LANScreenCast::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();
    };
}

namespace winrt::LANScreenCast::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
}

