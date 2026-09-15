[CmdletBinding()]
param(
    [string]$Executable = (Join-Path $PSScriptRoot 'build\du.exe'),
    [string]$Reference = 'C:\Program Files\Git\usr\bin\du.exe'
)

$ErrorActionPreference = 'Stop'
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('unix-du-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null
$script:checks = 0

function Invoke-Du([string]$Options, [string]$Program = $Executable, [string]$InputText = '', [hashtable]$Variables = @{}) {
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = $Program
    $start.Arguments = $Options
    $start.WorkingDirectory = $fixture
    $start.UseShellExecute = $false
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.RedirectStandardInput = $true
    $start.StandardOutputEncoding = [Text.Encoding]::UTF8
    foreach ($key in 'DU_BLOCK_SIZE', 'BLOCK_SIZE', 'BLOCKSIZE', 'POSIXLY_CORRECT', 'TIME_STYLE') { $start.EnvironmentVariables.Remove($key) }
    $start.EnvironmentVariables['LC_ALL'] = 'C'
    $start.EnvironmentVariables['TZ'] = 'UTC0'
    foreach ($key in $Variables.Keys) { $start.EnvironmentVariables[$key] = $Variables[$key] }
    $process = [Diagnostics.Process]::Start($start)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if ($InputText) {
        $bytes = [Text.Encoding]::UTF8.GetBytes($InputText)
        $process.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length)
    }
    $process.StandardInput.Close()
    if (-not $process.WaitForExit(30000)) { $process.Kill(); $process.Dispose(); throw "du timed out: $Options" }
    $result = [pscustomobject]@{ Text = $stdout.GetAwaiter().GetResult(); Error = $stderr.GetAwaiter().GetResult(); Code = $process.ExitCode }
    $process.Dispose()
    return $result
}

function Assert([bool]$Condition, [string]$Message) {
    ++$script:checks
    if (-not $Condition) { throw $Message }
}

function Compare-Gnu([string]$Options, [string]$InputText = '', [hashtable]$Variables = @{}) {
    $actual = Invoke-Du $Options $Executable $InputText $Variables
    $expected = Invoke-Du $Options $Reference $InputText $Variables
    # Directory visitation order is filesystem-defined; compare record contents.
    $a = ($actual.Text.Split("`n") | Sort-Object) -join "`n"
    $e = ($expected.Text.Split("`n") | Sort-Object) -join "`n"
    Assert ($actual.Code -eq $expected.Code -and $a -ceq $e) "GNU mismatch for $Options`nActual [$($actual.Code)]: $a`nExpected [$($expected.Code)]: $e`nErrors: $($actual.Error)"
}

