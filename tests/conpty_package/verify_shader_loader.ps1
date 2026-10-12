[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ConptySourceDirectory,
    [Parameter(Mandatory = $true)][string]$PackageDirectory,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [Parameter(Mandatory = $true)][string]$VisualStudioDirectory,
    [string]$QueueCommand = 'queued-build'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ConptySourceDirectory = (Resolve-Path -LiteralPath $ConptySourceDirectory).Path
$PackageDirectory = (Resolve-Path -LiteralPath $PackageDirectory).Path
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use an unused probe output directory.' }
$vcvars = Join-Path $VisualStudioDirectory 'VC/Auxiliary/Build/vcvarsall.bat'
if ($vcvars -match '["&|<>^]') { throw 'Unsupported toolchain path.' }
$environmentLines = & $env:ComSpec /d /c "call `"$vcvars`" x64 -vcvars_ver=14.51 && set"
if ($LASTEXITCODE -ne 0) { throw 'The VS2026 developer environment failed.' }
foreach ($line in $environmentLines) {
    if ($line -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
    }
}
$compiler = Join-Path $env:VCToolsInstallDir 'bin/Hostx64/x64/cl.exe'
if (-not $compiler.StartsWith((Join-Path $VisualStudioDirectory 'VC/Tools/MSVC/14.51.'),
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The probe requires the selected VS2026 v145 toolchain.'
}
$generated = Join-Path $ConptySourceDirectory 'obj/vnm-conpty-release/shared-shaders'
$receipt = Get-Content -LiteralPath (Join-Path $PackageDirectory 'conpty-source-build.json') -Raw | ConvertFrom-Json
$loaderHash = (Get-FileHash -LiteralPath (Join-Path $generated 'conpty-shared-shaders.h') -Algorithm SHA256).Hash
if ($loaderHash -ine $receipt.producer.shader_loader_sha256) {
    throw 'The generated probe loader differs from the packaged host producer.'
}
$wil = Join-Path $ConptySourceDirectory 'packages/Microsoft.Windows.ImplementationLibrary.1.0.240122.1/include'
$hostDirectory = Join-Path $OutputDirectory 'x64'
$shaderDirectory = Join-Path $OutputDirectory 'conpty-shaders'
$workingDirectory = Join-Path $OutputDirectory 'unrelated-cwd'
[void](New-Item -ItemType Directory -Path $hostDirectory, $shaderDirectory, $workingDirectory)
Copy-Item -LiteralPath (Join-Path $PackageDirectory 'build/native/runtimes/conpty-shaders') -Destination $workingDirectory -Recurse
Copy-Item -Path (Join-Path $PackageDirectory 'build/native/runtimes/conpty-shaders/*.dxbc') -Destination $shaderDirectory
$program = Join-Path $hostDirectory 'shader_loader_probe.exe'
$arguments = @('/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/O2', '/MT', '/DNDEBUG',
    '/DUNICODE', '/D_UNICODE', '/D_WIN32_WINNT=0x0A00', '/guard:cf',
    "/I$generated", "/I$wil", (Join-Path $PSScriptRoot 'shader_loader_probe.cpp'),
    "/Fo$(Join-Path $OutputDirectory 'probe.obj')", "/Fe$program", '/link',
    '/DEFAULTLIB:ucrt.lib', '/NODEFAULTLIB:libucrt.lib', '/DEBUG:NONE', '/NOCOFFGRPINFO',
    '/CGTHREADS:1', '/DYNAMICBASE', '/NXCOMPAT', '/GUARD:CF', '/CETCOMPAT')
& $QueueCommand --slots 1 -- $compiler @arguments 2>&1 |
    Tee-Object -FilePath (Join-Path $OutputDirectory 'build.log')
if ($LASTEXITCODE -ne 0) { throw 'The shared-loader probe build failed.' }
Push-Location $workingDirectory
try {
    & $program
    if ($LASTEXITCODE -ne 0) { throw 'The valid shared shader/WARP probe failed.' }
    $names = @('shader_vs', 'shader_ps', 'custom_shader_vs', 'custom_shader_ps')
    for ($index = 0; $index -lt $names.Count; ++$index) {
        $path = Join-Path $shaderDirectory "$($names[$index]).dxbc"
        $bytes = [IO.File]::ReadAllBytes($path)
        try {
            Remove-Item -LiteralPath $path
            & $program missing $index
            if ($LASTEXITCODE -ne 0) { throw "The loader did not reject the missing resource $path." }
            $damaged = [byte[]]$bytes.Clone()
            $damaged[-1] = $damaged[-1] -bxor 1
            [IO.File]::WriteAllBytes($path, $damaged)
            & $program corrupt $index
            if ($LASTEXITCODE -ne 0) { throw "The loader did not reject the corrupt resource $path." }
        } finally {
            [IO.File]::WriteAllBytes($path, $bytes)
        }
    }
} finally {
    Pop-Location
}
Write-Host 'Shared shader loader: WARP, unrelated CWD, and all four missing/corrupt cases passed.'
