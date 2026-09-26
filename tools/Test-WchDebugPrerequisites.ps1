[CmdletBinding()]
param(
    [string]$MounRiverRoot = 'E:\MounRiver\MounRiver_Studio2',
    [string]$OpenOcdPath,
    [ValidateSet('Inventory', 'WchLink', 'Bootloader', 'Both', 'DebugPrerequisites')]
    [string]$Require = 'Inventory',
    [switch]$Json
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$isWindowsHost = if (Get-Variable -Name IsWindows -ErrorAction SilentlyContinue) {
    $IsWindows
}
else {
    $env:OS -eq 'Windows_NT'
}

if (-not $isWindowsHost) {
    throw 'This inventory script requires Windows and the PnpDevice PowerShell module.'
}

try {
    Import-Module PnpDevice -ErrorAction Stop
    $presentDevices = @(Get-PnpDevice -PresentOnly -ErrorAction Stop)
}
catch {
    throw "Unable to enumerate present Plug and Play devices: $($_.Exception.Message)"
}

$wchLinkDevices = @(
    $presentDevices |
        Where-Object InstanceId -Match '(?i)VID_1A86&PID_8010'
)
$wchLinkDebugDevices = @(
    $wchLinkDevices |
        Where-Object InstanceId -Match '(?i)VID_1A86&PID_8010&MI_00'
)
$bootloaderDevices = @(
    $presentDevices |
        Where-Object InstanceId -Match '(?i)VID_4348&PID_55E0'
)

if ([string]::IsNullOrWhiteSpace($OpenOcdPath)) {
    $OpenOcdPath = Join-Path -Path $MounRiverRoot -ChildPath 'resources\app\resources\win32\components\WCH\OpenOCD\OpenOCD\bin\openocd.exe'
}

$openOcdExists = Test-Path -LiteralPath $OpenOcdPath -PathType Leaf
$openOcdVersion = $null
$openOcdSupportsWlinke = $false
$openOcdSupportsSdi = $false
$openOcdSupportsWchRiscv = $false
$openOcdAdapterExitCode = $null
$openOcdTransportExitCode = $null
$openOcdTargetExitCode = $null
$openOcdProbeError = $null
$openOcdDirectory = [System.IO.Path]::GetDirectoryName($OpenOcdPath)
$openOcdConfigPath = if ([string]::IsNullOrWhiteSpace($openOcdDirectory)) {
    $null
}
else {
    [System.IO.Path]::Combine($openOcdDirectory, 'wch-riscv.cfg')
}
$openOcdConfigReady = $false

function Invoke-NativeCommandCapture {
    param(
        [Parameter(Mandatory)]
        [string]$FilePath,
        [string[]]$ArgumentList = @()
    )

    # Windows PowerShell 5 surfaces native stderr as NativeCommandError. Keep it
    # captured as ordinary text without letting the script-wide Stop policy abort.
    $savedErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $lines = @(& $FilePath @ArgumentList 2>&1 | ForEach-Object { $_.ToString() })
        $commandExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $savedErrorActionPreference
    }

    [pscustomobject]@{
        Lines    = $lines
        ExitCode = $commandExitCode
    }
}

if ($openOcdExists) {
    try {
        $versionResult = Invoke-NativeCommandCapture -FilePath $OpenOcdPath -ArgumentList @('--version')
        if ($versionResult.Lines.Count -gt 0) {
            $openOcdVersion = [string]$versionResult.Lines[0]
        }

        # These commands query compiled-in capabilities only. They deliberately
        # do not issue "init", open the WCH-Link, halt/reset the target, or touch flash.
        $adapterResult = Invoke-NativeCommandCapture -FilePath $OpenOcdPath -ArgumentList @(
            '-c', 'adapter list', '-c', 'shutdown'
        )
        $openOcdAdapterExitCode = $adapterResult.ExitCode
        $openOcdSupportsWlinke =
            $adapterResult.ExitCode -eq 0 -and
            [bool]($adapterResult.Lines -match '(?im)^\s*\d+:\s*wlinke\s*$')

        $transportResult = Invoke-NativeCommandCapture -FilePath $OpenOcdPath -ArgumentList @(
            '-c', 'adapter driver wlinke', '-c', 'transport list', '-c', 'shutdown'
        )
        $openOcdTransportExitCode = $transportResult.ExitCode
        $openOcdSupportsSdi =
            $transportResult.ExitCode -eq 0 -and
            [bool]($transportResult.Lines -match '(?im)^\s*sdi\s*$')

        $targetResult = Invoke-NativeCommandCapture -FilePath $OpenOcdPath -ArgumentList @(
            '-c', 'target types', '-c', 'shutdown'
        )
        $openOcdTargetExitCode = $targetResult.ExitCode
        $openOcdSupportsWchRiscv =
            $targetResult.ExitCode -eq 0 -and
            [bool]($targetResult.Lines -match '(?i)(^|\s)wch_riscv(\s|$)')
    }
    catch {
        $openOcdProbeError = $_.Exception.Message
    }
}

if (
    -not [string]::IsNullOrWhiteSpace($openOcdConfigPath) -and
    (Test-Path -LiteralPath $openOcdConfigPath -PathType Leaf)
) {
    $configText = Get-Content -Raw -LiteralPath $openOcdConfigPath
    $openOcdConfigReady =
        $configText -match '(?im)^\s*adapter\s+driver\s+wlinke\s*$' -and
        $configText -match '(?im)^\s*transport\s+select\s+sdi\s*$' -and
        $configText -match '(?im)^\s*target\s+create\s+.+\s+wch_riscv\b'
}

