# typeurl.ps1 -Url edge://gpu [-Title Edge] - (user session, via runuser2) focus a window by title, Ctrl+L, type the
# URL, Enter. For browser-internal pages that cannot be opened from the command line (edge://gpu, chrome://gpu).
param([string]$Url = 'edge://gpu', [string]$Title = 'Edge', [string]$Keys = '')   # -Keys: send only these SendKeys keys (e.g. '{PGDN}')
Add-Type -AssemblyName Microsoft.VisualBasic, System.Windows.Forms
$p = Get-Process | Where-Object { $_.MainWindowTitle -like "*$Title*" } | Select-Object -First 1
if ($p) { [Microsoft.VisualBasic.Interaction]::AppActivate($p.Id) }
Start-Sleep -Milliseconds 700
if ($Keys) { [System.Windows.Forms.SendKeys]::SendWait($Keys); $Url = $Keys } else {
  [System.Windows.Forms.SendKeys]::SendWait('^l')
  Start-Sleep -Milliseconds 300
  [System.Windows.Forms.SendKeys]::SendWait($Url + '{ENTER}')
}
"typed $Url into pid $($p.Id)" | Out-File C:\S10eTmp\typeurl.txt