try {
    New-Item -ItemType Directory -Path (Join-Path $fixture 'data\sub'), (Join-Path $fixture 'empty') | Out-Null
    foreach ($entry in @(@('data\a', 3), @('data\b', 1100), @('data\sub\c', 10001), @('data\.hidden', 7), @('data\space name', 21))) {
        [IO.File]::WriteAllBytes((Join-Path $fixture $entry[0]), (New-Object byte[] $entry[1]))
    }
    $r = Invoke-Du '-b data/a'
    Assert ($r.Code -eq 0 -and $r.Text -eq "3`tdata/a`n") 'Apparent file size failed'
    Assert ((Invoke-Du '-b -d0 data').Text -match "^11132`tdata\n$") 'Directory aggregation failed'
    Assert ((Invoke-Du '--inodes -s data').Text -eq "7`tdata`n") 'Identity count failed'
    Assert ((Invoke-Du '-b -S -s data').Text -eq "1131`tdata`n") 'Separate-directory total failed'
    Assert ((Invoke-Du '-b --exclude=*.hidden -s data').Text -eq "11125`tdata`n") 'Exclusion failed'
    Assert ((Invoke-Du '-b --max-depth=0 data').Text -eq (Invoke-Du '-bs data').Text) 'Depth zero differs from summary'
    $r = Invoke-Du '-b -0 data/a'
    Assert ($r.Text -eq "3`tdata/a`0") 'NUL output failed'
    Assert ((Invoke-Du '--files0-from=- -bc' $Executable "data/a`0data/b`0").Text -eq "3`tdata/a`n1100`tdata/b`n1103`ttotal`n") 'Streaming operand input failed'
    Assert ((Invoke-Du '--files0-from=-' $Executable '').Text -eq '') 'Empty input must not default to current directory'
    Assert ((Invoke-Du '--files0-from=- data').Code -eq 1) 'Files-from/operand conflict accepted'
    Assert ((Invoke-Du '-sa data').Code -eq 1) 'Summary/all conflict accepted'
    Assert ((Invoke-Du '-s -d1 data').Code -eq 1) 'Summary/depth conflict accepted'
    Assert ((Invoke-Du '-d-1 data').Code -eq 1) 'Negative depth accepted'
    Assert ((Invoke-Du '--unknown').Code -eq 1) 'Unknown option accepted'
    Assert ((Invoke-Du '-b missing data/a').Code -eq 1) 'Missing operand error not propagated'
    [IO.File]::WriteAllBytes((Join-Path $fixture 'names'), [Text.Encoding]::UTF8.GetBytes("data/a`0data/b`0"))
    [IO.File]::WriteAllText((Join-Path $fixture 'patterns'), "sub`nb`n", (New-Object Text.UTF8Encoding $false))
    New-Item -ItemType HardLink -Path (Join-Path $fixture 'hard') -Target (Join-Path $fixture 'data\a') | Out-Null
    Assert ((Invoke-Du '-bc data/a hard').Text -eq "3`tdata/a`n3`ttotal`n") 'Hard link counted twice'
    Assert ((Invoke-Du '-blc data/a hard').Text -eq "3`tdata/a`n3`thard`n6`ttotal`n") '-l failed to count links'
    if (Test-Path -LiteralPath $Reference) {
        foreach ($options in @(
            '-b data/a', '-ab data', '-sb data', '-b -d1 data', '-b -d0 data', '-bSc data',
            '-b --exclude=sub data', '-b --exclude=*[ab] data', '-b --exclude=*[!c] data',
            '-b --exclude=*[[:digit:]] data', '-b --exclude=data/sub/* data', '-b -X patterns data',
            '-b -t1000 data', '-ab -t-1100 data', '-b -t999999 -c data', '-b -t0 data', '-ab -t-0 data',
            '-b -B1K data', '-b -BM data', '-b -BKB data', '-b -BKiB data', '-b -k data', '-b -m data',
            '-bh data', '-b --si data', '-b -Bhuman-readable data', '-b --block-size=si data',
            '--inodes data', '--inodes -a data', '--inodes -S data', '--inodes -sc data', '--inodes -b data',
            '--inodes -h data', '--inodes -t3 data', '-bc data/a hard', '-blc data/a hard',
            '-bc data data', '-blc data data', '-b --files0-from=names', '-b --summ data',
            '-b -- data/a', '-b -x data', '-b --apparent-size data', '-b data/a -c'
        )) { Compare-Gnu $options }
        Compare-Gnu '-b --files0-from=-' "data/a`0data/b`0"
        Compare-Gnu '-b -X - data' "sub`nb`n"
        Compare-Gnu '--apparent-size data/b' '' @{ DU_BLOCK_SIZE = '100' }
        Compare-Gnu '--apparent-size data/b' '' @{ BLOCK_SIZE = '100' }
        Compare-Gnu '--apparent-size data/b' '' @{ BLOCKSIZE = '100' }
        Compare-Gnu '--apparent-size data/b' '' @{ POSIXLY_CORRECT = '1' }
        Compare-Gnu '--apparent-size -B10 data/b' '' @{ DU_BLOCK_SIZE = '100' }
        [IO.File]::SetLastWriteTimeUtc((Join-Path $fixture 'data\a'), ([datetime]'2021-01-01T12:34:56').AddTicks(1234567))
        foreach ($style in 'full-iso', 'long-iso', 'iso', '+%Y %m %d %H %M %S %N %z', '+%a %A %b %B %c %x %X', '+%C %D %e %F %g %G %h %I %j %k %l %p %P %q %r %R %s %T %u %U %V %w %W %y', '+%-d %_m %0e %^B %% %:z %::z %:::z', '+%3N %6N %12N') {
            Compare-Gnu ('-b --time --time-style="' + $style + '" data/a')
        }
        Compare-Gnu '-b --time data/a' '' @{ TIME_STYLE = 'iso' }
        Compare-Gnu '-b --time=a --time-style=l data/a'
        Compare-Gnu '-b --time --time-style=locale data/a'
        Compare-Gnu '-b --time --time-style=+%+5Y data/a'
        Compare-Gnu '-b -B"''1" data'
    } else { Write-Warning 'GNU comparison tests skipped: pass -Reference with GNU du.exe.' }
    $junction = Join-Path $fixture 'data\sub\loop'
    New-Item -ItemType Junction -Path $junction -Target (Join-Path $fixture 'data') | Out-Null
    try {
        $r = Invoke-Du '-bL data'
        Assert ($r.Code -eq 0 -and $r.Text -notmatch 'loop/') 'Logical traversal failed to prune cycle'
        Assert ((Invoke-Du '-bP data/sub/loop').Text -notmatch 'loop/') 'Physical traversal followed junction'
        Assert ((Invoke-Du '-bH data/sub/loop').Text -match 'loop/sub') 'Operand dereference failed'
    } finally { Remove-Item -LiteralPath $junction -Force }
    if ([IO.Path]::GetPathRoot($fixture) -ne [IO.Path]::GetPathRoot($PSScriptRoot)) {
        $cross = Join-Path $fixture 'data\cross-volume'
        New-Item -ItemType Junction -Path $cross -Target $PSScriptRoot | Out-Null
        try {
            Assert ((Invoke-Du '--inodes -sxL data').Text -eq "7`tdata`n") '-x followed a link onto another volume'
        } finally { Remove-Item -LiteralPath $cross -Force }
    }
    $unicodeName = 'unicode-' + [char]0x03bb
    [IO.File]::WriteAllBytes((Join-Path $fixture $unicodeName), (New-Object byte[] 5))
    Assert ((Invoke-Du ('-b ' + $unicodeName)).Text -eq "5`t$unicodeName`n") 'Unicode filename failed'
    Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class DuSparseFixture {
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool DeviceIoControl(SafeFileHandle h, uint code, IntPtr input, uint inputSize,
        IntPtr output, uint outputSize, out uint returned, IntPtr overlapped);
    public static void Create(string path) {
        using (var file = new FileStream(path, FileMode.CreateNew, FileAccess.ReadWrite)) {
            uint returned;
            if (!DeviceIoControl(file.SafeFileHandle, 0x900c4, IntPtr.Zero, 0, IntPtr.Zero, 0, out returned, IntPtr.Zero))
                throw new Win32Exception();
            file.SetLength(5L * 1024 * 1024 * 1024);
            file.Position = file.Length - 1;
            file.WriteByte(1);
            file.Flush(true);
        }
    }
}
'@
    [DuSparseFixture]::Create((Join-Path $fixture 'sparse'))
    $r = Invoke-Du '-B1 sparse'
    $allocated = [long]$r.Text.Split("`t")[0]
    Assert ($r.Code -eq 0 -and $allocated -gt 0 -and $allocated -lt 1MB) "Sparse allocation accounting failed: $allocated bytes, exit $($r.Code), $($r.Error)"
    Assert ((Invoke-Du '-b sparse').Text -eq "5368709120`tsparse`n") '64-bit apparent size failed'
    $deep = Join-Path $fixture 'deep'
    [IO.Directory]::CreateDirectory($deep) | Out-Null
    for ($i = 0; $i -lt 40; ++$i) { $deep = Join-Path $deep 'directory'; [IO.Directory]::CreateDirectory($deep) | Out-Null }
    Assert ((Invoke-Du '--inodes -s deep').Text -eq "41`tdeep`n") 'Deep/long path traversal failed'
    $extended = '\\?\' + (Join-Path $fixture 'deep')
    Assert ((Invoke-Du ('--inodes -s "' + $extended + '"')).Text -eq "41`t$extended`n") 'Extended path traversal failed'
    $bulk = Join-Path $fixture 'bulk'
    [IO.Directory]::CreateDirectory($bulk) | Out-Null
    for ($i = 0; $i -lt 1200; ++$i) { [IO.File]::WriteAllBytes((Join-Path $bulk "file-$i"), [byte[]]@()) }
    Assert ((Invoke-Du '--inodes -sc bulk bulk').Text -eq "1201`tbulk`n1201`ttotal`n") 'Large identity set/deduplication failed'
    Write-Output "All $script:checks du checks passed."
} finally {
    $resolved = [IO.Path]::GetFullPath($fixture)
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if ($resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -and
        [IO.Path]::GetFileName($resolved) -match '^unix-du-test-[0-9a-f]{32}$') {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
