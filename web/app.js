/* Frontend for the ESP32 heating controller. Vanilla JS, no deps. */
const $ = (id) => document.getElementById(id);
let lastState = null;

async function api(path, opts) {
  try {
    const r = await fetch(path, opts || {});
    const t = await r.text();
    try { return JSON.parse(t); } catch { return t; }
  } catch (e) { console.warn(path, e); return null; }
}
function post(path, body, isJson) {
  return api(path, { method: 'POST', body: isJson ? JSON.stringify(body) : body,
    headers: isJson ? { 'Content-Type': 'application/json' } : {} });
}
function fmtT(v) { return (v === null || v === undefined || v <= -98) ? '--' : v.toFixed(2) + '°C'; }
function fmtUp(ms) {
  const s = Math.floor(ms / 1000);
  const d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600), m = Math.floor((s % 3600) / 60);
  return (d ? d + 'd ' : '') + h + 'h ' + m + 'm';
}

/* Dashboard only — safe to run every couple of seconds; it never touches form
 * inputs, so ticks can't wipe unsaved edits (the old "checkboxes uncheck
 * themselves" bug). */
function renderDashboard(s) {
  lastState = s;
  const hlth = $('healthMark'), stt = $('stateMark');
  hlth.className = 'mark ' + (s.health ? 'mark-ok' : 'mark-bad');
  hlth.textContent = s.health ? '● Zdrowy' : '● Problem';
  stt.textContent = s.state;
  $('sysTemp').textContent = fmtT(s.system);
  $('extTemp').textContent = fmtT(s.external);
  $('sensOk').textContent = (s.sensors || []).filter(x => x.quality === 'OK' || x.quality === 'SIMULATED').length + '/' + (s.sensors||[]).length;
  $('uptime').textContent = fmtUp(s.uptime);
  $('simBanner').classList.toggle('hidden', !s.sim);
  $('sensCount').textContent = (s.sensors || []).filter(x => !x.external).length;

  /* Flash (LittleFS data partition) usage + wear indicator. */
  const fb = $('flashBar'), fwEl = $('flashWear');
  if (s.fs_total > 0) {
    const pct = Math.min(100, Math.round(s.fs_used * 100 / s.fs_total));
    $('flashV').textContent = (s.fs_used / 1024).toFixed(0) + ' / ' + (s.fs_total / 1024).toFixed(0) + ' KB (' + pct + '%)';
    fb.style.width = pct + '%';
    fb.className = 'pbar-fill' + (pct >= 90 ? ' err' : pct >= 70 ? ' warn' : '');

    /* Flash wear estimate based on ESP32 NOR flash rated at 100k erase cycles. */
    if (fwEl) {
      const wp = s.flash_wear_pct || 0;
      const ec = s.flash_erase_cycles || 0;
      fwEl.textContent = 'Zużycie: ' + wp.toFixed(2) + '% (' + ec + ' / 100 000 cykli)';
      fwEl.style.color = wp >= 1.0 ? 'var(--err)' : wp >= 0.5 ? 'var(--warn)' : 'var(--muted)';
    }
  } else {
    $('flashV').textContent = '--';
    fb.style.width = '0';
    if (fwEl) fwEl.textContent = '';
  }

  const fi = $('faultInfo');
  if (s.fault && s.fault !== 'NONE') { fi.classList.remove('hidden'); fi.textContent = 'Awaria: ' + s.fault; }
  else fi.classList.add('hidden');

  /* Boiler status: flame in circle when heating, blue when idle, gray X when disabled. */
  const bc = $('boilerCircle'), bi = $('boilerIcon'), bs = $('boilerStatus');
  if (bc && bi && bs) {
    if (s.disabled) {
      bc.className = 'boiler-circle off';
      bi.textContent = '✕';
      bi.style.color = '#666';
      bs.textContent = 'Wyłączony';
    } else if (s.heating) {
      bc.className = 'boiler-circle on';
      bi.textContent = '🔥';
      bi.style.color = '';
      bs.textContent = 'Grzeje';
    } else {
      bc.className = 'boiler-circle';
      bi.textContent = '';
      bi.style.color = '';
      bs.textContent = 'Nie grzeje';
    }
  }
  /* Kill button: toggle label + color, moved to end of button row. */
  $('btnKill').textContent = s.disabled ? 'Włącz ogrzewanie' : 'Wyłącz ogrzewanie';
  $('btnKill').className = s.disabled ? 'btn' : 'btn btn-danger';
  $('btnBoost').textContent = s.boost ? 'Anuluj BOOST' : 'Grzanie 5 min (BOOST)';

  /* Heating time in the current zoom window. */
  if (window._lastHeatMins !== undefined) {
    $('heatTime').textContent = window._lastHeatMins + ' min';
  }

  /* Time sync indicator. */
  const ts = $('timeSync');
  if (ts) {
    ts.textContent = s.time_synced ? '🕐 Czas zsynchronizowany' : '🕐 Czas lokalny';
    ts.style.color = s.time_synced ? 'var(--ok)' : 'var(--warn)';
  }
  /* System clock. */
  if ($('clockTime') && s.time_now) {
    const d = new Date(s.time_now * 1000);
    $('clockTime').textContent = d.toLocaleTimeString('pl-PL');
  }
  /* Device IP so the user knows what address to use. */
  if ($('devIp') && s.device_ip) {
    $('devIp').textContent = 'http://' + s.device_ip + '/';
  }
}

