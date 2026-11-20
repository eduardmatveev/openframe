[CmdletBinding()]
param([string]$Ffmpeg = 'ffmpeg', [string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
if (-not $OutputDirectory) { $OutputDirectory = Join-Path (Split-Path -Parent $PSScriptRoot) 'test-results\fixtures' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
# These inputs are mathematical signals and test patterns, never user media.
$syncPath = Join-Path $OutputDirectory 'sync24-lossless.mkv'
& $Ffmpeg -v error -nostdin -y -f lavfi -i 'testsrc2=size=320x180:rate=24:duration=5' -f lavfi -i 'aevalsrc=0.3*sin(2*PI*440*t)*lt(mod(t\,1)\,0.04):s=48000:d=5' -vf 'drawbox=x=0:y=0:w=64:h=64:color=white:t=fill:enable=lt(mod(n\,24)\,1)' -c:v ffv1 -pix_fmt yuv420p -c:a pcm_s16le -shortest $syncPath
if ($LASTEXITCODE) { throw "Sync fixture generation failed: $LASTEXITCODE" }
& $Ffmpeg -v error -nostdin -y -i $syncPath -f lavfi -i 'aevalsrc=0.2*sin(2*PI*440*t):s=48000:d=3' -f lavfi -i 'aevalsrc=0.2*sin(2*PI*880*t)|0.1*sin(2*PI*660*t):s=44100:d=3' -map 0:v:0 -map 1:a:0 -map 2:a:0 -t 3 -c:v copy -c:a pcm_s16le -metadata:s:a:0 title=Mono440 -metadata:s:a:0 language=eng -metadata:s:a:1 title=Stereo880_660 -metadata:s:a:1 language=fra -disposition:a:0 default -disposition:a:1 0 (Join-Path $OutputDirectory 'two-audio-tracks.mkv')
if ($LASTEXITCODE) { throw "Multiple audio tracks fixture generation failed: $LASTEXITCODE" }
& $Ffmpeg -v error -nostdin -y -i $syncPath -c:v libx264 -crf 18 -g 48 -bf 3 -c:a aac (Join-Path $OutputDirectory 'sync24-bframes.mp4')
if ($LASTEXITCODE) { throw "B-frame fixture generation failed: $LASTEXITCODE" }
& $Ffmpeg -v error -nostdin -y -i $syncPath -an -vf 'select=not(eq(mod(n\,5)\,0))' -fps_mode vfr -c:v ffv1 (Join-Path $OutputDirectory 'sync24-vfr.mkv')
if ($LASTEXITCODE) { throw "VFR fixture generation failed: $LASTEXITCODE" }
& $Ffmpeg -v error -nostdin -y -i $syncPath -vf 'select=not(eq(mod(n\,5)\,2))+not(mod(n\,24))' -fps_mode vfr -c:v ffv1 -c:a copy (Join-Path $OutputDirectory 'sync24-vfr-audio.mkv')
if ($LASTEXITCODE) { throw "VFR audio fixture generation failed: $LASTEXITCODE" }
& $Ffmpeg -v error -nostdin -y -f lavfi -i 'testsrc2=size=320x180:rate=24000/1001:duration=4' -f lavfi -i 'sine=frequency=880:sample_rate=44100:duration=4' -c:v ffv1 -c:a pcm_s16le -ac 2 -shortest (Join-Path $OutputDirectory 'rational23976-stereo44100.mkv')
if ($LASTEXITCODE) { throw "Rational-fps fixture generation failed: $LASTEXITCODE" }
& $Ffmpeg -v error -nostdin -y -f lavfi -i 'sine=frequency=880:sample_rate=44100:duration=5' -ac 2 -c:a pcm_s16le (Join-Path $OutputDirectory 'stereo44100.wav')
if ($LASTEXITCODE) { throw "Audio fixture generation failed: $LASTEXITCODE" }
& $Ffmpeg -v error -nostdin -y -f lavfi -i 'testsrc2=size=320x180:rate=24:duration=2' -f lavfi -i 'aevalsrc=0.3*sin(2*PI*440*t)*lt(mod(t\,1)\,0.04):s=48000:d=2' -vf 'drawbox=x=0:y=0:w=64:h=64:color=white:t=fill:enable=lt(mod(n\,24)\,1),setpts=PTS+2/TB' -af 'asetpts=PTS+2/TB' -c:v ffv1 -c:a pcm_s16le -copyts (Join-Path $OutputDirectory 'start2s-lossless.mkv')
if ($LASTEXITCODE) { throw "Nonzero-start fixture generation failed: $LASTEXITCODE" }
# Retain the fixture names used by the original regression cases as well.
Copy-Item -LiteralPath $syncPath -Destination (Join-Path $OutputDirectory 'cfr24-flash-audio.mkv') -Force
Copy-Item -LiteralPath (Join-Path $OutputDirectory 'sync24-bframes.mp4') -Destination (Join-Path $OutputDirectory 'bframes24-audio.mp4') -Force
Copy-Item -LiteralPath (Join-Path $OutputDirectory 'sync24-vfr.mkv') -Destination (Join-Path $OutputDirectory 'vfr-selected.mkv') -Force

# Ninety-second H.264/AAC inputs exercise seeking near the end of a 30-second GOP.
# A visible source ordinal makes first/last exported frame comparisons interpretable.
foreach ($longCase in @(
    @{ Name = 'long2997-gop900-aac.mp4'; Rate = '30000/1001'; Frames = 2700; Seconds = '90.09'; Gop = 900 },
    @{ Name = 'long60-gop1800-aac.mp4'; Rate = '60'; Frames = 5400; Seconds = '90'; Gop = 1800 }
)) {
    $longPattern = 'testsrc2=size=160x90:rate=' + $longCase.Rate
    $longAudio = 'aevalsrc=0.16*sin(2*PI*440*t)|0.12*sin(2*PI*880*t):s=48000'
    $longLabels = "drawtext=fontfile='C\:/Windows/Fonts/consola.ttf':text='%{n}':x=4:y=4:fontsize=16:fontcolor=white:box=1:boxcolor=black"
    & $Ffmpeg -v error -nostdin -y -f lavfi -i $longPattern -f lavfi -i $longAudio -vf $longLabels -frames:v $longCase.Frames -t $longCase.Seconds -c:v libx264 -preset fast -crf 16 -pix_fmt yuv420p -g $longCase.Gop -keyint_min $longCase.Gop -sc_threshold 0 -bf 3 -x264-params 'b-adapt=0:open-gop=0' -colorspace bt709 -color_trc bt709 -color_primaries bt709 -color_range tv -c:a aac -b:a 128k -ar 48000 -ac 2 -shortest (Join-Path $OutputDirectory $longCase.Name)
    if ($LASTEXITCODE) { throw "Long-GOP fixture generation failed: $($longCase.Name): $LASTEXITCODE" }
}
Write-Host "Synthetic media: $OutputDirectory"
