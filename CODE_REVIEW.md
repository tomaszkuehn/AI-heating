# Code Review — `origin/main...HEAD`

> **Status (2026-07-10): wszystkie 15 findingów naprawione** i wgrane na
> urządzenie (COM3, bez erase). Weryfikacja na żywym module: restart e-mail
> `[Sterownik Grota] RESTART` przy ~62 s **real uptime** (nie ~7 s — #15), brak
> TWDT, brak stack-overflow, urządzenie stabilne przez cały 100 s przechwyt
> (`/api/state` → HTTP 200, `fault=NONE`). **Regression wykryty przy flashu i
> naprawiony:** off-loop worker `notify` (wprowadzony przez #2) miał stack 5120
> → przepełnienie przy `smtp_send` → `SW_CPU_RESET` reboot-loop co ~66 s; podbito
> stack do 10240 (worker niesie ~1,2 KB `notify_cmd_t` na stosie, więc potrzebuje
> więcej niż 6144-B `control_task`, który wysyłał bez cmd na stosie). Szczegóły
> per-finding w sekcji „Resolution" na końcu.

**Scope:** `git diff @{upstream}...HEAD` — the "configurable device name + restart
notification (email/SMS)" feature plus the profile-grid (12h/3-slots/export-import)
changes. Working tree was clean.

**Method:** Max-effort recall review. 10 independent finder angles (line-by-line,
removed-behavior, cross-file tracer, language-pitfall, wrapper/proxy+reuse,
simplification, efficiency/hot-paths, altitude, sweep/gaps, frontend/UX) → dedup →
source-level verification of every surviving candidate. Verification included reading
the ESP-IDF `nvs_api.cpp` source to settle the NVS length-mismatch behavior and
`fault_manager.c` to confirm the no-fallback path.

**Two candidates were refuted** (kept out of the list):
- *"Total config wipe on upgrade"* → actually a **partial-read tail corruption** (see #1).
  `nvs_get_blob` with a buffer *larger* than the stored blob returns `ESP_OK` and copies
  only the stored bytes; `storage_load_config` then skips its `memset` and `main.c`
  skips re-seeding.
- *"SMTP header injection via `device_name`"* → `smtp_send` runs `sanitize_line` on the
  subject (notification_manager.c:123), replacing CR/LF with spaces; `sms_send`
  `url_encode`s the message. The only un-sanitized sink is the JSON in `h_state` (#3).

**Verified non-bugs** (false positives ruled out): the restart-email sensor loop has
*no* off-by-one (internal sensors are `sensors[0..sensor_count-1]`; external is at
`sensors[HE_MAX_SENSORS]` — the struct comment "idx 0 = external" is backwards);
`strncpy` null-termination in `seed_defaults`/`h_device` is safe; the `pos += snprintf`
`size_t` underflow is unreachable (loop guard keeps `pos < 448`); control_task stack
(6144 B) is not overflowed; `profile_path` traversal is mitigated by the literal
`profile_` prefix; `sdt/1000` truncation-to-0 is not a problem in normal mode
(`vTaskDelay(1000)+work` ⇒ `dt >= 1000`); `read_body` clamps/null-terminates; profile
import is server-validated.

---

## Findings (15, most-severe first)

### 1. [CRITICAL] Mid-struct `device_name[32]` insertion corrupts protection limits on upgrade → `fault_grace_sec=0` → false `NO_HEAT_RISE` → no heating
**File:** `main/storage_manager.h:47` (insertion) · `main/storage_manager.c:139` (load) · `main/fault_manager.c:112` (no zero-fallback)

`device_name[32]` is inserted in the middle of `system_config_t` (between
`heating_disabled` and `fault_grace_sec`). The whole struct is persisted as an opaque
NVS blob (`nvs_set_blob`/`nvs_get_blob` of `sizeof(*cfg)`). On a device with a saved
"cfg" blob from any prior firmware, `nvs_get_blob` is called with `needed =
sizeof(new struct)` > stored old blob; per the ESP-IDF source the `*length < dataSize`
check is false, so it returns `ESP_OK` and copies only the smaller old bytes.
`storage_load_config` skips its `memset` (err == `ESP_OK`) and `main.c:117` skips
re-seeding. Fields before the insertion (wifi, profile, sensors, notify) load fine, but
`fault_grace_sec`/`max_on_sec`/`max_on_break_sec` now sit beyond the old blob and read
as 0 (BSS). `fault_manager.c:112` is `int grace_ms = s_cfg ? s_cfg->fault_grace_sec * 1000
: 300000;` — the `:300000` fallback fires only when `s_cfg` is NULL, **not** when
`fault_grace_sec == 0`, so `grace_ms = 0` and the NO_HEAT_RISE check (line 117,
`since_us > grace_ms*1000`) fires on the first heating tick, before the house can warm
→ `raise_fault(FAULT_NO_HEAT_RISE)` → `ST_FAULT` → relay off. **Every upgraded device
cannot heat.** `device_name` is also corrupted (old limit bytes). `max_on_sec`/
`max_on_break_sec = 0` are masked by their `> 0` fallbacks in `hysteresis_decision`.

**Fix:** append new fields at the struct tail and/or add `config_version` + migration;
at minimum, a size-tolerant read that detects `needed != sizeof(*cfg)` and re-seeds the
trailing fields (or migrate under a new NVS key).

### 2. [CRITICAL/HIGH] Blocking SMTP/SMS send inside `control_tick` (watchdog task + `he_config_lock`) → TWDT reboot / boot loop; UI + control frozen
**File:** `main/control_engine.c:314`

`notification_send_restart()` is called synchronously inside `control_tick`, which
`control_task` (`main.c:88`) runs on the `esp_task_wdt`-subscribed task while holding
`he_config_lock` (`main.c:93`). `smtp_send`/`sms_send` are blocking TCP I/O (4 s
timeouts across ~6–8 recv/send rounds, plus DNS). `esp_task_wdt_reset()` runs only
*after* `control_tick` returns (`main.c:101`), and `CONFIG_ESP_TASK_WDT_TIMEOUT_S = 10`
(`sdkconfig.defaults:14`). With email+AUTH pointed at a slow/unreachable SMTP server,
`smtp_send` blocks up to ~24–36 s → the 10 s task watchdog fires → TWDT panic → reboot.
`s_restart_notified`/`s_restart_age_s` are BSS and reset each boot, so the next boot
re-fires at 60 s → **permanent boot loop**; the device never stays up long enough for the
user to disable notifications. Even without a reboot, `he_config_lock` is held for the
whole transaction, so every `/api` handler (`CFG_LOCK == he_config_lock`) and the next
control iteration stall. The pre-existing `notification_send_alert` shares the flaw but
was fault-gated; this call makes it fire deterministically on every boot.

**Fix:** dispatch notifications to a dedicated worker task/queue, off the control loop.

### 3. [HIGH/MEDIUM] `device_name` with `"` or `\` breaks `/api/state` JSON → dashboard soft-brick (persists in NVS)
**File:** `main/web_ui_api.c:259` (`h_state` `%s`) · `main/web_ui_api.c:400` (`h_device` / `json_str`)

`h_state` emits `device_name` raw into the JSON (`"device_name":"%s"`) with no escaping.
`json_str` (line 59) copies until the next `"` with no backslash-escape handling, and
`h_device` does no sanitization. User clicks the dashboard title, types `a"b`, presses
Enter → `JSON.stringify` sends `{"name":"a\"b"}` → `json_str` stops at the escaped quote
and stores `a\` → `h_state` emits `"device_name":"a\"` → the backslash escapes the closing
quote so the JSON string never terminates → `JSON.parse(/api/state)` throws →
`renderDashboard` never runs → every dashboard tile freezes. The bad name is in NVS, so
it persists across reboots and the user cannot fix it via the (broken) UI (must `curl
POST /api/device` with a safe name). The SMTP/SMS sinks are safe (`sanitize_line` /
`url_encode`), so JSON is the only vulnerable sink. Anyone on the device's network can
`POST /api/device` (no auth) and brick the dashboard for all viewers. The
upgrade-corruption in #1 can also place a raw control char in `device_name`, triggering
the same JSON break.

**Fix:** JSON-escape `device_name` (and all string fields) before emission, or validate
to printable-safe chars in `h_device`.

### 4. [MEDIUM] `h_profile_file` calls `read_body` (blocking) after `CFG_LOCK` → stalls control loop + web UI; can TWDT-reboot
**File:** `main/web_ui_api.c:364` (lock at `:360`)

`h_profile_file` calls `read_body()` (blocking `httpd_req_recv` loop, up to 5 timeouts)
*after* `CFG_LOCK()` at line 360, holding the global config/control mutex during a
client-controlled body read. Every other mutating handler reads the body *before*
locking (`h_profile_post:343→348`, `h_device:398→401`, `h_pump`, `h_notify`); the old
save path read no body at all. A slow/stalled client POSTing a slot-save body holds
`he_config_lock` for the whole body-read duration → `control_task` blocks at
`main.c:93` (no sensor poll, no relay management, no `esp_task_wdt_reset`) → if the
stall exceeds the 10 s watchdog, TWDT reboot. All other `/api` handlers queue behind it.

**Fix:** move `read_body` + `profile_from_json` above `CFG_LOCK()`, matching
`h_profile_post`.

### 5. [MEDIUM] `smtp_send` truncates the 512-byte restart body to 256 and flattens `\r\n` → "detailed report" garbled
**File:** `main/notification_manager.c:269` (call) · `:124` (`sanitize_line` into `msg[256]`)

`notification_send_restart` builds a 512-byte multi-line "detailed report" body, then
passes it to `smtp_send`, which runs `sanitize_line(body, msg, sizeof(msg))` where
`msg` is `char[256]` — truncating to 255 chars **and** replacing every `\r`/`\n` with a
space, collapsing the report into one flat line. A 6-sensor device's body (~95-byte
header + ~35–45 B/sensor) exceeds 256, so the per-sensor table the README promises is
cut mid-list and arrives unformatted: `Urzadzenie: ... Czujnik 1 (...) OK 23.5 C Czujnik
2 (...) OK 22.8 C Czuj`. The alert path worked only because its message was already one
short line.

**Fix:** size `msg` to match the body buffer (≥512), or send the body directly and
`sanitize_line` only the header fields (the actual CRLF-injection risk).

### 6. [MEDIUM] Restart notification dropped forever if Wi-Fi not up at the 60 s instant
**File:** `main/control_engine.c:310` (latch) · `:312` (network check)

`s_restart_notified` is latched `true` **unconditionally** at the 60 s instant, *before*
the `fault_manager_network_up()` check. If Wi-Fi isn't up at exactly 60 s (slow STA
association, weak signal, DHCP > 60 s), the send is skipped and — the flag already true
— never retried. Device boots in STA mode, slow router, Wi-Fi comes up at t=90 s, but
the restart email/SMS is never delivered — exactly on the flaky-remote deployments where
restart monitoring matters most, with no indication of failure.

**Fix:** latch `s_restart_notified` only after a successful send (or after a hard
deadline), and keep retrying while the network is down.

### 7. [MEDIUM] Escape key in inline device-name edit commits the typed value instead of cancelling
**File:** `web/app.js:45`

The Escape handler does `dn.textContent = old`, which detaches the focused `<input>`
from the DOM; DOM removal fires `blur`, whose `onblur` then commits `inp.value`. User
clicks the name, types `BadName`, presses Escape → `onkeydown` sets `dn.textContent =
old`, removing the input → blur fires → `onblur` sees `v = 'BadName'` (truthy), sets
`dn.textContent = 'BadName'` and `POST /api/device {name:'BadName'}`. The cancelled
edit is committed to NVS. Only an empty input reverts.

**Fix:** set a `cancelled` flag in `keydown` that `onblur` checks before committing, or
commit/cancel solely in `keydown` and call `inp.remove()`.

### 8. [MEDIUM] `h_profile_file` save silently falls back to the active profile on malformed body, returns `ok`
**File:** `main/web_ui_api.c:368` (else branch)

When a body is present but `profile_from_json` fails, the code falls back to
`storage_save_profile_file(name, &s_cfg->profile)` (the active config) and returns
`200 "ok"`. User clears a grid cell (or types non-numeric) and clicks a one-click Zapisz
slot button → `collectProfile()` yields `[null, x]` (`parseFloat('') == NaN` →
`JSON.stringify` emits `null`) → `profile_from_json('null')` fails → the else branch
writes the active profile to `profile_N` and responds `ok`. The button flashes green; the
user believes slot N holds their edits, but it holds the active config. Later Wczytaj
restores the wrong profile. Compare `h_profile_post:345` which returns `400 "bad
profile json"` for the same input.

**Fix:** return 400 on parse failure — distinguish `blen == 0` (back-compat) from
`blen > 0 && parse fails` (error).

### 9. [MEDIUM] `h_profile_file` save skips `profile_validate` → "successfully saved" slot is unloadable
**File:** `main/web_ui_api.c:366` (save branch)

The new save-with-body branch calls `profile_from_json` but **not** `profile_validate`,
while the load branch (`:375`) does call `profile_validate`. A syntactically-valid but
semantically-invalid profile (on ≥ off, out-of-range, gap < 0.2) can be written to a slot
but is rejected on load. User types on=25/off=20 and clicks Zapisz slot 1 (bypasses
`/api/profile` validation) → slot written without validation. Later Wczytaj →
`profile_validate` rejects → `400 "invalid profile file"` → frontend alerts `Brak profilu
1`. The slot is a permanent trap until overwritten, with no explanation. The old save
path stored the already-validated active profile, so it could never produce an
unloadable slot.

**Fix:** call `profile_validate` before `storage_save_profile_file`, matching
`h_profile_post`.

### 10. [LOW-MEDIUM] Device-name display written once → stale forever after first render
**File:** `web/app.js:30`

`dn.textContent = s.device_name` lives inside the `!dn._init` one-time-setup guard, so
after the first dashboard render the header is never re-synced from the server. If the
name is changed from another browser/client or restored differently from NVS after
reboot, this page keeps showing the first-render name until a full page reload.

**Fix:** always set `dn.textContent = s.device_name` on render (skip while an input is
live); guard only the `onclick` wiring.

### 11. [LOW-MEDIUM] `onblur` commits before the POST and never reverts on failure → UI/server disagree
**File:** `web/app.js:40`

`onblur` sets `dn.textContent = v` immediately, then `post('/api/device', {name:v},
true)` is fire-and-forget (no `.then`/refresh; `api()` swallows fetch/parse errors →
`null`). If the POST fails (transient Wi-Fi blip) or the server returns 400, the header
shows the new name while the server holds the old. Because `_init` is already true (#10),
the 2 s poll never overwrites `dn.textContent`, so the stale name persists until a full
page reload — UI and server silently disagree.

**Fix:** `refresh()` after the POST, or revert to `old` on a non-ok response.

### 12. [LOW-MEDIUM] Failed profile import silently wipes unsaved grid edits
**File:** `web/app.js:713` (`profileEdited` cleared at `:712`)

Import only validates `arr.length === 24 && Array.isArray(arr[0])` (not `arr[1..23]`),
clears `profileEdited` *before* the POST, and chains no `.catch`. User imports a file
where `arr[5]` is a string → passes the client check → `profileEdited = false` →
`post('/api/profile', arr, true).then(() => refresh())`. Server `profile_from_json`
returns false → 400, but `api()` returns the text and the promise resolves; `refresh()`
overwrites the grid with the server's current profile. Any unsaved manual edits are
gone, `profileEdited` is cleared, and no error is shown — the grid silently reverts.

**Fix:** validate all 24 sub-arrays before posting; handle a non-ok response with an
alert; don't clear `profileEdited` until success.

### 13. [LOW-MEDIUM] UTF-8 (Polish) `device_name` in SMTP subject → strict MTAs reject/mangle the restart email
**File:** `main/notification_manager.c:229` (subject) · `:235` (body)

A non-ASCII `device_name` is emitted raw into the SMTP `Subject` header (and body) with
no RFC 2047 encoded-word / SMTPUTF8 EHLO; `sanitize_line` only strips CR/LF, so 8-bit
bytes pass through. In a Polish-localized product (default sensors are already Polish —
Salon/Kuchnia/Sypialnia), a Polish name such as `Łazienka` puts raw 8-bit UTF-8 in the
subject; `smtp_send` sends `EHLO heating` without SMTPUTF8, so a strict RFC 5321 MTA
rejects or mangles the message. The restart email is not delivered for users who name
their device in Polish — likely, not an edge.

**Fix:** MIME-encode the subject (RFC 2047 encoded-word), or restrict `device_name` to
ASCII.

### 14. [LOW] Restart email omits the external sensor from the per-sensor table
**File:** `main/notification_manager.c:249`

The per-sensor loop iterates `i < sensor_count` (internal sensors only); the external
sensor at `s_cfg->sensors[HE_MAX_SENSORS]` is never listed, so its name/quality/
effective-temp are absent from the per-sensor table. The README promises "stan każdego
czujnika (nazwa, jakość, temperatura efektywna)". A device with `has_external = true`
(the default) shows the external temp only on the "Temp. zewnetrzna" line (value only);
the external sensor's name and health are not in the report.

**Fix:** include the external sensor in the loop (iterate `sensor_count + 1` when
`has_external`, or append a line for `sensors[HE_MAX_SENSORS]`).

### 15. [LOW] Restart timer is sim-scaled → fires at ~6 real seconds in accelerated simulation
**File:** `main/control_engine.c:309`

`s_restart_age_s += sdt / 1000`, but `sdt = dt_ms * scale` and `scale =
HE_SIM_TIME_SCALE (×10)` when `simulate_heating && sim_time_accel`. With sim+accel on
(persisted in config), `sdt ≈ 10000`, so `sdt/1000 = 10` per tick and the 60 s gate fires
after ~6 real seconds — a spurious `[name] RESTART` email/SMS even though no reboot
occurred. The restart notification is a real-world event and should be gated on real
uptime (`esp_timer_get_time() / 1000000`), not the virtual clock. (Normal mode is fine:
`vTaskDelay(1000)+work` ⇒ `dt >= 1000` ⇒ `sdt/1000 >= 1` — the issue is only the sim
scaling.)

**Fix:** accumulate `dt_ms / 1000` (real time) instead of `sdt / 1000`, or compare
against `esp_timer_get_time()`.

---

## Cut at the 15 cap (correctness outranks cleanup)

Kept out to honor the 15-finding limit (correctness first):

- **Latent `pos += snprintf` negative-guard gap** (`notification_manager.c:244`): the
  `if (pos < 0) pos = 0;` runs only after the first `snprintf`; later ones lack it. The
  `pos > sizeof(body)` `size_t`-underflow path is **unreachable** (loop guard keeps
  `pos < 448`), and a negative return needs an encoding error that won't occur with
  these ASCII/`%s`/`%f` formats. Near-impossible trigger.
- **`h_device` truncates `name` at 31 bytes with no UTF-8 boundary awareness** → a
  >31-byte multi-byte name splits mid-character → invalid UTF-8 in `device_name`
  (compounds #3/#13). Edge.

Cleanup/reuse findings (valid, lower priority):

- `QUAL_*`→string switch duplicated from `qname()` (`web_ui_api.c:162`) in
  `notification_send_restart` (`notification_manager.c:252`) — hoist `qname()` to a
  shared header.
- `"Sterownik CO"` literal duplicated in `main.c:66` and `web_ui_api.c:268` — centralize
  as `#define HE_DEFAULT_DEVICE_NAME`.
- Misleading comment `/* idx 0 = external */` at `storage_manager.h:30` is **backwards**
  (external is at index `HE_MAX_SENSORS`; internal are `0..sensor_count-1`) — this
  already misled the review; fix the comment.
- `s_diag` reset only inside the email block (`notification_manager.c:268`) → stale
  diagnostic when only SMS is configured (inconsistent with `notification_send_alert`,
  which resets unconditionally).
- Convoluted split-line `Temp. zewnetrzna` formatting (`notification_manager.c:237`):
  valid ext temp prints an empty `%s` label line, then the value on a separate
  24-space-indented line — should be inline like `Temp. systemowa`.
- Three profile slots triplicated as hand-written HTML (`web/index.html:115`) —
  generate from a 3-iteration loop.

---

**Fix before merge:** #1 (every upgraded device stops heating) and #2 (slow SMTP
boot-loops the device). Both are introduced by this diff and affect every field
deployment.

---

## Resolution (2026-07-10)

Wszystkie 15 findingów naprawione + cleanup z Part C. Szczegóły w commit i w kodzie.

| # | Finding | Resolution |
|---|---|---|
| 1 | NVS mid-struct corruption | `HE_DEFAULT_*` defaults w `app_config.h`; size-tolerant `storage_load_config` (zero-fill tail + `ESP_LOGW`); `repair_config` clamps limitów/nazwy; zero-fallback karencji w `fault_manager.c`; komentarz `storage_manager.h:30` poprawiony |
| 2 | blocking SMTP/SMS on control loop | off-loop worker: `notify_cmd_t` queue (depth 2) + `notify_worker_task` (prio 4, **bez WDT**); `notification_dispatch_*` z control_engine/fault_manager (snapshot pod lockiem, `xQueueSend(...,0)`) |
| 3 | JSON break via `device_name` | `json_escape()` na emit (`h_state`: device_name + nazwy czujników); `sanitize_name()` + HTTP 400 na write (`h_device`, `h_sensor_post`); `json_str` obsługuje `\"` |
| 4 | `h_profile_file` lock-order | `read_body` + `profile_from_json` + `profile_validate` **przed** `CFG_LOCK` (jak `h_profile_post`) |
| 5 | `smtp_send` body truncate/flatten | body wysyłany **raw** (osobny `SEND(body)` po headerach), `sanitize_line` tylko na `rcpt`/`subj`; usunięto `msg[256]` |
| 6 | restart dropped if no Wi-Fi @60 s | worker retry do 30 s na `fault_manager_network_up()` (brak network gate w dispatch) |
| 7 | Escape commits device-name | flaga `cancelled` w `keydown`, `onblur` sprawdza przed commit; Escape → `inp.blur()` |
| 8 | `h_profile_file` silent fallback | `blen==0` = back-compat (zapis aktywnej); `blen>0 && parse-fail` → HTTP 400 (nie cichy fallback) |
| 9 | `h_profile_file` brak validate | `profile_validate` przed `storage_save_profile_file` (400 na invalid) |
| 10 | device-name stale | `dn.textContent = s.device_name` co render (skip gdy `<input>` live); guard tylko na `_init` |
| 11 | `onblur` no-revert | optimistic set + `POST` + `refresh()` / revert do `old` + alert na non-ok |
| 12 | profile import wipes edits | walidacja 24 sub-array (`Array.isArray`), `profileEdited` czyszczone dopiero po sukcesie, alert na odrzuceniu |
| 13 | UTF-8 w SMTP subject | RFC 2047 `=?UTF-8?B?<b64>?=` (`b64()`) gdy `device_name` zawiera non-ASCII |
| 14 | brak czujnika zewn. w restarcie | append `Czujnik zew. (...)` dla `sensors[HE_MAX_SENSORS]` gdy `has_external` |
| 15 | restart sim-scaled ~7 s | gate `control_now_ms() / 1000 >= 60` (real uptime `esp_timer_get_time()`), nie `sdt/1000` |

**Cleanup (Part C):** `qname()` hoistowane do `data_model` jako `sensor_quality_name()` (likwidacja triplicacji switch QUAL_* w `web_ui_api.c` i `notification_manager.c`); `HE_DEFAULT_DEVICE_NAME` scentralizowane (zamiast literału `"Sterownik CO"` w `main.c`/`web_ui_api.c`); `s_diag` reset unconditional w `notification_send_restart` (zgodnie z `notification_send_alert`).

**Regression (naprawiony przy weryfikacji):** worker `notify` stack 5120 → **10240**. Worker wykonuje `smtp_send`/`sms_send` off-loop z `notify_cmd_t` (~1,2 KB) na stosie; 5120 przepełniało się przy pełnym SMTP (e-mail restartu przy ~60 s → stack overflow → `SW_CPU_RESET` → reboot-loop co ~66 s). 10240 = httpd (8192) + zapas na cmd. Komentarz w `notification_init` dokumentuje sizing.

---

## Addendum — diagnostyka żywego modułu 10.168.34.11 (2026-07-10)

Osobny pass diagnostyczny (nieczęść 15-findingowej recenzji feature'a wyżej):
na prośbę użytkownika przejęto ster, przetestowano żywy moduł (`device_name`
„Sterownik Grota", tryb SYMULATION, SSID NCC-1701), znaleziono 5 usterek
widocznych w UI/API i wgrano poprawki (build + flash COM3 bez erase, weryfikacja
na żywym module). App 0xf79b0 (3% free).

| # | Usterka | Objaw na żywym module | Resolution |
|---|---|---|---|
| F1 | `daily.csv` zanieczyszczony datkami z wirtualnego zegara | `/api/daily` zawierał powielone wiersze „20231114" + rósł bez limitu (wirtualny zegar zasiewany na `HE_TIME_VALID_EPOCH`=2023-11-14 zanim SNTP złapie; `aggregate_day` dopisywał bez dedup) | `storage_record_minute` agreguje dany dzień **tylko gdy `he_time_valid()`** (flaga `s_cur_day_real`); `dedup_daily_csv()` przy starcie czyści istniejący plik (ostatni wiersz na dzień, sort rosnąco) |
| F2 | zalewanie `events.log` przy oscylacji awarii | `NO_HEAT_RISE` ↔ `SENSOR_IFACE` logował każdy przełącz | `raise_fault` limituje: ten sam `fault_class_t` → log co ≤1 raz / 60 s (`s_last_log_fault`/`s_last_log_us`) |
| F3 | brak możliwości wyczyszczenia logu | „Logi/alarmy" bez przycisku | `POST /api/log/clear` → `storage_clear_log()` (usuwa `events.log`); przycisk „Wyczyść log" w `index.html` (`btnLogClear`, confirm) |
| F4 | wyłączony czujnik z zaciśniętym `window_open=true` | Salon (`active:false`) pokazywał ⚫ z oknem — stany nie spójne z DISABLED | `sensor_manager_poll` czyści `s->window_open=false` przy `!active` (samonaprawa w 1 s) |
| F5 | wpisy logu przed SNTP z 1970 | pierwszy wpis po restarcie pokazywał datę 1970 (`time(NULL)` = uptime przed SNTP) | `loadLog()` w `app.js`: `ts < 1700000000` → `boot +Ns` (sekundy od startu) |

**Build gotcha (przyrostowy):** kompresja gzip zasobów WWW w `main/CMakeLists.txt`
musi mieć flagę `-f` (`gzip -9 -n -f`). Bez niej przyrostowa edycja pliku WWW →
ninja regeneruje `.gz`, który już istnieje → `X.gz already exists; not
overwritten` → build fail. Utajniony błąd (piera pełna kompilacja po gzip-slim
działała, bo `.gz` nie istniał); ujawnił się przy edycji `index.html`/`app.js`
w tym passie. Komentarz w `CMakeLists.txt:71-75` dokumentuje przyczynę.

**Weryfikacja na żywym module (po flashu):** `/api/daily` = 4 posortowane,
zdeduplikowane wiersze (F1); `POST /api/log/clear` → `ok`, `/api/log` → `[]`
(F3); Salon `window:false` (F4); uptime narasta 190028→197687 ms bez rebootu,
`fault=NONE`, `time_synced=true` (F2/F5). `daily.csv deduped (N -> N rows)` w
logu pierwszego startu (drugi start: plik czysty → brak przepisania → brak
komunikatu, zgodnie z oczekiwaniem).