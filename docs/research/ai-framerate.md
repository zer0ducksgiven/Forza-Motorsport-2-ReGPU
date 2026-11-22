FINAL REPORT: why FM2's AI misbehaves above 60 fps, and the fix

I did no new code reading for this report. It is a judgement of the three designs, based on the verified claims in the research. Every address below is a TU address. Ghidra functions are FUN_82xxxxxx and recomp functions are sub_82xxxxxx.

## 1. How FM2 timing works, and why the AI goes wrong

**The frame dt.** The main loop FUN_82229188 takes dt as `uVar14 * 0.001f`, where uVar14 is the whole-millisecond delta of KeTimeStampBundle.TickCount. TickCount is read through sub_82425908 (`return *(DAT_8200089c + 0x10)`).
- Only a delta of exactly 16 or 17 ms is smoothed: it becomes 1/59.94 = 0.016683 s, with a carry.
- Any other delta is used raw, with a floor of 1e-4 s. On that path the carry is reset every iteration.
- So the console smoothing never runs on PC, at any frame cap.
- A bypass path, LAB_82229418, is taken when FUN_821f64f0() or 829abb5c->vf 0x1c is true. It skips the floor and can pass dt = 0. What those two predicates mean is UNCONFIRMED.

**The loop rate on PC.** The fake vblank in src/gpu/gpu.cpp:1406 runs at `clamp(FrameCap*2, 120, 720)`. The loop waits on the vblank event DAT_829d27b0, which FUN_8221be58 signals.
- A race loop therefore runs at about 120 iterations/s at the default 60 cap, 288/s at 144, 480/s at 240 and about 700/s at 360.
- The resulting dt is 8 or 9 ms at 60, 3 or 4 ms at 144, 1 to 3 ms at 240, and 0 (becoming 0.1 ms), 1 or 2 ms at 360.
- TickCount is refreshed by a host thread that sleeps 1 ms (src/kernel/runtime.cpp TimeStampThread), which adds about 1 ms of jitter.
- The console ran the loop at 60/s with dt = 16.683 ms.

**There is no fixed step anywhere.** The chain from the main loop down is:
1. The main loop calls `(param_1+8)->vf 0x28` with dt. This is very likely CGameType::vf10 (82353718). It clamps dt to [1e-5, 0.2] and runs `do{...}while(iVar4<1)`, which is exactly one iteration.
2. That calls FUN_8223CA00, the race update, which does the following in order:
   - each car's FUN_821fac98
   - CPhysicsWorld::vf15 (82674A78), which takes ceil(dt*50) world substeps
   - each car's FUN_821f9f38
   - the RacingAI system update FUN_82485EA0 at 8223D470
   - each driver's FUN_821EEA88
