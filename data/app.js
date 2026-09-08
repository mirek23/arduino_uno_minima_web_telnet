// ─────────────────────────────────────────────────────────────────────────────
// UNO R4 Minima monitor - SSE client, encoder reset and IP configuration popup
// ─────────────────────────────────────────────────────────────────────────────

'use strict';

const $ = (id) => document.getElementById(id);

// ── DOM refs ─────────────────────────────────────────────────────────────────
const connDot   = $('connDot');
const hdrName   = $('hdrName');

const countVal  = $('countVal');
const resetBtn  = $('resetBtn');

const tempVal   = $('tempVal');
const tempLabel = $('tempLabel');
const gaugeArc  = $('gaugeArc');

const netName   = $('netName');
const netIp     = $('netIp');
const netMode   = $('netMode');
const netLink   = $('netLink');
const dirtyWarn = $('dirtyWarn');

const log       = $('log');

const cfgDialog = $('cfgDialog');
const cfgOpen   = $('cfgOpen');
const cfgSave   = $('cfgSave');
const cfgReboot = $('cfgReboot');
const cfgClose  = $('cfgClose');
const cfgMsg    = $('cfgMsg');
const fwVer     = $('fwVer');

const modeStatic   = $('modeStatic');
const modeDhcp     = $('modeDhcp');
const staticFields = $('staticFields');
const fName = $('fName'), fIp = $('fIp'), fSn = $('fSn');
const fGw   = $('fGw'),   fDns = $('fDns'), fMac = $('fMac');

// ── Constants ────────────────────────────────────────────────────────────────
const TEMP_MIN   = -20;
const TEMP_MAX   = 100;
const ARC_LENGTH = 283;      // half-circle length in the gauge's SVG units
const MAX_LOG    = 40;

// The board pushes its state at least every SSE_HEARTBEAT_MS (5 s). If nothing
// arrives for this long the stream is dead even though the socket still looks
// open, so reconnect. This is the only way to notice a reboot: the W5500 is
// reset without closing its TCP connections, the browser's socket is left
// half-open, and onerror never fires.
const WATCHDOG_MS = 15000;
const RETRY_MS    = 3000;

// ── Event log ────────────────────────────────────────────────────────────────
function logEvent(msg) {
  const li = document.createElement('li');
  li.textContent = '[' + new Date().toLocaleTimeString() + '] ' + msg;
  log.prepend(li);
  while (log.children.length > MAX_LOG) log.removeChild(log.lastChild);
}

// ── Rendering ────────────────────────────────────────────────────────────────

function renderTemp(t, ok) {
  if (!ok || t === null || t === undefined || isNaN(t)) {
    tempVal.textContent   = '--.--';
    tempVal.style.color   = '';
    tempLabel.textContent = 'No sensor detected';
    gaugeArc.style.strokeDashoffset = ARC_LENGTH;
    return;
  }

  tempVal.textContent = t.toFixed(2);

  let colour, label;
  if      (t < 0)  { colour = '#3498db'; label = 'Freezing'; }
  else if (t < 15) { colour = '#58d68d'; label = 'Cold'; }
  else if (t < 25) { colour = '#f4d03f'; label = 'Comfortable'; }
  else if (t < 35) { colour = '#f39c12'; label = 'Warm'; }
  else             { colour = '#e74c3c'; label = 'Hot'; }

  tempVal.style.color   = colour;
  gaugeArc.style.stroke = colour;
  tempLabel.textContent = label + ' · 12-bit resolution';

  const pct = Math.min(1, Math.max(0, (t - TEMP_MIN) / (TEMP_MAX - TEMP_MIN)));
  gaugeArc.style.strokeDashoffset = ARC_LENGTH * (1 - pct);
}

// Previous values, so the log only records real transitions.
let prev = { temp: undefined, tempOk: undefined, count: undefined,
             ip: undefined, link: undefined, name: undefined };

