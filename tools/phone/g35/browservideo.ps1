# browservideo.ps1 [-Browser msedge] [-Clip C:\S10eTmp\mfc\test.mp4] [-Wait 20] - (user session, via runuser2.ps1)
# G35 (notes/g35-browser-video.md): open a local page that plays the clip (muted, looped, <video> + a frame counter
# from requestVideoFrameCallback shown in the title), wait, screenshot (shot.ps1) -> C:\S10eTmp\g35\<browser>.png.
# The MFC probe (does the browser hold the MFC?) runs from SSH next to it (g35/videorun.sh).
param([string]$Browser = 'msedge', [string]$Clip = 'C:\S10eTmp\mfc\test.mp4', [int]$Wait = 20)
$d = 'C:\S10eTmp\g35'; New-Item -ItemType Directory -Force $d | Out-Null
Copy-Item $Clip "$d\clip.mp4" -Force
@'
<!doctype html><html><head><meta charset="utf-8"><title>g35 0</title>
<style>body{margin:0;background:#000;color:#fff;font:20px sans-serif}video{width:100vw;height:90vh}</style></head>
<body><video id="v" src="clip.mp4" autoplay muted loop playsinline></video><div id="s">-</div>
<script>
const v = document.getElementById('v'), s = document.getElementById('s'); let n = 0, t0 = 0;
function cb(now, m) { n++; if (!t0) t0 = now; const fps = n > 1 ? (n - 1) * 1000 / (now - t0) : 0;
  s.textContent = 'frames ' + n + '  fps ' + fps.toFixed(1) + '  dropped ' + (v.getVideoPlaybackQuality ? v.getVideoPlaybackQuality().droppedVideoFrames : '?');
  document.title = 'g35 ' + n; v.requestVideoFrameCallback(cb); }
v.requestVideoFrameCallback(cb);
</script></body></html>
'@ | Set-Content "$d\play.html" -Encoding UTF8
$exe = switch ($Browser) { 'brave' { 'C:\Program Files\BraveSoftware\Brave-Browser\Application\brave.exe' } default { 'msedge' } }
Start-Process -FilePath $exe -ArgumentList '--new-window', "file:///C:/S10eTmp/g35/play.html"
Start-Sleep -Seconds $Wait
& C:\S10eTmp\shot.ps1 -Out "$d\$Browser.png"
