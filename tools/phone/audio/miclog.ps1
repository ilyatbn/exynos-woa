# miclog.ps1 [-Pattern 'mic|AboxCapture'] - lines of the Exynos9820Audio run log (notes/d8-mic.md, mictest.sh).
param([string]$Pattern = 'mic|AboxCapture|Exynos9820Audio 1')
Select-String -Path C:\Windows\System32\LogFiles\Exynos9820Audio-run.txt -Pattern $Pattern | ForEach-Object { $_.Line }
