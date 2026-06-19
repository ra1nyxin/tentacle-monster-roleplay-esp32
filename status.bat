@echo off
setlocal

cd /d "%~dp0"

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "Write-Host '=== iPhone Camera Bridge Status ==='; " ^
  "$listeners = Get-NetTCPConnection -LocalPort 7777 -State Listen -ErrorAction SilentlyContinue; " ^
  "if ($listeners) { foreach ($l in $listeners) { Write-Host ('Listening: ' + $l.LocalAddress + ':' + $l.LocalPort + ' PID=' + $l.OwningProcess) } } else { Write-Host 'Listening: no' }; " ^
  "$clients = Get-NetTCPConnection -LocalPort 7777 -State Established -ErrorAction SilentlyContinue; " ^
  "if ($clients) { foreach ($c in $clients) { Write-Host ('Client: ' + $c.RemoteAddress + ':' + $c.RemotePort + ' -> PID=' + $c.OwningProcess) } } else { Write-Host 'Client: none' }; " ^
  "if (Test-Path 'latest.json') { Write-Host ''; Write-Host 'Latest frame metadata:'; Get-Content -LiteralPath 'latest.json' -Raw } else { Write-Host ''; Write-Host 'latest.json: not found' }; " ^
  "if (Test-Path 'latest.jpg') { $img = Get-Item -LiteralPath 'latest.jpg'; Write-Host ('latest.jpg: ' + $img.Length + ' bytes, updated ' + $img.LastWriteTime) } else { Write-Host 'latest.jpg: not found' }"

echo.
echo Press any key to close this window.
pause >nul
