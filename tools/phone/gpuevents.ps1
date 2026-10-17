# gpuevents.ps1 [-Minutes 15] - recent GPU trouble in the System log: TDRs (Display 4101), LiveKernelEvents (117/141),
# dxgkrnl and MaliG76Wddm entries. G17 pacing (notes/g17-dxva-mfc.md): were the ~1 s GPU stalls resets?
param([int]$Minutes = 15)
Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = (Get-Date).AddMinutes(-$Minutes) } -ErrorAction SilentlyContinue |
  Where-Object { $_.ProviderName -match 'Display|DxgKrnl|Mali|LiveKernel' -or $_.Id -in 4101, 141, 117 } |
  Select-Object -First 12 TimeCreated, Id, ProviderName, @{ n = 'Msg'; e = { ($_.Message -split "`n")[0] } } |
  Format-Table -AutoSize -Wrap
