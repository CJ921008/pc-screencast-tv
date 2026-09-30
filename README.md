# LAN ScreenCast

Windows → Android TV 局域网投屏项目。当前为 **Milestone 0：项目骨架**。两端只显示等待界面并记录启动日志；设备发现、配对、信令、WebRTC、屏幕采集和音频尚未实现。

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

需要 Windows 10/11、Visual Studio 2022（C++ 桌面开发与 WinUI 应用开发组件）和 Windows SDK。打开 `windows/LANScreenCast.sln`，还原 NuGet 包，选择 **Debug | x64**，按 F5 构建、部署并启动。命令行构建：

```powershell
msbuild windows\LANScreenCast.sln /restore /m /p:Configuration=Debug /p:Platform=x64 /p:GenerateAppxPackageOnBuild=false /p:AppxPackageSigningEnabled=false
```

工程使用 Windows App SDK 2.4.0 和 MSBuild。首次部署可能需要启用 Windows 开发人员模式。启动后显示“等待连接电视”。日志位于应用私有 LocalFolder 下的 `logs\lanscreencast.log`，通常在 `%LOCALAPPDATA%\Packages\LANScreenCast.Sender_*\LocalState\logs\`。

GitHub Actions 的 `LANScreenCast-Windows-x64-MSIX` 产物包含 Release 安装包和测试证书。解压后，在该目录打开**管理员 PowerShell**，执行：

```powershell
Import-Certificate -FilePath .\LANScreenCast-Test.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople
Add-AppxPackage .\Dependencies\x64\Microsoft.VCLibs.x64.14.00.appx
Add-AppxPackage .\Dependencies\x64\Microsoft.VCLibs.x64.14.00.Desktop.appx
Add-AppxPackage .\LANScreenCast-x64.msix
```

证书仅用于此测试版；安装前应核对证书来源。旧的 `LANScreenCast-Windows-x64-debug` 是开发构建，不能当安装程序运行。

## Android TV 构建与运行

需要 JDK 17、Android SDK Platform 37 和 Build Tools 36.0.0。Android Studio 可直接打开 `android-tv/`；命令行：

```sh
cd android-tv
./gradlew :app:assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

安装后可在手机应用列表或 Android TV 启动器中找到 **LAN 投屏接收器**（深蓝底、青色屏幕图标）。打开后显示“等待连接”。日志位于应用私有目录 `files/logs/lanscreencast.log`。调试设备可用 `adb shell run-as com.lanscreencast.tv cat files/logs/lanscreencast.log` 查看。

如果设备上曾安装不同签名的调试包，`adb install -r` 会报 `INSTALL_FAILED_UPDATE_INCOMPATIBLE`；先卸载旧版，再安装新包。卸载会清除该应用的本地数据和日志。

## CI 与协议

`.github/workflows/build.yml` 在 GitHub Actions 上分别编译 Windows Release MSIX 和 Android Debug APK，并上传 `LANScreenCast-Windows-x64-MSIX` 与 `LANScreenCast-Android-TV-debug` 两个产物。

`protocol/signaling.schema.json` 只定义消息外层字段；`protocol/protocol-version.md` 固定协议版本 1。该阶段没有网络消息交换。
