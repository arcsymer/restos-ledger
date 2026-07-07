# Boot a local 3-node restos-ledger cluster.  Usage: scripts\cluster.ps1 [-Build build]
param([string]$Build = "build")

$node = Join-Path $Build "restos-node.exe"
if (-not (Test-Path $node)) { $node = Join-Path $Build "restos-node" }
$data = Join-Path ([System.IO.Path]::GetTempPath()) ("restos-ledger-" + [guid]::NewGuid())
New-Item -ItemType Directory -Force $data | Out-Null

$peers = @(
  "1:127.0.0.1:5001,2:127.0.0.1:5002",
  "0:127.0.0.1:5000,2:127.0.0.1:5002",
  "0:127.0.0.1:5000,1:127.0.0.1:5001"
)
$procs = 0..2 | ForEach-Object {
  Start-Process -PassThru -NoNewWindow $node `
    -ArgumentList "--id", "$_", "--port", "$(5000 + $_)", "--peers", $peers[$_], "--data", $data
}
Write-Host "3-node cluster up on ports 5000-5002 (data: $data)."
Write-Host "Try:  $Build\restos-cli --nodes 127.0.0.1:5000,127.0.0.1:5001,127.0.0.1:5002 put a hello"
Write-Host "Press Enter to stop."
[void][System.Console]::ReadLine()
$procs | ForEach-Object { Stop-Process $_ -Force -ErrorAction SilentlyContinue }
Remove-Item -Recurse -Force $data -ErrorAction SilentlyContinue