/* Full render: dashboard + editable forms. Used on first load and after a
 * save (so saved values are reflected back into the forms). */
function render(s) {
  renderDashboard(s);
  renderSensors(s.sensors || []);
  if (!profileEdited) renderProfile(s.profile);
  renderModes(s);
}

/* Live update of the non-input sensor cells (quality + effective temp) only,
 * so typing into a row is never wiped by the periodic refresh. */
function updateSensorCells(sensors) {
  const tb = $('sensorsTable').querySelector('tbody');
  if (!tb) return;
  sensors.forEach(sx => {
    const row = tb.querySelector('tr[data-sid="' + sx.id + '"]');
    if (!row) return;
    const hc = row.querySelector('.hcell');
    if (hc) hc.innerHTML = healthDot(sx.quality);
    const q = row.querySelector('.qcell');
    if (q) { q.className = 'qcell q-' + sx.quality; q.textContent = sx.quality + (sx.window ? ' ⊗' : ''); }
    const e = row.querySelector('.ecell');
    if (e) e.textContent = fmtT(sx.eff);
    /* Update health dot click handler for live changes. */
    const hd = row.querySelector('.hdot');
    if (hd && sx.quality !== 'OK' && sx.quality !== 'SIMULATED' && sx.quality !== 'DISABLED') {
      hd.style.cursor = 'pointer';
      hd.onclick = () => showSensorDetail(sx);
    } else if (hd) {
      hd.style.cursor = 'default';
      hd.onclick = null;
    }
  });
}

