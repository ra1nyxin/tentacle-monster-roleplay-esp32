param(
    [string]$SerialPort = "COM3",
    [int]$Baud = 115200,
    [int]$ListenPort = 25363,
    [string]$ListenAddress = "127.0.0.1"
)

$ErrorActionPreference = "Stop"

$script:Serial = $null

function Write-BridgeLog {
    param([string]$Message)
    $ts = Get-Date -Format "yyyy-MM-dd HH:mm:ss.fff"
    Write-Host "[$ts] $Message"
}

function Close-GalakuSerial {
    param([switch]$SendStop)

    if ($null -eq $script:Serial) {
        return
    }

    try {
        if ($script:Serial.IsOpen -and $SendStop) {
            try {
                $script:Serial.Write("STOP`n")
                Write-BridgeLog "COM <= STOP"
                Start-Sleep -Milliseconds 150
            } catch {
                Write-BridgeLog "Failed to send STOP while closing serial: $($_.Exception.Message)"
            }
        }
    } finally {
        try {
            if ($script:Serial.IsOpen) {
                $script:Serial.Close()
            }
        } finally {
            $script:Serial.Dispose()
            $script:Serial = $null
        }
    }
}

function Open-GalakuSerial {
    Close-GalakuSerial

    $sp = [System.IO.Ports.SerialPort]::new($SerialPort, $Baud, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
    $sp.DtrEnable = $false
    $sp.RtsEnable = $false
    $sp.ReadTimeout = 200
    $sp.WriteTimeout = 1000
    $sp.NewLine = "`n"
    $sp.Open()

    Start-Sleep -Seconds 3
    try {
        $sp.DiscardInBuffer()
        $sp.DiscardOutBuffer()
    } catch {
        Write-BridgeLog "Serial buffer discard warning: $($_.Exception.Message)"
    }

    $script:Serial = $sp
    Send-GalakuSerialLine -Line "PING" -NoRetry | Out-Null
    Start-Sleep -Milliseconds 800

    $reply = ""
    try {
        $reply = $script:Serial.ReadExisting().Trim()
    } catch {
        Write-BridgeLog "Initial serial read warning: $($_.Exception.Message)"
    }

    Write-BridgeLog "Serial opened on $SerialPort, initial reply: $reply"
}

function Ensure-GalakuSerial {
    if ($null -eq $script:Serial -or -not $script:Serial.IsOpen) {
        Open-GalakuSerial
    }
}

function Select-GalakuProtocolReply {
    param(
        [string]$RawReply,
        [string]$CommandLine
    )

    if ([string]::IsNullOrWhiteSpace($RawReply)) {
        return ""
    }

    $protocolLines = @(
        $RawReply -split "\r?\n" |
            ForEach-Object { $_.Trim() } |
            Where-Object {
                $_.Length -gt 0 -and
                ($_ -match '^(PONG|STATUS\b|OK\b|ERR\b|SERVICES\b)')
            }
    )

    if ($protocolLines.Count -eq 0) {
        return ""
    }

    $verb = (($CommandLine.Trim() -split '\s+', 2)[0]).ToUpperInvariant()
    $expectedPattern = switch ($verb) {
        "PING" { '^PONG$' }
        "STATUS" { '^STATUS\b' }
        "SCAN" { '^OK SCAN\b' }
        "SERVICES" { '^(OK SERVICES\b|SERVICES failed\b|ERR\b)' }
        "SET" { '^OK SET\b' }
        "HIT" { '^OK HIT\b' }
        "STOP" { '^OK STOP\b' }
        default { '^(PONG|STATUS\b|OK\b|ERR\b|SERVICES\b)' }
    }

    $matches = @($protocolLines | Where-Object { $_ -match $expectedPattern })
    if ($matches.Count -gt 0) {
        return $matches[-1]
    }

    return $protocolLines[-1]
}

function Send-GalakuSerialLine {
    param(
        [Parameter(Mandatory = $true)][string]$Line,
        [switch]$NoRetry
    )

    Ensure-GalakuSerial

    try {
        $script:Serial.Write("$Line`n")
        Write-BridgeLog "COM <= $Line"
    } catch {
        if ($NoRetry) {
            throw
        }

        Write-BridgeLog "Serial write failed, reopening ${SerialPort}: $($_.Exception.Message)"
        Open-GalakuSerial
        $script:Serial.Write("$Line`n")
        Write-BridgeLog "COM <= $Line"
    }

    Start-Sleep -Milliseconds 900

    $reply = ""
    try {
        $rawReply = $script:Serial.ReadExisting().Trim()
        if ($rawReply.Length -gt 0) {
            Write-BridgeLog "COM => $rawReply"
            $reply = Select-GalakuProtocolReply -RawReply $rawReply -CommandLine $Line
            if ($reply.Length -gt 0 -and $reply -ne $rawReply) {
                Write-BridgeLog "TCP => $reply"
            }
        }
    } catch {
        Write-BridgeLog "Serial read warning: $($_.Exception.Message)"
    }

    return $reply
}

Open-GalakuSerial

$listenIp = [System.Net.IPAddress]::Parse($ListenAddress)
$listener = [System.Net.Sockets.TcpListener]::new($listenIp, $ListenPort)
$listener.Server.SetSocketOption([System.Net.Sockets.SocketOptionLevel]::Socket, [System.Net.Sockets.SocketOptionName]::ReuseAddress, $true)
$listener.Start()
Write-BridgeLog "GALAKU TCP bridge listening on ${ListenAddress}:$ListenPort"

try {
    while ($true) {
        $client = $listener.AcceptTcpClient()
        $remote = $client.Client.RemoteEndPoint
        Write-BridgeLog "Client connected: $remote"

        try {
            $stream = $client.GetStream()
            $reader = [System.IO.StreamReader]::new($stream, [System.Text.Encoding]::ASCII, $false, 1024, $true)
            $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
            $writer = [System.IO.StreamWriter]::new($stream, $utf8NoBom, 1024, $false)
            $writer.NewLine = "`n"
            $writer.AutoFlush = $true

            while ($true) {
                $line = $reader.ReadLine()
                if ($null -eq $line) {
                    break
                }

                $line = $line.Trim()
                if ($line.Length -eq 0) {
                    continue
                }
                if ($line.Length -gt 64) {
                    Write-BridgeLog "Ignoring overlong command from $remote"
                    $writer.WriteLine("ERR command too long")
                    continue
                }

                $reply = Send-GalakuSerialLine -Line $line
                if ($reply.Length -gt 0) {
                    $writer.WriteLine($reply)
                } else {
                    $writer.WriteLine("OK SENT $line")
                }
            }
        } catch {
            Write-BridgeLog "Client error from ${remote}: $($_.Exception.Message)"
        } finally {
            try {
                $client.Close()
            } catch {
            }
            Write-BridgeLog "Client disconnected: $remote"
        }
    }
} finally {
    try {
        $listener.Stop()
    } catch {
    }
    Close-GalakuSerial -SendStop
}
