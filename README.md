# LAN ScreenCast

当前版本 **0.4.0**：Windows 主显示器 → Android 手机/Android TV，默认 H.264/WebRTC 视频，720p30，支持选择 1080p30。新版手机接收端可在 Windows 授权后操作键盘和鼠标。自动探测与采集显卡匹配的 Media Foundation 硬件编码器；不可用时降到软件 H.264/720p。JPEG 路径保留为用户主动选择的兼容模式。

## 目录

```text
.
├── windows/                 C++20 / WinUI 3 Windows 发送端
│   ├── LANScreenCast.sln
│   └── LANScreenCast/       单项目 MSIX 应用与文件日志
├── android-tv/              Kotlin / Compose for TV 接收端
│   ├── app/
│   └── gradle/wrapper/
├── protocol/                版本 1 的信令消息外层结构
└── .github/workflows/       双端构建工作流
```

## Windows 构建与运行

需要 Windows 10/11、固定版本的 vcpkg（见下方）、Visual Studio 2022（C++ 桌面开发与 WinUI 应用开发组件）和 Windows SDK。打开 `windows/LANScreenCast.sln`，还原 NuGet 包，选择 **Debug | x64**，按 F5 构建、部署并启动。命令行构建：

```powershell
msbuild windows\LANScreenCast.sln /restore /m /p:Configuration=Debug /p:Platform=x64 /p:GenerateAppxPackageOnBuild=false /p:AppxPackageSigningEnabled=false
```

WebRTC 原生依赖使用 vcpkg。首次 Windows 构建先执行：

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\tools\vcpkg
git -C C:\tools\vcpkg checkout 2750401336fb7c95f6619657a46a7e798661341c
C:\tools\vcpkg\bootstrap-vcpkg.bat -disableMetrics
$env:VCPKG_ROOT = 'C:\tools\vcpkg'
```

工程会从 `windows/vcpkg.json` 自动还原 `x64-windows-static-md` 依赖，锁定 libdatachannel 0.24.5。首次构建包含依赖编译，耗时明显高于后续构建。打开 Visual Studio 时同样需要继承 `VCPKG_ROOT` 环境变量。

工程使用 Windows App SDK 2.4.0 和 MSBuild。首次部署可能需要启用 Windows 开发人员模式。Windows 默认选择 H.264/WebRTC、720p、30 FPS。日志位于应用私有 LocalFolder 下的 `logs\lanscreencast.log`，通常在 `%LOCALAPPDATA%\Packages\LANScreenCast.Sender_*\LocalState\logs\`。

GitHub Actions 的 `LANScreenCast-Windows-x64-MSIX` 产物包含 Release 安装包和测试证书。解压后，在该目录打开**管理员 PowerShell**，执行：

```powershell
Import-Certificate -FilePath .\LANScreenCast-Test.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople
Add-AppxPackage .\Dependencies\x64\Microsoft.VCLibs.x64.14.00.appx
Add-AppxPackage .\Dependencies\x64\Microsoft.VCLibs.x64.14.00.Desktop.appx -ForceApplicationShutdown
Add-AppxPackage .\LANScreenCast-x64.msix
```

每次 CI 都生成新的测试签名证书。升级先前 CI 安装的 Windows 包前，先在 PowerShell 执行 `Get-AppxPackage LANScreenCast.Sender | Remove-AppxPackage`，再导入本次产物的证书并安装。证书仅用于此测试版；安装前应核对证书来源。旧的 `LANScreenCast-Windows-x64-debug` 是开发构建，不能当安装程序运行。

## Android TV 构建与运行

需要 JDK 17、Android SDK Platform 37 和 Build Tools 36.0.0。Android Studio 可直接打开 `android-tv/`；命令行：

```sh
cd android-tv
./gradlew :app:assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

安装后可在手机或 Android TV 启动器中找到 **LAN 投屏接收器**。打开后显示本机局域网 IP；在 Windows 输入此 IP，选择画质并点击“开始投屏”，在接收设备上选择“接受”。画面会直接通过 WebRTC 的 MediaCodec 解码和 EGL 渲染；停止或断线后返回等待界面。一次只接收一个会话，接受请求的等待阶段也会占用接收端。

H.264 信令地址为 `ws://IP:47475/signaling`；局域网 ICE 使用动态 UDP 端口，不依赖公网服务器。电脑与接收端应处于允许设备互访的同一局域网。Windows 防火墙提示出现时，应允许此应用在**专用网络**通信；访客 Wi-Fi 的设备隔离会阻止连接。`Test-NetConnection IP -Port 47475` 只能验证信令端口，不能证明 UDP 视频通道可达。

### 手机键鼠

1. 在 Windows 选择 H.264 并勾选“允许接收手机控制键盘和鼠标”，然后开始投屏。在手机接受后，画面下方出现控制栏。每次新会话都需要勾选。
2. 默认触控板模式下，单指滑动移动指针，单指轻点左键，双指轻点右键，双指滑动滚动。“拖拽”按下左键，再点“松开”释放。
3. 点“触控板”切换至直接触摸。轻点桌面对应位置，长按右键。选择“拖拽”后，从目标位置拖动并松手。
4. “输入文字”使用手机输入法，点“发送”将已输入的文字打到 Windows 当前窗口。“快捷键”可以先选 Ctrl、Shift、Alt 或 Win，再选字母、方向键、功能键等。
5. Windows 可点“撤销键鼠控制”继续观看，或点“停止投屏”结束会话。手机退到后台或断开连接时，Windows 释放远程拖拽按键。

键鼠需要两端都升级至 0.4.0，适用于可信局域网内的普通 Windows 桌面应用。Windows 输入完整性限制适用于管理员窗口、UAC 安全桌面和锁屏界面。

Windows 窗口显示实际发送/渲染 FPS、编码器模式、码率、RTT 与丢包。软件编码降到 720p，性能由设备决定。选择“JPEG 兼容模式”时使用原有 `LSC1` 和 47474 端口，没有接受请求窗口；新模式失败会报告原因，用户可手动切换。

Android 日志位于 `files/logs/lanscreencast.log`，可用 `adb shell run-as com.lanscreencast.tv cat files/logs/lanscreencast.log` 查看。`DECODER` 记录实际解码器；`FIRST_FRAME_MS` 记录从接受到首帧的时间；`STATS` 记录渲染 FPS/码率/RTT/丢包。Windows `VIDEO/STATS` 还记录进程 CPU、captureMs、encodeMs、queueDropped。captureMs 包含等待桌面更新的时间；encodeMs 包含颜色转换、编码和发送回调，均不能等同于端到端延迟。


如果设备上曾安装不同签名的调试包，`adb install -r` 会报 `INSTALL_FAILED_UPDATE_INCOMPATIBLE`；先卸载旧版，再安装新包。卸载会清除该应用的本地数据和日志。

## CI 与协议

`.github/workflows/build.yml` 在 GitHub Actions 上分别编译 Windows Release MSIX 和 Android Debug APK，并上传 `LANScreenCast-Windows-x64-MSIX` 与 `LANScreenCast-Android-TV-debug` 两个产物。

WebRTC 信令协议版本为 2，JSON Schema 包含各类消息的 payload 约束。旧信令外层文档存档为 v1；JPEG 媒体协议保持不变。依赖说明见 [THIRD_PARTY.md](THIRD_PARTY.md)，实机检查见 [tests/ACCEPTANCE.md](tests/ACCEPTANCE.md)。