function healthDot(q) {
  const cls = q === 'OK' || q === 'SIMULATED' ? 'hdot-ok' :
              q === 'WINDOW_OPEN' ? 'hdot-warn' :
              q === 'DISABLED' ? 'hdot-off' : 'hdot-err';
  const hasDetail = q !== 'OK' && q !== 'SIMULATED' && q !== 'DISABLED';
  return `<span class="hdot ${cls}" data-hq="${q}" style="cursor:${hasDetail ? 'pointer' : 'default'}"></span>`;
}
function sensorHealthDetail(sx) {
  const q = sx.quality;
  const age = sx.last_seen !== undefined ? sx.last_seen : '?';
  const name = sx.name || ('#' + sx.id);
  let desc = '';
  if (q === 'TIMEOUT') {
    desc = `<b>${name}: brak komunikacji</b><br><br>
      Ostatni odczyt: <b>${age} s</b> temu (limit: 90 s).<br>
      Czujnik nie odpowiada na zapytania — możliwe przyczyny:<br>
      • przerwany lub poluzowany przewód<br>
      • uszkodzony czujnik / interfejs UART<br>
      • brak zasilania modułu czujnika<br><br>
      <b>Skutek:</b> czujnik wykluczony ze średniej systemowej.`;
  } else if (q === 'STALE') {
    desc = `<b>${name}: odczyt zamrożony</b><br><br>
      Temperatura nie zmieniła się o więcej niż 0,05°C od <b>ponad 10 minut</b>.<br>
      Ostatni odczyt: <b>${age} s</b> temu.<br>
      Prawdziwy czujnik prawie zawsze delikatnie dryfuje —<br>
      idealna stałość sugeruje zawieszony/uszkodzony sensor.<br><br>
      <b>Skutek:</b> czujnik wykluczony ze średniej systemowej do czasu<br>
      pierwszej znaczącej zmiany odczytu.`;
  } else if (q === 'OUT_OF_RANGE') {
    desc = `<b>${name}: odczyt poza zakresem</b><br><br>
      Wartość poza przedziałem <b>−40°C … +85°C</b>.<br>
      Typowo zwarcie lub rozwarcie toru pomiarowego.<br>
      Ostatni odczyt: <b>${age} s</b> temu.<br><br>
      <b>Skutek:</b> odczyt odrzucony, czujnik wykluczony ze średniej.`;
  } else if (q === 'WINDOW_OPEN') {
    desc = `<b>${name}: wykryto otwarte okno</b><br><br>
      Lokalny szybki spadek temperatury (>1,5°C) znacząco<br>
      większy niż w pozostałych czujnikach — prawdopodobnie<br>
      otwarte okno w pobliżu tego czujnika.<br><br>
      <b>Skutek:</b> czujnik czasowo wykluczony ze średniej,<br>
      aby chwilowe wychłodzenie nie wymusiło grzania.<br>
      Powrót automatyczny po odbudowie temperatury<br>
      lub ręcznie przyciskiem „Przywróć".`;
  }
  return desc;
}
function showSensorDetail(sx) {
  const sd = $('sensorDetail');
  $('sensorDetailTitle').textContent = (sx.name || ('Czujnik #' + sx.id)) + ' — ' + sx.quality;
  $('sensorDetailBody').innerHTML = sensorHealthDetail(sx);
  sd.classList.remove('hidden');
}
function renderSensors(sensors) {
  const tb = $('sensorsTable').querySelector('tbody');
  tb.innerHTML = '';
  sensors.forEach(sx => {
    const tr = document.createElement('tr');
    tr.dataset.sid = sx.id;
    const q = sx.quality;
    tr.innerHTML = `
      <td>${sx.id}${sx.external ? ' ★' : ''}${sx.sim ? ' SIM' : ''}</td>
      <td><input class="name" value="${sx.name}"></td>
      <td><input type="checkbox" ${sx.active ? 'checked' : ''}></td>
      <td><input type="number" step="0.01" value="${sx.weight}"></td>
      <td><input type="number" step="0.1" value="${sx.calib}"></td>
      <td><input type="number" step="0.1" value="${sx.comfort}"></td>
      <td class="hcell">${healthDot(q)}</td>
      <td class="qcell q-${q}">${q}${sx.window ? ' ⊗' : ''}</td>
      <td class="ecell">${fmtT(sx.eff)}</td>
      <td><button class="btn" style="padding:4px 8px" data-id="${sx.id}">Zapisz</button>${sx.window ? `<button class="btn" style="padding:4px 8px;margin-left:4px" data-restore="${sx.id}">Przywróć</button>` : ''}</td>`;
    const inputs = tr.querySelectorAll('input');
    tr.querySelector('button[data-id]').onclick = () => {
      const body = { name: inputs[0].value, active: inputs[1].checked,
        weight: parseFloat(inputs[2].value), calib: parseFloat(inputs[3].value),
        comfort: parseFloat(inputs[4].value) };
      post('/api/sensor?id=' + sx.id, body, true).then(() => refresh());
    };
    const rb = tr.querySelector('button[data-restore]');
    if (rb) rb.onclick = () => post('/api/sensor/restore?id=' + sx.id, '').then(() => refresh());
    /* Click on health dot for problem sensors shows detail panel. */
    const hd = tr.querySelector('.hdot');
    if (hd && q !== 'OK' && q !== 'SIMULATED' && q !== 'DISABLED') {
      hd.onclick = () => showSensorDetail(sx);
    }
    tb.appendChild(tr);
  });
  /* Close sensor detail panel. */
  const sdc = $('sensorDetailClose');
  if (sdc) sdc.onclick = () => $('sensorDetail').classList.add('hidden');
}

let profileEdited = false;
function renderProfile(p) {
  const g = $('profileGrid'); g.innerHTML = '';
  (p || []).forEach((h, idx) => {
    const d = document.createElement('div'); d.className = 'ph';
    d.innerHTML = `<div class="h">${idx}:00</div>
      <input data-h="${idx}" data-k="on" placeholder="ON" value="${h[0]}">
      <input data-h="${idx}" data-k="off" placeholder="OFF" value="${h[1]}">`;
    d.querySelectorAll('input').forEach(i => i.oninput = () => profileEdited = true);
    g.appendChild(d);
  });
}
function collectProfile() {
  const arr = [];
  $('profileGrid').querySelectorAll('[data-h]').forEach(inp => {
    const h = +inp.dataset.h, k = inp.dataset.k;
    if (!arr[h]) arr[h] = [0, 0];
    arr[h][k === 'on' ? 0 : 1] = parseFloat(inp.value);
  });
  return arr;
}

function renderModes(s) {
  if (!profileEdited) {
    $('pumpEn').checked = s.pump.enabled; $('pumpImpulse').value = s.pump.impulse;
    $('pumpPeriod').value = s.pump.period; $('pumpTotal').value = s.pump.total;
    $('emEn').checked = s.emergency.enabled; $('emOn').value = s.emergency.on; $('emPeriod').value = s.emergency.period;
    $('simHeat').checked = s.sim_heating; $('simMixed').checked = false;
    $('simAccel').checked = s.sim_accel;
    /* protection limits */
    if ($('limGrace')) $('limGrace').value = s.fault_grace_sec || 300;
    if ($('limMaxOn')) $('limMaxOn').value = s.max_on_sec || 14400;
    if ($('limBreak')) $('limBreak').value = s.max_on_break_sec || 600;
  }
}

