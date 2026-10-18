# (user session, via run-as-user.ps1) gamebarinstall.ps1 - restore the stock Xbox Game Bar (Microsoft Store
# 9NZKPSTSNW4P) with winget: this image lacks it, so every game start opens an unhandled ms-gamingoverlay: link and its
# "Get an app" popup takes the foreground (NFSU2 crashes). MS Q&A https://learn.microsoft.com/en-us/answers/a/1429604.
# -> C:\S10eTmp\gamebarinstall.out. notes/games-nfsu2.md.
$out = 'C:\S10eTmp\gamebarinstall.out'
"start $(Get-Date -Format HH:mm:ss)" | Set-Content $out
& winget install --id 9NZKPSTSNW4P --source msstore --accept-package-agreements --accept-source-agreements --disable-interactivity 2>&1 |
  ForEach-Object { "$_" } | Where-Object { $_ -match '\S' -and $_ -notmatch '^[\s\-\|/]+$' } | Add-Content $out
"exit $LASTEXITCODE" | Add-Content $out
"package: $((Get-AppxPackage -Name Microsoft.XboxGamingOverlay | ForEach-Object { $_.Name + ' ' + $_.Version + ' ' + $_.Architecture }) -join ', ')" | Add-Content $out
"protocol ms-gamingoverlay: $(Test-Path 'Registry::HKEY_CLASSES_ROOT\ms-gamingoverlay')" | Add-Content $out
