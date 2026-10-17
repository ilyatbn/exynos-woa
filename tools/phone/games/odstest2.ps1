# dbgcap self-test in the current session (e.g. over ssh): dbgcap 5 s, three OutputDebugString lines, print the file.
Remove-Item C:\S10eTmp\dbgcap2.txt -ErrorAction SilentlyContinue
$cap = Start-Process -FilePath C:\S10eTmp\dbgcap.exe -ArgumentList '5', 'C:\S10eTmp\dbgcap2.txt' -PassThru -NoNewWindow -RedirectStandardError C:\S10eTmp\dbgcap2.err
Start-Sleep -Milliseconds 800
Add-Type -Namespace W -Name K -MemberDefinition '[DllImport("kernel32.dll", CharSet=CharSet.Ansi)] public static extern void OutputDebugStringA(string s);'
1..3 | ForEach-Object { [W.K]::OutputDebugStringA("ssh line $_"); Start-Sleep -Milliseconds 100 }
$cap | Wait-Process -Timeout 15
"exit $($cap.ExitCode)"
Get-Content C:\S10eTmp\dbgcap2.txt
Get-Content C:\S10eTmp\dbgcap2.err -ErrorAction SilentlyContinue