async function refresh(live) {
  const s = await api('/api/state');
  if (!s) return;
  if (live) { renderDashboard(s); updateSensorCells(s.sensors || []); }
  else render(s);
}

async function loadDiag() {
  const d = await api('/api/diagnostics');
  if (d) $('diag').textContent = JSON.stringify(d, null, 2);
}

async function loadLog() {
  const l = await api('/api/log');
  if (l) $('log').textContent = l.map(e => `[${new Date(e[0]*1000).toLocaleString()}] ${e[1]}/${e[2]} ${e[3]}`).join('\n');
}

/* ---- charts ---- */
function drawSeries(cv, series, labels, colors) {
  const dpr = window.devicePixelRatio || 1;
  const W = cv.clientWidth || (cv.parentElement && cv.parentElement.clientWidth) || 600;
  const H = +cv.getAttribute('height') || 220;   /* stable logical height (attribute) */
  cv.width = Math.round(W * dpr);
  cv.height = Math.round(H * dpr);
  cv.style.height = H + 'px';                     /* lock display size so it can't grow */
  const ctx = cv.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, W, H);
  const n = Math.max(1, ...series.map(s => s.length));   /* spread points across width */
  let lo = Infinity, hi = -Infinity, pts = 0;
  series.forEach(s => s.forEach(v => { if (v !== null && v > -98) { lo = Math.min(lo, v); hi = Math.max(hi, v); pts++; } }));
  if (!isFinite(lo)) { lo = 0; hi = 30; }
  if (hi - lo < 1) { hi += 1; lo -= 1; }
  const pad = 28;
  const x = (i) => pad + (n > 1 ? i * (W - pad - 8) / (n - 1) : 0);
  const y = (v) => H - 18 - (v - lo) * (H - 30) / (hi - lo);
  // grid
  ctx.strokeStyle = '#262b35'; ctx.fillStyle = '#8a93a3'; ctx.font = '10px monospace';
  for (let g = 0; g < 4; g++) { const yy = 12 + g * (H - 30) / 3; ctx.beginPath(); ctx.moveTo(pad, yy + 6); ctx.lineTo(W - 8, yy + 6); ctx.stroke(); const val = hi - g * (hi - lo) / 3; ctx.fillText(val.toFixed(1), 2, yy + 9); }
  if (pts === 0) {
    ctx.fillStyle = '#8a93a3'; ctx.font = '12px sans-serif'; ctx.textAlign = 'center';
    ctx.fillText('brak danych (nagrywanie co 60 s)', W / 2, H / 2); ctx.textAlign = 'start';
  } else {
    series.forEach((s, si) => {
      ctx.strokeStyle = colors[si % colors.length]; ctx.lineWidth = 1.4; ctx.beginPath();
      let first = true;
      s.forEach((v, i) => { if (v === null || v <= -98) { first = true; return; } const px = x(i), py = y(v); if (first) { ctx.moveTo(px, py); first = false; } else ctx.lineTo(px, py); });
      ctx.stroke();
    });
  }
  // legend
  ctx.font = '11px sans-serif'; let lx = pad;
  labels.forEach((lb, i) => { ctx.fillStyle = colors[i % colors.length]; ctx.fillRect(lx, 2, 10, 10); ctx.fillStyle = '#e7ecf3'; ctx.fillText(lb, lx + 14, 11); lx += lb.length * 6 + 28; });
}

/* Time-based 24h chart: X is real wall-clock (now-24h .. now) so the trace
 * scrolls left as time advances; system + external plotted by timestamp, with
 * the daily profile ON/OFF band overlaid and an hour scale on the X axis. */
let samples24 = [];   /* cached rows [ts,sys,ext,heat,state,s0..s5] */

