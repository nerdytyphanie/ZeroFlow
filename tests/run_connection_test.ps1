param([ValidateSet('server','client')][string]$Role, [string]$HostAddress, [int]$Seconds = 180)
$ErrorActionPreference = 'Stop'
$testRoot = $PSScriptRoot
$info = New-Object System.Diagnostics.ProcessStartInfo
$info.FileName = Join-Path $testRoot 'ZeroFlow.exe'
$info.WorkingDirectory = $testRoot
$info.UseShellExecute = $false
$info.CreateNoWindow = $true
$info.RedirectStandardInput = $true
$info.RedirectStandardOutput = $true
$info.RedirectStandardError = $true
$info.Arguments = "--$Role --headless --status-json --control-stdin --settings " + [char]34 + (Join-Path $testRoot "$Role.ini") + [char]34
if($Role -eq 'client') { $info.Arguments += " --host $HostAddress" }
$child = New-Object System.Diagnostics.Process
$child.StartInfo = $info
$log = Join-Path $testRoot "$Role.log"
$status = Join-Path $testRoot "$Role.ndjson"
[IO.File]::WriteAllText($log, '')
[IO.File]::WriteAllText($status, '')
try {
    if(-not $child.Start()) { throw 'Could not start test process' }
    [IO.File]::WriteAllText((Join-Path $testRoot "$Role.pid"), [string]$child.Id)
    $stdoutRead = $child.StandardOutput.ReadLineAsync()
    $stderrRead = $child.StandardError.ReadLineAsync()
    $deadline = [DateTime]::UtcNow.AddSeconds($Seconds)
    while(-not $child.HasExited -and [DateTime]::UtcNow -lt $deadline -and -not (Test-Path (Join-Path $testRoot 'stop-test'))) {
        while($stdoutRead.IsCompleted) {
            $line = $stdoutRead.GetAwaiter().GetResult()
            if($null -eq $line) { break }
            [IO.File]::AppendAllText($status, $line + [Environment]::NewLine); $stdoutRead = $child.StandardOutput.ReadLineAsync()
        }
        while($stderrRead.IsCompleted) {
            $line = $stderrRead.GetAwaiter().GetResult()
            if($null -eq $line) { break }
            [IO.File]::AppendAllText($log, $line + [Environment]::NewLine); $stderrRead = $child.StandardError.ReadLineAsync()
        }
        Start-Sleep -Milliseconds 100
    }
} finally {
    if(-not $child.HasExited) {
        $child.StandardInput.WriteLine('{"command":"stop"}')
        $child.StandardInput.Close()
        if(-not $child.WaitForExit(4000)) { $child.Kill(); $child.WaitForExit() }
    }
    [IO.File]::AppendAllText($log, "Test process exited: " + $child.ExitCode + [Environment]::NewLine)
    $child.Dispose()
}
