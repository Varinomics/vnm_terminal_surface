[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$SourceDirectory,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [Parameter(Mandatory = $true)][string]$VisualStudioDirectory,
    [Parameter(Mandatory = $true)][string]$VCToolsVersion,
    [Parameter(Mandatory = $true)][string]$WindowsSdkVersion,
    [ValidateRange(1, 4)][int]$Slots = 2,
    [string]$QueueCommand = 'queued-build'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$sourceCommit = '5a830b2bf7c053d5c7ac22208fe5a346cb5dd3dc'
$packageVersion = '1.24.260710001'
$wilVersion = '1.0.240122.1'
$wilSha256 = '2762643e7865c42ac60d809fe951a813ae10960141e56bb74ad28843f2d6b5c1'
$SourceDirectory = (Resolve-Path -LiteralPath $SourceDirectory).Path
$VisualStudioDirectory = (Resolve-Path -LiteralPath $VisualStudioDirectory).Path
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $OutputDirectory) {
    throw 'The ConPTY output directory must be unused.'
}
$revision = & git -C $SourceDirectory rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $revision -cne $sourceCommit) {
    throw "ConPTY must use the adopted upstream source commit $sourceCommit."
}
$changes = & git -C $SourceDirectory status --porcelain --untracked-files=no
if ($LASTEXITCODE -ne 0 -or $changes) {
    throw 'ConPTY source has tracked modifications; use a clean isolated checkout.'
}
$vcvars = Join-Path $VisualStudioDirectory 'VC/Auxiliary/Build/vcvarsall.bat'
$msbuild = Join-Path $VisualStudioDirectory 'MSBuild/Current/Bin/amd64/MSBuild.exe'
$compilerRoot = Join-Path $VisualStudioDirectory "VC/Tools/MSVC/$VCToolsVersion"
$vcpkgRoot = Join-Path $VisualStudioDirectory 'VC/vcpkg'
$vcpkg = Join-Path $vcpkgRoot 'vcpkg.exe'
foreach ($path in @($vcvars, $msbuild, $vcpkg,
        "$compilerRoot/bin/Hostx64/x64/cl.exe", "$compilerRoot/bin/Hostx64/arm64/cl.exe",
        "$compilerRoot/lib/arm64/libcmt.lib")) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required native toolchain input is absent: $path"
    }
}
if ($vcvars -match '["&|<>^]' -or $VCToolsVersion -notmatch '^14[.]51[.][0-9]+$') {
    throw 'ConPTY requires the selected VS2026 v145 toolchain.'
}
$environmentLines = & $env:ComSpec /d /c "call `"$vcvars`" x64 -vcvars_ver=14.51 && set"
if ($LASTEXITCODE -ne 0) { throw 'The Visual Studio developer environment failed.' }
foreach ($line in $environmentLines) {
    if ($line -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
    }
}
if ([IO.Path]::GetFullPath($env:VCToolsInstallDir).TrimEnd('\') -ine $compilerRoot.TrimEnd('\')) {
    throw 'The developer environment selected a different MSVC toolchain.'
}
$env:VCPKG_MAX_CONCURRENCY = "$Slots"
$env:VCPKG_VISUAL_STUDIO_PATH = $VisualStudioDirectory
$env:VCPKG_BINARY_SOURCES = 'clear'
$env:VCPKG_ROOT = $vcpkgRoot
$env:CL_MPCount = '1'
$env:VCPKG_KEEP_ENV_VARS = 'VCPKG_MAX_CONCURRENCY'
$buildEvidence = Join-Path $SourceDirectory 'obj/vnm-conpty-release'
[void](New-Item -ItemType Directory -Path $buildEvidence -Force)
$policy = Join-Path $PSScriptRoot 'conpty-release.targets'
$triplets = Join-Path $PSScriptRoot 'conpty-triplets'
$sourcePatch = Join-Path $PSScriptRoot 'conpty-shared-shaders.patch'
$shaderPreparation = Join-Path $PSScriptRoot 'prepare_conpty_shaders.ps1'
$shaderLoader = Join-Path $PSScriptRoot 'conpty-shared-shaders.h'
$producerInputs = @($PSCommandPath, $policy, "$triplets/x64-windows-static.cmake",
    "$triplets/arm64-windows-static.cmake", "$triplets/release-flags.cmake",
    $sourcePatch, $shaderPreparation, $shaderLoader)
$producerHashes = @{}
foreach ($inputPath in $producerInputs) {
    $producerHashes[$inputPath] = (Get-FileHash -LiteralPath $inputPath -Algorithm SHA256).Hash
}

function Invoke-QueuedNative {
    param([string]$Program, [string[]]$Arguments, [string]$LogName, [int]$Width = $Slots)
    @{ program = $Program; arguments = $Arguments; slots = $Width } |
        ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $buildEvidence "$LogName.command.json") -Encoding utf8
    & $QueueCommand --slots $Width -- $Program @Arguments 2>&1 |
        Tee-Object -FilePath (Join-Path $buildEvidence $LogName)
    if ($LASTEXITCODE -ne 0) {
        throw "Native build failed: $Program (see $LogName)."
    }
}

function Assert-ConptyImage {
    param([string]$Path)
    $bytes = [IO.File]::ReadAllBytes($Path)
    $pe = [BitConverter]::ToInt32($bytes, 0x3c)
    if ([BitConverter]::ToUInt32($bytes, $pe) -ne 0x4550) { throw "Invalid PE input: $Path" }
    $optional = $pe + 24
    $sectionCount = [BitConverter]::ToUInt16($bytes, $pe + 6)
    $sectionTable = $optional + [BitConverter]::ToUInt16($bytes, $pe + 20)
    $directory = $optional + $(if ([BitConverter]::ToUInt16($bytes, $optional) -eq 0x20b) { 112 } else { 96 })
    $debugRva = [BitConverter]::ToUInt32($bytes, $directory + 48)
    $debugSize = [BitConverter]::ToUInt32($bytes, $directory + 52)
    if ([BitConverter]::ToUInt32($bytes, $pe + 12) -ne 0 -or
        [BitConverter]::ToUInt32($bytes, $pe + 16) -ne 0) {
        throw "ConPTY retains COFF symbols: $Path"
    }
    $debugOffset = $null
    for ($index = 0; $index -lt $sectionCount; ++$index) {
        $section = $sectionTable + 40 * $index
        $rva = [BitConverter]::ToUInt32($bytes, $section + 12)
        $size = [BitConverter]::ToUInt32($bytes, $section + 16)
        if ($debugSize -gt 0 -and $debugRva -ge $rva -and $debugRva + $debugSize -le $rva + $size) {
            $debugOffset = [BitConverter]::ToUInt32($bytes, $section + 20) + $debugRva - $rva
        }
    }
    if ($debugSize % 28 -ne 0 -or ($debugSize -gt 0 -and $null -eq $debugOffset)) {
        throw "Invalid ConPTY debug directory: $Path"
    }
    $cet = $false
    for ($index = 0; $index -lt $debugSize; $index += 28) {
        $entry = $debugOffset + $index
        $type = [BitConverter]::ToUInt32($bytes, $entry + 12)
        if ($type -notin @(16, 20)) {
            throw "ConPTY retains debugging metadata type ${type}: $Path"
        }
        if ($type -eq 20) {
            $data = [BitConverter]::ToUInt32($bytes, $entry + 24)
            $cet = ([BitConverter]::ToUInt32($bytes, $data) -band 1) -ne 0
        }
    }
    $characteristics = [BitConverter]::ToUInt16($bytes, $optional + 70)
    if (($characteristics -band 0x4140) -ne 0x4140 -or
        ([BitConverter]::ToUInt16($bytes, $pe + 4) -eq 0x8664 -and -not $cet)) {
        throw "ConPTY must retain ASLR, NX, CFG and x64 CET: $Path"
    }
}

# Only WIL is a NuGet input to these native projects. The Terminal UI, tests,
# PGO service and managed packages are deliberately not restored or built.
$wilArchive = Join-Path $buildEvidence "wil-$wilVersion.zip"
if (-not (Test-Path -LiteralPath $wilArchive)) {
    Invoke-WebRequest -Uri "https://api.nuget.org/v3-flatcontainer/microsoft.windows.implementationlibrary/$wilVersion/microsoft.windows.implementationlibrary.$wilVersion.nupkg" -OutFile $wilArchive
}
if ((Get-FileHash -LiteralPath $wilArchive -Algorithm SHA256).Hash -ine $wilSha256) {
    throw 'The WIL NuGet input differs from the adopted source dependency.'
}
$wilDirectory = Join-Path $SourceDirectory "packages/Microsoft.Windows.ImplementationLibrary.$wilVersion"
Expand-Archive -LiteralPath $wilArchive -DestinationPath $wilDirectory -Force

. $shaderPreparation
$shaderDirectory = Join-Path $buildEvidence 'shared-shaders'
$fxc = Join-Path $env:WindowsSdkDir "bin/$WindowsSdkVersion/x64/fxc.exe"
if (-not (Test-Path -LiteralPath $fxc -PathType Leaf)) { throw "Missing selected SDK shader compiler: $fxc" }
$sharedResources = @(Initialize-ConptySharedShaders $SourceDirectory $shaderDirectory $fxc)
$patchedSource = Join-Path $SourceDirectory 'src/renderer/atlas/BackendD3D.cpp'
$originalSource = [IO.File]::ReadAllBytes($patchedSource)
& git -C $SourceDirectory apply --ignore-space-change --check $sourcePatch
if ($LASTEXITCODE -ne 0) { throw 'The shared-shader patch does not match the frozen source.' }
& git -C $SourceDirectory apply --ignore-space-change $sourcePatch
if ($LASTEXITCODE -ne 0) { throw 'Could not apply the shared-shader source patch.' }
try {

foreach ($architecture in @('x64', 'arm64')) {
    $installed = Join-Path $SourceDirectory "obj/$architecture/vcpkg"
    Invoke-QueuedNative $vcpkg @('install', '--triplet', "$architecture-windows-static",
        '--host-triplet', 'x64-windows-static', '--overlay-triplets', $triplets,
        '--x-manifest-root', $SourceDirectory, '--x-install-root', $installed) "vcpkg-$architecture.log"
    foreach ($cache in Get-ChildItem -LiteralPath "$installed/vcpkg/blds" -Filter CMakeCache.txt -Recurse) {
        $cacheLines = Get-Content -LiteralPath $cache.FullName
        foreach ($entry in $cacheLines | Where-Object { $_ -match '^CMAKE_(?:C|CXX)_COMPILER:[^=]+=' }) {
            $compiler = ($entry -split '=', 2)[1]
            if (-not [IO.Path]::GetFullPath($compiler).StartsWith("$compilerRoot\", [StringComparison]::OrdinalIgnoreCase)) {
                throw "ConPTY dependency selected a different compiler in $($cache.FullName)."
            }
        }
        foreach ($entry in $cacheLines | Where-Object { $_ -match '^CMAKE_(?:C|CXX)_FLAGS(?:_RELEASE)?:' }) {
            if ($entry -cmatch '/Z[iI7]\b' -or $entry -match '/MP(?![0-9])') {
                throw "ConPTY dependency has debug generation or unbounded compiler parallelism in $($cache.FullName)."
            }
        }
    }
    $targets = if ($architecture -eq 'x64') { 'Conhost\Host_EXE;Conhost\winconpty_DLL' } else { 'Conhost\Host_EXE' }
    $platform = if ($architecture -eq 'arm64') { 'ARM64' } else { 'x64' }
    Invoke-QueuedNative $msbuild @((Join-Path $SourceDirectory 'OpenConsole.sln'),
        "/m:$Slots", '/nologo', '/verbosity:minimal', "/t:$targets",
        '/p:Configuration=Release', "/p:Platform=$platform", '/p:PlatformToolset=v145',
        "/p:VCToolsVersion=$VCToolsVersion", "/p:WindowsTargetPlatformVersion=$WindowsSdkVersion",
        '/p:EnableHybridCRT=true', '/p:WindowsTerminalBranding=Release', '/p:PGOBuildMode=',
        '/p:LinkIncremental=false', '/p:MultiProcessorCompilation=false', '/p:CL_MPCount=1',
        '/p:VcpkgManifestInstall=false', "/p:VcpkgRoot=$vcpkgRoot/",
        "/p:VcpkgInstalledDir=$installed/", "/p:ForceImportAfterCppTargets=$policy",
        "/p:VnmConptyShaderIncludes=$shaderDirectory") "build-$architecture.log"
}
} finally {
    & git -C $SourceDirectory apply --ignore-space-change --reverse $sourcePatch
    if ($LASTEXITCODE -ne 0) { throw 'Could not restore the frozen ConPTY source after its maintained patch.' }
    $restoredSource = [IO.File]::ReadAllBytes($patchedSource)
    $originalText = [Text.Encoding]::UTF8.GetString($originalSource).Replace("`r`n", "`n")
    $restoredText = [Text.Encoding]::UTF8.GetString($restoredSource).Replace("`r`n", "`n")
    if ($restoredText -cne $originalText) {
        throw 'The patched ConPTY source changed during the build; refusing to overwrite it.'
    }
    # git apply preserves patch line endings, which may differ from the frozen
    # upstream -text file. Restore its exact original bytes after reversal.
    [IO.File]::WriteAllBytes($patchedSource, $originalSource)
}