function render24(cv, rows, profile, sensors) {
  const winH = chartZoomH;  /* current zoom level */
  const dpr = window.devicePixelRatio || 1;
  const W = (cv.parentElement && cv.parentElement.clientWidth - 28) || 600;
  const H = 220;  /* fixed logical height — do NOT read back the attribute we set */
  cv.width = Math.round(W * dpr);
  cv.height = Math.round(H * dpr);
  cv.style.width = W + 'px';
  cv.style.height = H + 'px';
  const ctx = cv.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, W, H);

  const padL = 32, padR = 8, padT = 16, padB = 20;
  let t1 = 0;
  rows.forEach(d => { if (d[0] > t1) t1 = d[0]; });
  if (!t1) t1 = Date.now() / 1000;
  const t0 = t1 - winH * 3600;

  const internal = (sensors || []).filter(s => !s.external).slice(0, 6);
  /* Build set of "healthy" sensor column indices — only plot sensors
   * whose quality is OK or SIMULATED. Failed (TIMEOUT/STALE/etc.)
   * sensors keep their last value on-disk but should not be drawn
   * until they return to normal. */
  const healthyCols = new Set();
  internal.forEach((s, i) => {
    if (s.quality === 'OK' || s.quality === 'SIMULATED') healthyCols.add(i);
  });

  /* y-range from sample values AND profile thresholds so both fit.
   * Only consider sensor columns that are currently healthy. */
  let lo = Infinity, hi = -Infinity, pts = 0;
  const consider = (v) => { if (v !== null && v !== undefined && v > -98) { lo = Math.min(lo, v); hi = Math.max(hi, v); } };
  rows.forEach(d => {
    if (d[0] < t0) return;
    let any = false;
    consider(d[1]); consider(d[2]);
    if (d[1] > -98 || d[2] > -98) any = true;
    healthyCols.forEach(i => { consider(d[5 + i]); if (d[5 + i] > -98) any = true; });
    if (any) pts++;
  });
  if (profile && profile.length === 24) profile.forEach(h => { consider(h[0]); consider(h[1]); });
  if (!isFinite(lo)) { lo = 0; hi = 30; }
  if (hi - lo < 1) { hi += 1; lo -= 1; }
  const m = (hi - lo) * 0.08; lo -= m; hi += m;

  const X = (t) => padL + (t - t0) * (W - padL - padR) / (t1 - t0);
  const Y = (v) => H - padB - (v - lo) * (H - padT - padB) / (hi - lo);

  /* Y grid + temperature labels. */
  ctx.font = '10px monospace';
  for (let g = 0; g <= 3; g++) {
    const val = hi - g * (hi - lo) / 3, yy = Y(val);
    ctx.strokeStyle = '#262b35'; ctx.beginPath(); ctx.moveTo(padL, yy); ctx.lineTo(W - padR, yy); ctx.stroke();
    ctx.fillStyle = '#8a93a3'; ctx.fillText(val.toFixed(1), 2, yy + 3);
  }

  /* X axis: tick interval scales with zoom level. */
  ctx.textAlign = 'center';
  const tickSec = winH <= 1 ? 900 : winH <= 6 ? 3600 : 3 * 3600;  /* 15 min / 1 h / 3 h */
  const firstTick = Math.ceil(t0 / tickSec) * tickSec;
  for (let tt = firstTick; tt <= t1; tt += tickSec) {
    const xx = X(tt);
    ctx.strokeStyle = '#1c2028'; ctx.beginPath(); ctx.moveTo(xx, padT); ctx.lineTo(xx, H - padB); ctx.stroke();
    const d = new Date(tt * 1000);
    const label = winH <= 1 ? (d.getHours()<10?'0':'')+d.getHours()+':'+(d.getMinutes()<10?'0':'')+d.getMinutes()
                : (d.getHours()<10?'0':'')+d.getHours()+':00';
    ctx.fillStyle = '#8a93a3'; ctx.fillText(label, xx, H - 6);
  }
  ctx.textAlign = 'start';

  /* Profile overlay: ON/OFF band + stepped thresholds, per hour of day. */
  if (profile && profile.length === 24) {
    const h0 = Math.floor(t0 / 3600) * 3600;
    ctx.fillStyle = 'rgba(245,158,11,0.07)';
    for (let tt = h0; tt < t1; tt += 3600) {
      const hr = new Date(tt * 1000).getHours();
      const xs = X(Math.max(tt, t0)), xe = X(Math.min(tt + 3600, t1));
      const yOn = Y(profile[hr][0]), yOff = Y(profile[hr][1]);
      ctx.fillRect(xs, yOff, xe - xs, yOn - yOff);
    }
    const step = (idx, color) => {
      ctx.strokeStyle = color; ctx.lineWidth = 1; ctx.setLineDash([4, 3]); ctx.beginPath();
      let started = false;
      for (let tt = h0; tt < t1; tt += 3600) {
        const hr = new Date(tt * 1000).getHours();
        const yy = Y(profile[hr][idx]);
        const xs = X(Math.max(tt, t0)), xe = X(Math.min(tt + 3600, t1));
        if (!started) { ctx.moveTo(xs, yy); started = true; } else ctx.lineTo(xs, yy);
        ctx.lineTo(xe, yy);
      }
      ctx.stroke(); ctx.setLineDash([]);
    };
    step(0, 'rgba(245,158,11,0.55)');   /* ON threshold  */
    step(1, 'rgba(245,158,11,0.85)');   /* OFF threshold */
  }

  const SENSOR_COLORS = ['#a855f7', '#ec4899', '#14b8a6', '#eab308', '#f97316', '#38bdf8'];
  const plot = (getV, color, width) => {
    ctx.strokeStyle = color; ctx.lineWidth = width; ctx.beginPath();
    let first = true;
    rows.forEach(d => {
      const v = getV(d);
      if (d[0] < t0 || v === null || v === undefined || v <= -98) { first = true; return; }
      const px = X(d[0]), py = Y(v);
      if (first) { ctx.moveTo(px, py); first = false; } else ctx.lineTo(px, py);
    });
    ctx.stroke();
  };

  const legend = [];
  if (pts === 0) {
    ctx.fillStyle = '#8a93a3'; ctx.font = '12px sans-serif'; ctx.textAlign = 'center';
    ctx.fillText('brak danych (nagrywanie co 60 s)', W / 2, H / 2); ctx.textAlign = 'start';
  } else {
    /* per-sensor lines first (thin), then external and system on top (thick).
     * per_sensor[i] lives at sample column 5+i, matching the internal sensor
     * order in /api/state. Skip failed sensors — don't draw stale data. */
    internal.forEach((s, i) => {
      if (!visibleSensors.has(s.id)) return;          /* skip hidden sensor lines */
      if (!healthyCols.has(i)) return;                /* skip failed sensors */
      const col = SENSOR_COLORS[i % SENSOR_COLORS.length];
      plot(d => d[5 + i], col, 1);
    });
    /* Legend — show all sensors; mark failed ones so user knows why they disappeared. */
    internal.forEach((s, i) => {
      const col = SENSOR_COLORS[i % SENSOR_COLORS.length];
      const label = (s.name || ('S' + (i + 1))) + (healthyCols.has(i) ? '' : ' ❌');
      legend.push([label, col]);
    });
    plot(d => d[2], '#22c55e', 1.4); legend.push(['zewn.', '#22c55e']);
    plot(d => d[1], '#3b82f6', 1.8); legend.push(['system', '#3b82f6']);
  }
  legend.push(['profil ON/OFF', '#f59e0b']);

  /* Heating bar: 4px red strip at chart bottom for each minute the relay was ON. */
  let heatMins = 0;
  rows.forEach(d => {
    if (d[0] < t0 || d[0] > t1) return;
    if (d[3] === 1) {
      const x0 = X(Math.max(d[0] - 30, t0)), x1 = X(Math.min(d[0] + 30, t1));
      ctx.fillStyle = 'rgba(239,68,68,0.6)';
      ctx.fillRect(x0, H - padB + 4, Math.max(1, x1 - x0), 5);
      heatMins++;
    }
  });
  /* Expose for the dashboard. */
  window._lastHeatMins = heatMins;

  /* legend */
  ctx.font = '11px sans-serif';
  let lx = padL;
  legend.forEach(([lb, col]) => { ctx.fillStyle = col; ctx.fillRect(lx, 2, 10, 10); ctx.fillStyle = '#e7ecf3'; ctx.fillText(lb, lx + 14, 11); lx += lb.length * 6 + 26; });
}

