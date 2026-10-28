[CmdletBinding()]
param([Parameter(Mandatory)][string]$Glslc, [string]$SpirvVal)
$ErrorActionPreference = 'Stop'
$shader = Join-Path $PSScriptRoot 'Private/EnvironmentGeometry.comp'
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('mrfps-depth-' + [Guid]::NewGuid().ToString('N') + '.spv')
$header = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../Source/MRFPSXR/Private/EnvironmentGeometrySpirv.h'))
try {
    & $Glslc --target-env=vulkan1.0 -O -fshader-stage=compute $shader -o $temporary
    if ($LASTEXITCODE -ne 0) { throw "glslc failed: $LASTEXITCODE" }
    if ($SpirvVal) {
        & $SpirvVal --target-env vulkan1.0 $temporary
        if ($LASTEXITCODE -ne 0) { throw "spirv-val failed: $LASTEXITCODE" }
    }
    $bytes = [IO.File]::ReadAllBytes($temporary)
    if ($bytes.Length -eq 0 -or ($bytes.Length % 4) -ne 0) { throw 'Invalid SPIR-V size' }
    $lines = [Collections.Generic.List[string]]::new()
    $lines.Add('// Generated from Shaders/Private/EnvironmentGeometry.comp by BuildEnvironmentShader.ps1.')
    $lines.Add('#pragma once')
    $lines.Add('inline constexpr uint32 GMRFPSEnvironmentGeometrySpirv[] = {')
    for ($i=0; $i -lt $bytes.Length; $i+=32) {
        $words = for ($j=$i; $j -lt [Math]::Min($i+32,$bytes.Length); $j+=4) { '0x{0:x8}u' -f [BitConverter]::ToUInt32($bytes,$j) }
        $lines.Add('    ' + ($words -join ', ') + ',')
    }
    $lines.Add('};')
    [IO.File]::WriteAllLines($header,$lines,[Text.UTF8Encoding]::new($false))
    Get-FileHash -LiteralPath $shader,$header -Algorithm SHA256
} finally {
    if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
}
