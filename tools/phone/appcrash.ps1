# appcrash.ps1 [-Minutes 30] [-Name dwm] - application crashes (Application Error 1000 / WER 1001) of a process in
# the last -Minutes: faulting module, exception code, offset. G17 (notes/g17-dxva-mfc.md): DWM died with UMD 0099.
param([int]$Minutes = 30, [string]$Name = 'dwm')
Get-WinEvent -FilterHashtable @{ LogName = 'Application'; StartTime = (Get-Date).AddMinutes(-$Minutes); Id = 1000, 1001 } -ErrorAction SilentlyContinue |
  Where-Object { $_.Message -match $Name } | Select-Object -First 6 | ForEach-Object {
    '{0:HH:mm:ss} {1}' -f $_.TimeCreated, (($_.Message -split "`n") | Where-Object { $_ -match 'Faulting|Exception code|Fault offset|P[1-8]:' } | ForEach-Object { $_.Trim() }) -join ' | '
  }