function draw24h() {
  render24($('chart24'), samples24, lastState && lastState.profile, lastState && lastState.sensors);
}

async function load24h() {
  samples24 = await api('/api/samples') || [];
  draw24h();
}

function drawEnergyBars(cv, data) {
  if (!data || !data.length) return;
  const dpr = window.devicePixelRatio || 1;
  const W = (cv.parentElement && cv.parentElement.clientWidth - 28) || 600;
  const H = 180;  /* fixed logical height — do NOT read back the attribute we set */
  cv.width = Math.round(W * dpr);
  cv.height = Math.round(H * dpr);
  cv.style.width = W + 'px';
  cv.style.height = H + 'px';
  const ctx = cv.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, W, H);

  const padL = 36, padR = 8, padT = 16, padB = 20;
  const n = data.length;
  if (n < 2) { ctx.fillStyle='#8a93a3'; ctx.fillText('za mało danych', padL, H/2); return; }

  /* Find max heating minutes for Y scale, also max sys temp for overlay. */
  let maxMins = 0, maxSys = -Infinity, minSys = Infinity;
  data.forEach(d => { maxMins = Math.max(maxMins, d[3]||0); if (d[1]>-98) { maxSys=Math.max(maxSys,d[1]); minSys=Math.min(minSys,d[1]); } });
  if (maxMins < 1) maxMins = 1440;
  if (!isFinite(minSys)) minSys = 0;
  if (!isFinite(maxSys)) maxSys = 30;
  const barW = Math.max(2, (W - padL - padR) / n * 0.7);
  const gap = (W - padL - padR) / n;

  /* Y axis (left: heating minutes). */
  ctx.font = '10px monospace';
  const yM = v => H - padB - (v / maxMins) * (H - padT - padB);
  const yS = v => H - padB - ((v - minSys) / (maxSys - minSys || 1)) * (H - padT - padB);
  for (let g = 0; g <= 3; g++) {
    const val = maxMins * g / 3, yy = yM(val);
    ctx.strokeStyle = '#262b35'; ctx.beginPath(); ctx.moveTo(padL, yy); ctx.lineTo(W - padR, yy); ctx.stroke();
    ctx.fillStyle = '#8a93a3'; ctx.fillText(Math.round(val) + '', 2, yy + 3);
  }

  /* Bars + day labels. */
  data.forEach((d, i) => {
    const x = padL + i * gap + (gap - barW) / 2;
    const h = Math.max(1, (d[3]||0) / maxMins * (H - padT - padB));
    ctx.fillStyle = 'rgba(245,158,11,0.7)';
    ctx.fillRect(x, H - padB - h, barW, h);
    /* Day label every ~30 days. */
    if (i % Math.max(1, Math.floor(n / 12)) === 0) {
      const ts = d[0], ds = String(ts);
      const mmdd = ds.length === 8 ? ds.substring(4,6)+'-'+ds.substring(6,8) : ds;
      ctx.fillStyle = '#8a93a3';
      ctx.save(); ctx.translate(x + barW/2, H - 4); ctx.rotate(-0.6);
      ctx.fillText(mmdd, 0, 0); ctx.restore();
    }
  });

  /* System avg line overlay (blue). */
  ctx.strokeStyle = '#3b82f6'; ctx.lineWidth = 1.4; ctx.beginPath();
  let first = true;
  data.forEach((d, i) => {
    if (d[1] <= -98) { first = true; return; }
    const x = padL + i * gap + gap/2, py = yS(d[1]);
    if (first) { ctx.moveTo(x, py); first = false; } else ctx.lineTo(x, py);
  });
  ctx.stroke();

  /* Legend. */
  ctx.font = '11px sans-serif';
  ctx.fillStyle = 'rgba(245,158,11,0.7)'; ctx.fillRect(padL, 2, 10, 10);
  ctx.fillStyle = '#e7ecf3'; ctx.fillText('minuty grzania', padL + 14, 11);
  ctx.fillStyle = '#3b82f6'; ctx.fillRect(padL + 110, 2, 10, 10);
  ctx.fillStyle = '#e7ecf3'; ctx.fillText('śr. systemowa (°C)', padL + 124, 11);
}

