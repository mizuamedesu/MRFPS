[CmdletBinding()]
param(
    [ValidateSet('Editor','Windows','LinuxServer','Quest')][string]$Target = 'Editor',
    [ValidateSet('Development','Shipping')][string]$Configuration = 'Development',
    [string]$EngineRoot, [string]$SdkRoot, [string]$JavaRoot,
    [string]$ArchiveDirectory, [switch]$DryRun, [switch]$SkipSetup,
    [ValidateRange(1,2)][int]$MaxParallelActions = 1,
    [switch]$LowMemoryWindowsLink = $true,
    [switch]$LinuxPortableSymbols,
    [switch]$AndroidFullDebugSymbols
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'MRFPS.Tools.psm1') -Force
$root = Get-MRFPSRoot
$engine = Resolve-MRFPSEngine $EngineRoot
if (-not $DryRun) {
    $sdkPython = Join-Path $engine 'Engine\Binaries\ThirdParty\Python3\Win64\python.exe'
    $sdkArgs = @((Join-Path $root 'tools\mobits\ensure_sdk.py'))
    if ($Target -eq 'Quest') { $sdkArgs += '--quest-identify-headers' }
    & $sdkPython @sdkArgs
    if ($LASTEXITCODE -ne 0) { throw 'MoBits SDK setup failed. Check access to the private SDK repository.' }
}
$project = Join-Path $root 'MRFPS.uproject'
if (-not (Test-Path -LiteralPath $project)) { throw "Missing project: $project" }
if (-not $ArchiveDirectory) { $ArchiveDirectory = Join-Path $root 'Artifacts\Packages' }
$ArchiveDirectory = [IO.Path]::GetFullPath($ArchiveDirectory)
$log = Join-Path $root ('Saved\BuildLogs\{0}-{1}-{2}.log' -f $Target, $Configuration, (Get-Date -Format yyyyMMdd-HHmmss))
$arguments = @('BuildCookRun',"-project=$project",'-noP4','-utf8output','-build','-cook','-stage','-pak','-package','-archive',"-archivedirectory=$ArchiveDirectory",'-map=/Game/Maps/MRFPSArena',"-clientconfig=$Configuration",'-unattended',"-ubtargs=-MaxParallelActions=$MaxParallelActions -NoUBA")
$previousEnvironment = @{}
try {
    # Apply limits before every UBT invocation, including Editor and Setup.
    # Restore the caller's environment on success, early return, or failure.
    foreach ($name in @('DOTNET_GCHeapHardLimit','DOTNET_PROCESSOR_COUNT','MSBUILDDISABLENODEREUSE')) {
        $previousEnvironment[$name]=[Environment]::GetEnvironmentVariable($name,'Process')
    }
    if (-not $previousEnvironment['DOTNET_GCHeapHardLimit']) { $env:DOTNET_GCHeapHardLimit='0xC0000000' }
    $env:DOTNET_PROCESSOR_COUNT='2'
    $env:MSBUILDDISABLENODEREUSE='1'
    # UE 5.8 keeps the UBA local scheduler; -NoUBA disables its detouring.
    # Keep one action and guard total memory, including native symbol generation.
    if ($Target -eq 'Editor' -or -not $SkipSetup) {
        Invoke-MRFPSCommand (Join-Path $engine 'Engine\Build\BatchFiles\Build.bat') @('MRFPSEditor','Win64','Development',"-Project=$project",'-WaitMutex','-NoHotReloadFromIDE','-NoUBA',"-MaxParallelActions=$MaxParallelActions") -LogPath ($log -replace '\.log$', '-editor.log') -DryRun:$DryRun
    }
    if (-not $SkipSetup) {
        Invoke-MRFPSCommand (Join-Path $engine 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe') @($project,'-run=MRFPSSetup','-unattended','-NullRHI','-nohmd','-nosound','-nop4') -LogPath ($log -replace '\.log$', '-setup.log') -DryRun:$DryRun
    }
    if ($Target -eq 'Editor') { return }
    switch ($Target) {
        'Windows' {
            $windowsNativeTargets = @()
            if ($SkipSetup) { $windowsNativeTargets += @{ Name='MRFPSEditor'; Config='Development' } }
            $windowsNativeTargets += @(
                @{ Name='ShaderCompileWorker'; Config='Development' },
                @{ Name='UnrealPak'; Config='Development' },
                @{ Name='MRFPS'; Config=$Configuration }
            )
            foreach ($native in $windowsNativeTargets) {
                $nativeLog = $log -replace '\.log$', ('-' + $native.Name + '-native.log')
                $nativeArguments = @($native.Name,'Win64',$native.Config,"-Project=$project",'-WaitMutex','-NoHotReloadFromIDE','-NoUBA',"-MaxParallelActions=$MaxParallelActions")
                if ($LowMemoryWindowsLink -and $native.Name -eq 'MRFPS' -and $native.Config -eq 'Development') {
                    # The pinned VS2022 toolchain supports indexed PDBs. Reuse
                    # existing objects instead of merging a full monolithic PDB.
                    $nativeArguments += '-LinkerArguments=/DEBUG:FASTLINK /INCREMENTAL:NO'
                }
                Invoke-MRFPSCommand (Join-Path $engine 'Engine\Build\BatchFiles\Build.bat') $nativeArguments -LogPath $nativeLog -DryRun:$DryRun
            }
            $arguments += @('-skipbuild','-target=MRFPS','-platform=Win64')
        }
        'LinuxServer' {
            if (-not (Test-Path -LiteralPath (Join-Path $engine 'Engine\Source\Programs\UnrealBuildTool')) ) {
                throw 'Linux dedicated servers require an Unreal source build or a compatible installed build with Server support.'
            }
            # UAT otherwise puts the Editor, ShaderCompileWorker, UnrealPak and
            # Linux server into one UBT invocation. Their combined dependency
            # graphs exceeded this PC's memory budget even with one action.
            # Release each UBT process before loading the next target's graph.
            foreach ($native in @(
                @{ Name='ShaderCompileWorker'; Platform='Win64'; Config='Development' },
                @{ Name='UnrealPak'; Platform='Win64'; Config='Development' },
                @{ Name='MRFPSServer'; Platform='Linux'; Config=$Configuration }
            )) {
                $nativeLog = $log -replace '\.log$', ('-' + $native.Name + '-native.log')
                $nativeArguments = @($native.Name,$native.Platform,$native.Config,"-Project=$project",'-WaitMutex','-NoHotReloadFromIDE','-NoUBA',"-MaxParallelActions=$MaxParallelActions")
                if ($native.Platform -eq 'Linux' -and -not $LinuxPortableSymbols) {
                    # Breakpad conversion alone exceeded the RAM reserve on this
                    # PC. Full DWARF and Breakpad symbols are opt-in below.
                    $nativeArguments += '-NoDumpSyms'
                }
                if ($native.Platform -eq 'Linux') {
                    # MaxParallelActions does not limit LLD's internal workers.
                    # A GDB index is optional and expensive for this monolith.
                    $nativeArguments += $(if ($LinuxPortableSymbols) { '-LinkerArguments=-Wl,--threads=1' } else { '-LinkerArguments=-Wl,--threads=1,--no-gdb-index,--strip-debug' })
                }
                Invoke-MRFPSCommand (Join-Path $engine 'Engine\Build\BatchFiles\Build.bat') $nativeArguments -LogPath $nativeLog -DryRun:$DryRun
            }
            $arguments += '-skipbuild'
            $arguments += @('-target=MRFPSServer','-server','-noclient','-serverplatform=Linux',"-serverconfig=$Configuration")
        }
        'Quest' {
            $sdk = Resolve-MRFPSAndroidSdk $SdkRoot
            $java = Resolve-MRFPSJava $JavaRoot
            $required = Get-Content -Raw -LiteralPath (Join-Path $engine 'Engine\Config\Android\Android_SDK.json') | ConvertFrom-Json
            $ndk = Join-Path $sdk ('ndk\' + $required.ndk)
            foreach ($path in @((Join-Path $ndk 'source.properties'), (Join-Path $sdk ('build-tools\' + $required.'build-tools' + '\aapt2.exe')), (Join-Path $sdk ('platforms\' + $required.platforms + '\android.jar')))) {
                if (-not (Test-Path -LiteralPath $path)) { throw "Missing Android requirement: $path. Run Scripts/Initialize-Android.ps1." }
            }
            foreach ($name in @('ANDROID_HOME','ANDROID_SDK_ROOT','NDKROOT','NDK_ROOT','JAVA_HOME','GRADLE_USER_HOME')) { $previousEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
            $env:ANDROID_HOME = $sdk
            $env:ANDROID_SDK_ROOT = $sdk
            $env:NDKROOT = $ndk
            $env:NDK_ROOT = $ndk
            $env:JAVA_HOME = $java
            # Keep Kotlin 2.2 dex transforms separate from the older UE 5.8
            # toolchain's shared Gradle cache. This is scoped to this build.
            $env:GRADLE_USER_HOME = Join-Path $root '.local\gradle-mrfps-mobits'
            # Keep the editor/tools/Android dependency graphs in separate UBT
            # processes, as for LinuxServer. MaxParallelActions alone does not
            # bound the memory of a combined four-target UAT invocation.
            $questNativeTargets = @()
            if ($SkipSetup) { $questNativeTargets += @{ Name='MRFPSEditor'; Platform='Win64'; Config='Development' } }
            $questNativeTargets += @(
                @{ Name='ShaderCompileWorker'; Platform='Win64'; Config='Development' },
                @{ Name='UnrealPak'; Platform='Win64'; Config='Development' },
                @{ Name='MRFPS'; Platform='Android'; Config=$Configuration }
            )
            foreach ($native in $questNativeTargets) {
                $nativeLog = $log -replace '\.log$', ('-' + $native.Name + '-native.log')
                $nativeArguments=@($native.Name,$native.Platform,$native.Config,"-Project=$project",'-WaitMutex','-NoHotReloadFromIDE','-NoUBA',"-MaxParallelActions=$MaxParallelActions")
                if ($native.Platform -eq 'Android') {
                    $env:DOTNET_GCHeapHardLimit=$(if($AndroidFullDebugSymbols){'0x80000000'}else{'0x60000000'})
                    # AndroidToolChain concatenates this value directly after
                    # --end-group, so the leading space is intentional.
                    # The APK is stripped during packaging anyway. Avoid
                    # materializing multi-GB DWARF output on constrained hosts.
                    $nativeArguments+=$(if($AndroidFullDebugSymbols){'-LinkerArguments= -Wl,--threads=1,--no-gdb-index'}else{'-LinkerArguments= -Wl,--threads=1,--no-gdb-index,--strip-debug'})
                }
                Invoke-MRFPSCommand (Join-Path $engine 'Engine\Build\BatchFiles\Build.bat') $nativeArguments -LogPath $nativeLog -DryRun:$DryRun
            }
            $arguments += '-skipbuild'
            $arguments += @('-target=MRFPS','-platform=Android','-cookflavor=ASTC')
        }
    }
    Invoke-MRFPSCommand (Join-Path $engine 'Engine\Build\BatchFiles\RunUAT.bat') $arguments -LogPath $log -DryRun:$DryRun
} finally {
    foreach ($name in $previousEnvironment.Keys) { [Environment]::SetEnvironmentVariable($name, $previousEnvironment[$name], 'Process') }
}
