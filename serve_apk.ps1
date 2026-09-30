param(
    [int]$Port = 8081
)

$file = Join-Path $PSScriptRoot "flutter_app\build\app\outputs\flutter-apk\app-release.apk"
if (-not (Test-Path -LiteralPath $file)) {
    throw "APK not found at $file. Run the Flutter APK build first."
}

$ip = $null
foreach ($line in (ipconfig)) {
    if ($line -match 'IPv4 Address[.\s]*:\s*(\d+\.\d+\.\d+\.\d+)') {
        $candidate = $matches[1]
        if ($candidate -match '^192\.168\.') {
            $ip = $candidate
            break
        }
        if (-not $ip -and $candidate -match '^(10\.|172\.(1[6-9]|2\d|3[0-1])\.)') {
            $ip = $candidate
        }
    }
}

if (-not $ip) {
    $ip = "127.0.0.1"
}

$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Any, $Port)
$listener.Start()

Write-Host "Serving app-release.apk on http://$ip`:$Port/app-release.apk"
Write-Host "Press Ctrl+C to stop"

while ($true) {
    $client = $listener.AcceptTcpClient()

    try {
        $stream = $client.GetStream()
        $buffer = New-Object byte[] 4096
        $read = $stream.Read($buffer, 0, $buffer.Length)
        $request = [System.Text.Encoding]::ASCII.GetString($buffer, 0, $read)
        $firstLine = ($request -split "`r?`n")[0]
        $isHead = $firstLine -like "HEAD *"
        $path = "/"

        if ($firstLine -match '^\w+\s+([^\s]+)') {
            $path = [System.Uri]::UnescapeDataString($matches[1].Split('?')[0])
        }

        if ($path -eq "/app-release.apk") {
            $apk = Get-Item -LiteralPath $file
            $headers = @(
                "HTTP/1.1 200 OK"
                "Content-Type: application/vnd.android.package-archive"
                "Content-Length: $($apk.Length)"
                "Content-Disposition: attachment; filename=app-release.apk"
                "Connection: close"
                ""
                ""
            ) -join "`r`n"

            $headerBytes = [System.Text.Encoding]::ASCII.GetBytes($headers)
            $stream.Write($headerBytes, 0, $headerBytes.Length)

            if (-not $isHead) {
                $fileStream = [System.IO.File]::OpenRead($file)
                try {
                    $fileStream.CopyTo($stream)
                } finally {
                    $fileStream.Dispose()
                }
            }
        } else {
            $body = "<a href='/app-release.apk'>Download APK</a>"
            $bodyBytes = [System.Text.Encoding]::UTF8.GetBytes($body)
            $headers = @(
                "HTTP/1.1 200 OK"
                "Content-Type: text/html; charset=utf-8"
                "Content-Length: $($bodyBytes.Length)"
                "Connection: close"
                ""
                ""
            ) -join "`r`n"

            $headerBytes = [System.Text.Encoding]::ASCII.GetBytes($headers)
            $stream.Write($headerBytes, 0, $headerBytes.Length)
            if (-not $isHead) {
                $stream.Write($bodyBytes, 0, $bodyBytes.Length)
            }
        }
    } catch {
        Write-Warning $_.Exception.Message
    } finally {
        $client.Close()
    }
}