async function loadDaily() {
  const data = await api('/api/daily') || [];
  drawEnergyBars($('chartDaily'), data);
}

/* ---- zoom + sensor toggles ---- */
let chartZoomH = 24;
let visibleSensors = new Set();

function populateSensorToggles(sensors) {
  const div = $('sensorToggles'); if (!div) return;
  const internal = (sensors || []).filter(s => !s.external);
  if (!visibleSensors.size) internal.forEach(s => visibleSensors.add(s.id));
  div.innerHTML = '';
  const COLS = ['#a855f7','#ec4899','#14b8a6','#eab308','#f97316','#38bdf8'];
  internal.forEach((s, i) => {
    const col = COLS[i % COLS.length];
    const lb = document.createElement('label');
    lb.style.cssText = 'font-size:12px;margin:0 10px 0 0;cursor:pointer;display:inline-flex;align-items:center;gap:4px;color:' + col;
    lb.innerHTML = '<input type="checkbox" ' + (visibleSensors.has(s.id) ? 'checked' : '') + '><span style="display:inline-block;width:10px;height:10px;background:' + col + ';border-radius:2px"></span>' + (s.name || ('S' + (i + 1)));
    lb.querySelector('input').onchange = function () {
      this.checked ? visibleSensors.add(s.id) : visibleSensors.delete(s.id);
      draw24h();
    };
    div.appendChild(lb);
  });
}

