param(
    [Parameter(Mandatory = $true)]
    [string] $PackageDirectory
)

$ErrorActionPreference = 'Stop'
$msix = Join-Path $PackageDirectory 'VanJScreen-x64.msix'
$dependencies = @(
    (Join-Path $PackageDirectory 'Dependencies\x64\Microsoft.VCLibs.x64.14.00.appx'),
    (Join-Path $PackageDirectory 'Dependencies\x64\Microsoft.VCLibs.x64.14.00.Desktop.appx')
)
Add-AppxPackage -Path $msix -DependencyPath $dependencies
$package = Get-AppxPackage -Name LANScreenCast.Sender
if (!$package) { throw 'VanJScreen was not registered for the test user' }

Add-Type @'
using System;
using System.Runtime.InteropServices;

[ComImport]
[Guid("2E941141-7F97-4756-BA1D-9DECDE894A3D")]
[InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface ITestAppActivationManager
{
    uint ActivateApplication(
        [MarshalAs(UnmanagedType.LPWStr)] string appUserModelId,
        [MarshalAs(UnmanagedType.LPWStr)] string arguments,
        uint options);
}

public static class TestAppActivation
{
    public static uint Start(string appUserModelId)
    {
        Guid clsid = new Guid("45BA127D-10A8-46EA-8AB7-56EA9078943C");
        var manager = (ITestAppActivationManager) Activator.CreateInstance(Type.GetTypeFromCLSID(clsid));
        return manager.ActivateApplication(appUserModelId, null, 2);
    }
}
'@

$log = Join-Path $env:LOCALAPPDATA "Packages\$($package.PackageFamilyName)\LocalState\logs\lanscreencast.log"
$previous = if (Test-Path $log) { @(Get-Content $log).Count } else { 0 }
$startedAt = Get-Date
$appPid = $null
try {
    $appPid = [TestAppActivation]::Start("$($package.PackageFamilyName)!App")
    $deadline = (Get-Date).AddSeconds(30)
    $ready = $false
    while ((Get-Date) -lt $deadline) {
        $process = Get-Process -Id $appPid -ErrorAction SilentlyContinue
        if (!$process) { throw 'VanJScreen exited during startup' }
        if ($process.MainWindowHandle -ne [IntPtr]::Zero -and (Test-Path $log)) {
            $lines = Get-Content $log | Select-Object -Skip $previous
            if ($lines -match 'WINDOW_READY') {
                $ready = $true
                break
            }
        }
        Start-Sleep -Milliseconds 250
    }
    if (!$ready) { throw 'VanJScreen did not show its main window within 30 seconds' }
    Start-Sleep -Seconds 2
    if (!(Get-Process -Id $appPid -ErrorAction SilentlyContinue)) {
        throw 'VanJScreen exited after showing its main window'
    }
    Write-Host "Installed VanJScreen $($package.Version) opened its main window"
}
catch {
    if (Test-Path $log) { Get-Content $log | Select-Object -Skip $previous }
    Get-WinEvent -FilterHashtable @{LogName='Application'; StartTime=$startedAt.AddSeconds(-1)} -ErrorAction SilentlyContinue |
        Where-Object { $_.Message -match 'LANScreenCast|VanJScreen' } |
        Select-Object -First 3 TimeCreated, Id, Message | Format-List
    throw
}
finally {
    if ($appPid) {
        Stop-Process -Id $appPid -ErrorAction SilentlyContinue
    }
}