function applyState(s) {
  if (s.count !== undefined && s.count !== prev.count) {
    countVal.textContent = s.count;
    if (prev.count !== undefined) logEvent('Encoder count: ' + s.count);
    prev.count = s.count;
  }

  if (s.temp !== undefined && (s.temp !== prev.temp || s.tempOk !== prev.tempOk)) {
    renderTemp(s.temp, s.tempOk);
    if (prev.temp !== undefined && s.tempOk) {
      logEvent('Temperature: ' + s.temp.toFixed(2) + ' °C');
    } else if (prev.tempOk && !s.tempOk) {
      logEvent('Temperature sensor lost');
    }
    prev.temp   = s.temp;
    prev.tempOk = s.tempOk;
  }

  if (s.name !== undefined && s.name !== prev.name) {
    hdrName.textContent = s.name;
    netName.textContent = s.name;
    document.title      = s.name + ' monitor';
    prev.name = s.name;
  }

  if (s.ip !== undefined && s.ip !== prev.ip) {
    netIp.textContent = s.ip;
    prev.ip = s.ip;
  }

  if (s.dhcp !== undefined) {
    netMode.textContent = s.dhcp
      ? ('DHCP' + (s.bound ? '' : ' (no lease)'))
      : 'Static';
  }

  if (s.link !== undefined && s.link !== prev.link) {
    netLink.textContent = s.link ? 'up' : 'down';
    if (prev.link !== undefined) logEvent('Ethernet link ' + (s.link ? 'up' : 'down'));
    prev.link = s.link;
  }

  if (s.dirty !== undefined) dirtyWarn.classList.toggle('hidden', !s.dirty);
}

// ── SSE connection ───────────────────────────────────────────────────────────

let evtSource      = null;
let reconnectTimer = null;
let watchdogTimer  = null;
let lastMsgAt      = 0;
let everConnected  = false;

function setDot(state) {          // 'connected' | 'error' | ''
  connDot.className = 'status-dot' + (state ? ' ' + state : '');
  connDot.title = state === 'connected' ? 'Receiving live data'
                : state === 'error'     ? 'No data – reconnecting'
                : 'Connecting…';
}

function scheduleReconnect(why) {
  setDot('error');
  logEvent(why + ' – retrying in ' + (RETRY_MS / 1000) + ' s');
  if (evtSource) { evtSource.close(); evtSource = null; }
  clearTimeout(reconnectTimer);
  reconnectTimer = setTimeout(connectSSE, RETRY_MS);
}

function connectSSE() {
  if (evtSource) { evtSource.close(); evtSource = null; }
  clearTimeout(reconnectTimer);

  setDot('');
  evtSource = new EventSource('/events');
  // Treat the stream as silent until real data lands: onopen only proves the
  // socket opened, not that the board behind it is alive.
  lastMsgAt = Date.now();

  evtSource.onopen = () => {
    logEvent(everConnected ? 'Reconnected' : 'Live connection established');
    everConnected = true;
    lastMsgAt = Date.now();
  };

  evtSource.onmessage = (e) => {
    lastMsgAt = Date.now();
    setDot('connected');           // the dot means "data is flowing"
    try { applyState(JSON.parse(e.data)); }
    catch (err) { console.warn('SSE parse error:', err); }
  };

  evtSource.onerror = () => scheduleReconnect('Connection lost');

  // One watchdog for the life of the page, not one per connection.
  if (!watchdogTimer) {
    watchdogTimer = setInterval(() => {
      if (!evtSource) return;                       // a retry is already queued
      if (Date.now() - lastMsgAt < WATCHDOG_MS) return;
      scheduleReconnect('No data for ' + (WATCHDOG_MS / 1000) + ' s');
    }, 1000);
  }
}

// ── Encoder reset ────────────────────────────────────────────────────────────

resetBtn.addEventListener('click', async () => {
  resetBtn.disabled = true;
  try {
    const r = await fetch('/api/reset', { method: 'POST' });
    if (!r.ok) throw new Error('HTTP ' + r.status);
    logEvent('Encoder count reset');
  } catch (err) {
    logEvent('ERROR resetting count: ' + err.message);
  } finally {
    resetBtn.disabled = false;
  }
});

// ── IP configuration popup ───────────────────────────────────────────────────

function showMsg(text, ok) {
  cfgMsg.textContent = text;
  cfgMsg.className   = 'msg ' + (ok ? 'ok' : 'err');
}

