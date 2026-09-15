[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('unix-glob-test-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($fixture) | Out-Null
$script:checks = 0

function Invoke-Utility([string]$Utility, [string]$Arguments, [string]$InputText = '') {
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = Join-Path $PSScriptRoot "build\$Utility.exe"
    $start.Arguments = $Arguments
    $start.WorkingDirectory = $fixture
    $start.UseShellExecute = $false
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.RedirectStandardInput = $true
    $start.StandardOutputEncoding = [Text.Encoding]::UTF8
    $process = [Diagnostics.Process]::Start($start)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if ($InputText) {
        $bytes = [Text.Encoding]::UTF8.GetBytes($InputText)
        $process.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length)
    }
    $process.StandardInput.Close()
    if (-not $process.WaitForExit(30000)) { $process.Kill(); $process.Dispose(); throw 'Utility timed out.' }
    $result = [pscustomobject]@{ Text = $stdout.GetAwaiter().GetResult(); Error = $stderr.GetAwaiter().GetResult(); Code = $process.ExitCode }
    $process.Dispose()
    return $result
}

function Assert([bool]$Condition, [string]$Message) {
    ++$script:checks
    if (-not $Condition) { throw $Message }
}

function Same-As-Explicit([string]$Utility, [string]$Pattern, [string]$Explicit) {
    $actual = Invoke-Utility $Utility $Pattern
    $expected = Invoke-Utility $Utility $Explicit
    Assert ($actual.Code -eq 0 -and $expected.Code -eq 0 -and $actual.Text -ceq $expected.Text) "$Utility $Pattern differs from explicit operands.`nActual: $($actual.Text)$($actual.Error)`nExpected: $($expected.Text)$($expected.Error)"
}

try {
    foreach ($dir in 'alpha', 'beta\subdir', '.secret', 'space dir', 'empty') {
        [IO.Directory]::CreateDirectory((Join-Path $fixture $dir)) | Out-Null
    }
    foreach ($name in 'alpha\foo.txt', 'alpha\food.bin', 'alpha\other', 'beta\foo.txt', 'beta\fool.txt', 'beta\subdir\child', '.secret\foo.txt', 'alpha\.hidden', 'space dir\foo file', 'rootfile', '-operand', 'literal[abc]') {
        [IO.File]::WriteAllText((Join-Path $fixture $name), 'contents')
    }
    $fooOperands = 'alpha/foo.txt alpha/food.bin beta/foo.txt beta/fool.txt "space dir/foo file"'
    Same-As-Explicit du '-s */foo*' ('-s ' + $fooOperands)
    Same-As-Explicit du '-bsc */foo*' ('-bsc ' + $fooOperands)
    Same-As-Explicit ls '-lrt */*' '-lrt alpha/foo.txt alpha/food.bin alpha/other beta/foo.txt beta/fool.txt beta/subdir "space dir/foo file"'
    foreach ($utility in 'ls', 'du') {
        $flags = if ($utility -eq 'ls') { '-d1' } else { '-bs' }
        Same-As-Explicit $utility "$flags alpha/foo.?xt" "$flags alpha/foo.txt"
        Same-As-Explicit $utility "$flags [ab]*/foo.txt" "$flags alpha/foo.txt beta/foo.txt"
        Same-As-Explicit $utility "$flags [!a]*/foo.txt" "$flags beta/foo.txt"
        Same-As-Explicit $utility "$flags */sub*/child" "$flags beta/subdir/child"
        Same-As-Explicit $utility "$flags */sub*/" "$flags beta/subdir/"
        Same-As-Explicit $utility "$flags */.hidden" "$flags alpha/.hidden"
        Same-As-Explicit $utility "$flags .secret/foo*" "$flags .secret/foo.txt"
        Same-As-Explicit $utility "$flags .*/foo*" "$flags .secret/foo.txt"
        Same-As-Explicit $utility "$flags alpha\foo*" "$flags alpha\foo.txt alpha\food.bin"
        Same-As-Explicit $utility "$flags ./alpha/foo*" "$flags ./alpha/foo.txt ./alpha/food.bin"
        Same-As-Explicit $utility "$flags alpha/foo* alpha/foo*" "$flags alpha/foo.txt alpha/food.bin alpha/foo.txt alpha/food.bin"
        Same-As-Explicit $utility "$flags -- -oper*" "$flags -- -operand"
        Same-As-Explicit $utility "$flags literal[[]abc]" "$flags literal[abc]"
        $actual = Invoke-Utility $utility "$flags absent*/foo* alpha/foo.txt"
        Assert ($actual.Code -eq 1 -and $actual.Error -match 'absent\*/foo\*' -and $actual.Text -match 'alpha/foo.txt') "$utility unmatched pattern lost or stopped subsequent operands"
        $actual = Invoke-Utility $utility "$flags rootfile/*"
        Assert ($actual.Code -eq 1) "$utility treated a file as an intermediate directory"
        $absolute = $fixture.Replace('\', '/')
        Same-As-Explicit $utility "$flags `"$absolute/alpha/foo*`"" "$flags `"$absolute/alpha/foo.txt`" `"$absolute/alpha/food.bin`""
    }
    Same-As-Explicit du '-bs --exclude=foo* alpha/*' '-bs --exclude=foo* alpha/foo.txt alpha/food.bin alpha/other'
    $literalInput = Invoke-Utility du '-bs --files0-from=-' "alpha/foo*`0"
    Assert ($literalInput.Code -eq 1 -and $literalInput.Text -eq '') '--files0-from names were globbed'
    $extended = '\\?\' + $fixture
    Same-As-Explicit du "-bs `"$extended\alpha\foo*`"" "-bs `"$extended\alpha\foo.txt`" `"$extended\alpha\food.bin`""
    $unicode = 'alpha\' + [char]0x03bb + '.txt'
    [IO.File]::WriteAllText((Join-Path $fixture $unicode), 'unicode')
    Same-As-Explicit ls '-d1 alpha/?.txt' ('-d1 alpha/' + [char]0x03bb + '.txt')
    Same-As-Explicit ls '-d1 alpha/*.*' ('-d1 alpha/foo.txt alpha/food.bin alpha/' + [char]0x03bb + '.txt')
    $caseCheck = Invoke-Utility ls '-d1 alpha/FOO*'
    Assert ($caseCheck.Code -eq 1) 'Glob matching used case-insensitive DOS semantics'
    $hiddenFile = Join-Path $fixture 'alpha\hidden-attribute'
    [IO.File]::WriteAllText($hiddenFile, 'hidden')
    [IO.File]::SetAttributes($hiddenFile, [IO.FileAttributes]::Hidden)
    Same-As-Explicit ls '-d1 alpha/hidden-*' '-d1 alpha/hidden-attribute'
    $deep = Join-Path $fixture 'deep'
    for ($i = 0; $i -lt 25; ++$i) { $deep = Join-Path $deep 'long-directory' }
    [IO.Directory]::CreateDirectory((Join-Path $deep 'child')) | Out-Null
    [IO.File]::WriteAllText((Join-Path $deep 'child\foo.txt'), 'deep')
    Same-As-Explicit du "-bs `"$deep/*/foo*`"" "-bs `"$deep/child/foo.txt`""
    New-Item -ItemType HardLink -Path (Join-Path $fixture 'alpha\foo-hard') -Target (Join-Path $fixture 'beta\foo.txt') | Out-Null
    Same-As-Explicit du '-bsc */foo*' '-bsc alpha/foo-hard alpha/foo.txt alpha/food.bin beta/foo.txt beta/fool.txt "space dir/foo file"'
    Same-As-Explicit du '-bscl */foo*' '-bscl alpha/foo-hard alpha/foo.txt alpha/food.bin beta/foo.txt beta/fool.txt "space dir/foo file"'
    $junction = Join-Path $fixture 'alias'
    New-Item -ItemType Junction -Path $junction -Target (Join-Path $fixture 'alpha') | Out-Null
    try { Same-As-Explicit du '-bsc a*/foo*' '-bsc alias/foo-hard alias/foo.txt alias/food.bin alpha/foo-hard alpha/foo.txt alpha/food.bin' }
    finally { Remove-Item -LiteralPath $junction -Force }
    Write-Output "All $script:checks glob checks passed."
} finally {
    $resolved = [IO.Path]::GetFullPath($fixture)
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if ($resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -and
        [IO.Path]::GetFileName($resolved) -match '^unix-glob-test-[0-9a-f]{32}$') {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
