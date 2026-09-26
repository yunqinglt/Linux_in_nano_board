[CmdletBinding()]
param(
    [string]$Port = 'COM19',
    [int]$BaudRate = 115200,
    [int]$TimeoutSeconds = 20,
    [string]$Command = 'probe'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$timeoutJob = Start-Job -ScriptBlock {
    param($Seconds)
    Start-Sleep -Seconds $Seconds
} -ArgumentList $TimeoutSeconds

$serial = New-Object System.IO.Ports.SerialPort(
    $Port,
    $BaudRate,
    [System.IO.Ports.Parity]::None,
    8,
    [System.IO.Ports.StopBits]::One
)
$serial.Handshake = [System.IO.Ports.Handshake]::None
$serial.DtrEnable = $false
$serial.RtsEnable = $false
$serial.ReadTimeout = 50

$lineBuffer = ''
$w25Seen = $false
$apsSeen = $false
$commandSent = [string]::IsNullOrWhiteSpace($Command)

try {
    $serial.Open()
    Write-Host ("{0}: {1} ready at {2} baud; flash the firmware manually now." -f (Get-Date -Format 'HH:mm:ss.fff'), $Port, $BaudRate)

    while ($timeoutJob.State -eq 'Running') {
        if ($serial.BytesToRead -gt 0) {
            $lineBuffer += $serial.ReadExisting()

            $consoleReady = $lineBuffer.Contains('Command table:') -or
                $lineBuffer.Contains('ch32> ')
            if (-not $commandSent -and $consoleReady) {
                $serial.Write("$Command`r")
                $commandSent = $true
                $w25Seen = $false
                $apsSeen = $false
                Write-Host ("[{0}] TX: {1}" -f (Get-Date -Format 'HH:mm:ss.fff'), $Command)
            }

            while ($lineBuffer.Contains("`n")) {
                $newline = $lineBuffer.IndexOf("`n")
                $line = $lineBuffer.Substring(0, $newline).TrimEnd("`r")
                $lineBuffer = $lineBuffer.Substring($newline + 1)
                Write-Host ("[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss.fff'), $line)

                if ($line -match '(?i)SPI1\s+W25Q64.*\bid=') {
                    $w25Seen = $true
                }
                if ($line -match '(?i)SPI1\s+APS6404L.*\b(mf|kgd)=') {
                    $apsSeen = $true
                }

                if ($w25Seen -and $apsSeen) {
                    Stop-Job -Job $timeoutJob
                    Write-Host 'Both SPI probe lines received; stopped the 20-second wait.'
                    break
                }
            }
        }

        if ($w25Seen -and $apsSeen) {
            break
        }
        Start-Sleep -Milliseconds 20
    }

    if (-not $commandSent) {
        Write-Warning ("The {0}-second wait elapsed before the command prompt appeared; '{1}' was not sent." -f $TimeoutSeconds, $Command)
    }
    if (-not ($w25Seen -and $apsSeen)) {
        Write-Warning ("The {0}-second wait elapsed before both SPI probe lines arrived." -f $TimeoutSeconds)
    }
}
finally {
    if ($timeoutJob.State -eq 'Running') {
        Stop-Job -Job $timeoutJob -ErrorAction SilentlyContinue
    }
    Remove-Job -Job $timeoutJob -Force -ErrorAction SilentlyContinue
    if ($serial.IsOpen) {
        $serial.Close()
    }
    $serial.Dispose()
}
