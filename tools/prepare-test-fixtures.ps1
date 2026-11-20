[CmdletBinding()]
param([string]$FixtureDirectory, [string]$BuildDirectory)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $FixtureDirectory) { $FixtureDirectory = Join-Path $root 'test-results\fixtures' }
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $root 'build' }
$required = @('bframes24-audio.mp4','cfr24-flash-audio.mkv','rational23976-stereo44100.mkv','start2s-lossless.mkv','stereo44100.wav','sync24-lossless.mkv','sync24-vfr-audio.mkv','two-audio-tracks.mkv','vfr-selected.mkv','color-fixtures.json')
foreach ($kind in @('sdr','pq','hlg')) { foreach ($bits in @(10,12)) { $required += "$kind-ramp-${bits}bit.mkv" } }
$stampPath = Join-Path $FixtureDirectory '.generators.sha256'
$stamp = (@('make_test_media.ps1','make_color_fixtures.py') | ForEach-Object { (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $_) -Algorithm SHA256).Hash }) -join ':'
$missing = @($required | Where-Object { -not (Test-Path -LiteralPath (Join-Path $FixtureDirectory $_) -PathType Leaf) })
if ($missing.Count -eq 0 -and (Test-Path -LiteralPath $stampPath) -and (Get-Content -LiteralPath $stampPath -Raw).Trim() -eq $stamp) {
    Write-Host 'Synthetic test fixtures are ready.'
    return
}
$ffmpeg = $env:OPENFRAME_TEST_FFMPEG
if (-not $ffmpeg) {
    $command = Get-Command ffmpeg.exe -ErrorAction SilentlyContinue
    if ($command) { $ffmpeg = $command.Source } else { $ffmpeg = Join-Path $env:ProgramFiles 'ffmpeg\bin\ffmpeg.exe' }
}
if (-not (Test-Path -LiteralPath $ffmpeg)) { throw 'Fixture generation requires FFmpeg CLI. Set OPENFRAME_TEST_FFMPEG to ffmpeg.exe.' }
$ffprobe = Join-Path (Split-Path -Parent $ffmpeg) 'ffprobe.exe'
if (-not (Test-Path -LiteralPath $ffprobe)) { throw 'ffprobe.exe must be beside ffmpeg.exe.' }
$python = $null
$cache = Join-Path $BuildDirectory 'CMakeCache.txt'
if (Test-Path -LiteralPath $cache) {
    foreach ($line in (Get-Content -LiteralPath $cache)) {
        if ($line -match '^_?Python3_EXECUTABLE:[^=]+=(.+)$') { $python = $Matches[1] }
    }
}
if (-not $python) { $python = (Get-Command python.exe -ErrorAction Stop).Source }
Write-Host 'Generating synthetic test fixtures...'
& (Join-Path $PSScriptRoot 'make_test_media.ps1') -Ffmpeg $ffmpeg -OutputDirectory $FixtureDirectory
& $python (Join-Path $PSScriptRoot 'make_color_fixtures.py') --directory $FixtureDirectory --ffmpeg $ffmpeg --ffprobe $ffprobe
if ($LASTEXITCODE) { throw 'Color fixture generation failed.' }
foreach ($name in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $FixtureDirectory $name) -PathType Leaf)) { throw "Missing generated fixture: $name" }
}
Set-Content -LiteralPath $stampPath -Value $stamp -Encoding ascii
