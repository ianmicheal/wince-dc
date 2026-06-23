<#
  unwrap-image.ps1 : 0winceos.bin (Sega-wrapped) -> raw memory image (for inspection)
  Strips the 0x800 header using its own offset/length fields.
  usage: powershell -File unwrap-image.ps1 -In 0winceos.bin -Out rawmem.bin
#>
param(
  [Parameter(Mandatory)][string]$In,
  [Parameter(Mandatory)][string]$Out
)
$ErrorActionPreference = "Stop"
$b = [System.IO.File]::ReadAllBytes($In)
if ($b[0] -ne 0xD6 -or $b[1] -ne 0x1A) { throw "not a Sega-wrapped image (magic != D6 1A)" }
function GetU32([byte[]]$a,[int]$o){ [uint32]$a[$o] -bor ([uint32]$a[$o+1] -shl 8) -bor ([uint32]$a[$o+2] -shl 16) -bor ([uint32]$a[$o+3] -shl 24) }
$base = GetU32 $b 0x14
$off  = GetU32 $b 0x18
$len  = GetU32 $b 0x1C
$take = [math]::Min([int]$len, $b.Length - [int]$off)
$payload = New-Object byte[] $take
[Array]::Copy($b, [int]$off, $payload, 0, $take)
[System.IO.File]::WriteAllBytes($Out, $payload)
"unwrapped -> {0}" -f $Out
"  base=0x{0:X8}  payload-off=0x{1:X}  len=0x{2:X}  wrote={3} bytes" -f $base,$off,$len,$take