foreach ($inputPath in $producerInputs) {
    if ((Get-FileHash -LiteralPath $inputPath -Algorithm SHA256).Hash -cne $producerHashes[$inputPath]) {
        throw 'ConPTY producer inputs changed during the build; rerun with the final policy.'
    }
}

$binaries = @(
    @{ source = 'bin/x64/Release/conpty.dll'; package = 'runtimes/win-x64/native/conpty.dll'; path = 'conpty.dll' },
    @{ source = 'bin/x64/Release/OpenConsole.exe'; package = 'build/native/runtimes/x64/OpenConsole.exe'; path = 'x64/OpenConsole.exe' },
    @{ source = 'bin/ARM64/Release/OpenConsole.exe'; package = 'build/native/runtimes/arm64/OpenConsole.exe'; path = 'arm64/OpenConsole.exe' }
)
$dumpbin = Join-Path $compilerRoot 'bin/Hostx64/x64/dumpbin.exe'
$files = @()
foreach ($binary in $binaries) {
    $source = Join-Path $SourceDirectory $binary.source
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing built ConPTY binary: $source" }
    Assert-ConptyImage $source
    $headers = & $dumpbin /headers $source
    if ($LASTEXITCODE -ne 0) { throw "Cannot inspect ConPTY headers: $source" }
    $imports = & $dumpbin /dependents $source
    if ($LASTEXITCODE -ne 0) { throw "Cannot inspect ConPTY dependencies: $source" }
    if ($imports -match '(?i)\b(?:msvcp|vcruntime)[^\s]*[.]dll\b|ucrtbased[.]dll') {
        throw "ConPTY did not retain the upstream Release Hybrid CRT: $source"
    }
    $evidenceName = $binary.path.Replace('/', '-')
    $headers | Set-Content -LiteralPath (Join-Path $buildEvidence "$evidenceName-headers.txt") -Encoding utf8
    $imports | Set-Content -LiteralPath (Join-Path $buildEvidence "$evidenceName-imports.txt") -Encoding utf8
    $files += [ordered]@{ path = $binary.path; size = (Get-Item -LiteralPath $source).Length
        sha256 = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant() }
}
$licenseSources = @((Join-Path $SourceDirectory 'LICENSE'),
    (Join-Path $wilDirectory 'LICENSE'),
    (Join-Path $SourceDirectory 'obj/x64/vcpkg/x64-windows-static/share/fmt/copyright'),
    (Join-Path $SourceDirectory 'obj/x64/vcpkg/x64-windows-static/share/ms-gsl/copyright'),
    (Join-Path $SourceDirectory 'oss/pcg/LICENSE-APACHE.txt'))
