@echo off
setlocal

cd /d "%~dp0"

if not exist "certs" mkdir "certs"

if not exist "certs\key.pem" (
    echo [camera-bridge] Generating local HTTPS certificate...
    powershell -NoProfile -ExecutionPolicy Bypass -File "scripts\generate-cert.ps1"
    if errorlevel 1 (
        echo [camera-bridge] Failed to generate certificate.
        pause
        exit /b 1
    )
)

if not exist "certs\cert.pem" (
    echo [camera-bridge] Missing certs\cert.pem.
    pause
    exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$existing = Get-NetTCPConnection -LocalPort 7777 -ErrorAction SilentlyContinue | Select-Object -First 1; " ^
  "if ($existing) { Write-Host '[camera-bridge] Port 7777 is already in use by PID' $existing.OwningProcess; exit 0 }; " ^
  "$p = Start-Process -FilePath 'node.exe' -ArgumentList 'server.js' -WorkingDirectory (Get-Location) -WindowStyle Hidden -RedirectStandardOutput 'server.log' -RedirectStandardError 'server.err.log' -PassThru; " ^
  "Set-Content -LiteralPath 'server.pid' -Value $p.Id -Encoding ascii; " ^
  "Start-Sleep -Milliseconds 500; " ^
  "$ips = Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -notlike '127.*' -and $_.InterfaceAlias -notlike 'VMware*' -and $_.PrefixOrigin -ne 'WellKnown' } | Select-Object -ExpandProperty IPAddress; " ^
  "Write-Host '[camera-bridge] Started PID' $p.Id; " ^
  "Write-Host '[camera-bridge] Open on iPhone Safari:'; " ^
  "foreach ($ip in $ips) { Write-Host ('  https://' + $ip + ':7777/') }; " ^
  "Write-Host '[camera-bridge] Latest frame will be saved to:' (Join-Path (Get-Location) 'latest.jpg')"

echo.
echo Press any key to close this window. The service keeps running in background.
pause >nul
