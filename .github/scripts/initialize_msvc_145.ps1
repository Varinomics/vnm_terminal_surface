Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-VisualStudioInstallation {
    $instances = @(& $vswhere -latest -products '*' -version '[18.0,19.0)' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json | ConvertFrom-Json)
    if ($LASTEXITCODE -ne 0 -or $instances.Count -ne 1 -or
        !$instances[0].isComplete -or !$instances[0].isLaunchable) {
        throw 'A complete, launchable Visual Studio 2026 C++ installation is required.'
    }
    return $instances[0]
}

function Get-MsvcCompiler($installationPath) {
    $toolset = Get-ChildItem -LiteralPath (Join-Path $installationPath 'VC/Tools/MSVC') -Directory |
        Where-Object Name -Match '^14[.]51[.][0-9]+$' |
        Sort-Object { [version]$_.Name } -Descending |
        Select-Object -First 1
    if ($toolset) {
        $compiler = Join-Path $toolset.FullName 'bin\Hostx64\x64\cl.exe'
        if (Test-Path -LiteralPath $compiler -PathType Leaf) {
            return $compiler
        }
    }
}

$installerDirectory = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer'
$vswhere = Join-Path $installerDirectory 'vswhere.exe'
$installation = Get-VisualStudioInstallation
$expected = Get-MsvcCompiler $installation.installationPath
if (!$expected) {
    if ($env:GITHUB_ACTIONS -cne 'true' -or $env:RUNNER_ENVIRONMENT -cne 'github-hosted') {
        throw 'Automatic MSVC toolset provisioning requires a GitHub-hosted runner.'
    }
    # The hosted image can advance its default compiler. Add the versioned
    # x64 component to the existing installation to keep the hosted SDK.
    $installer = Join-Path $installerDirectory 'setup.exe'
    $arguments = @('modify', '--installPath', ('"{0}"' -f $installation.installationPath),
        '--channelId', 'VisualStudio.18.Release', '--quiet', '--norestart',
        '--add', 'Microsoft.VisualStudio.Component.VC.14.51.x86.x64')
    Write-Host 'Installing the Microsoft MSVC 14.51 x64/x86 component.'
    $setup = Start-Process -FilePath $installer -ArgumentList $arguments `
        -WorkingDirectory $env:RUNNER_TEMP -WindowStyle Hidden -Wait -PassThru
    if ($setup.ExitCode -notin @(0, 3010)) {
        throw "MSVC 14.51 component installation failed with exit code $($setup.ExitCode)."
    }
    if ($setup.ExitCode -eq 3010) {
        Write-Host 'Visual Studio Setup reported a pending restart.'
    }

    $installation = Get-VisualStudioInstallation
    $expected = Get-MsvcCompiler $installation.installationPath
    if (!$expected) {
        throw 'The MSVC 14.51 x64 compiler is still unavailable.'
    }
}

$vcvars = Join-Path $installation.installationPath 'VC\Auxiliary\Build\vcvarsall.bat'
$probe = & cmd.exe /d /c "call `"$vcvars`" x64 -vcvars_ver=14.51 && where cl"
if ($LASTEXITCODE -ne 0) {
    throw 'The MSVC 14.51 x64 developer environment could not be initialized.'
}
$compiler = @($probe | Where-Object { $_ -match '\\cl\.exe$' })
if ($compiler.Count -eq 0 -or $compiler[0] -ine $expected) {
    throw "The developer environment did not select the expected MSVC 14.51 compiler: $expected"
}
Write-Host "Visual Studio $($installation.installationVersion) at $($installation.installationPath)"
Write-Host "MSVC x64 compiler: $expected"
[pscustomobject]@{
    VcVarsAll = $vcvars
    CompilerDirectory = Split-Path -Parent $expected
}
