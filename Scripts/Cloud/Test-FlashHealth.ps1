[CmdletBinding()]
param([string]$Distribution = 'Ubuntu')
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if (-not $env:LINUX_MULTIARCH_ROOT) { throw 'LINUX_CROSS_COMPILER_REQUIRED' }
$toolchain = Join-Path $env:LINUX_MULTIARCH_ROOT 'x86_64-unknown-linux-gnu'
$compiler = Join-Path $toolchain 'bin/clang++.exe'
if (-not (Test-Path -LiteralPath $compiler)) { throw 'LINUX_CROSS_COMPILER_REQUIRED' }
$memory = Get-CimInstance Win32_OperatingSystem
$commit = Get-CimInstance Win32_PerfFormattedData_PerfOS_Memory
if ($memory.FreePhysicalMemory -lt 10MB -or $commit.PercentCommittedBytesInUse -gt 75) { throw 'MEMORY_GUARD_BLOCKED' }
$directory = Join-Path $root 'Saved/Validation/FlashHealthFixture'
$marker = Join-Path $directory '.mrfps-health-test-owner'
if (Test-Path -LiteralPath $directory) {
    if (-not (Test-Path -LiteralPath $marker) -or (Get-Content -LiteralPath $marker -Raw).Trim() -ne 'MRFPS_FLASH_HEALTH_FIXTURE_V1') {
        throw 'HEALTH_FIXTURE_DIRECTORY_OWNERSHIP_REQUIRED'
    }
} else {
    New-Item -ItemType Directory -Path $directory | Out-Null
    [IO.File]::WriteAllText($marker, 'MRFPS_FLASH_HEALTH_FIXTURE_V1')
}
$lockPath = Join-Path $directory '.lock'
$lock = [IO.File]::Open($lockPath, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
try {
    $temporary = Join-Path $directory 'health-responder.incoming'
    $binary = Join-Path $directory 'health-responder'
    & $compiler --target=x86_64-unknown-linux-gnu "--sysroot=$toolchain" -I (Join-Path $root 'Source/MRFPS/Public') -std=c++17 -Wall -Wextra -Werror -O0 `
        -DMRFPS_HEALTH_STANDALONE=1 (Join-Path $root 'Source/MRFPS/Private/MRFPSHealthResponder.cpp') `
        (Join-Path $root 'Scripts/tests/health_responder_fixture.cpp') -o $temporary
    if ($LASTEXITCODE -ne 0) { throw 'HEALTH_FIXTURE_BUILD_FAILED' }
    if (Test-Path -LiteralPath $binary) { [IO.File]::Replace($temporary, $binary, [NullString]::Value) }
    else { [IO.File]::Move($temporary, $binary) }
    $linuxBinary = (& wsl.exe -d $Distribution -u root --exec wslpath -a $binary | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or -not $linuxBinary.StartsWith('/')) { throw 'HEALTH_FIXTURE_WSL_PATH_FAILED' }
    $linuxTests = (& wsl.exe -d $Distribution -u root --exec wslpath -a (Join-Path $root 'Scripts/test_deployment.py') | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or -not $linuxTests.StartsWith('/')) { throw 'HEALTH_TEST_WSL_PATH_FAILED' }
    & wsl.exe -d $Distribution -u root --exec /usr/bin/setpriv --reuid=10001 --regid=10001 --clear-groups `
        --bounding-set=-all --inh-caps=-all --ambient-caps=-all --no-new-privs `
        env "MRFPS_HEALTH_FIXTURE_BIN=$linuxBinary" python3 $linuxTests
    if ($LASTEXITCODE -ne 0) { throw 'FLASH_HEALTH_FIXTURE_TEST_FAILED' }
} finally {
    $lock.Dispose()
}
