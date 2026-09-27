[CmdletBinding()]
param(
    [string]$ProjectRoot = '',
    [string]$MounRiverRoot = 'E:\MounRiver\MounRiver_Studio2',
    [ValidateSet('Product', 'Debug')]
    [string]$Profile = 'Product'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = Split-Path -Parent $PSScriptRoot
}

$toolBin = Join-Path $MounRiverRoot 'resources\app\resources\win32\components\WCH\Toolchain\RISC-V Embedded GCC\bin'
$gcc = Join-Path $toolBin 'riscv-none-embed-gcc.exe'
$objcopy = Join-Path $toolBin 'riscv-none-embed-objcopy.exe'
$objdump = Join-Path $toolBin 'riscv-none-embed-objdump.exe'
$sizeTool = Join-Path $toolBin 'riscv-none-embed-size.exe'
$outputRoot = Join-Path $ProjectRoot 'obj'
$productBuild = $Profile -eq 'Product'
$productDefine = if ($productBuild) { '1' } else { '0' }

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
    '-flto',
    '-fmax-errors=20',
    '-Os',
    '-fmessage-length=0',
    '-fsigned-char',
    '-ffunction-sections',
    '-fdata-sections',
    '-fno-common',
    '-fno-unwind-tables',
    '-fno-asynchronous-unwind-tables',
    '-Wunused',
    '-Wuninitialized',
    '-g'
)
$cFlags = $commonFlags + @(
    '-DDEBUG=2',
    "-DPRODUCT_BUILD=$productDefine",
    '-DCONFIG_IS_EMBEDDED_SYSTEM=1',
    '-DMIPSEL_EMU_USER_CONFIG_HEADER=<mipsel_emu_board_config.h>',
    '-I../Debug',
    '-I../Core',
    '-I../User',
    '-I../User/usb_usbd',
    '-I../Peripheral/inc',
    '-I../lib',
    '-I../third_party/stm32_in_c/user/src/mipsel-emu',
    '-I../third_party/openwch-ch32v20x/EVT/EXAM/USB/USBD/SimulateCDC/User/USBLIB/CONFIG',
    '-I../third_party/openwch-ch32v20x/EVT/EXAM/USB/USBD/SimulateCDC/User/USBLIB/USB-Driver/inc',
    '-std=gnu99'
)
$sourceDirectories = @('Core', 'Debug', 'Peripheral/src',
                       'User', 'User/usb_usbd', 'lib')
if ($productBuild) {
    $sourceDirectories += 'Product'
}

# 产品配置排除调试入口、USART 命令行和其 ISR；这些文件仍保留在 User/lib，
# 通过 -Profile Debug 可完整恢复上一版调试固件，不修改或删除原实现。
$excludedSources = @()
if ($productBuild) {
    $excludedSources = @(
        'User/main.c',
        'User/ch32v20x_it.c',
        'User/usart_console.c',
        'lib/cmdline.c',
        'lib/ring_buffer.c',
        'lib/usart_engine.c',
        'lib/usart_proxy.c'
    )
}
$externalSources = @(
    @{ Path = 'third_party/stm32_in_c/user/src/mipsel-emu/emu.c'; Object = 'Mipsel/emu.o' },
    @{ Path = 'third_party/stm32_in_c/user/src/mipsel-emu/instru.c'; Object = 'Mipsel/instru.o' },
    @{ Path = 'third_party/stm32_in_c/user/src/mipsel-emu/op.c'; Object = 'Mipsel/op.o' },
    @{ Path = 'third_party/stm32_in_c/user/src/mipsel-emu/exception.c'; Object = 'Mipsel/exception.o' },
    @{ Path = 'third_party/stm32_in_c/user/src/mipsel-emu/registers.c'; Object = 'Mipsel/registers.o' },
    @{ Path = 'third_party/stm32_in_c/user/src/mipsel-emu/platform.c'; Object = 'Mipsel/platform.o' },
    @{ Path = 'third_party/stm32_in_c/user/src/mipsel-emu/image_loader.c'; Object = 'Mipsel/image_loader.o' },
    @{ Path = 'third_party/stm32_in_c/user/src/mipsel-emu/uart16550.c'; Object = 'Mipsel/uart16550.o' },
    @{ Path = 'third_party/openwch-ch32v20x/EVT/EXAM/USB/USBD/SimulateCDC/User/USBLIB/USB-Driver/src/usb_core.c'; Object = 'USBD/usb_core.o' },
    @{ Path = 'third_party/openwch-ch32v20x/EVT/EXAM/USB/USBD/SimulateCDC/User/USBLIB/USB-Driver/src/usb_init.c'; Object = 'USBD/usb_init.o' },
    @{ Path = 'third_party/openwch-ch32v20x/EVT/EXAM/USB/USBD/SimulateCDC/User/USBLIB/USB-Driver/src/usb_int.c'; Object = 'USBD/usb_int.o' },
    @{ Path = 'third_party/openwch-ch32v20x/EVT/EXAM/USB/USBD/SimulateCDC/User/USBLIB/USB-Driver/src/usb_mem.c'; Object = 'USBD/usb_mem.o' },
    @{ Path = 'third_party/openwch-ch32v20x/EVT/EXAM/USB/USBD/SimulateCDC/User/USBLIB/USB-Driver/src/usb_regs.c'; Object = 'USBD/usb_regs.o' },
    @{ Path = 'third_party/openwch-ch32v20x/EVT/EXAM/USB/USBD/SimulateCDC/User/USBLIB/USB-Driver/src/usb_sil.c'; Object = 'USBD/usb_sil.o' },
    @{ Path = 'third_party/openwch-ch32v20x/EVT/EXAM/USB/USBD/SimulateCDC/User/USBLIB/CONFIG/usb_desc.c'; Object = 'USBD/usb_desc.o' },
    @{ Path = 'third_party/openwch-ch32v20x/EVT/EXAM/USB/USBD/SimulateCDC/User/USBLIB/CONFIG/usb_prop.c'; Object = 'USBD/usb_prop.o' },
    @{ Path = 'third_party/openwch-ch32v20x/EVT/EXAM/USB/USBD/SimulateCDC/User/USBLIB/CONFIG/usb_istr.c'; Object = 'USBD/usb_istr.o' },
    @{ Path = 'third_party/openwch-ch32v20x/EVT/EXAM/USB/USBD/SimulateCDC/User/USBLIB/CONFIG/usb_pwr.c'; Object = 'USBD/usb_pwr.o' }
)
$objects = [System.Collections.Generic.List[string]]::new()

