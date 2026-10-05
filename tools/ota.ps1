# Upload firmware to the device OTA endpoint.
# The default N16R8 build includes the shared DWC2 timing fixes.
# To upload a reviewed archive instead, pass -Bin explicitly, for example:
#   .\tools\ota.ps1 -Bin .\.cache\firmware-1.2.51\firmware.bin
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
$firmwarePath = (Resolve-Path -LiteralPath $Bin).Path
$bytes = [IO.File]::ReadAllBytes($firmwarePath)
if ($bytes.Length -eq 0) { throw "firmware is empty: $firmwarePath" }
$firmwareHash = (Get-FileHash -LiteralPath $firmwarePath -Algorithm SHA256).Hash.ToLowerInvariant()
"firmware: $firmwarePath"
"size: $($bytes.Length) bytes; SHA256: $firmwareHash"

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
$resp = $null
try {
    $resp = $client.PostAsync("http://$Host_/api/ota/upload", $content).GetAwaiter().GetResult()
    "HTTP $([int]$resp.StatusCode)"
    $responseBody = $resp.Content.ReadAsStringAsync().GetAwaiter().GetResult()
    $responseBody
    if (-not $resp.IsSuccessStatusCode) {
        throw "OTA upload failed: HTTP $([int]$resp.StatusCode)"
    }
    $reply = $responseBody | ConvertFrom-Json
    if ($reply.success -ne $true) {
        throw 'The device did not confirm successful OTA activation.'
    }
}
finally {
    if ($null -ne $resp) { $resp.Dispose() }
    $content.Dispose()
    $client.Dispose()
}
