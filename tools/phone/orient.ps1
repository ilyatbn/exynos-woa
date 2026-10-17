# orient.ps1 - the primary display's current mode as the user session sees it (resolution, orientation), via
# System.Windows.Forms.Screen and the WMI desktop monitor - run in the user session (userrun.ps1).
Add-Type -AssemblyName System.Windows.Forms
$b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
"primary screen {0}x{1} -> {2}" -f $b.Width, $b.Height, $(if ($b.Width -gt $b.Height) { 'landscape (rotated from the 1080x2280 panel)' } else { 'portrait (panel native)' })
