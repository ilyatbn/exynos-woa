# umdtrace.ps1 [-Off] [-Trace] - machine-wide Mesa winsys logging: MALIKM_TGSI_DIR (timing log + stderr capture per process,
# malikm-<exe>-<pid>[-stderr].txt) and MALIKM_TRACE (verbose stderr traces). DWM picks it up at the next boot.
param([switch]$Off, [switch]$Trace)
$dir = 'C:\S10eTmp\tlog'
if ($Off) {
  [Environment]::SetEnvironmentVariable('MALIKM_TGSI_DIR', $null, 'Machine')
  [Environment]::SetEnvironmentVariable('MALIKM_TRACE', $null, 'Machine')
} else {
  New-Item -ItemType Directory -Force $dir | Out-Null
  [Environment]::SetEnvironmentVariable('MALIKM_TGSI_DIR', $dir, 'Machine')
  # MALIKM_TRACE logs every ioctl to stderr (MBs per minute in DWM): only with -Trace
  [Environment]::SetEnvironmentVariable('MALIKM_TRACE', $(if ($Trace) { '1' } else { $null }), 'Machine')
}
"MALIKM_TGSI_DIR=" + [Environment]::GetEnvironmentVariable('MALIKM_TGSI_DIR', 'Machine') + " MALIKM_TRACE=" + [Environment]::GetEnvironmentVariable('MALIKM_TRACE', 'Machine')
