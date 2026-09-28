<#
  Extracts the files of an Xbox 360 disc image (.iso, XDVDFS / XGD2 / XGD3) into a folder.

    powershell -ExecutionPolicy Bypass -File extract_xiso.ps1 -Iso game.iso -Out game_root [-OnlyXex]

  -OnlyXex extracts only default.xex (and writes the full file list), which is all the code generator needs.
  Nothing is sent anywhere: it reads the ISO on your computer and writes the files next to it.
#>
param(
  [Parameter(Mandatory = $true)][string]$Iso,
  [string]$Out = "game_root",
  [switch]$OnlyXex
)
$ErrorActionPreference = "Stop"
$SectorSize = 2048
$MAGIC = [Text.Encoding]::ASCII.GetBytes("MICROSOFT*XBOX*MEDIA")

$fs = [IO.File]::Open((Resolve-Path $Iso), 'Open', 'Read', 'Read')
$br = New-Object IO.BinaryReader($fs)

function Read-At([long]$pos, [int]$len) { $fs.Position = $pos; return $br.ReadBytes($len) }
function Test-Magic([long]$pos) {
  if ($pos + 20 -gt $fs.Length) { return $false }
  $b = Read-At $pos 20
  for ($i = 0; $i -lt 20; $i++) { if ($b[$i] -ne $MAGIC[$i]) { return $false } }
  return $true
}

# Game partition offsets: plain XISO, XGD3, XGD2, XGD1.
$partition = $null
foreach ($off in @(0, 0x2080000, 0xFD90000, 0x18300000)) {
  if (Test-Magic ($off + 32 * $SectorSize)) { $partition = [long]$off; break }
}
if ($null -eq $partition) { throw "No XDVDFS volume found: is this an Xbox 360 disc image?" }
Write-Host ("Game partition at 0x{0:X}" -f $partition)

$vd = Read-At ($partition + 32 * $SectorSize + 20) 8
$rootSector = [BitConverter]::ToUInt32($vd, 0)
$rootSize = [BitConverter]::ToUInt32($vd, 4)

New-Item -ItemType Directory -Force -Path $Out | Out-Null
$outRoot = (Resolve-Path $Out).Path
$listing = New-Object Collections.Generic.List[string]
$buffer = New-Object byte[] (4MB)

function Copy-File([long]$pos, [long]$size, [string]$dest) {
  $imageLength = $fs.Length
  if ($pos + $size -gt $imageLength) {
    throw ("File data out of range: offset 0x{0:X} + {1} bytes, image is {2} bytes" -f $pos, $size, $imageLength)
  }
  $in = [IO.File]::Open((Resolve-Path $Iso), 'Open', 'Read', 'Read')
  $o = [IO.File]::Create($dest)
  try {
    [void]$in.Seek($pos, [IO.SeekOrigin]::Begin)
    [long]$remaining = $size
    while ($remaining -gt 0) {
      [int]$want = $buffer.Length
      if ($remaining -lt $want) { $want = [int]$remaining }
      [int]$n = $in.Read($buffer, 0, $want)
      if ($n -le 0) {
        throw ("Read returned 0 at offset 0x{0:X} (image {1} bytes, {2} bytes left)" -f $in.Position, $imageLength, $remaining)
      }
      $o.Write($buffer, 0, $n); $remaining -= $n
    }
  } finally { $o.Close(); $in.Close() }
}

# Each directory is a binary tree of entries; offsets are in 4-byte units from the start of the directory.
$dirs = New-Object Collections.Stack
$dirs.Push(@($rootSector, $rootSize, ""))
while ($dirs.Count -gt 0) {
  $d = $dirs.Pop()
  $data = Read-At ($partition + [long]$d[0] * $SectorSize) ([int]$d[1])
  $nodes = New-Object Collections.Stack
  $nodes.Push(0)
  $seen = @{}
  while ($nodes.Count -gt 0) {
    $o = [int]$nodes.Pop()
    if ($seen.ContainsKey($o) -or $o + 14 -gt $data.Length) { continue }
    $seen[$o] = $true
    $left = [BitConverter]::ToUInt16($data, $o)
    if ($left -eq 0xFFFF) { continue }
    $right = [BitConverter]::ToUInt16($data, $o + 2)
    $sector = [BitConverter]::ToUInt32($data, $o + 4)
    $size = [BitConverter]::ToUInt32($data, $o + 8)
    $attr = $data[$o + 12]
    $nlen = $data[$o + 13]
    $name = [Text.Encoding]::ASCII.GetString($data, $o + 14, $nlen)
    if ($left -ne 0) { $nodes.Push($left * 4) }
    if ($right -ne 0) { $nodes.Push($right * 4) }
    $rel = if ($d[2]) { "$($d[2])\$name" } else { $name }
    if ($attr -band 0x10) {
      $listing.Add("$rel\")
      if (-not $OnlyXex) { New-Item -ItemType Directory -Force -Path (Join-Path $outRoot $rel) | Out-Null }
      if ($size -gt 0) { $dirs.Push(@($sector, $size, $rel)) }
    } else {
      $listing.Add(("{0}`t{1}" -f $rel, $size))
      $want = (-not $OnlyXex) -or ($rel -ieq "default.xex")
      if ($want) {
        $dest = Join-Path $outRoot $rel
        New-Item -ItemType Directory -Force -Path (Split-Path $dest) | Out-Null
        Write-Host ("  {0} ({1:N1} MB)" -f $rel, ($size / 1MB))
        Copy-File ($partition + [long]$sector * $SectorSize) $size $dest
      }
    }
  }
}
$fs.Close()
$listing.Sort()
[IO.File]::WriteAllLines((Join-Path $outRoot "..\file_list.txt"), $listing)
Write-Host "Done: $($listing.Count) entries. File list written to file_list.txt"
