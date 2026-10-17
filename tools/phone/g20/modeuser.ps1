# (user session, via run-as-user.ps1) s10emode.exe with the arguments in C:\S10eTmp\s10emode.args -> s10emode.out
$a = (Get-Content C:\S10eTmp\s10emode.args -Raw).Trim() -split '\s+'
& C:\S10eTmp\s10emode.exe @a *> C:\S10eTmp\s10emode.out
"exit $LASTEXITCODE" | Add-Content C:\S10eTmp\s10emode.out
