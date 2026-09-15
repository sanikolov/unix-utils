[CmdletBinding()]
param([string]$Executable = (Join-Path $PSScriptRoot 'build\ls.exe'))

$ErrorActionPreference = 'Stop'
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('unix-ls-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null

function Invoke-Ls([string]$Options) {
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = $Executable
    $start.Arguments = $Options
    $start.WorkingDirectory = $fixture
    $start.UseShellExecute = $false
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = [Text.Encoding]::UTF8
    $process = [Diagnostics.Process]::Start($start)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(30000)) {
        $process.Kill()
        $process.Dispose()
        throw "ls timed out: $Options"
    }
    $result = [pscustomobject]@{ Text = $stdout.GetAwaiter().GetResult(); Error = $stderr.GetAwaiter().GetResult(); Code = $process.ExitCode }
    $process.Dispose()
    return $result
}

function Assert([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

try {
    [IO.File]::WriteAllText((Join-Path $fixture 'alpha'), 'a')
    [IO.File]::WriteAllText((Join-Path $fixture 'beta'), 'bbbb')
    [IO.File]::SetLastWriteTime((Join-Path $fixture 'alpha'), [datetime]'2020-01-01T12:00:00')
    [IO.File]::SetLastWriteTime((Join-Path $fixture 'beta'), [datetime]'2021-01-01T12:00:00')
    $r = Invoke-Ls '-1'
    Assert ($r.Code -eq 0 -and $r.Text -eq "alpha`nbeta`n") 'Default name sorting failed'
    Assert ((Invoke-Ls '-r').Text -eq "beta`nalpha`n") 'Reverse sorting failed'
    Assert ((Invoke-Ls '-t').Text -eq "beta`nalpha`n") 'Time sorting failed'
    Assert ((Invoke-Ls '-S').Text -eq "beta`nalpha`n") 'Size sorting failed'
    Assert ((Invoke-Ls '-lrt').Text -match '(?s)2020-01-01 .*alpha\n.*2021-01-01 .*beta\n') 'Long reverse time sorting failed'
    Assert ((Invoke-Ls '-l1').Text -eq "alpha`nbeta`n") '-1 must override -l'
    [IO.File]::WriteAllText((Join-Path $fixture '.dot'), '')
    [IO.File]::WriteAllText((Join-Path $fixture 'hidden'), '')
    [IO.File]::SetAttributes((Join-Path $fixture 'hidden'), [IO.FileAttributes]::Hidden)
    Assert ((Invoke-Ls '').Text -eq "alpha`nbeta`n") 'Hidden entries leaked'
    $r = Invoke-Ls '-A'
    Assert ($r.Text -eq ".dot`nalpha`nbeta`nhidden`n") '-A filtering failed'
    $r = Invoke-Ls '-la'
    Assert ($r.Text -match '(?m) \.\.$' -and $r.Text -match '(?m) \.dot$') '-la filtering failed'
    New-Item -ItemType Directory -Path (Join-Path $fixture 'sub\nested') | Out-Null
    [IO.File]::WriteAllText((Join-Path $fixture 'sub\nested\leaf'), '')
    $r = Invoke-Ls '-R'
    Assert ($r.Code -eq 0 -and $r.Text -match 'sub\\nested:' -and $r.Text -match '(?m)^leaf$') 'Recursive descent failed'
    Assert ((Invoke-Ls '-dR sub').Text -eq "sub`n") '-d must override -R'
    New-Item -ItemType Directory -Path (Join-Path $fixture '.secret') | Out-Null
    [IO.File]::WriteAllText((Join-Path $fixture '.secret\secret-leaf'), '')
    Assert ((Invoke-Ls '-R').Text -notmatch 'secret-leaf') 'Recursion entered hidden directory'
    Assert ((Invoke-Ls '-RA').Text -match 'secret-leaf') '-RA missed hidden directory'
    $junction = Join-Path $fixture 'sub\loop'
    New-Item -ItemType Junction -Path $junction -Target $fixture | Out-Null
    try {
        $r = Invoke-Ls '-RA'
        Assert ($r.Code -eq 0 -and $r.Text -notmatch 'loop:') 'Recursion followed a junction'
        $r = Invoke-Ls '-l sub'
        Assert ($r.Code -eq 0 -and $r.Text -match '(?m)^l.* loop -> ') 'Junction target missing from directory listing'
        $r = Invoke-Ls '-l sub\loop'
        Assert ($r.Code -eq 0 -and $r.Text -match '(?m)^l.* sub\\loop -> ' -and $r.Text -notmatch 'alpha') 'Long listing followed junction operand'
    } finally { Remove-Item -LiteralPath $junction -Force }
    Add-Type @'
using System.Runtime.InteropServices;
public static class LsTestLinks {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.U1)]
    public static extern bool CreateSymbolicLink(string link, string target, uint flags);
}
'@
    $links = @(
        @('file-link', 'alpha', 0),
        @('dir-link', 'sub', 1),
        @('broken-link', 'missing-target', 0),
        @('broken-dir-link', 'missing-dir', 1)
    )
    $createdLinks = @()
    try {
        foreach ($link in $links) {
            $linkPath = Join-Path $fixture $link[0]
            $ok = [LsTestLinks]::CreateSymbolicLink($linkPath, $link[1], ([uint32]$link[2] -bor 2))
            if (-not $ok) { $ok = [LsTestLinks]::CreateSymbolicLink($linkPath, $link[1], [uint32]$link[2]) }
            if (-not $ok) { throw "Cannot create symlink fixture: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())" }
            $createdLinks += $linkPath
            $r = Invoke-Ls ('-l ' + $link[0])
            Assert ($r.Code -eq 0 -and $r.Text -match ('(?m)^l.* ' + $link[0] + ' -> ' + $link[1] + '$')) "Symlink operand failed: $($link[0])"
            Assert ((Invoke-Ls ('-d ' + $link[0])).Text -eq ($link[0] + "`n")) 'Plain listing annotated link'
        }
        foreach ($option in '-l', '-lU', '-lR') {
            $r = Invoke-Ls $option
            foreach ($link in $links) {
                Assert ($r.Code -eq 0 -and $r.Text -match ('(?m)^l.* ' + $link[0] + ' -> ' + $link[1] + '$')) "Symlink listing failed: $option $($link[0])"
            }
            Assert ($r.Text -notmatch 'dir-link:') 'Recursive listing followed directory symlink'
        }
        Assert ((Invoke-Ls 'dir-link').Text -match '(?m)^nested$') 'Plain directory link operand should list contents'
    } finally {
        foreach ($linkPath in $createdLinks) { Remove-Item -LiteralPath $linkPath -Force }
    }
    [IO.File]::WriteAllText((Join-Path $fixture '-dash'), '')
    Assert ((Invoke-Ls '-- -dash').Text -eq "-dash`n") '-- failed'
    $unicodeName = 'space ' + [char]0x03bb + '.txt'
    [IO.File]::WriteAllText((Join-Path $fixture $unicodeName), '')
    Assert ((Invoke-Ls ('"' + $unicodeName + '"')).Text -eq ($unicodeName + "`n")) 'Unicode/spaced operand failed'
    $r = Invoke-Ls 'missing alpha'
    Assert ($r.Code -eq 1 -and $r.Error -match 'missing' -and $r.Text -match 'alpha') 'Error recovery failed'
    Assert ((Invoke-Ls '-z').Code -eq 2) 'Invalid option exit status failed'
    $sorted = ((Invoke-Ls '-A').Text.TrimEnd("`n").Split("`n") | Sort-Object) -join "`n"
    $streamed = ((Invoke-Ls '-AU').Text.TrimEnd("`n").Split("`n") | Sort-Object) -join "`n"
    Assert ($sorted -eq $streamed) '-U entries differ from sorted listing'
    $bulk = Join-Path $fixture 'bulk'
    New-Item -ItemType Directory -Path $bulk | Out-Null
    for ($i = 999; $i -ge 0; --$i) { [IO.File]::WriteAllText((Join-Path $bulk ('item-{0:D4}' -f $i)), '') }
    $expected = ((0..999 | ForEach-Object { 'item-{0:D4}' -f $_ }) -join "`n") + "`n"
    Assert ((Invoke-Ls 'bulk').Text -eq $expected) 'Large listing/sort/output buffering failed'
    Write-Output 'All ls tests passed.'
} finally {
    $resolved = [IO.Path]::GetFullPath($fixture)
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if ($resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -and
        [IO.Path]::GetFileName($resolved) -match '^unix-ls-test-[0-9a-f]{32}$') {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
