# (user session) dbgcap self-test: listen 6 s while this script emits three OutputDebugString lines.
Remove-Item C:\S10eTmp\dbgcap.txt -ErrorAction SilentlyContinue
$cap = Start-Process -FilePath C:\S10eTmp\dbgcap.exe -ArgumentList '6', 'C:\S10eTmp\dbgcap.txt' -WindowStyle Hidden -PassThru
Start-Sleep -Milliseconds 800
Add-Type -Namespace W -Name K -MemberDefinition '[DllImport("kernel32.dll", CharSet=CharSet.Ansi)] public static extern void OutputDebugStringA(string s);'
1..3 | ForEach-Object { [W.K]::OutputDebugStringA("odstest line $_"); Start-Sleep -Milliseconds 100 }
$cap | Wait-Process -Timeout 15
