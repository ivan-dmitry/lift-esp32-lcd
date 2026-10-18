# Update the display's firmware over WiFi.
# Asks for the technician code: it is typed here, sent only to the display, never stored.
#   powershell -ExecutionPolicy Bypass -File tools/ota_upload.ps1 [-Ip 192.168.31.16] [-Bin firmware.bin]
param(
  [string]$Ip  = "192.168.31.16",
  [string]$Bin = "$PSScriptRoot\..\.pio\build\lift\firmware.bin"
)
if (-not (Test-Path $Bin)) { Write-Host "No firmware at $Bin - build it first (pio run -e lift)"; exit 1 }
$Bin = (Resolve-Path $Bin).Path

# Is the display there at all? (before asking for the code)
$st = (curl.exe -s -m 6 "http://$Ip/api/state") -join ""
if (-not $st) { Write-Host "The display at $Ip does not answer - is it powered and on the network?"; exit 1 }

# The code comes from firmware/ota_code.txt when the owner created that file (one
# line, the 4-digit code) — then updates run without anyone typing it. Otherwise ask.
$codeFile = Join-Path $PSScriptRoot "..\ota_code.txt"
if (Test-Path $codeFile) {
  $code = (Get-Content $codeFile -TotalCount 1).Trim()
  Write-Host "Using the technician code from ota_code.txt"
} else {
  $sec  = Read-Host "Technician code for the display at $Ip" -AsSecureString
  $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec)
  $code = [Runtime.InteropServices.Marshal]::PtrToStringAuto($bstr)
  [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr)
}
# -join: curl's output is a string array in PowerShell; -match on an array filters
# instead of matching and leaves $Matches empty
$r = (curl.exe -s -m 10 --data-urlencode "pin=$code" "http://$Ip/api/login") -join ""
$code = $null
$m = [regex]::Match($r, '"t":"([0-9A-Fa-f]+)"')
if (-not $m.Success) {
  if (-not $r) { Write-Host "No answer from the display at $Ip during login - try again" }
  else { Write-Host "Login refused: $r" }
  exit 1
}
$tok = $m.Groups[1].Value

$size = (Get-Item $Bin).Length
Write-Host "Uploading $size bytes - about 30 s; the display restarts when it is done..."
$res = curl.exe -s -m 240 -F "firmware=@$Bin" "http://$Ip/api/update?t=$tok"
Write-Host $res
if ($res -match '"ok":true') { Write-Host "Done. The display is restarting with the new firmware." }
else { Write-Host "Upload failed - the old firmware keeps running." ; exit 1 }
