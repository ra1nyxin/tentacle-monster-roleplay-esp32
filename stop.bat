@echo off
setlocal

cd /d "%~dp0"

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$stopped = $false; " ^
  "if (Test-Path 'server.pid') { " ^
  "  $pidText = (Get-Content -LiteralPath 'server.pid' -Raw).Trim(); " ^
  "  if ($pidText -match '^\d+$') { " ^
  "    $p = Get-Process -Id ([int]$pidText) -ErrorAction SilentlyContinue; " ^
  "    if ($p) { Stop-Process -Id $p.Id -Force; Write-Host '[camera-bridge] Stopped PID' $p.Id; $stopped = $true } " ^
  "  } " ^
  "  Remove-Item -LiteralPath 'server.pid' -Force -ErrorAction SilentlyContinue; " ^
  "} " ^
  "$listeners = Get-NetTCPConnection -LocalPort 7777 -State Listen -ErrorAction SilentlyContinue; " ^
  "foreach ($l in $listeners) { " ^
  "  $p = Get-Process -Id $l.OwningProcess -ErrorAction SilentlyContinue; " ^
  "  if ($p -and $p.ProcessName -eq 'node') { Stop-Process -Id $p.Id -Force; Write-Host '[camera-bridge] Stopped port 7777 PID' $p.Id; $stopped = $true } " ^
  "} " ^
  "if (-not $stopped) { Write-Host '[camera-bridge] No running camera bridge found.' }"

echo.
echo Press any key to close this window.
pause >nul
