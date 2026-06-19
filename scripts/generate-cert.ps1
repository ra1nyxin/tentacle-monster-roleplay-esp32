param(
    [string]$CertDir = "certs"
)

$ErrorActionPreference = "Stop"

if (-not (Get-Command openssl.exe -ErrorAction SilentlyContinue)) {
    throw "openssl.exe was not found in PATH. Install OpenSSL or run this from a shell where openssl.exe is available."
}

New-Item -ItemType Directory -Force -Path $CertDir | Out-Null

$configPath = Join-Path $CertDir "openssl.generated.cnf"
$keyPath = Join-Path $CertDir "key.pem"
$certPath = Join-Path $CertDir "cert.pem"

$ips = @(
    Get-NetIPAddress -AddressFamily IPv4 |
        Where-Object {
            $_.IPAddress -notlike "127.*" -and
            $_.InterfaceAlias -notlike "VMware*" -and
            $_.PrefixOrigin -ne "WellKnown"
        } |
        Select-Object -ExpandProperty IPAddress
)

$config = @"
[req]
default_bits = 2048
prompt = no
default_md = sha256
x509_extensions = v3_req
distinguished_name = dn

[dn]
CN = iphone-camera-bridge.local

[v3_req]
subjectAltName = @alt_names

[alt_names]
DNS.1 = localhost
DNS.2 = iphone-camera-bridge.local
IP.1 = 127.0.0.1
"@

$index = 2
foreach ($ip in $ips) {
    $config += "`nIP.$index = $ip"
    $index += 1
}

Set-Content -LiteralPath $configPath -Value $config -Encoding ASCII

& openssl.exe req -x509 -nodes -days 3650 -newkey rsa:2048 `
    -keyout $keyPath `
    -out $certPath `
    -config $configPath

Write-Host "[camera-bridge] Generated local HTTPS certificate:"
Write-Host "  $certPath"

