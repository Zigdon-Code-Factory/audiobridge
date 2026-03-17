$file = "C:\Users\Zack\Desktop\Projects\Personal\audiobridge\flutter_app\build\app\outputs\flutter-apk\app-release.apk"
$port = 8080
$listener = New-Object System.Net.HttpListener
$listener.Prefixes.Add("http://+:$port/")
$listener.Start()
Write-Host "Serving app-release.apk on http://192.168.0.78:$port/app-release.apk"
Write-Host "Press Ctrl+C to stop"
while ($listener.IsListening) {
    $context = $listener.GetContext()
    $response = $context.Response
    if ($context.Request.Url.AbsolutePath -eq "/app-release.apk") {
        $bytes = [System.IO.File]::ReadAllBytes($file)
        $response.ContentType = "application/vnd.android.package-archive"
        $response.ContentLength64 = $bytes.Length
        $response.AddHeader("Content-Disposition", "attachment; filename=app-release.apk")
        $response.OutputStream.Write($bytes, 0, $bytes.Length)
    } else {
        $msg = [System.Text.Encoding]::UTF8.GetBytes("<a href='/app-release.apk'>Download APK</a>")
        $response.ContentType = "text/html"
        $response.ContentLength64 = $msg.Length
        $response.OutputStream.Write($msg, 0, $msg.Length)
    }
    $response.Close()
}