function clearMsg() {
  cfgMsg.textContent = '';
  cfgMsg.className   = 'msg hidden';
}

function syncModeFields() {
  // The static address fields are meaningless while DHCP is selected.
  const dhcp = modeDhcp.checked;
  [fIp, fSn, fGw, fDns].forEach((el) => { el.disabled = dhcp; });
  staticFields.style.opacity = dhcp ? '.5' : '1';
}

modeStatic.addEventListener('change', syncModeFields);
modeDhcp.addEventListener('change', syncModeFields);

cfgOpen.addEventListener('click', async () => {
  clearMsg();
  try {
    const r = await fetch('/api/netcfg');
    if (!r.ok) throw new Error('HTTP ' + r.status);
    const c = await r.json();

    // Revision from `git describe --tags` at build time, so it names the tag
    // the running firmware was built from.
    fwVer.textContent  = c.fw || 'unknown';

    modeDhcp.checked   = !!c.dhcp;
    modeStatic.checked = !c.dhcp;
    fName.value = c.name || '';
    fIp.value   = c.ip   || '';
    fSn.value   = c.sn   || '';
    fGw.value   = c.gw   || '';
    fDns.value  = c.dns  || '';
    fMac.value  = c.mac  || '';
    syncModeFields();

    if (c.jumper) {
      showMsg('The defaults jumper was grounded at boot, so the stored settings ' +
              'were ignored for this session. Saving still works.', false);
    } else if (c.dirty) {
      showMsg('There are staged changes that are not in EEPROM yet.', false);
    }
  } catch (err) {
    showMsg('Could not read the configuration: ' + err.message, false);
  }
  cfgDialog.showModal();
});

cfgClose.addEventListener('click', () => cfgDialog.close());

cfgSave.addEventListener('click', async () => {
  clearMsg();
  cfgSave.disabled = true;

  // Stage the form, then commit — the same two steps the telnet console uses.
  const params = new URLSearchParams();
  params.set('dhcp', modeDhcp.checked ? '1' : '0');
  params.set('name', fName.value.trim());
  params.set('mac',  fMac.value.trim());
  if (!modeDhcp.checked) {
    params.set('ip',  fIp.value.trim());
    params.set('sn',  fSn.value.trim());
    params.set('gw',  fGw.value.trim());
    params.set('dns', fDns.value.trim());
  }

  try {
    const stage = await (await fetch('/api/netcfg?' + params.toString(),
                                     { method: 'POST' })).json();
    if (!stage.ok) { showMsg(stage.msg || 'Rejected', false); return; }

    const saved = await (await fetch('/api/save', { method: 'POST' })).json();
    if (!saved.ok) { showMsg(saved.msg || 'EEPROM write failed', false); return; }

    showMsg('Saved to EEPROM. Reboot to apply the network settings.', true);
    logEvent('Configuration saved to EEPROM');
  } catch (err) {
    showMsg('Request failed: ' + err.message, false);
  } finally {
    cfgSave.disabled = false;
  }
});

cfgReboot.addEventListener('click', async () => {
  if (!confirm('Reboot the board now? Any unsaved changes are lost.')) return;
  clearMsg();
  cfgReboot.disabled = true;
  try {
    await fetch('/api/reboot', { method: 'POST' });
    showMsg('Rebooting – this page will reconnect automatically.', true);
    logEvent('Reboot requested');
  } catch (err) {
    // A reset can cut the connection before the response arrives; that is
    // the expected outcome, not a failure.
    showMsg('Reboot requested.', true);
  } finally {
    setTimeout(() => { cfgReboot.disabled = false; }, 3000);
  }
});

// ── Boot ─────────────────────────────────────────────────────────────────────

renderTemp(null, false);
syncModeFields();
logEvent('Dashboard loaded');
connectSSE();

// Browsers throttle timers in background tabs, so a hidden page can sit on a
// dead stream. Re-check as soon as it comes back to the foreground.
document.addEventListener('visibilitychange', () => {
  if (document.visibilityState !== 'visible') return;
  if (evtSource && Date.now() - lastMsgAt < WATCHDOG_MS) return;
  scheduleReconnect('Tab resumed with a stale stream');
});
