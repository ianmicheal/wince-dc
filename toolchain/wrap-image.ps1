<#
  wrap-image.ps1  :  NK.bin (B000FF CE ROM)  ->  0winceos.bin (Sega-wrapped, DC-loadable)

  Chain (validated 2026-06-23):
    DUMPNK NK.bin rawmem        # B000FF ROM -> flat raw memory image
    prepend 0x800 Sega header   # magic D61A, load base = phys(start), off 0x800, padded len
    append raw + pad to 0x800   # -> bootable 0winceos.bin

  usage:
    powershell -File wrap-image.ps1 -NkBin C:\wcedreamcast\release\retail\NK.bin `
                                    -Out   C:\wcedreamcast\release\retail\0winceos.bin
#>
param(
  [Parameter(Mandatory)][string]$NkBin,
  [Parameter(Mandatory)][string]$Out,
  [string]$DcSdk = "C:\wcedreamcast"
)
$ErrorActionPreference = "Stop"
$dumpnk = Join-Path $DcSdk "tools\DUMPNK.exe"
if (-not (Test-Path $dumpnk)) { throw "DUMPNK not found: $dumpnk" }
if (-not (Test-Path $NkBin))  { throw "NK.bin not found: $NkBin" }

$raw = [System.IO.Path]::GetTempFileName()
$log = & $dumpnk $NkBin $raw 2>&1
$m = $log | Select-String -Pattern 'Physical start address\s*:\s*([0-9a-fA-F]+)'
if (-not $m) { Write-Host ($log -join "`n"); throw "could not parse DUMPNK 'Physical start address'" }
$start = [Convert]::ToUInt32($m.Matches[0].Groups[1].Value, 16)
$phys  = $start -band 0x1FFFFFFF        # cached 0x8Cxxxxxx -> physical 0x0Cxxxxxx
$rawBytes = [System.IO.File]::ReadAllBytes($raw)
Remove-Item $raw -Force

$hdrSize = 0x800
$padLen  = [int]([math]::Ceiling($rawBytes.Length / [double]$hdrSize) * $hdrSize)

function PutU32([byte[]]$a,[int]$off,[uint32]$v){
  $a[$off]=[byte]($v -band 0xFF); $a[$off+1]=[byte](($v -shr 8) -band 0xFF)
  $a[$off+2]=[byte](($v -shr 16) -band 0xFF); $a[$off+3]=[byte](($v -shr 24) -band 0xFF)
}
$hdr = New-Object byte[] $hdrSize
$hdr[0]=0xD6; $hdr[1]=0x1A
PutU32 $hdr 0x10 ([uint32]1)            # region count
PutU32 $hdr 0x14 $phys                  # load base (physical)
PutU32 $hdr 0x18 ([uint32]$hdrSize)     # payload offset = 0x800
PutU32 $hdr 0x1C ([uint32]$padLen)      # payload length (padded to 0x800)
PutU32 $hdr 0x20 $phys                  # base (repeat)

$fs = [System.IO.File]::Open($Out,'Create')
try {
  $fs.Write($hdr,0,$hdr.Length)
  $fs.Write($rawBytes,0,$rawBytes.Length)
  $pad = $padLen - $rawBytes.Length
  if ($pad -gt 0) { $fs.Write((New-Object byte[] $pad),0,$pad) }
} finally { $fs.Close() }

"wrapped -> {0}" -f $Out
"  base=0x{0:X8}  rawlen={1}  padlen=0x{2:X}  total={3} bytes" -f `
   $phys,$rawBytes.Length,$padLen,((Get-Item $Out).Length)
