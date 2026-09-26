[CmdletBinding()]
param(
    [string]$ProjectRoot = '',
    [string]$MounRiverRoot = 'E:\MounRiver\MounRiver_Studio2'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = Split-Path -Parent $PSScriptRoot
}

$toolBin = Join-Path $MounRiverRoot `
    'resources\app\resources\win32\components\WCH\Toolchain\RISC-V Embedded GCC\bin'
$gcc = Join-Path $toolBin 'riscv-none-embed-gcc.exe'
$objcopy = Join-Path $toolBin 'riscv-none-embed-objcopy.exe'
$objdump = Join-Path $toolBin 'riscv-none-embed-objdump.exe'
$sizeTool = Join-Path $toolBin 'riscv-none-embed-size.exe'
$outputRoot = Join-Path $ProjectRoot 'obj'

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
    '-I../Debug',
    '-I../Core',
    '-I../User',
    '-I../Peripheral/inc',
    '-I../lib',
    '-std=gnu99'
)
$sourceDirectories = @('Core', 'Debug', 'Peripheral/src', 'User', 'lib')
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
    foreach ($directory in $sourceDirectories) {
        $sourceRoot = Join-Path $ProjectRoot $directory
        $objectDirectory = Join-Path $outputRoot $directory
        New-Item -ItemType Directory -Path $objectDirectory -Force | Out-Null

        foreach ($source in Get-ChildItem -LiteralPath $sourceRoot -Filter '*.c' -File |
                     Sort-Object Name) {
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

Write-Host "Firmware ready: $(Join-Path $outputRoot 'CH32V203F6P.hex')"
