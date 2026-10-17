# templog.ps1 [-Seconds 130] [-Out C:\S10eTmp\templog.txt] - S10ePower temperatures, guard level and granted clocks
# once a second (s10epowerctl state), for stability runs (notes/clocks-dvfs.md): did a zone really reach the 83 C
# guard trip, or was it one odd reading?
param([int]$Seconds = 130, [string]$Out = 'C:\S10eTmp\templog.txt')
Remove-Item $Out -ErrorAction SilentlyContinue
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
  $s = & C:\S10eTmp\s10epowerctl.exe state
  $a = $s[0] -replace '.*temp ', '' -replace ' \(reads.*', ''
  $g = $s[0] -replace '.*guard (\d+).*', '$1'
  $c = ($s[1] -replace 'cap \d+ req \d+ ', '' -replace ' kHz', '')
  '{0,5:N0} s  guard {1}  {2}  |  {3}' -f ((Get-Date) - $t0).TotalSeconds, $g, $a, $c | Add-Content $Out
  Start-Sleep -Milliseconds 900
}
