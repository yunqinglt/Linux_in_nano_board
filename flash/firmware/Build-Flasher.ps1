[CmdletBinding()]
param(
    [string]$ProjectRoot = '',
    [string]$MounRiverRoot = 'E:\MounRiver\MounRiver_Studio2'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    # 归档脚本位于 flash/firmware，项目根目录在其上两级。
    $ProjectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
}

$toolBin = Join-Path $MounRiverRoot `
    'resources\app\resources\win32\components\WCH\Toolchain\RISC-V Embedded GCC\bin'
$gcc = Join-Path $toolBin 'riscv-none-embed-gcc.exe'
$objcopy = Join-Path $toolBin 'riscv-none-embed-objcopy.exe'
$objdump = Join-Path $toolBin 'riscv-none-embed-objdump.exe'
$sizeTool = Join-Path $toolBin 'riscv-none-embed-size.exe'
$firmwareUserRoot = Join-Path $PSScriptRoot 'User'
$outputRoot = Join-Path (Split-Path -Parent $PSScriptRoot) 'build'

foreach ($tool in @($gcc, $objcopy, $objdump, $sizeTool)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "Required tool not found: $tool"
    }
}

$commonFlags = @(
    '-march=rv32imacxw',
    '-mabi=ilp32',
    '-msmall-data-limit=8',
    '-msave-restore',
    '-fmax-errors=20',
    '-Os',
    '-fmessage-length=0',
    '-fsigned-char',
    '-ffunction-sections',
    '-fdata-sections',
    '-fno-common',
    '-Wunused',
    '-Wuninitialized',
    '-g'
)
$cFlags = $commonFlags + @(
    '-DDEBUG=2',
    '-I../../Debug',
    '-I../../Core',
    '-I../firmware/User',
    '-I../firmware/User/usb_usbd',
    '-I../../Peripheral/inc',
    '-I../../lib',
    '-I../../third_party/openwch-ch32v20x/EVT/EXAM/USB/USBD/SimulateCDC/User/USBLIB/USB-Driver/inc',
    '-I../../third_party/openwch-ch32v20x/EVT/EXAM/USB/USBD/SimulateCDC/User/USBLIB/CONFIG',
    '-include',
    '../firmware/User/usb_usbd/usb_conf.h',
    '-std=gnu99'
)
$sourceSets = @(
    @{ Name = 'Core'; Path = (Join-Path $ProjectRoot 'Core'); Relative = '../../Core' },
    @{ Name = 'Debug'; Path = (Join-Path $ProjectRoot 'Debug'); Relative = '../../Debug' },
    @{ Name = 'Peripheral/src'; Path = (Join-Path $ProjectRoot 'Peripheral/src'); Relative = '../../Peripheral/src' },
    @{ Name = 'User'; Path = $firmwareUserRoot; Relative = '../firmware/User' },
    @{ Name = 'lib'; Path = (Join-Path $ProjectRoot 'lib'); Relative = '../../lib' }
)
$usbdSourceRoot = 'third_party/openwch-ch32v20x/EVT/EXAM/USB/USBD/SimulateCDC/User/USBLIB'
$usbdSources = @(
    "$usbdSourceRoot/USB-Driver/src/usb_core.c",
    "$usbdSourceRoot/USB-Driver/src/usb_init.c",
    "$usbdSourceRoot/USB-Driver/src/usb_int.c",
    "$usbdSourceRoot/USB-Driver/src/usb_mem.c",
    "$usbdSourceRoot/USB-Driver/src/usb_regs.c",
    "$usbdSourceRoot/USB-Driver/src/usb_sil.c",
    "$usbdSourceRoot/CONFIG/usb_istr.c",
    "$usbdSourceRoot/CONFIG/usb_pwr.c"
)
$objects = [System.Collections.Generic.List[string]]::new()

# 运行一个工具链步骤，并把非零退出码提升为构建错误。
function Invoke-Checked {
    param(
        [Parameter(Mandatory)] [string]$Executable,
        [Parameter(Mandatory)] [object[]]$Arguments,
        [Parameter(Mandatory)] [string]$Description
    )

    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Description failed with exit code $LASTEXITCODE"
    }
}

New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
Push-Location $outputRoot
try {
    foreach ($sourceSet in $sourceSets) {
        $directory = $sourceSet.Name
        $sourceRoot = $sourceSet.Path
        $objectDirectory = Join-Path $outputRoot $directory
        New-Item -ItemType Directory -Path $objectDirectory -Force | Out-Null

        foreach ($source in Get-ChildItem -LiteralPath $sourceRoot -Filter '*.c' -File |
                     Sort-Object Name) {
            $stem = [System.IO.Path]::GetFileNameWithoutExtension($source.Name)
            $object = Join-Path $objectDirectory "$stem.o"
            $dependency = Join-Path $objectDirectory "$stem.d"
            $relativeObject = $object.Substring($outputRoot.Length + 1)
            $relativeDependency = $dependency.Substring($outputRoot.Length + 1)
            $relativeSource = "$($sourceSet.Relative)/$($source.Name)"

            Invoke-Checked -Executable $gcc -Description "Compile $relativeSource" `
                -Arguments ($cFlags + @(
                    '-MMD',
                    '-MP',
                    "-MF$relativeDependency",
                    "-MT$relativeObject",
                    '-c',
                    '-o',
                    $relativeObject,
                    $relativeSource
                ))
            $objects.Add($relativeObject)
        }
    }

    # WCH legacy USBD 使用独立 PMA；只编译协议核心和中断/电源层，设备类由 User 实现。
    $usbdObjectDirectory = Join-Path $outputRoot 'USBD'
    New-Item -ItemType Directory -Path $usbdObjectDirectory -Force | Out-Null
    foreach ($relativeSourcePath in $usbdSources) {
        $source = Join-Path $ProjectRoot $relativeSourcePath
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            throw "Required WCH USBD source not found: $source"
        }

        $stem = [System.IO.Path]::GetFileNameWithoutExtension($source)
        $object = Join-Path $usbdObjectDirectory "$stem.o"
        $dependency = Join-Path $usbdObjectDirectory "$stem.d"
        $relativeObject = $object.Substring($outputRoot.Length + 1)
        $relativeDependency = $dependency.Substring($outputRoot.Length + 1)
        $compileSource = "../../$($relativeSourcePath.Replace('\\', '/'))"

        Invoke-Checked -Executable $gcc -Description "Compile $relativeSourcePath" `
            -Arguments ($cFlags + @(
                '-Wno-unused-variable',
                '-MMD',
                '-MP',
                "-MF$relativeDependency",
                "-MT$relativeObject",
                '-c',
                '-o',
                $relativeObject,
                $compileSource
            ))
        $objects.Add($relativeObject)
    }

    $startupDirectory = Join-Path $outputRoot 'Startup'
    New-Item -ItemType Directory -Path $startupDirectory -Force | Out-Null
    $startupObject = 'Startup/startup_ch32v20x_D6.o'
    Invoke-Checked -Executable $gcc -Description 'Compile startup' `
        -Arguments ($commonFlags + @(
            '-x',
            'assembler-with-cpp',
            '-I../../Startup',
            '-MMD',
            '-MP',
            '-MFStartup/startup_ch32v20x_D6.d',
            "-MT$startupObject",
            '-c',
            '-o',
            $startupObject,
            '../../Startup/startup_ch32v20x_D6.S'
        ))
    $objects.Add($startupObject)

    $linkFlags = $commonFlags + @(
        '-T',
        '../../Ld/Link.ld',
        '-nostartfiles',
        '-Xlinker',
        '--gc-sections',
        '-Wl,-Map,CH32V203F6P.map',
        '--specs=nano.specs',
        '--specs=nosys.specs',
        '-o',
        'CH32V203F6P.elf'
    )
    $linkObjects = @($objects.ToArray() | Sort-Object)
    Invoke-Checked -Executable $gcc -Description 'Link firmware' `
        -Arguments ($linkFlags + $linkObjects + @('-lm'))
    Invoke-Checked -Executable $objcopy -Description 'Create Intel HEX image' `
        -Arguments @('-O', 'ihex', 'CH32V203F6P.elf', 'CH32V203F6P.hex')

    & $objdump '--all-headers' '--demangle' '--disassemble' '-M' 'xw' `
        'CH32V203F6P.elf' | Set-Content -Encoding Ascii 'CH32V203F6P.lst'
    if ($LASTEXITCODE -ne 0) {
        throw "Create listing failed with exit code $LASTEXITCODE"
    }

    Invoke-Checked -Executable $sizeTool -Description 'Measure firmware' `
        -Arguments @('--format=berkeley', 'CH32V203F6P.elf')
}
finally {
    Pop-Location
}

Write-Host "Firmware ready: $(Join-Path $outputRoot 'CH32V203F6P.hex')"
