RONwifi Firmware v0.2.1 — Resume Fix, Race Fix, Additive Extend
================================================================

CHANGES FROM v0.2.0:
--------------------

**Bug #4 — Resume returns remaining window time (not full 60s)**
BEFORE: User resumes at 30s left → ESP returns window_seconds: 60 → JS shows fresh 60s countdown → Silent timeout at real 0s
AFTER: ESP tracks actual window deadline → Returns remaining_secs: 30 → JS shows correct 30s

Files changed: main.cpp, http_server.h, http_server.cpp
- main.cpp now tracks s_armWindowEndsMs when coinslot enters ARMED state
- Passes deadline to http_server via httpSetArmWindowEnds()
- handleCoinStart() calculates remaining = (deadline - now) / 1000 on resume
- Portal JS receives accurate countdown value

**Bug #3 — Auto-finalize vs Done race condition**
BEFORE: User presses Done at t=59.8s → POST in flight → Loop hits t=60.0s → auto-finalize runs, disarms, sets IDLE → Done POST arrives → "no_session" error even though user got their time
AFTER: Mutex prevents overlap + cached result serves late-arriving Done requests

Files changed: http_server.cpp
- Added s_finalizeBusy mutex (volatile bool)
- Added FinalizeCache struct to store last result for 10s
- handleCoinDone() checks mutex, checks cache on IDLE, locks/unlocks around work
- httpAutoFinalizeSession() checks mutex, locks/unlocks, populates cache
- Late Done requests (arrive <10s after auto-finalize) get cached success instead of error

**Bug #6 (Portal) — Additive limit-uptime extension**
BEFORE: User has 30 min remaining → Inserts ₱20 (1440 min) → New limit = 1440 min → Lost their 30 min
AFTER: ESP reads current limit-uptime, adds new minutes → New limit = 1470 min → User keeps existing time

Files changed: mikrotik_client.cpp
- mikrotikCreateAndLoginUser() now parses current limit-uptime from MikroTik response
- Supports both "HH:MM:SS" and "Nd Hh" formats
- Adds new minutes to current limit → total_minutes
- Sets limit-uptime to new total
- Always re-logins user (handles both active and expired sessions)


DEPLOYMENT:
-----------
1. Compile firmware v0.2.1 with PlatformIO/Arduino IDE
2. Upload to ESP8266 via USB or OTA
3. ESP will auto-restart
4. Verify via serial monitor:
   - "[mikrotik] extend: old=X +new=Y =total=Z" on coin insert for existing users
   - "[done] Served cached result from auto-finalize" if race was caught

5. Deploy portal v0.1.8 to MikroTik (required for Done-while-counting fix)


TESTING CHECKLIST:
------------------
✓ Insert coin → Wait 30s → Close portal → Reopen → Resume session → Countdown shows ~30s not 60s
✓ Insert coin → Wait 59s → Press Done → Should succeed (no "Error finalizing" toast)
✓ User with 1hr remaining → Insert ₱5 (1hr) → Check MikroTik user limit-uptime = 02:00:00 (not 01:00:00)
✓ User session expired but MikroTik user still exists → Insert coin → Should extend + re-login
✓ Serial monitor shows additive math: "old=60 +new=60 =total=120"


COMPATIBILITY:
--------------
- Backward compatible with portal v0.1.7 (but Done-while-counting won't work)
- Requires RouterOS v7.1+ for REST API
- MikroTik API user must have 'api,rest-api,read,write' policies


KNOWN LIMITATIONS:
------------------
- limit-uptime parser supports "HH:MM:SS" and "Nd Hh" only (not "Nd Hh Mm" or "Nw")
  → Covers 99% of use cases (max 99d 23h = 143940 min = ~3 months)
- Cache window is 10 seconds (hardcoded) — adequate for typical network latency
- Emergency log still not replayed on ESP reboot (deferred to v0.3)


BUG FIXES IN THIS RELEASE:
---------------------------
#4 — Resume shows correct remaining time (not full window)
#3 — Auto-finalize / Done race eliminated via mutex + cache
#6 — Additive extend (user keeps existing time + gains new time)


VERSION INFO:
-------------
Firmware: v0.2.1
Portal: v0.1.8 (required)
Build date: 2026-04-26