# 运行一个工具链步骤，并把非零退出码提升为构建错误。
function Invoke-Checked {
    param([string]$Executable, [object[]]$Arguments, [string]$Description)

    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Description failed with exit code $LASTEXITCODE" }
}

New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
Push-Location $outputRoot
try {
    foreach ($directory in $sourceDirectories) {
        $sourceRoot = Join-Path $ProjectRoot $directory
        $objectDirectory = Join-Path $outputRoot $directory
        New-Item -ItemType Directory -Path $objectDirectory -Force | Out-Null

        $sourceFiles = Get-ChildItem -LiteralPath $sourceRoot -Filter '*.c' -File |
                       Sort-Object Name
        foreach ($source in $sourceFiles) {
            $relativeSourceName = "$directory/$($source.Name)"
            if ($excludedSources -contains $relativeSourceName) {
                continue
            }
            $stem = [System.IO.Path]::GetFileNameWithoutExtension($source.Name)
            $object = Join-Path $objectDirectory "$stem.o"
            $dependency = Join-Path $objectDirectory "$stem.d"
            $relativeObject = $object.Substring($outputRoot.Length + 1)
            $relativeDependency = $dependency.Substring($outputRoot.Length + 1)
            $relativeSource = "../$directory/$($source.Name)"

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

    foreach ($source in $externalSources) {
        $object = Join-Path $outputRoot $source.Object
        $dependency = [System.IO.Path]::ChangeExtension($object, '.d')
        $relativeObject = $source.Object
        $relativeDependency = [System.IO.Path]::ChangeExtension($source.Object, '.d')
        $relativeSource = "../$($source.Path)"
        New-Item -ItemType Directory -Path (Split-Path -Parent $object) -Force | Out-Null

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

    $startupDirectory = Join-Path $outputRoot 'Startup'
    New-Item -ItemType Directory -Path $startupDirectory -Force | Out-Null
    $startupObject = 'Startup/startup_ch32v20x_D6.o'
    Invoke-Checked -Executable $gcc -Description 'Compile startup' `
        -Arguments ($commonFlags + @(
            '-x',
            'assembler-with-cpp',
            '-I../Startup',
            '-MMD',
            '-MP',
            '-MFStartup/startup_ch32v20x_D6.d',
            "-MT$startupObject",
            '-c',
            '-o',
            $startupObject,
            '../Startup/startup_ch32v20x_D6.S'
        ))
    $objects.Add($startupObject)

    $linkFlags = $commonFlags + @(
        '-T',
        '../Ld/Link.ld',
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

Write-Host "Firmware ready ($Profile): $(Join-Path $outputRoot 'CH32V203F6P.hex')"
