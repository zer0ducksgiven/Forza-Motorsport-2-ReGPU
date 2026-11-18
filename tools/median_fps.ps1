# Median of per-second perf fps readings, from 10 s after the race start, for existing out/bench_<label>.err logs.
# Usage: median_fps.ps1 <label pattern>   e.g.  median_fps.ps1 "i2-*"
param([string]$Pattern = "i*")
Set-Location (Join-Path (Split-Path $PSScriptRoot) "out")
Get-ChildItem "bench_$Pattern.err" | Where-Object { $_.Name -notmatch '_fail\d' } | ForEach-Object {
  $lines = Get-Content $_.FullName
  $start = $null; $v = @()
  foreach ($l in $lines) {
    if ($l -notmatch '^\[\s*([\d.]+)\]') { continue }
    $t = [double]$Matches[1]
    if ($null -eq $start -and $l -match 'fps meter') { $start = $t }
    if ($null -ne $start -and $t -gt $start + 10 -and $l -match 'perf: ([\d.]+) fps') { $v += [double]$Matches[1] }
  }
  $med = if ($v.Count) { $s = $v | Sort-Object; if ($s.Count % 2) { $s[($s.Count - 1) / 2] } else { ($s[$s.Count / 2 - 1] + $s[$s.Count / 2]) / 2 } } else { 0 }
  "{0,-28} median {1,5:N1} fps  ({2} readings)" -f ($_.BaseName -replace '^bench_', ''), $med, $v.Count
}
