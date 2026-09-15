[CmdletBinding()]
param([ValidateRange(1000, 1000000)][int]$Entries = 10000)

$ErrorActionPreference = 'Stop'
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('unix-utils-bench-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($fixture) | Out-Null
try {
    for ($i = 0; $i -lt $Entries; ++$i) { [IO.File]::WriteAllBytes((Join-Path $fixture ('entry-{0:D8}' -f $i)), [byte[]]@()) }
    foreach ($case in @(@('ls', '-1'), @('ls', '-U'), @('du', '--inodes -a'), @('du', '--inodes -a -L'))) {
        $exe = Join-Path $PSScriptRoot ("build\{0}.exe" -f $case[0])
        $start = New-Object Diagnostics.ProcessStartInfo
        $start.FileName = $exe
        $start.Arguments = $case[1] + ' "' + $fixture + '"'
        $start.UseShellExecute = $false
        $start.RedirectStandardOutput = $true
        $start.RedirectStandardError = $true
        $process = [Diagnostics.Process]::Start($start)
        $errors = $process.StandardError.ReadToEndAsync()
        $peak = 0L
        $private = 0L
        # Let the output pipe fill to make even these short-lived processes observable.
        $null = $process.WaitForExit(100)
        $process.Refresh()
        if (-not $process.HasExited) {
            $peak = $process.PeakWorkingSet64
            $private = $process.PrivateMemorySize64
        }
        $drain = $process.StandardOutput.BaseStream.CopyToAsync([IO.Stream]::Null)
        $deadline = [DateTime]::UtcNow.AddSeconds(60)
        while (-not $process.WaitForExit(1)) {
            if ([DateTime]::UtcNow -gt $deadline) { $process.Kill(); throw 'Benchmark timed out.' }
            $process.Refresh()
            if (-not $process.HasExited) {
                $peak = [Math]::Max($peak, $process.PeakWorkingSet64)
                $private = [Math]::Max($private, $process.PrivateMemorySize64)
            }
        }
        $null = $drain.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0) { throw $errors.GetAwaiter().GetResult() }
        [pscustomobject]@{
            Command = $case -join ' '
            Entries = $Entries
            ExeBytes = (Get-Item -LiteralPath $exe).Length
            SampledPeakWorkingSetKiB = [Math]::Round($peak / 1KB)
            SampledPeakPrivateKiB = [Math]::Round($private / 1KB)
        }
        $process.Dispose()
    }
} finally {
    $resolved = [IO.Path]::GetFullPath($fixture)
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if ($resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -and
        [IO.Path]::GetFileName($resolved) -match '^unix-utils-bench-[0-9a-f]{32}$') {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