function ConvertTo-DeviceRecord {
    param([Parameter(Mandatory)]$Device)

    $deviceProperties = @{}
    try {
        Get-PnpDeviceProperty -InstanceId $Device.InstanceId -ErrorAction Stop |
            ForEach-Object {
                $deviceProperties[$_.KeyName] = $_.Data
            }
    }
    catch {
        # Keep inventory useful when a device disappears during USB
        # re-enumeration. A missing service below will make the debug
        # prerequisite check fail closed.
    }

    [pscustomobject]@{
        FriendlyName   = $Device.FriendlyName
        Class          = $Device.Class
        Status         = [string]$Device.Status
        Problem        = [string]$Device.Problem
        InstanceId     = $Device.InstanceId
        Service        = [string]$deviceProperties['DEVPKEY_Device_Service']
        DriverInf      = [string]$deviceProperties['DEVPKEY_Device_DriverInfPath']
        DriverProvider = [string]$deviceProperties['DEVPKEY_Device_DriverProvider']
        DriverVersion  = [string]$deviceProperties['DEVPKEY_Device_DriverVersion']
    }
}

$wchLinkRecords = @($wchLinkDevices | ForEach-Object { ConvertTo-DeviceRecord $_ })
$wchLinkDebugRecords = @($wchLinkDebugDevices | ForEach-Object { ConvertTo-DeviceRecord $_ })
$healthyWchLinkDebugRecords = @(
    $wchLinkDebugRecords |
        Where-Object {
            $_.Status -eq 'OK' -and
            $_.Problem -in @('', '0', 'CM_PROB_NONE') -and
            $_.Service -match '(?i)^WCHLink(?:_A64)?$'
        }
)
$bootloaderRecords = @($bootloaderDevices | ForEach-Object { ConvertTo-DeviceRecord $_ })
$debugPrerequisitesReady =
    $healthyWchLinkDebugRecords.Count -gt 0 -and
    $openOcdExists -and
    $openOcdSupportsWlinke -and
    $openOcdSupportsSdi -and
    $openOcdSupportsWchRiscv -and
    $openOcdConfigReady

$result = [pscustomobject]@{
    ObservedAt                = (Get-Date).ToString('o')
    WchLinkVidPid             = '1A86:8010'
    WchLinkPresent            = $wchLinkRecords.Count -gt 0
    WchLinkDevices            = $wchLinkRecords
    WchLinkDebugInterfacePresent = $wchLinkDebugRecords.Count -gt 0
    WchLinkDebugInterfaceReady = $healthyWchLinkDebugRecords.Count -gt 0
    WchLinkDebugDevices       = $wchLinkDebugRecords
    BootloaderVidPid          = '4348:55E0'
    BootloaderPresent         = $bootloaderRecords.Count -gt 0
    BootloaderDevices         = $bootloaderRecords
    OpenOcdPath               = $OpenOcdPath
    OpenOcdExists             = $openOcdExists
    OpenOcdVersion            = $openOcdVersion
    OpenOcdSupportsWlinke     = $openOcdSupportsWlinke
    OpenOcdSupportsSdi        = $openOcdSupportsSdi
    OpenOcdSupportsWchRiscv   = $openOcdSupportsWchRiscv
    OpenOcdAdapterExitCode    = $openOcdAdapterExitCode
    OpenOcdTransportExitCode  = $openOcdTransportExitCode
    OpenOcdTargetExitCode     = $openOcdTargetExitCode
    OpenOcdProbeError         = $openOcdProbeError
    OpenOcdConfigPath         = $openOcdConfigPath
    OpenOcdConfigReady        = $openOcdConfigReady
    DebugPrerequisitesReady   = $debugPrerequisitesReady
    TargetDebugLinkProbed     = $false
    ProbePolicy               = 'USB inventory and OpenOCD capability only; no OpenOCD init, halt, reset, flash, or serial access.'
}

if ($Json) {
    $result | ConvertTo-Json -Depth 6
}
else {
    $result |
        Select-Object ObservedAt, WchLinkPresent, BootloaderPresent,
            WchLinkDebugInterfaceReady, OpenOcdPath, OpenOcdVersion,
            OpenOcdSupportsWlinke, OpenOcdSupportsSdi, OpenOcdSupportsWchRiscv,
            OpenOcdConfigReady, DebugPrerequisitesReady,
            TargetDebugLinkProbed, ProbePolicy |
        Format-List

    if ($wchLinkRecords.Count -gt 0) {
        'WCH-Link devices:'
        $wchLinkRecords | Format-Table -AutoSize
    }

    if ($bootloaderRecords.Count -gt 0) {
        'Bootloader devices:'
        $bootloaderRecords | Format-Table -AutoSize
    }
}

$requirementMet = switch ($Require) {
    'Inventory'          { $true }
    'WchLink'            { $wchLinkRecords.Count -gt 0 }
    'Bootloader'         { $bootloaderRecords.Count -gt 0 }
    'Both'               { $wchLinkRecords.Count -gt 0 -and $bootloaderRecords.Count -gt 0 }
    'DebugPrerequisites' { $debugPrerequisitesReady }
}

if (-not $requirementMet) {
    Write-Error "Requirement '$Require' was not met. Review the inventory above." -ErrorAction Continue
    exit 2
}