3. Car physics (FUN_82669b08) takes max(1, ceil(239*min(dt, 0.1))) substeps of dt/n. CarPhysicsUpdateHz is 239 (PhysicsSettings.ini:57).
   - Console: 4 substeps of 4.17 ms.
   - PC at 120 to 200 fps loop rates: 2 substeps of 2.5 to 4 ms.
   - Any dt of 4 ms or less: 1 substep of the raw dt, which can be 1, 2, 3, 4 or 0.1 ms.
   - (Note: one lens found a divisor param_2 in FUN_8266b7c0's formula. It is assumed to be 1 (UNCONFIRMED).)

**The AI runs once per loop iteration.** FUN_82485EA0 updates every AI car on every call, with no time-slicing. Its driver vf2 entry points take no dt. The global AI clock DAT_82a02f18 (+8 race time, +0x10 dt) is set from the struct at r4.

**Root cause: AI logic that counts update calls, not time.** All of the following were verified in code. At about 700 updates/s each runs 6 to 12 times faster in real time than on the console:
- **State timers.** CAIStateMachine2 increments `sm+8 += 1` once per update (FUN_8249DB10) and resets it on a state change (FUN_8249DA98). It is compared with frame thresholds:
  - 31 frames in FUN_82495468, state 82494DC0
  - 301 frames for passing before going to crash recovery (82493088)
  - 601 frames: MetaAI state 82494038, and crash recovery FUN_82497810 giving up (to 82496460)
  - 1801 frames in FUN_824926E0
- **Contact accumulators.** FUN_82488490 adds +1.0 per update to CAICar+0x8F0/0x8F4/0x8F8/0x8FC while the flags at 0x894 to 0x897 are set. Only the decay, x *= (1 - 0.1*dt), uses dt.
  - The stuck test is |+0x820| < 2 and accumulator[0 or 1] > 6.0 (FUN_82490B68, FUN_82496528/68). It forces crash recovery, state 824934E0.
  - 6.0 takes 100 ms of contact at 60/s but only 8.6 ms at 700/s, and the value then lingers for seconds.
  - Crash recovery re-tests the same > 6.0 condition when picking its sub-states.
  - This is the strongest candidate for the "strange reactions".
- **Passing patience.** CPassingDriver +0x70 rises by 5.0 per update (FUN_824A5FC0, when +0x75 is set) and falls by 5.0 per update (CPassingDriver::vf2@18 824A9B28). It is compared with +0x2AC = 1000 in FUN_824A8958.
  - 200 updates is 3.3 s on the console and 0.29 s at 700/s.
  - The +5 in FUN_824A5C48 is gated by a 0.5 s timer, so it is not per update.
  - Whether runtime tuning changes the 1000 is UNCONFIRMED.
- **AI brake filter.** FUN_8249E390 ramps 25% of the gap per update and halves on release. Reaching 95% takes 173 ms on the console and about 29 ms at 360/s.
- **Planner stagger.** sub_82485EA0 runs FUN_82488e60 and FUN_8249AD00 for one car in three per update (car+0x1C mod 3 against system+0xC). That is 20 Hz per car on the console and up to about 233 Hz on PC.

**Weaker or secondary contributors:**
- Dividing by dt = 1e-4 in the yaw-rate and lateral-rate terms (824a0000.c:302, FUN_8249F3B0) amplifies float noise. This is UNCONFIRMED as a visible effect.
- FUN_821E9CA8 computes driver velocity as Δpos / max(dt, 1 ms), so it reads low on any frame shorter than 1 ms. Whether steering uses it is UNCONFIRMED.
- Car physics step sizes vary and are smaller than on the console.

**Ruled out:**
- The slip filter FUN_8249E978 has almost no memory: the old value has weight 0.2 (checked in the asm).
- The dt-based steering derivatives are consistent with the simulation.
- The pass replan, look-ahead and recovery waits use race time.

**Two consistency notes:**
- At the default 60 cap the loop already runs at 120/s, so the AI is at 2x console speed, and players call that normal. The effect grows with the rate.
- The community "Project Forza Plus 2" ini sets CarPhysicsUpdateHz to 5.79 x fps "so that the AI can still corner properly". That confirms the symptom, but it only changes substeps.

## 2. Verdict on the designs and the recommended fix

**Scores:**
- **Design 0** (AI gate at sub_82485EA0, 60 Hz, OR-latched event flags, one-shot output clearing)
  - Correctness: high. It targets every confirmed frame-counted item directly.
  - Risk: low. It is a leaf hook with no visual cost and no change to physics or rendering.
  - Effort: about 150 lines.
  - Testability: good.
  - Its handling of events is the most careful of the three. It is the only design that keeps contact and event flags (0x894 to 0x8A3, plus 0x878 to 0x87C) between ticks and keeps the 0x914 to 0x91A requests one-shot.
- **Design 1** (quantise FUN_8223CA00 calls into q = 4.17 ms chunks, plus the AI gate)
  - Correctness: high. It also gives exact console car substeps.
  - Risk: medium. Everything inside 8223CA00 now gets n*q calls, which affects the HUD, replay recording, network, CMetaAIDriver::vf5 and the camera.
  - Its AI gate (Hook B) does not latch the events.
  - Its extras are useful: the exact TickCount (Hook D) and the trace.
- **Design 2** (fixed step at CGameType::vf10)
  - Correctness: medium. At its proposed default R=120 the AI still runs at 2x console speed. It needs the AI gate anyway.
  - Visual cost: high. With no interpolation, anything above R fps adds nothing and the motion judders.
  - Effort: low.
  - Useful as the zero-code proof (FM2_VBLANK_HZ=60) and for its loop-order evidence: vf11, vf12 and vf13 take no dt, so a skipped sim tick is safe for rendering.

**Recommendation: Design 0 as the shipping fix, with parts of the others added.**

**Phase 1: new file src/hooks/ai_cadence.cpp, following the audio_cadence.cpp pattern.**
- Override `PPC_FUNC(sub_82485EA0)`, calling `__imp__sub_82485EA0`.
  - It is a weak alias at ppc_recomp.50.cpp:21852.
  - Its only call site is `bl` at 8223D470, lr 8223D474 (ppc_recomp.6.cpp:9265).
  - Arguments: r3 = AI system DAT_82A02EA0 (+0x10 car count, +0x14 CAICar* array), r4 = &{double raceTime; float dt at +8}.
- Scheduler, controlled by FM2_AI_HZ (default 60, 0 = off; also add a LiveSwitch for A/B testing in one session):
  - Accumulate the game's own dt.
  - Tick when `t + dt/2 >= next`, then advance `next` by 1/60, resyncing if it falls behind.
  - On a tick, write the summed float(t - tLastTick) to r4+8 before calling the original. FUN_82489df0 copies it into DAT_82a02f18, so every /dt and every decay sees the summed time.
  - Force a tick and reset all state on the first call, when r3 changes, when the car count changes, or when raceTime goes backwards.
- On every call, OR the CAICar bytes 0x878 to 0x87C and 0x894 to 0x8A3 into a per-car shadow. Do not include 0x8A4, which holds state. On a tick, write the shadow back and clear it.
  - These bytes are refreshed each frame by CAICar::vf1 (824896F8, memcpy into +0x820) from FUN_821f7358, which latches and then clears them.
- On skipped frames, zero 0x914 to 0x91A for every car (the same stores FUN_82487968 makes). The pre-physics, post-physics and vf8 readers then see each request once.
- Between ticks the controls persist on their own:
  - SetControls (8265AB00) writes car+0x11C0/0x11C4.
  - vf15 latches that to +0xF30 every world step.
  - Mode-2 steering reads +0x11C4 live (FUN_82669840).

**Phase 1b, from Design 1 Hook D (cheap, low risk).** Make TickCount exact at read time by overriding sub_82425908. Use the same epoch as TimeStampThread so the clock never goes backwards: today a backwards step wraps unsigned to more than 4000 and becomes 16 ms. This removes the 1 to 2 ms staleness.

**Phase 2, optional, only if the Phase 1 metrics still show residual differences.**
- Add Design 1's Hook A, the 8223CA00 quantiser, behind FM2_SIM_STEP. Default off.
  - It makes every car substep exactly 4.17 ms.
  - It always passes through the first-update call from FUN_82241630 (lr 82241690), dt below 5e-5, and hitches above 8q.
- Do not ship the Design 2 vf10 fixed step or interpolation. Interpolation would need the pose fields the renderer reads, which are unknown.

**Things the fix does not touch:**
- FUN_821EEA88 and its FUN_821E9CA8 velocity. It also runs for the human driver, and the class at +0xb10 is unknown.
- Car physics substeps.
- audio_cadence.cpp, lua_gc.cpp and the UI clocks.

**Effect on human driving.** None directly. The human ControllerDelays (FUN_826602A0) and ABS/TCS run inside car physics, which is unchanged. The human car's Drivatar observer in the AI system would update at 60 Hz, as on the console. Whether that observer ever calls SetControls for a human car is UNCONFIRMED.

## 3. Test plan

All runs windowed at 1280x720 (quickrace does this), unattended with `--autodrive` (nohumans=true), 7 AI, 2 to 3 laps, 2 tracks, 3 runs per configuration.

**Step 0, no code (confirms the cause):**
`py -3 tools/quickrace.py --track T --ai 7 --laps 3 --autodrive FM2_MAX_FPS=240 FM2_VBLANK_HZ=60`
- With the loop at 60/s, the 16/17 ms console smoothing runs, so this matches the console.
- Compare against FM2_MAX_FPS=240 and FM2_MAX_FPS=360 with no override.
- If the strange AI goes away at VBLANK_HZ=60, the cause is confirmed.

**Instrumentation: FM2_AI_TRACE=1 in the same hook file, written to CSV.**
- Once per second: loop calls, AI ticks per second, and a histogram of the incoming dt in ms.
- A hook on sub_8249DA98 (sm, new fn): log race time and the new state. Count entries to 824934E0 (crash recovery) and 82493088 (passing), and exits to 82496460.
- Per car, per tick: the accumulators at 0x8F0/0x8F4, rising edges of the flags at 0x898 to 0x8A3, CPassingDriver +0x70, and the steer, throttle and brake bytes at car+0x11C4/0x11C0/0x11C1. Alternatively hook SetControls 8265AB00 (an AI car has car+0xF10 != 0, which is UNCONFIRMED).

**Metrics per AI car-minute:**
- crash-recovery entries and passing entries
- seconds with an accumulator above 6
- steering sign reversals (0.05 deadband) and steering total variation
- brake rise time
- lap times and finishing order (screenshot via fm2ctl.py)

**Configuration matrix:**

| Config | FM2_MAX_FPS | FM2_VBLANK_HZ | FM2_AI_HZ | Purpose |
|---|---|---|---|---|
| A | 60 | 60 | 0 | Console reference |
| B | 60 | default | 0 | Today's default |
| C | 240 | default | 0 | Bug at 240 |
| D | 360 | default | 0 | Bug at 360 |
| E | 240 | default | 60 | Fix at 240 |
| F | 360 | default | 60 | Fix at 360 |

**Pass criteria:**
- E and F fall within A's run-to-run spread on every metric.
- C and D are clearly worse than A, which shows the metrics detect the bug.
- E and F tick at 60 ± 1 per second.

**Regression checks:**
- a human race at 60, 144 and 360 (feel and input)
- engine sound by ear (history of engine warble)
- a replay recorded with the fix and a replay recorded before it, both played back
- race start countdown and finish
- pause
- Rogue Mode and Random Events
- one FM2 Live race
- Use the FX machine only after asking: other sessions use it.

## 4. Open questions

- Where the 0x878 to 0x87C bytes and the vf9/vf11 outputs (0x918/0x919) come from and what they mean. If any of them is a held level rather than an event, OR-latching or one-shot clearing would be wrong. The trace will show it.
- Whether a human car ever takes controls from the AI system (the CDrivatarDriverBase vf2@18 condition `+0x3D != 0 && +0x1C == 0`).
- Whether the passing threshold +0x2AC (1000 at 829aa9f8/829aa9bc) is overridden by tuning data or Lua at runtime.
- What FUN_821f64f0 and 829abb5c->vf 0x1c mean (online?). On that path dt can be 0: physics is skipped while the AI still runs.
- Whether the AI reads the FUN_821E9CA8 velocity (+0xb50), and what the driver object at +0xb10 is.
- Whether replays record car state or re-simulate it, and whether a 60 Hz AI changes playback.
- How often 0 ms and 4+ ms frames actually happen. This needs the dt histogram.
- What main-loop object +0xd8 controls. It is set when the vblank wait is under 1 ms or over 12 ms.

## Key files

- D:\fm2-port\src\hooks\audio_cadence.cpp (hook pattern to copy)
- D:\fm2-port\src\gpu\gpu.cpp:1406 (vblank rate)
- D:\fm2-port\src\kernel\runtime.cpp:32-49 (TickCount thread)
- D:\fm2-port\out\ppc_tu4_rexenon\ppc_recomp.6.cpp:9265 (AI call site)
- D:\fm2-port\out\ppc_tu4_rexenon\ppc_recomp.50.cpp:21852 (weak alias)
- D:\ghidra\export\c\unknown\82480000.c:4437 (FUN_82485EA0) and :6207 (FUN_82488490)
- D:\ghidra\export\c\unknown\82490000.c:3585-3662, 4721, 9226 (state machine and thresholds)
- D:\ghidra\export\c\unknown\824a0000.c (CPassingDriver)
- D:\ghidra\export\c\unknown\82220000.c:4507 (main loop)
- D:\ghidra\export\c\virtual\82350000.c:1411 (CGameType::vf10)
- D:\fm2-port\tools\quickrace.py