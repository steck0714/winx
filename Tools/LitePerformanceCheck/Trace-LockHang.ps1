#Requires -Version 5.1
#Requires -RunAsAdministrator
<#
Collect scheduling/interrupt/CPU-idle events alongside power transitions.
Uses the system-provider GUIDs and keyword masks from Windows SDK evntrace.h
(20348 and later). Does not change power policy or install drivers.

First run with -Seconds 5 while awake and inspect both ETLs before asking
for a reproduction. Use a new OutputDirectory for each run; evidence is
never overwritten. A suspended or terminated controller cannot guarantee
timely cleanup: query these exact session names after reconnecting.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$OutputDirectory,
    [ValidateRange(5, 600)]
    [int]$Seconds = 120,
    [ValidatePattern('^SwitchWOA[A-Za-z0-9]{1,40}$')]
    [string]$NamePrefix = 'SwitchWOALockExec'
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new()
Import-Module EventTracingManagement -ErrorAction Stop

$systemName = $NamePrefix + 'System'
$powerName = $NamePrefix + 'Power'
foreach ($name in @($systemName, $powerName)) {
    if (Get-EtwTraceSession -Name $name -ErrorAction SilentlyContinue) {
        throw "Trace session already exists: $name"
    }
}
$directory = [IO.Path]::GetFullPath($OutputDirectory)
$systemPath = Join-Path $directory 'execution.etl'
$powerPath = Join-Path $directory 'power.etl'
foreach ($path in @($systemPath, $powerPath)) {
    if (Test-Path -LiteralPath $path) { throw "Trace file already exists: $path" }
}
New-Item -ItemType Directory -Path $directory -Force | Out-Null

$settings = @{
    MaximumFileSize = 64
    BufferSize = 64
    MinimumBuffers = 16
    MaximumBuffers = 64
    FlushTimer = 1
}
$started = [Collections.Generic.List[string]]::new()
try {
    # SYSTEM_LOGGER_MODE | FILE_MODE_CIRCULAR, with a private session name.
    # Never take over the reserved NT Kernel Logger or an existing session.
    New-EtwTraceSession -Name $systemName -LogFileMode 0x02000002 `
        -LocalFilePath $systemPath @settings | Out-Null
    $started.Add($systemName)
    $providers = @(
        # SystemPower: general + processor idle + idle selection.
        @('c134884a-32d5-4488-80e5-14ed7abb8269', 0xD),
        # SystemScheduler: dispatcher + context switch.
        @('599a2a76-4d91-4910-9ac7-7d33f2e97a6c', 0x202),
        # SystemInterrupt: ISR/clock/DPC/queue/WDF/IPI.
        @('d4bbee17-b545-4888-858b-744169015b25', 0x7F),
        # SystemProcess: process + worker/thread + image loader.
        @('151f55dc-467d-471f-83b5-5f889d46ff66', 0x1C01)
    )
    foreach ($provider in $providers) {
        Add-EtwTraceProvider -SessionName $systemName -Guid $provider[0] `
            -Level 5 -MatchAnyKeyword ([UInt64]$provider[1]) | Out-Null
    }
    New-EtwTraceSession -Name $powerName -LogFileMode 2 `
        -LocalFilePath $powerPath @settings | Out-Null
    $started.Add($powerName)
    Add-EtwTraceProvider -SessionName $powerName `
        -Guid '331c3b3a-2005-44c2-ac5e-77220c37d6b4' -Level 5 `
        -MatchAnyKeyword ([UInt64]::MaxValue) | Out-Null
    Add-EtwTraceProvider -SessionName $powerName `
        -Guid '0f67e49f-fe51-4e9f-b490-6f2948cc6027' -Level 5 `
        -MatchAnyKeyword ([UInt64]9) | Out-Null
    [ordered]@{
        phase = 'trace_started'
        utc = [DateTime]::UtcNow.ToString('o')
        seconds_requested = $Seconds
        sessions = @($systemName, $powerName)
        files = @($systemPath, $powerPath)
        file_limit_mb_each = 64
    } | ConvertTo-Json -Depth 3 -Compress
    [Console]::Out.Flush()
    for ($remaining = $Seconds; $remaining -gt 0; $remaining -= 5) {
        Start-Sleep -Seconds ([Math]::Min(5, $remaining))
    }
} finally {
    for ($index = $started.Count - 1; $index -ge 0; --$index) {
        Stop-EtwTraceSession -Name $started[$index] -ErrorAction Continue | Out-Null
    }
}
Get-Item -LiteralPath $systemPath, $powerPath |
    Select-Object FullName, Length, LastWriteTimeUtc | ConvertTo-Json -Compress