/* ---- wiring ---- */
$('btnBoost').onclick = () => post('/api/boost?on=' + (lastState && lastState.boost ? 0 : 1), '').then(() => refresh());
$('btnSensInc').onclick = () => { const n = lastState ? (lastState.sensors||[]).filter(x=>!x.external).length + 1 : 1; post('/api/sensors/count?n=' + Math.min(6, n), '').then(() => refresh()); };
$('btnSensDec').onclick = () => { const n = lastState ? (lastState.sensors||[]).filter(x=>!x.external).length - 1 : 1; post('/api/sensors/count?n=' + Math.max(1, n), '').then(() => refresh()); };
/* Kill button: show inline confirmation, then toggle. */
let killPending = false;
$('btnKill').onclick = () => {
  if (killPending) return; /* already waiting for confirmation */
  const disabling = !(lastState && lastState.disabled);
  if (!disabling) {
    /* Re-enabling is safe, no confirmation needed. */
    post('/api/heating?disable=0', '').then(() => refresh());
    return;
  }
  /* Show inline confirmation next to the button. */
  killPending = true;
  $('killConfirm').style.display = 'inline-flex';
  $('btnKill').style.opacity = '0.5';
};
$('killYes').onclick = () => {
  post('/api/heating?disable=1', '').then(() => refresh());
  killPending = false;
  $('killConfirm').style.display = 'none';
  $('btnKill').style.opacity = '';
};
$('killNo').onclick = () => {
  killPending = false;
  $('killConfirm').style.display = 'none';
  $('btnKill').style.opacity = '';
};
$('btnFault').onclick = () => post('/api/fault/clear', '').then(() => refresh());
$('btnPump').onclick = () => post('/api/pump', { enabled: $('pumpEn').checked, impulse: +$('pumpImpulse').value, period: +$('pumpPeriod').value, total: +$('pumpTotal').value }, true).then(() => refresh());
$('btnEm').onclick = () => post('/api/emergency', { enabled: $('emEn').checked, on: +$('emOn').value, period: +$('emPeriod').value }, true).then(() => refresh());
$('btnNet').onclick = () => post('/api/network', { sta_mode: $('netSta').checked, ssid: $('netSsid').value, pass: $('netPass').value }, true).then(() => refresh());
$('btnNotify').onclick = () => post('/api/notify', { email_enabled: $('nEmail').checked, email_to: $('nEmailTo').value, smtp_host: $('nSmtpHost').value, smtp_user: $('nSmtpUser').value, smtp_pass: $('nSmtpPass').value, sms_enabled: $('nSms').checked, sms_phone: $('nSmsPhone').value, sms_gateway: $('nSmsGw').value }, true).then(() => refresh());
$('btnNotifyTest').onclick = async () => {
  const res = $('notifyResult');
  res.style.display = 'block'; res.textContent = 'Wysyłanie...';
  /* Save first, then test with current config. */
  await post('/api/notify', { email_enabled: $('nEmail').checked, email_to: $('nEmailTo').value, smtp_host: $('nSmtpHost').value, smtp_user: $('nSmtpUser').value, smtp_pass: $('nSmtpPass').value }, true);
  const r = await post('/api/notify/test', '');
  if (r && r.result) {
    res.textContent = r.result;
    res.style.color = r.result.indexOf('OK:') >= 0 ? 'var(--ok)' : 'var(--err)';
  } else {
    res.textContent = 'Brak odpowiedzi z serwera';
  }
};
$('btnSimHeat').onclick = () => post('/api/sim/heating', { enabled: $('simHeat').checked, mixed: $('simMixed').checked, accel: $('simAccel').checked, mode: +$('simMode').value, heat_rate: +$('simHR').value, cool_rate: +$('simCR').value, inertia: +$('simIn').value }, true).then(() => refresh()).then(load24h);
$('btnSimSen').onclick = () => post('/api/sim/sensor?id=' + $('simSenId').value, { src: +$('simSenSrc').value, base: +$('simSenBase').value, rate: 0.1, target: 0 }, true).then(() => refresh()).then(load24h);
$('btnProfileApply').onclick = () => { profileEdited = false; post('/api/profile', collectProfile(), true).then(() => refresh()); };
$('btnProfileFileSave').onclick = () => { post('/api/profile/file?name=' + encodeURIComponent($('profileName').value || 'default') + '&op=save', '').then(() => refresh()); };
$('btnProfileFileLoad').onclick = () => { profileEdited = false; post('/api/profile/file?name=' + encodeURIComponent($('profileName').value || 'default') + '&op=load', '').then(() => refresh()); };

$('btnLimits').onclick = () => post('/api/limits', {
  fault_grace_sec: +$('limGrace').value,
  max_on_sec: +$('limMaxOn').value,
  max_on_break_sec: +$('limBreak').value
}, true).then(() => refresh());

/* Collapsible config sections: clicking the header toggles its card. */
document.querySelectorAll('.card-toggle').forEach(h => {
  h.onclick = () => h.parentElement.classList.toggle('collapsed');
});

/* Zoom buttons. */
document.querySelectorAll('.zoom-btn').forEach(b => {
  b.onclick = () => {
    document.querySelectorAll('.zoom-btn').forEach(x => x.classList.remove('sel'));
    b.classList.add('sel');
    chartZoomH = +b.dataset.h;
    draw24h();
  };
});

refresh().then(load24h).then(() => populateSensorToggles(lastState && lastState.sensors)); loadDiag(); loadLog(); loadDaily();
setInterval(() => refresh(true), 2000);
setInterval(loadDiag, 5000);
setInterval(loadLog, 10000);
/* Refetch history often enough that the accelerated (x10) run visibly scrolls. */
setInterval(load24h, 8000);
setInterval(loadDaily, 60000);
window.addEventListener('resize', draw24h);