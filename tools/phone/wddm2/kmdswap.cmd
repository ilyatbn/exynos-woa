@echo off
set D=C:\Windows\System32\DriverStore\FileRepository\malig76wddm.inf_arm64_6557c1d415c2ef25
if exist "%D%\MaliG76Wddm.sys.prev" del /f /q "%D%\MaliG76Wddm.sys.prev"
move /y "%D%\MaliG76Wddm.sys" "%D%\MaliG76Wddm.sys.prev" > C:\S10eTmp\kmdswap.log 2>&1
copy /y C:\S10eTmp\MaliG76Wddm-new.sys "%D%\MaliG76Wddm.sys" >> C:\S10eTmp\kmdswap.log 2>&1
certutil -hashfile "%D%\MaliG76Wddm.sys" SHA256 >> C:\S10eTmp\kmdswap.log 2>&1
