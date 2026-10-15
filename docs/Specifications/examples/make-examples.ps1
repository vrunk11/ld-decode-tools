# Makes the example files of this directory with ld-container (PowerShell 5.1 or later).
#
#   .\make-examples.ps1 -Bin path\to\ld-container.exe [-Out .]
#
# The captures are made up: a TBC of 4 fields of 16 x 8 samples, and a CVBS capture of 2 NTSC frames with 100 samples of
# 24-bit audio. Their samples are ramps, so that the files are small and the content is easy to check. The expected content of
# each file, with the SHA-256 of what unpack gives back, is in README.md.
param(
    [Parameter(Mandatory = $true)][string]$Bin,
    [string]$Out = "."
)
$ErrorActionPreference = "Stop"
$Out = (Resolve-Path $Out).Path
$Bin = (Resolve-Path $Bin).Path
$work = Join-Path ([IO.Path]::GetTempPath()) ("ldx-examples-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force $work | Out-Null
Push-Location $work

function Metadata($system, $width, $height, $fields) {
    $video = [ordered]@{
        activeVideoEnd = 0; activeVideoStart = 0; black16bIre = 15360; blanking16bIre = 15360; colourBurstEnd = 0; colourBurstStart = 0
        fieldHeight = $height; fieldWidth = $width; gitBranch = "example"; gitCommit = "example"; isMapped = $false; isSourcePal = ($system -eq "PAL")
        isSubcarrierLocked = $true; isWidescreen = $false; numberOfSequentialFields = $fields; sampleRate = 14318181.818181818
        system = $system; white16bIre = 51200
    }
    $list = @()
    for ($i = 1; $i -le $fields; $i++) {
        $list += [ordered]@{ isFirstField = ($i % 2 -eq 1); seqNo = $i; syncConf = 100; fieldPhaseID = ((($i - 1) % 4) + 1); medianBurstIRE = 20.5; pad = $false }
    }
    [ordered]@{ fields = $list; pcmAudioParameters = [ordered]@{ bits = 16; isLittleEndian = $true; isSigned = $true; sampleRate = 44100 }; videoParameters = $video }
}

# --- a TBC of 4 fields of 16 x 8 samples, and 100 samples of 16-bit audio
$tbc = New-Object byte[] (4 * 16 * 8 * 2)
for ($f = 0; $f -lt 4; $f++) { for ($s = 0; $s -lt 128; $s++) {
    $v = 15360 + $f * 1000 + $s * 40
    $tbc[($f * 128 + $s) * 2] = $v -band 255; $tbc[($f * 128 + $s) * 2 + 1] = ($v -shr 8) -band 255 } }
[IO.File]::WriteAllBytes("$work\tiny.tbc", $tbc)
(Metadata "NTSC" 16 8 4) | ConvertTo-Json -Depth 8 | Set-Content "$work\tiny.tbc.json" -Encoding ascii
$pcm = New-Object byte[] (100 * 4)
for ($i = 0; $i -lt 100; $i++) { $l = $i * 100 - 5000; $r = 5000 - $i * 100
    $pcm[$i * 4] = $l -band 255; $pcm[$i * 4 + 1] = ($l -shr 8) -band 255; $pcm[$i * 4 + 2] = $r -band 255; $pcm[$i * 4 + 3] = ($r -shr 8) -band 255 }
[IO.File]::WriteAllBytes("$work\tiny.pcm", $pcm)
$efm = New-Object byte[] 300
for ($i = 0; $i -lt 300; $i++) { $efm[$i] = 3 + ($i % 9) }
[IO.File]::WriteAllBytes("$work\tiny.efm", $efm)

& $Bin pack tiny.tbc --pcm tiny.pcm --efm tiny.efm --pcm-rate 44100 -o "$Out\tbc-raw.ldx" | Out-Null
& $Bin pack tiny.tbc --pcm tiny.pcm --pcm-rate 44100 --codec flac -o "$Out\tbc-flac.ldx" | Out-Null
& $Bin pack tiny.tbc --pcm tiny.pcm --pcm-rate 44100 --live-metadata -o "$Out\tbc-stream.ldx" | Out-Null

# --- a Y/C capture: the chroma file is the luma with another offset
$chroma = $tbc.Clone(); for ($i = 0; $i -lt $chroma.Length; $i += 2) { $chroma[$i] = [byte](($chroma[$i] + 7) -band 255) }
[IO.File]::WriteAllBytes("$work\tiny_chroma.tbc", $chroma)
& $Bin pack tiny.tbc -o "$Out\tbc-yc.ldx" | Out-Null

# --- a CVBS capture of 2 NTSC frames (477,750 samples each, 10-bit values) and 100 samples of 24-bit stereo audio
$frame = 477750
$cvbs = New-Object byte[] (2 * $frame * 2)
for ($f = 0; $f -lt 2; $f++) { for ($s = 0; $s -lt $frame; $s++) {
    $v = (240 + (($s % 910) / 4) + $f * 8) % 1024
    $o = ($f * $frame + $s) * 2; $cvbs[$o] = [int]$v -band 255; $cvbs[$o + 1] = ([int]$v -shr 8) -band 255 } }
[IO.File]::WriteAllBytes("$work\tiny.cvbs", $cvbs)
(Metadata "NTSC" 910 263 4) | ConvertTo-Json -Depth 8 | Set-Content "$work\tiny.cvbs.json" -Encoding ascii
$wav = New-Object IO.MemoryStream
$w = New-Object IO.BinaryWriter($wav)
$data = 100 * 6
$w.Write([Text.Encoding]::ASCII.GetBytes("RIFF")); $w.Write([uint32](36 + $data)); $w.Write([Text.Encoding]::ASCII.GetBytes("WAVEfmt "))
$w.Write([uint32]16); $w.Write([uint16]1); $w.Write([uint16]2); $w.Write([uint32]48000); $w.Write([uint32](48000 * 6)); $w.Write([uint16]6); $w.Write([uint16]24)
$w.Write([Text.Encoding]::ASCII.GetBytes("data")); $w.Write([uint32]$data)
for ($i = 0; $i -lt 100; $i++) { foreach ($v in @(($i * 10000 - 500000), (500000 - $i * 10000))) {
    $b = [BitConverter]::GetBytes([int]$v); $w.Write($b[0]); $w.Write($b[1]); $w.Write($b[2]) } }
[IO.File]::WriteAllBytes("$work\tiny_audio_0.wav", $wav.ToArray())
& $Bin pack tiny.cvbs --codec auto -o "$Out\cvbs-ffv1.ldx" | Out-Null

Pop-Location
Get-ChildItem $Out -Filter *.ldx | Select-Object Name, Length
Write-Host "Work files (the inputs): $work"
