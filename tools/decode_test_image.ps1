[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$InputPath,
    [Parameter(Mandatory)][string]$OutputPath,
    [switch]$CompositeOverBlack
)
# Independent Windows raster decoder for our own generated QA fixtures.
# Maya 2024 writes PNG EXIF that FFmpeg rejects; the original file is unchanged.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing.Common
$bitmap = [System.Drawing.Bitmap]::new($InputPath, $false)
try {
    $pixels = [byte[]]::new($bitmap.Width * $bitmap.Height * 3)
    $index = 0
    $transparentPixels = 0
    for ($y = 0; $y -lt $bitmap.Height; $y++) {
        for ($x = 0; $x -lt $bitmap.Width; $x++) {
            $color = $bitmap.GetPixel($x, $y)
            if ($color.A -lt 255) { $transparentPixels++ }
            if ($CompositeOverBlack) {
                $pixels[$index++] = [math]::Floor(($color.R * $color.A + 127) / 255)
                $pixels[$index++] = [math]::Floor(($color.G * $color.A + 127) / 255)
                $pixels[$index++] = [math]::Floor(($color.B * $color.A + 127) / 255)
            } else {
                $pixels[$index++] = $color.R
                $pixels[$index++] = $color.G
                $pixels[$index++] = $color.B
            }
        }
    }
    [System.IO.File]::WriteAllBytes($OutputPath, $pixels)
    [pscustomobject]@{width=$bitmap.Width; height=$bitmap.Height; bytes=$pixels.Length; transparentPixels=$transparentPixels; compositeOverBlack=[bool]$CompositeOverBlack; decoder='Windows System.Drawing'} | ConvertTo-Json -Compress
} finally {
    $bitmap.Dispose()
}
