# Upload firmware to the device OTA endpoint.
# Invoke-WebRequest mangles large bodies on this PowerShell build, so build the
# multipart payload by hand and post it through System.Net.Http instead.
param(
    [string]$Host_ = "192.168.31.4",
    [string]$Bin   = ".pio\build\esp32s3_n16r8\firmware.bin"
)

$ErrorActionPreference = "Stop"
$OutputEncoding = [Console]::OutputEncoding = [Text.Encoding]::UTF8
Add-Type -AssemblyName System.Net.Http

if (-not (Test-Path -LiteralPath $Bin)) { throw "firmware not found: $Bin" }
$bytes = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Bin))

$boundary = "----oc$([guid]::NewGuid().ToString('N'))"
$head = "--$boundary`r`n" +
        "Content-Disposition: form-data; name=`"firmware`"; filename=`"firmware.bin`"`r`n" +
        "Content-Type: application/octet-stream`r`n`r`n"
$tail = "`r`n--$boundary--`r`n"

$hb = [Text.Encoding]::ASCII.GetBytes($head)
$tb = [Text.Encoding]::ASCII.GetBytes($tail)
$body = New-Object byte[] ($hb.Length + $bytes.Length + $tb.Length)
[Array]::Copy($hb, 0, $body, 0, $hb.Length)
[Array]::Copy($bytes, 0, $body, $hb.Length, $bytes.Length)
[Array]::Copy($tb, 0, $body, $hb.Length + $bytes.Length, $tb.Length)

$client = New-Object System.Net.Http.HttpClient
$client.Timeout = [TimeSpan]::FromSeconds(300)
$content = New-Object System.Net.Http.ByteArrayContent(, $body)
$content.Headers.ContentType =
    [System.Net.Http.Headers.MediaTypeHeaderValue]::Parse("multipart/form-data; boundary=$boundary")

"posting $($body.Length) bytes to http://$Host_/api/ota/upload ..."
try {
    $resp = $client.PostAsync("http://$Host_/api/ota/upload", $content).GetAwaiter().GetResult()
    "HTTP $([int]$resp.StatusCode)"
    $resp.Content.ReadAsStringAsync().GetAwaiter().GetResult()
}
finally {
    $client.Dispose()
}
