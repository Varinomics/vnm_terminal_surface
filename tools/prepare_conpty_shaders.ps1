function Initialize-ConptySharedShaders {
    param([string]$SourceRoot, [string]$OutputRoot, [string]$Fxc)
    [void](New-Item -ItemType Directory -Path $OutputRoot -Force)
    $records = @()
    $metadata = @('#pragma once', 'namespace vnm_conpty {',
        'inline constexpr std::array<Shader_resource, 4> shader_resources{{')
    foreach ($name in @('shader_vs', 'shader_ps', 'custom_shader_vs', 'custom_shader_ps')) {
        $target = if ($name.EndsWith('_vs')) { 'vs_4_0' } else { 'ps_4_0' }
        $output = Join-Path $OutputRoot "$name.dxbc"
        Invoke-QueuedNative $Fxc @('/nologo', '/WX', '/E', 'main', '/T', $target,
            '/all_resources_bound', '/O3', '/Qstrip_reflect', '/Fo', $output,
            (Join-Path $SourceRoot "src/renderer/atlas/$name.hlsl")) "shader-$name.log" 1 | Out-Host
        $bytes = [IO.File]::ReadAllBytes($output)
        if ($bytes.Length -lt 32 -or [Text.Encoding]::ASCII.GetString($bytes, 0, 4) -cne 'DXBC' -or
            [BitConverter]::ToUInt32($bytes, 24) -ne $bytes.Length) {
            throw "Invalid complete shader container: $output"
        }
        $count = [BitConverter]::ToUInt32($bytes, 28)
        if ($count -ne 3) { throw "Unexpected shader container parts: $output" }
        $parts = @()
        for ($index = 0; $index -lt $count; ++$index) {
            $offset = [BitConverter]::ToUInt32($bytes, 32 + 4 * $index)
            if ($offset -lt 32 + 4 * $count -or $offset + 8 -gt $bytes.Length) {
                throw "Invalid shader part offset: $output"
            }
            $kind = [Text.Encoding]::ASCII.GetString($bytes, $offset, 4)
            $length = [BitConverter]::ToUInt32($bytes, $offset + 4)
            if ($kind -notin @('ISGN', 'OSGN', 'SHDR') -or $kind -in $parts -or
                $offset + 8 + $length -gt $bytes.Length) {
                throw "Unexpected or debugging shader part ${kind}: $output"
            }
            $parts += $kind
        }
        $hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
        $records += [ordered]@{ path = "conpty-shaders/$name.dxbc"; size = $bytes.Length; sha256 = $hash }
        $hashBytes = for ($index = 0; $index -lt 64; $index += 2) { '0x' + $hash.Substring($index, 2) }
        $metadata += "    { L`"$name.dxbc`", $($bytes.Length), { $($hashBytes -join ', ') } },"
    }
    $metadata += '}};', '} // namespace vnm_conpty'
    [IO.File]::WriteAllText((Join-Path $OutputRoot 'conpty-shader-metadata.h'), ($metadata -join "`n") + "`n")
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'conpty-shared-shaders.h') -Destination $OutputRoot -Force
    return $records
}
