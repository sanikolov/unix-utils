[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    # Use the x64 tools even when launched from an ordinary PowerShell prompt.
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $installation = & $vswhere -latest -prerelease -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($installation) {
            $devcmd = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
            $environment = & $env:ComSpec /d /c "call `"$devcmd`" -no_logo -arch=x64 -host_arch=x64 >nul && set"
            if ($LASTEXITCODE -ne 0) { throw 'Visual Studio environment setup failed.' }
            foreach ($line in $environment) {
                if ($line -match '^([^=]+)=(.*)$') {
                    [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
                }
            }
        }
    }
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw 'Install Visual Studio Build Tools with Desktop development with C++, or run from an x64 Developer PowerShell.'
    }
    New-Item -ItemType Directory -Force build | Out-Null
    foreach ($utility in 'ls', 'du') {
        & cl.exe /nologo /TC /std:c11 /W4 /WX /O1 /Os /Oi /GL /GS- /MD /Zl "/Fobuild\$utility.obj" "/Febuild\$utility.exe" "$utility.c" /link /NODEFAULTLIB /ENTRY:mainCRTStartup /SUBSYSTEM:CONSOLE /MACHINE:X64 /LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO /DYNAMICBASE /NXCOMPAT kernel32.lib
        if ($LASTEXITCODE -ne 0) { throw "cl.exe failed for $utility with exit code $LASTEXITCODE" }
    }
    Get-Item build\ls.exe, build\du.exe | Select-Object FullName, Length
} finally {
    Pop-Location
}
