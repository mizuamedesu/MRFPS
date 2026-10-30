# Device-login HTTPS verification

`Config/DefaultEngine.ini` explicitly enables UE's `n.VerifyPeer`. The local
configuration regression and fixture checks do not start Unreal:

```powershell
$py = 'C:\Users\ilike\OneDrive\Desktop\script\MRFPS\.local\auth-tools\Scripts\python.exe'
& $py tools\auth\test_tls_config.py
& $py tools\auth\tls_fixture.py self-test
```

The self-test exercises Python's TLS implementation and the fixture only. It
does not establish how an Unreal build or Quest behaves.

After the project and Editor modules containing `MRFPSTlsFixtureTests.cpp` are
built, the following command runs the **actual UE HTTP implementation** in a
fresh Editor process and under the project's Windows memory guard. Use the
project and scripts from the same built worktree. The run needs the Python
`cryptography` package and a fresh output path:

```powershell
$repo = (Get-Location).ProviderPath
$task = [Guid]::NewGuid().ToString('N')
$run = Join-Path $env:LOCALAPPDATA ('MRFPS\TlsFixture-' + $task)
$guard = $run + '-guard\report.json'
$editor = 'C:\UnrealEngine\UE5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$project = Join-Path $repo 'MRFPS.uproject'
$fixtureArguments = @('-Python',$py,'-Editor',$editor,'-Project',$project,
          '-OutputDirectory',(Join-Path $run 'ue'),'-GuardReport',$guard)
& (Join-Path $repo 'Scripts\Invoke-MRFPSResourceLimited.ps1') `
  -ScriptFile (Join-Path $repo 'Scripts\Test-TlsPeerFixture.ps1') `
  -ScriptArguments $fixtureArguments -MinimumFreeGB 10 -MaximumCommitPercent 75 -ReportPath $guard
```

The fixture binds two random ports on `127.0.0.1`, makes two short-lived,
unrelated local CAs, and passes **only one** CA to this Editor process through
UE's development-only `OverrideCertificateBundlePath`. It never installs a CA
in the system or browser. The test requires a matching `localhost` certificate
to succeed, the same trusted certificate at `127.0.0.1` to fail hostname
verification, and the second CA to fail trust verification. It also requires
the server to have accepted all three TCP connections, so an unavailable
fixture cannot masquerade as a certificate rejection. A missing test, missing
report, timeout, or failed positive control is a failure. The guard terminates
only the processes in its own job. A forcibly killed fixture can leave its
throwaway temporary CA files in the OS temp directory.

This Editor test does not prove the packaged Windows or Android build uses the
new config, nor test login redirects. Rebuild each client and independently
check its `bVerifyPeer` startup log; run device-specific positive and negative
CA/hostname tests before claiming those targets are verified. UE's CA override
is disabled in Shipping builds, and the current login test URL accepts only
loopback HTTP, so the Editor fixture is not a Shipping or Quest test.
