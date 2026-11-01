# Lift checks on a test PC

Copied into `<install>\lift\` on the test PC (the FX), with the check build as `build\tu\fm2-lift.exe`. The game needs
the logged-in desktop, so a scheduled task (`schtasks /create /tn FM2LiftTest /tr <install>\lift\job.cmd /sc once
/st 00:00 /sd 01/01/2030 /it /f`) runs each line of `lift\job.txt`; `schtasks /run /tn FM2LiftTest` starts it, and
`lift\job.done` appears when it ends (output in `lift\job.out`).

- `runwatch.py <log> <tour> [enable file | -] [KEY=VALUE ...]`: one windowed 720p run with the debug server; killed
  when the frame count stops for FREEZE seconds (8; checks.py uses 120, as checks during boot stall frames for ~30).
On this PC the same scripts run from the repository root (FM2_LIFT_OUT=out/fx_local, the check build in
build/cov), so both machines can check at once.

- `checks.py [tag=_a] [routes] [KEY=VALUE ...]`: the checked routes (FM2_DECOMP_CHECK=lift), each route's
  lift_check.tsv into `lift\lift_check_<route><tag>.tsv`, for `lift.py --keep`.
- `live.py [N] [routes]`: the routes live, N times each, with coverage files for coverage.py.
- `bisect_w.py <file of addresses>`: finds the lifted functions that break the short menu tour (FM2_LIFT_ENABLE).