$notices = foreach ($license in $licenseSources) { Get-Content -LiteralPath $license -Raw }
$upstreamNotices = Get-Content -LiteralPath (Join-Path $SourceDirectory 'NOTICE.md') -Raw
foreach ($component in @('chromium/base/numerics', 'interval_tree', 'wyhash', 'stb', 'Oklab', 'ColorBrewer')) {
    $section = [regex]::Match($upstreamNotices, "(?ms)^## $([regex]::Escape($component))\r?\n.*?(?=^## |\z)")
    if (-not $section.Success) { throw "Missing upstream native dependency notice: $component" }
    $notices += $section.Value
}
foreach ($binary in $binaries) {
    $destination = Join-Path $OutputDirectory $binary.package
    [void](New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force)
    Copy-Item -LiteralPath (Join-Path $SourceDirectory $binary.source) -Destination $destination
}
$sharedDestination = Join-Path $OutputDirectory 'build/native/runtimes/conpty-shaders'
[void](New-Item -ItemType Directory -Path $sharedDestination -Force)
foreach ($resource in $sharedResources) {
    Copy-Item -LiteralPath (Join-Path $shaderDirectory (Split-Path -Leaf $resource.path)) -Destination $sharedDestination
}
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'LICENSE'), ($notices -join "`n`n"))
$receipt = [ordered]@{
    schema_version = 1; capability_id = 'conpty-source-build'; upstream_commit = $sourceCommit
    package_version = $packageVersion; configuration = 'Release'
    release_policy = @{ debug_symbols = $false; hybrid_crt = $true
        sha256 = (Get-FileHash -LiteralPath $policy -Algorithm SHA256).Hash.ToLowerInvariant() }
    toolchain = @{ visual_studio = $VisualStudioDirectory; msvc = $VCToolsVersion
        windows_sdk = $WindowsSdkVersion; platform_toolset = 'v145'; compiler_id = 'MSVC'
        compiler_version = (Get-Item -LiteralPath "$compilerRoot/bin/Hostx64/x64/cl.exe").VersionInfo.FileVersion
        compiler_x64_sha256 = (Get-FileHash -LiteralPath "$compilerRoot/bin/Hostx64/x64/cl.exe" -Algorithm SHA256).Hash.ToLowerInvariant()
        compiler_arm64_sha256 = (Get-FileHash -LiteralPath "$compilerRoot/bin/Hostx64/arm64/cl.exe" -Algorithm SHA256).Hash.ToLowerInvariant() }
    producer = @{ name = 'vnm_terminal_surface/prepare_windows_conpty.ps1'
        source_patch_sha256 = (Get-FileHash -LiteralPath $sourcePatch -Algorithm SHA256).Hash.ToLowerInvariant()
        shader_loader_sha256 = (Get-FileHash -LiteralPath $shaderLoader -Algorithm SHA256).Hash.ToLowerInvariant()
        shader_preparation_sha256 = (Get-FileHash -LiteralPath $shaderPreparation -Algorithm SHA256).Hash.ToLowerInvariant()
        script_sha256 = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()
        policy_sha256 = (Get-FileHash -LiteralPath $policy -Algorithm SHA256).Hash.ToLowerInvariant()
        triplets_sha256 = @{
            'x64-windows-static.cmake' = (Get-FileHash -LiteralPath "$triplets/x64-windows-static.cmake" -Algorithm SHA256).Hash.ToLowerInvariant()
            'arm64-windows-static.cmake' = (Get-FileHash -LiteralPath "$triplets/arm64-windows-static.cmake" -Algorithm SHA256).Hash.ToLowerInvariant()
            'release-flags.cmake' = (Get-FileHash -LiteralPath "$triplets/release-flags.cmake" -Algorithm SHA256).Hash.ToLowerInvariant() } }
    inputs = @{ wil_version = $wilVersion; wil_sha256 = $wilSha256
        vcpkg_baseline = 'fe1cde61e971d53c9687cf9a46308f8f55da19fa' }
    license_sha256 = (Get-FileHash -LiteralPath (Join-Path $OutputDirectory 'LICENSE') -Algorithm SHA256).Hash.ToLowerInvariant()
    shared_shader_resources = @{ schema_version = 1; files = $sharedResources }
    files = $files
}
$receipt | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'conpty-source-build.json') -Encoding utf8
Write-Output "Prepared source-built ConPTY package: $OutputDirectory"
