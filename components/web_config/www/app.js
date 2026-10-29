'use strict';
/*
 * Configuration portal UI. Offline (served by the device), no framework, no CDN.
 * All device data is inserted with textContent / createElement: SSIDs, sensor names and labels
 * come from the network and must never be interpreted as HTML.
 */
(function () {
  const $ = (id) => document.getElementById(id);

  const TSTAT_MODES = { 0: 'Tắt', 3: 'Làm lạnh', 4: 'Sưởi', 7: 'Chỉ quạt', 8: 'Hút ẩm (khô)' };
  const AC_MODES = ['Lạnh', 'Sưởi', 'Khô', 'Chỉ quạt', 'Tự động'];
  const AC_MODE_FAN = 3;
  const AC_FANS = ['Tự động', 'Mức 1', 'Mức 2', 'Mức 3', 'Mức 4', 'Mức 5', 'Êm', 'Mạnh'];
  const SWINGS = ['Tắt', 'Bật'];
  const PAIR_STATES = [
    'Không có thao tác',
    'Đang tìm cảm biến trên mạng (DNS-SD)',
    'Đang thiết lập phiên bảo mật (PASE)',
    'Đang ghép: xác thực thiết bị, cấp chứng thư, ACL, CASE',
    'Đang đọc thông tin và endpoint nhiệt độ',
    'Hoàn tất',
    'Thất bại',
    'Đang gỡ fabric của ESP khỏi cảm biến',
  ];
  const LEARN_STATES = ['Chưa học', 'Đang chờ tín hiệu từ remote', 'Đang nhận tín hiệu', 'Đã nhận lệnh', 'Học thất bại', 'Đã hủy'];
  const RESOLVE_KINDS = ['đúng trạng thái', 'dùng nhiệt độ đã học gần nhất', 'dùng quạt/đảo gió thay thế'];
  const IR_SOURCES = ['thermostat', 'thủ công', 'phát thử'];
  const WIFI_JOBS = { 1: 'Đang kết nối mạng mới…', 2: 'Đã kết nối mạng mới', 3: 'Không kết nối được, đã quay lại mạng cũ', 4: 'Không kết nối được mạng mới' };

  let token = '';
  let page = 'overview';
  let timer = null;
  let closed = false;
  let fast = false;
  const st = { ac: null, editSlot: null, codesSlot: null, codes: [], learnSession: 0, sensor: null, dirty: {} };

  /* ------------------------------------------------------------------ helpers */
  function el(tag, attrs, ...kids) {
    const e = document.createElement(tag);
    if (attrs) {
      for (const k of Object.keys(attrs)) {
        const v = attrs[k];
        if (v == null || v === false) continue;
        if (k === 'class') e.className = v;
        else if (k.startsWith('on')) e.addEventListener(k.slice(2), v);
        else e.setAttribute(k, v === true ? '' : String(v));
      }
    }
    for (const kid of kids) {
      if (kid == null) continue;
      e.append(kid instanceof Node ? kid : document.createTextNode(String(kid)));
    }
    return e;
  }
  function text(id, v) {
    const e = $(id);
    if (e) e.textContent = v == null || v === '' ? '-' : String(v);
  }
  function show(id, on) {
    const e = typeof id === 'string' ? $(id) : id;
    if (e) e.hidden = !on;
  }
  function msgbox(id, msg) {
    const e = $(id);
    if (!e) return;
    e.textContent = msg || '';
    e.hidden = !msg;
  }
  function fillSelect(sel, options, value) {
    const cur = value != null ? String(value) : sel.value;
    sel.replaceChildren(...options.map(([v, l]) => el('option', { value: v }, l)));
    if (options.some(([v]) => String(v) === cur)) sel.value = cur;
  }
  function nearest(options, v) {
    let best = null;
    for (const [x] of options) if (best == null || Math.abs(x - v) < Math.abs(best - v)) best = x;
    return best;
  }
  function fmtTemp(c, digits) {
    if (c == null) return '--';
    const v = c / 100;
    let s;
    if (digits != null) s = v.toFixed(digits);
    else s = c % 100 === 0 ? v.toFixed(0) : c % 10 === 0 ? v.toFixed(1) : v.toFixed(2);
    return s.replace('.', ',') + ' °C';
  }
  const fmtHalf = (h) => (h ? fmtTemp(h * 50) : '-');
  const signed = (c) => (c > 0 ? '+' : '') + fmtTemp(c);
  function fmtAge(s) {
    if (s == null) return 'chưa có';
    if (s < 5) return 'vừa xong';
    if (s < 60) return s + ' giây trước';
    if (s < 3600) return Math.floor(s / 60) + ' phút trước';
    if (s < 86400) return Math.floor(s / 3600) + ' giờ ' + Math.floor((s % 3600) / 60) + ' phút trước';
    return Math.floor(s / 86400) + ' ngày trước';
  }
  function fmtDur(s) {
    if (s == null) return '-';
    if (s < 60) return s + ' giây';
    if (s < 3600) return Math.floor(s / 60) + ' phút' + (s % 60 ? ' ' + (s % 60) + ' giây' : '');
    if (s < 86400) return Math.floor(s / 3600) + ' giờ ' + Math.floor((s % 3600) / 60) + ' phút';
    return Math.floor(s / 86400) + ' ngày ' + Math.floor((s % 86400) / 3600) + ' giờ';
  }
  function fmtBytes(n) {
    if (n == null) return '-';
    if (n >= 1048576) return (n / 1048576).toFixed(2).replace('.', ',') + ' MB';
    if (n >= 1024) return (n / 1024).toFixed(1).replace('.', ',') + ' KB';
    return n + ' B';
  }
  const hex = (n, w) => '0x' + Number(n).toString(16).toUpperCase().padStart(w || 4, '0');
  function stateDesc(s) {
    if (!s) return '-';
    if (!s.power) return 'Tắt';
    let d = AC_MODES[s.mode] || '?';
    if (s.mode !== AC_MODE_FAN && s.temp_half) d += ' ' + fmtHalf(s.temp_half);
    return d + ', quạt ' + (AC_FANS[s.fan] || '?').toLowerCase() + ', đảo gió ' + (SWINGS[s.swing] || '?').toLowerCase();
  }
  function parseKey(k) {
    if (k === 'off') return { power: 0, mode: 0, temp_half: 0, fan: 0, swing: 0, key: k };
    const m = /^m(\d+)t(\d+)f(\d+)s(\d+)$/.exec(k);
    return m ? { power: 1, mode: +m[1], temp_half: +m[2], fan: +m[3], swing: +m[4], key: k } : null;
  }
  const cmpState = (a, b) => a.power - b.power || a.mode - b.mode || a.temp_half - b.temp_half || a.fan - b.fan || a.swing - b.swing;
  function formatManual(code) {
    if (/^\d{11}$/.test(code)) return code.slice(0, 4) + '-' + code.slice(4, 7) + '-' + code.slice(7);
    if (/^\d{21}$/.test(code)) return [code.slice(0, 4), code.slice(4, 7), code.slice(7, 11), code.slice(11, 16), code.slice(16)].join('-');
    return code;
  }
  function kv(id, rows) {
    $(id).replaceChildren(...rows.flatMap(([k, v]) => [el('dt', null, k), el('dd', null, v == null || v === '' ? '-' : String(v))]));
  }
  function trackDirty(id) {
    const f = $(id);
    const mark = () => { st.dirty[id] = true; };
    f.addEventListener('input', mark);
    f.addEventListener('change', mark);
  }
  const isDirty = (id) => !!st.dirty[id];

  /* ------------------------------------------------------------------ API */
  function setOffline(off) { show('banner-offline', off && !closed); }
  function showPortalLeft(s) { text('portal-left', s > 0 ? 'Tự tắt sau ' + fmtDur(s) : ''); }

  async function getToken() {
    const r = await fetch('/api/session', { cache: 'no-store', headers: { 'X-User-Action': '1' } });
    const j = await r.json();
    if (!r.ok || !j.ok) throw new Error(j.error || 'Không tạo được phiên');
    token = j.token;
    text('dev-name', j.device);
    text('dev-ver', 'Firmware ' + j.version);
    show('banner-test', j.test_mode);
    show('sim-card', j.test_mode);
  }

  async function api(path, body, userAction) {
    const opt = { method: body ? 'POST' : 'GET', cache: 'no-store', headers: { 'X-CSRF-Token': token } };
    if (userAction) opt.headers['X-User-Action'] = '1';
    if (body) opt.body = new URLSearchParams(body);
    let r;
    try {
      r = await fetch('/api/' + path, opt);
    } catch (e) {
      setOffline(true);
      throw new Error('Mất kết nối với thiết bị');
    }
    setOffline(false);
    let j = null;
    try { j = await r.json(); } catch (e) { j = null; }
    if (j && typeof j.portal_left_s === 'number') showPortalLeft(j.portal_left_s);
    if (!r.ok || !j || !j.ok) {
      const msg = (j && j.error) || 'Lỗi ' + r.status;
      if (r.status === 403 && /Phiên/.test(msg)) {
        try { await getToken(); } catch (e) { /* retried on the next poll */ }
      }
      throw new Error(msg);
    }
    return j;
  }

  let toastTimer = null;
  function toast(msg, isErr) {
    const t = $('toast');
    t.textContent = msg;
    t.className = 'toast' + (isErr ? ' error' : '');
    t.hidden = false;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => { t.hidden = true; }, isErr ? 6000 : 3500);
  }

  async function act(path, body, okMsg) {
    try {
      const j = await api(path, body || {}, true);
      if (okMsg !== null) toast(j.message || okMsg || 'Đã lưu');
      schedule(300);
      return j;
    } catch (e) {
      toast(e.message, true);
      return null;
    }
  }

  /* ------------------------------------------------------------------ navigation / polling */
  const loaders = {};

  function schedule(ms) {
    clearTimeout(timer);
    if (!closed) timer = setTimeout(() => refresh(false), ms);
  }

  async function refresh(user) {
    clearTimeout(timer);
    if (closed) return;
    fast = false;
    try {
      await loaders[page](user);
    } catch (e) {
      if (user) toast(e.message, true);
    }
    if (!document.hidden) schedule(fast ? 1000 : page === 'diag' ? 5000 : 2500);
  }

  function go(p) {
    page = p;
    for (const b of document.querySelectorAll('#tabs button')) b.classList.toggle('active', b.dataset.page === p);
    for (const s of document.querySelectorAll('main > section.page')) s.hidden = s.id !== 'page-' + p;
    st.dirty = {};
    try { history.replaceState(null, '', '#' + p); } catch (e) { /* ignore */ }
    refresh(true);
  }

  document.addEventListener('visibilitychange', () => { if (!document.hidden) refresh(false); });

  /* ------------------------------------------------------------------ Tổng quan */
  loaders.overview = async (user) => {
    const j = await api('status', null, user);
    const t = j.temp, s = j.sensor, c = j.climate, th = j.thermostat, ir = j.ir, w = j.wifi, m = j.matter, p = j.profile;
    text('ov-temp', t.valid ? fmtTemp(t.centi, 1) : 'Không khả dụng');
    text('ov-temp-src', t.source_name);
    text('ov-temp-age', t.valid ? 'giá trị ' + fmtAge(t.value_age_s) + (t.source === 1 ? ', báo cáo cuối ' + fmtAge(t.alive_age_s) : '') : '-');
    text('ov-sensor', s.bound ? s.name + ' · ' + s.state_name + (s.stale ? ' · dữ liệu quá cũ' : '') : 'Chưa ghép cảm biến Matter');
    text('ov-mode', TSTAT_MODES[th.mode] || th.mode);
    text('ov-target', th.mode === 0 ? '-' : fmtTemp(th.mode === 4 ? th.heat_sp : th.cool_sp));
    text('ov-desired', c.desired ? c.desired.desc : '-');
    text('ov-control', c.control_mode === 1 ? 'Theo cảm biến ngoài' + (c.compensating ? ' (bù ' + signed(c.offset) + ')' : '') : 'Direct');
    text('ov-reason', c.reason_text);
    text('ov-profile', p.slot >= 0 ? p.name + ' · ' + p.codes + ' lệnh · ' + (p.locked ? 'đã khóa' : 'chưa khóa') : 'Chưa có');
    msgbox('ov-notready', c.ready ? '' : 'Chưa điều khiển tự động: ' + (c.not_ready || 'profile chưa sẵn sàng'));
    text('ov-ir-label', ir.last_age_s == null ? 'Chưa phát lệnh nào' : ir.last_label);
    text('ov-ir-age', ir.last_age_s == null ? 'chưa' : fmtAge(ir.last_age_s) + ' (' + (IR_SOURCES[ir.last_source] || '') + ')');
    text('ov-ir-pending', c.pending_ms ? 'sau ' + Math.ceil(c.pending_ms / 1000) + ' giây' : ir.busy ? 'đang phát' : 'không');
    text('ov-ir-stats', ir.sent + ' đã phát, ' + ir.failed + ' lỗi, ' + ir.dropped_duplicate + ' trùng bị bỏ, ' + ir.superseded + ' bị thay thế');
    msgbox('ov-ir-err', c.last_error || (ir.last_error ? 'Lỗi phát IR: ' + ir.last_error : ''));
    text('ov-wifi', w.configured ? w.ssid + ' · ' + (w.connected ? 'đã kết nối (' + w.rssi + ' dBm)' : 'chưa kết nối') : 'Chưa cấu hình');
    text('ov-ip', w.ip4);
    text('ov-matter', (m.home_fabrics > 0 ? 'đã ghép ' + m.home_fabrics + ' hệ sinh thái' : 'chưa ghép') + (m.window_open ? ' · cửa sổ ghép đang mở' : ''));
    text('ov-fabb', m.commissioner_ready ? 'sẵn sàng' : 'chưa sẵn sàng');
    text('ov-uptime', fmtDur(j.uptime_s));
    text('ov-errors', j.error_count);
    show('ov-restart', p.restart_needed);
  };

  /* ------------------------------------------------------------------ Wi-Fi */
  loaders.wifi = async (user) => {
    const j = await api('wifi', null, user);
    const w = j.wifi;
    text('wf-ssid', w.configured ? w.ssid : 'Chưa cấu hình');
    text('wf-state', w.connected ? 'Đã kết nối' : w.configured ? 'Chưa kết nối' : '-');
    $('wf-state').className = w.connected ? 'ok' : 'bad';
    text('wf-ip4', w.ip4);
    text('wf-ip6', w.ip6.length ? w.ip6.join('\n') : '-');
    text('wf-rssi', w.connected ? w.rssi + ' dBm · kênh ' + w.channel : '-');
    msgbox('wf-job', w.job ? w.job_message || WIFI_JOBS[w.job] : '');
    show('wf-conflict', w.conflict);
    text('ap-ssid', w.ap_ssid);
    text('ap-clients', w.ap_clients);
    $('wf-scan-state').textContent = w.scanning ? 'Đang quét…' : j.scan.length ? j.scan.length + ' mạng' : '';
    fast = w.scanning || w.job === 1;
    $('wf-list').replaceChildren(
      ...j.scan.map((n) =>
        el('li', { onclick: () => { $('wf-in-ssid').value = n.ssid; $('wf-in-pass').focus(); } },
          el('span', null, n.ssid),
          el('span', { class: 'muted small' }, n.rssi + ' dBm · ' + (n.open ? 'mạng mở' : 'có mật khẩu')))));
  };

  $('wf-scan').addEventListener('click', () => act('wifi/scan', {}, 'Đang quét mạng…'));
  $('wf-show').addEventListener('change', (e) => { $('wf-in-pass').type = e.target.checked ? 'text' : 'password'; });
  $('wf-form').addEventListener('submit', async (ev) => {
    ev.preventDefault();
    const ssid = $('wf-in-ssid').value;
    const pass = $('wf-in-pass').value;
    if (pass && (pass.length < 8 || pass.length > 63)) {
      toast('Mật khẩu Wi-Fi phải dài 8-63 ký tự (để trống nếu mạng không có mật khẩu)', true);
      return;
    }
    const j = await act('wifi/sta', { ssid: ssid, password: pass }, 'Đã lưu, đang kết nối…');
    if (j) $('wf-in-pass').value = '';
  });
  $('ap-form').addEventListener('submit', async (ev) => {
    ev.preventDefault();
    const f = ev.target.elements;
    if (f.password.value !== f.confirm.value) {
      toast('Hai mật khẩu không khớp', true);
      return;
    }
    const j = await act('wifi/ap_password', { password: f.password.value, confirm: f.confirm.value });
    if (j) ev.target.reset();
  });

  /* ------------------------------------------------------------------ Nhà thông minh */
  function drawQr(box, size, rows) {
    const NS = 'http://www.w3.org/2000/svg';
    const quiet = 4;
    const n = size + 2 * quiet;
    const svg = document.createElementNS(NS, 'svg');
    svg.setAttribute('viewBox', '0 0 ' + n + ' ' + n);
    svg.setAttribute('shape-rendering', 'crispEdges');
    svg.setAttribute('role', 'img');
    svg.setAttribute('aria-label', 'Mã QR ghép Matter');
    const bg = document.createElementNS(NS, 'rect');
    bg.setAttribute('width', n);
    bg.setAttribute('height', n);
    bg.setAttribute('fill', '#fff');
    svg.append(bg);
    let d = '';
    rows.forEach((row, y) => {
      for (let x = 0; x < size; x++) {
        const nib = parseInt(row[x >> 2], 16);
        if (nib & (8 >> (x & 3))) d += 'M' + (x + quiet) + ' ' + (y + quiet) + 'h1v1h-1z';
      }
    });
    const path = document.createElementNS(NS, 'path');
    path.setAttribute('d', d);
    path.setAttribute('fill', '#000');
    svg.append(path);
    box.replaceChildren(svg);
  }

  loaders.home = async (user) => {
    const j = await api('matter', null, user);
    const m = j.matter;
    text('hm-window', m.window_open ? 'Đang mở' : 'Đóng');
    text('hm-count', m.home_fabrics);
    const sel = $('hm-minutes');
    if (!sel.options.length) {
      const max = Math.floor(j.window_max_s / 60);
      const opts = [3, 5, 10, 15].filter((x) => x <= max).map((x) => [x, x + ' phút']);
      fillSelect(sel, opts, opts[opts.length - 1][0]);
    }
    $('hm-fabrics').replaceChildren(
      ...(j.fabrics.length
        ? j.fabrics.map((f) =>
            el('li', null, el('span', null, f.label || 'Không có nhãn'), el('span', { class: 'muted small' }, 'fabric ' + f.index + ' · VID ' + hex(f.vendor_id))))
        : [el('li', { class: 'muted' }, 'Chưa thuộc hệ sinh thái nào')]));
  };

  $('hm-open').addEventListener('click', () => act('matter/window', { action: 'open', minutes: $('hm-minutes').value }, 'Đã mở cửa sổ ghép'));
  $('hm-close').addEventListener('click', () => act('matter/window', { action: 'close' }, 'Đã đóng cửa sổ ghép'));
  $('hm-show').addEventListener('click', async () => {
    try {
      const j = await api('matter/onboarding', null, true);
      text('hm-manual', formatManual(j.manual));
      text('hm-qrtext', j.qr);
      if (j.qr_rows) drawQr($('hm-qr'), j.qr_size, j.qr_rows);
      show('hm-code', true);
    } catch (e) {
      toast(e.message, true);
    }
  });

  /* ------------------------------------------------------------------ Cảm biến Matter */
  function renderEndpoints(p, s) {
    const box = $('pg-endpoints');
    const done = p.state === 5;
    const eps = p.endpoints || [];
    const sameAsBound = s.bound && s.node_id === p.node_id;
    show('pg-bind', done && eps.some((e) => e.read_ok));
    show('pg-remove', done && !sameAsBound && p.node_id !== '0x0000000000000000');
    if (!done) {
      box.replaceChildren();
      return;
    }
    if (!eps.length) {
      box.replaceChildren(el('div', { class: 'warn' }, 'Thiết bị không có endpoint Temperature Measurement (cluster 0x0402).'));
      return;
    }
    const prev = box.querySelector('input[name=ep]:checked');
    let chosen = prev ? +prev.value : sameAsBound ? s.endpoint : null;
    if (chosen == null) {
      const first = eps.find((e) => e.read_ok);
      chosen = first ? first.endpoint : null;
    }
    box.replaceChildren(
      el('h3', null, 'Chọn nguồn nhiệt độ'),
      ...eps.map((e) => {
        const val = e.read_ok ? (e.null ? 'chưa có giá trị (null)' : fmtTemp(e.value)) : 'không đọc được';
        const range = e.min != null || e.max != null ? ' · phạm vi ' + (e.min != null ? fmtTemp(e.min) : '?') + ' đến ' + (e.max != null ? fmtTemp(e.max) : '?') : '';
        return el('label', { class: 'ep' },
          el('input', { type: 'radio', name: 'ep', value: e.endpoint, checked: e.endpoint === chosen, disabled: !e.read_ok }),
          el('span', null, 'Endpoint ' + e.endpoint + ': ' + val + range));
      }));
  }

  loaders.sensor = async (user) => {
    const j = await api('sensor', null, user);
    st.sensor = j;
    const s = j.sensor, p = j.progress;
    show('sn-none', !s.bound);
    show('sn-info', s.bound);
    if (s.bound) {
      text('sn-name', [s.name, s.vendor_name && s.vendor_name !== s.name ? s.vendor_name : null].filter(Boolean).join(' · '));
      text('sn-node', s.node_id + ' / endpoint ' + s.endpoint);
      text('sn-state', s.state_name + (s.stale ? ' · dữ liệu quá cũ' : ''));
      text('sn-value', s.value_null ? 'Cảm biến báo không đo được (null)' : s.value == null ? '-' : fmtTemp(s.value) + (s.value_plausible ? '' : ' (ngoài phạm vi hợp lệ)'));
      text('sn-vage', fmtAge(s.value_age_s));
      text('sn-aage', fmtAge(s.alive_age_s));
      text('sn-interval', s.max_interval_s ? s.min_interval_s + '–' + s.max_interval_s + ' giây' : '-');
      text('sn-stale', fmtDur(s.stale_timeout_s));
      text('sn-retry', s.retries + (s.next_retry_ms ? ' · lần tới sau ' + Math.ceil(s.next_retry_ms / 1000) + ' giây' : ''));
      msgbox('sn-error', s.error);
    }
    text('sn-local', s.local_configured ? (s.local_valid ? fmtTemp(s.local, 1) + ' · ' + fmtAge(s.local_age_s) : 'không đọc được') : 'không lắp');
    const active = (p.state >= 1 && p.state <= 4) || p.state === 7;
    show('pg-card', p.state !== 0 || !!p.stage);
    text('pg-state', p.state === 0 && p.stage === 'RemoveFabric OK' ? 'Đã gỡ fabric của ESP khỏi cảm biến' : PAIR_STATES[p.state] || p.state);
    text('pg-stage', p.stage);
    const dev = [p.vendor_name, p.product_name, p.node_label].filter(Boolean).join(' · ');
    text('pg-device', dev ? dev + ' (VID ' + hex(p.vendor_id) + ', PID ' + hex(p.product_id) + ')' : '-');
    msgbox('pg-error', p.state === 6 ? p.error : '');
    msgbox('pg-att', p.attestation_overridden ? 'Đã chấp nhận thiết bị chưa được chứng nhận: ' + p.attestation_note : '');
    renderEndpoints(p, s);
    fast = active || j.commissioner.busy;
  };

  $('pair-form').addEventListener('submit', async (ev) => {
    ev.preventDefault();
    const code = $('pair-code').value.trim();
    const j = await act('sensor/pair', { code: code, uncertified: $('pair-unc').checked ? 1 : 0 }, 'Đang ghép cảm biến…');
    if (j) $('pair-code').value = '';
  });
  $('pair-cancel').addEventListener('click', () => act('sensor/cancel', {}, 'Đã hủy'));
  $('pg-bind').addEventListener('click', () => {
    const sel = $('pg-endpoints').querySelector('input[name=ep]:checked');
    if (!sel || !st.sensor) {
      toast('Hãy chọn một endpoint', true);
      return;
    }
    act('sensor/bind', { node: st.sensor.progress.node_id, endpoint: sel.value }, 'Đã lưu cảm biến');
  });
  $('sn-retry-btn').addEventListener('click', () => act('sensor/retry', {}, 'Đang kết nối lại'));
  $('sn-read-btn').addEventListener('click', () => act('sensor/read', {}, 'Đang đọc cảm biến…'));
  const removeSensor = () => {
    if (confirm('Gỡ fabric của ESP khỏi cảm biến? Cảm biến vẫn hoạt động bình thường với hub cũ.')) act('sensor/remove', {}, null);
  };
  $('sn-remove-btn').addEventListener('click', removeSensor);
  $('pg-remove').addEventListener('click', removeSensor);
  $('sn-forget-btn').addEventListener('click', () => {
    if (confirm('Chỉ xóa liên kết trên ESP? Fabric của ESP vẫn còn trong cảm biến.')) act('sensor/forget', {}, 'Đã xóa liên kết');
  });
  $('sim-form').addEventListener('submit', (ev) => {
    ev.preventDefault();
    act('sensor/simulate', { enable: 1, centi: Math.round(+ev.target.elements.temp.value * 100) }, 'Đã bật giả lập');
  });
  $('sim-off').addEventListener('click', () => act('sensor/simulate', { enable: 0 }, 'Đã tắt giả lập'));

  /* ------------------------------------------------------------------ Máy lạnh */
  function editProfile() {
    return st.ac && st.editSlot != null ? st.ac.profiles[st.editSlot] : null;
  }

  async function loadCodes() {
    if (st.editSlot == null) {
      st.codes = [];
      st.codesSlot = null;
      return;
    }
    const j = await api('ac/codes?slot=' + st.editSlot);
    st.codes = j.codes;
    st.codesSlot = j.slot;
  }

  function renderCodes() {
    const p = editProfile();
    const locked = p && p.locked;
    const codes = st.codes.map(parseKey).filter(Boolean).sort(cmpState);
    text('ace-count', codes.length);
    if (!codes.length) {
      $('ace-codes').replaceChildren(el('li', { class: 'muted' }, 'Chưa học lệnh nào'));
      return;
    }
    $('ace-codes').replaceChildren(
      ...codes.map((c) =>
        el('li', null,
          el('span', null, stateDesc(c)),
          el('span', { class: 'acts' },
            el('button', { class: 'btn small', type: 'button', onclick: () => act('ac/code/test', { slot: st.editSlot, key: c.key }, 'Đã phát thử') }, 'Phát thử'),
            locked ? null : el('button', {
              class: 'btn small danger', type: 'button',
              onclick: async () => {
                if (!confirm('Xóa lệnh "' + stateDesc(c) + '"?')) return;
                if (await act('ac/code/delete', { slot: st.editSlot, key: c.key }, 'Đã xóa lệnh')) st.codesSlot = null;
              },
            }, 'Xóa')))));
  }

  function learnTempOptions(stepHalf) {
    const opts = [];
    for (let h = 20; h <= 70; h += stepHalf === 1 ? 1 : 2) opts.push([h, fmtHalf(h)]);
    return opts;
  }

  function updateLearnForm() {
    const on = $('lf-power').value === '1';
    for (const e of document.querySelectorAll('#learn-form .lf-on')) e.hidden = !on;
    if (on && +$('lf-mode').value === AC_MODE_FAN) for (const e of document.querySelectorAll('#learn-form .lf-temp')) e.hidden = true;
  }

  function renderControl(j) {
    const th = j.thermostat, c = j.climate;
    const dirty = isDirty('ctl-form');
    const modes = [[0, 'Tắt'], [3, 'Làm lạnh']];
    if (th.heat) modes.push([4, 'Sưởi']);
    if (th.dry) modes.push([8, 'Hút ẩm (khô)']);
    if (th.fan_only) modes.push([7, 'Chỉ quạt']);
    fillSelect($('ctl-mode'), modes, dirty ? null : th.mode);
    const mode = +$('ctl-mode').value;
    const prof = j.active >= 0 ? j.profiles[j.active] : null;
    const step = prof && prof.step_half === 1 ? 50 : 100;
    const lo = mode === 4 ? th.heat_min : th.cool_min;
    const hi = mode === 4 ? th.heat_max : th.cool_max;
    const sps = [];
    for (let v = Math.ceil(lo / step) * step; v <= hi; v += step) sps.push([v, fmtTemp(v)]);
    fillSelect($('ctl-sp'), sps, dirty ? null : nearest(sps, mode === 4 ? th.heat_sp : th.cool_sp));
    show('ctl-sp-wrap', mode === 3 || mode === 4 || mode === 8);
    const fans = prof && prof.fans_mask ? prof.fans_mask : 0xff;
    const swings = prof && prof.swing_mask ? prof.swing_mask : 0x3;
    fillSelect($('ctl-fan'), AC_FANS.map((n, i) => [i, n]).filter(([i]) => fans & (1 << i)), dirty ? null : c.fan);
    fillSelect($('ctl-swing'), SWINGS.map((n, i) => [i, n]).filter(([i]) => swings & (1 << i)), dirty ? null : c.swing);
    text('ctl-last', c.has_sent ? c.last_sent.desc + ' · ' + fmtAge(c.last_sent_age_s) + (c.last_kind ? ' (' + RESOLVE_KINDS[c.last_kind] + ')' : '') : 'chưa');
    text('ctl-pending', c.pending_ms ? 'sau ' + Math.ceil(c.pending_ms / 1000) + ' giây: ' + c.desired.desc : 'không');
    msgbox('ctl-error', c.last_error);
  }

  function renderAc() {
    const j = st.ac;
    if (!j) return;
    show('ac-restart', j.restart_needed);
    const rd = $('ac-ready');
    rd.className = j.ready ? 'info' : 'warn';
    rd.textContent = j.ready ? 'Profile đang dùng đã khóa: điều khiển tự động đang hoạt động.' : 'Chưa điều khiển tự động: ' + (j.not_ready || '');

    $('ac-profiles').replaceChildren(
      ...j.profiles.map((p) =>
        el('tr', {
          class: p.slot === st.editSlot ? 'sel' : null,
          onclick: () => {
            if (!p.present) return;
            st.editSlot = p.slot;
            st.dirty['ace-form'] = false;
            renderAc();
            loadCodes().then(renderCodes).catch(() => {});
          },
        },
          el('td', null, String(p.slot + 1)),
          el('td', null, p.present ? p.name : el('span', { class: 'muted' }, 'trống')),
          el('td', null, p.present ? String(p.codes) : ''),
          el('td', null, p.present ? (p.slot === j.active ? 'đang dùng · ' : '') + (p.locked ? 'đã khóa' : 'chưa khóa') : ''))));

    const empty = j.profiles.filter((p) => !p.present);
    show('ac-create', empty.length > 0);
    if (empty.length) fillSelect($('ac-create-slot'), empty.map((p) => [p.slot, 'Ô ' + (p.slot + 1)]));

    const p = editProfile();
    show('ace-card', !!(p && p.present));
    show('learn-card', !!(p && p.present && !p.locked));
    if (p && p.present) {
      text('ace-title', p.name);
      const info = $('ace-lockinfo');
      info.className = p.locked ? 'info' : 'warn';
      info.textContent = p.locked
        ? 'Đã khóa: chỉ phát các lệnh đã học, không học/xóa thêm. Mở khóa để sửa.'
        : 'Chưa khóa: điều khiển tự động chưa dùng profile này. Học ít nhất lệnh Tắt và một lệnh Làm lạnh, phát thử, rồi khóa để xác nhận.';
      const f = $('ace-form').elements;
      if (!isDirty('ace-form')) {
        f.name.value = p.name;
        f.step.value = String(p.step_half);
        f.fan.value = String(p.default_fan);
        f.swing.value = String(p.default_swing);
        f.carrier.value = String(p.carrier_hz);
      }
      for (const e of $('ace-form').querySelectorAll('input, select, button')) e.disabled = p.locked;
      show('ace-select', p.slot !== j.active);
      $('ace-lock').textContent = p.locked ? 'Mở khóa' : 'Khóa profile';
      $('ace-delete').disabled = p.locked;
      const lt = $('lf-temp');
      const opts = learnTempOptions(p.step_half);
      if (lt.dataset.step !== String(p.step_half)) {
        fillSelect(lt, opts, nearest(opts, lt.value ? +lt.value : 50));
        lt.dataset.step = String(p.step_half);
      }
    }
    renderCodes();

    const l = j.learn;
    const learning = l.state === 1 || l.state === 2;
    const ls = $('learn-status');
    ls.hidden = l.state === 0;
    ls.className = l.state === 4 ? 'err' : 'info';
    ls.textContent = LEARN_STATES[l.state] + (l.message ? ': ' + l.message : '') +
      (l.state === 3 ? ' (' + l.segments + ' đoạn, ' + l.levels + ' xung, ' + l.duration_ms + ' ms)' : '');
    show('learn-result', l.state === 3);
    st.learnSession = l.session;
    $('learn-start').disabled = learning;
    $('learn-cancel').disabled = !learning;
    if (learning) fast = true;
    renderControl(j);
  }

  loaders.ac = async (user) => {
    const j = await api('ac', null, user);
    st.ac = j;
    const valid = (s) => s != null && j.profiles[s] && j.profiles[s].present;
    if (!valid(st.editSlot)) {
      const first = j.profiles.find((p) => p.present);
      st.editSlot = valid(j.active) ? j.active : first ? first.slot : null;
    }
    const p = editProfile();
    if (st.codesSlot !== st.editSlot || (p && p.present && p.codes !== st.codes.length)) await loadCodes();
    renderAc();
  };

  $('ac-create').addEventListener('submit', async (ev) => {
    ev.preventDefault();
    const f = ev.target.elements;
    const slot = +f.slot.value;
    const j = await act('ac/profile/create', { slot: slot, name: f.name.value.trim(), step: f.step.value, carrier: f.carrier.value || 0 }, 'Đã tạo profile');
    if (j) {
      st.editSlot = slot;
      ev.target.reset();
    }
  });
  $('ace-form').addEventListener('submit', async (ev) => {
    ev.preventDefault();
    const f = ev.target.elements;
    const j = await act('ac/profile/update', {
      slot: st.editSlot, name: f.name.value.trim(), step: f.step.value, fan: f.fan.value, swing: f.swing.value, carrier: f.carrier.value || 0,
    }, 'Đã lưu profile');
    if (j) st.dirty['ace-form'] = false;
  });
  $('ace-select').addEventListener('click', async () => {
    const j = await act('ac/profile/select', { slot: st.editSlot }, 'Đã chọn profile');
    if (j && j.restart_needed) toast('Đã chọn profile. Khởi động lại để ứng dụng nhà nhận giới hạn nhiệt độ/chế độ mới.');
  });
  $('ace-lock').addEventListener('click', async () => {
    const p = editProfile();
    if (!p) return;
    if (!p.locked && !confirm('Khóa profile? Chỉ khóa sau khi đã phát thử và thấy máy lạnh nhận đúng các lệnh.')) return;
    const j = await act('ac/profile/lock', { slot: st.editSlot, locked: p.locked ? 0 : 1 }, p.locked ? 'Đã mở khóa' : 'Đã khóa profile');
    if (j && j.restart_needed) toast('Khởi động lại để ứng dụng nhà nhận giới hạn nhiệt độ/chế độ của profile.');
  });
  $('ace-delete').addEventListener('click', async () => {
    const p = editProfile();
    if (!p || !confirm('Xóa profile "' + p.name + '" và toàn bộ lệnh đã học?')) return;
    if (await act('ac/profile/delete', { slot: st.editSlot }, 'Đã xóa profile')) {
      st.editSlot = null;
      st.codesSlot = null;
    }
  });

  $('lf-power').addEventListener('change', updateLearnForm);
  $('lf-mode').addEventListener('change', updateLearnForm);
  function learnState() {
    const f = $('learn-form').elements;
    const s = { power: f.power.value };
    if (f.power.value === '1') {
      s.mode = f.mode.value;
      s.fan = f.fan.value;
      s.swing = f.swing.value;
      if (+f.mode.value !== AC_MODE_FAN) s.temp_half = f.temp_half.value;
    }
    return s;
  }
  $('learn-start').addEventListener('click', () => act('ac/learn/start', {}, 'Bộ thu đã sẵn sàng: hướng remote vào đầu thu và bấm'));
  $('learn-cancel').addEventListener('click', () => act('ac/learn/cancel', {}, null));
  $('learn-test').addEventListener('click', () => act('ac/learn/test', { session: st.learnSession }, 'Đã phát thử lệnh vừa học'));
  $('learn-save').addEventListener('click', async () => {
    const body = Object.assign({ session: st.learnSession, slot: st.editSlot }, learnState());
    const j = await act('ac/learn/save', body, null);
    if (j) {
      toast('Đã lưu lệnh: ' + j.state.desc);
      st.codesSlot = null;
    }
  });

  $('ctl-mode').addEventListener('change', () => { if (st.ac) renderControl(st.ac); });
  $('ctl-form').addEventListener('submit', async (ev) => {
    ev.preventDefault();
    const mode = +$('ctl-mode').value;
    const body = { mode: mode, fan: $('ctl-fan').value, swing: $('ctl-swing').value };
    if (mode === 3 || mode === 4 || mode === 8) body.sp = $('ctl-sp').value;
    if (await act('ac/control', body, 'Đã gửi tới thermostat')) st.dirty['ctl-form'] = false;
  });
  $('ctl-resend').addEventListener('click', () => act('ac/resend', {}, 'Sẽ phát lại trạng thái hiện tại'));

  /* ------------------------------------------------------------------ Thuật toán */
  loaders.algo = async (user) => {
    const j = await api('config', null, user);
    const c = j.config;
    const form = $('algo-form');
    const f = form.elements;
    if (!isDirty('algo-form')) {
      for (const r of form.querySelectorAll('input[name=control_mode]')) r.checked = +r.value === c.control_mode;
      f.deadband.value = c.deadband_centi / 100;
      f.max_offset.value = c.max_offset_centi / 100;
      f.comp_interval.value = Math.round(c.comp_interval_s / 60);
      f.filter_tau.value = c.filter_tau_s / 60;
      f.debounce.value = c.user_debounce_ms / 1000;
      f.stale_min.value = Math.round(c.stale_min_s / 60);
      f.allow_local_fallback.checked = c.allow_local_fallback;
      f.resend_on_boot.checked = c.resend_on_boot;
    }
    f.allow_local_fallback.disabled = !j.local_configured;
    const cl = j.climate;
    text('al-filtered', cl.filtered == null ? 'chưa có' : fmtTemp(cl.filtered, 2));
    text('al-target', cl.user_mode === 0 ? 'Tắt' : fmtTemp(cl.user_mode === 4 ? cl.heat_target : cl.cool_target));
    text('al-offset', cl.compensating ? signed(cl.offset) : '0 (không bù)');
    text('al-irsp', cl.desired.power ? (cl.desired.temp_half ? fmtHalf(cl.desired.temp_half) : '-') : 'Tắt');
    text('al-reason', cl.reason_text);
  };

  $('algo-form').addEventListener('submit', async (ev) => {
    ev.preventDefault();
    const form = ev.target;
    const f = form.elements;
    const mode = form.querySelector('input[name=control_mode]:checked');
    const body = {
      control_mode: mode ? mode.value : 0,
      deadband_centi: Math.round(+f.deadband.value * 100),
      max_offset_centi: Math.round(+f.max_offset.value * 100),
      comp_interval_s: Math.round(+f.comp_interval.value * 60),
      filter_tau_s: Math.round(+f.filter_tau.value * 60),
      user_debounce_ms: Math.round(+f.debounce.value * 1000),
      stale_min_s: Math.round(+f.stale_min.value * 60),
      allow_local_fallback: f.allow_local_fallback.checked ? 1 : 0,
      resend_on_boot: f.resend_on_boot.checked ? 1 : 0,
    };
    if (await act('config', body, 'Đã lưu thuật toán')) st.dirty['algo-form'] = false;
  });

  /* ------------------------------------------------------------------ Chẩn đoán */
  loaders.diag = async (user) => {
    const j = await api('diag', null, user);
    const fw = j.firmware, ch = j.chip, hi = j.heap_internal, hp = j.heap_psram, fb = j.fabric_b, m = j.matter, ir = j.ir;
    kv('dg-fw', [
      ['Dự án', fw.project], ['Phiên bản', fw.version], ['ESP-IDF', fw.idf], ['ESP-Matter', fw.esp_matter],
      ['Build', fw.date + ' ' + fw.time], ['Chế độ thử nghiệm', fw.test_mode ? 'BẬT' : 'tắt'],
      ['Thời gian chạy', fmtDur(j.uptime_s)], ['Lý do khởi động', j.reset_reason],
    ]);
    kv('dg-mem', [
      ['Chip', 'ESP32-S3 v' + Math.floor(ch.revision / 100) + '.' + (ch.revision % 100) + ', ' + ch.cores + ' nhân'],
      ['Flash', fmtBytes(ch.flash)], ['PSRAM', fmtBytes(ch.psram)], ['MAC Wi-Fi', ch.mac],
      ['RAM trong còn trống', fmtBytes(hi.free) + ' / ' + fmtBytes(hi.total)], ['RAM trong thấp nhất', fmtBytes(hi.min_free)],
      ['Khối RAM trong lớn nhất', fmtBytes(hi.largest)], ['PSRAM còn trống', fmtBytes(hp.free) + ' / ' + fmtBytes(hp.total)],
      ['PSRAM thấp nhất', fmtBytes(hp.min_free)],
      ['NVS chính (mục dùng/tổng)', j.nvs_main.used + ' / ' + j.nvs_main.total],
      ['NVS lệnh IR (mục dùng/tổng)', j.nvs_ir.used + ' / ' + j.nvs_ir.total],
    ]);
    kv('dg-matter', [
      ['Hệ sinh thái nhà', m.home_fabrics], ['Endpoint thermostat', m.endpoint], ['Tính năng Sưởi', m.heat ? 'có' : 'không'],
      ['VID / PID', hex(m.vendor_id) + ' / ' + hex(m.product_id)], ['Discriminator', m.discriminator],
      ['Fabric B', fb.ready ? 'sẵn sàng (fabric index ' + fb.fabric_index + ')' : 'lỗi: ' + (fb.init_error || 'chưa khởi tạo')],
      ['Fabric ID', fb.fabric_id], ['Compressed fabric ID', fb.compressed_fabric_id], ['Node ID của ESP', fb.controller_node_id],
      ['Số PAA tin cậy', fb.paa_count], ['Cổng UDP commissioner', fb.listen_port],
    ]);
    kv('dg-ir', [
      ['Sóng mang', ir.carrier_hz + ' Hz'], ['Đã phát', ir.sent], ['Lỗi', ir.failed], ['Trùng bị bỏ', ir.dropped_duplicate],
      ['Bị lệnh mới thay thế', ir.superseded], ['Lệnh cuối', ir.last_label], ['Lỗi cuối', ir.last_error || 'không'],
    ]);
    $('dg-errors').replaceChildren(
      ...(j.errors.length
        ? j.errors.map((e) => el('li', null, el('span', null, '[' + e.module + '] ' + e.msg), el('span', { class: 'muted small' }, fmtAge(e.age_s))))
        : [el('li', { class: 'muted' }, 'Không có lỗi')]));
  };

  async function reboot() {
    if (!confirm('Khởi động lại thiết bị? Cấu hình đã lưu được giữ nguyên.')) return;
    if (await act('system/reboot', {}, null)) {
      toast('Đang khởi động lại. Wi-Fi cấu hình sẽ mở lại sau khoảng 15 giây; kết nối lại rồi tải lại trang.');
    }
  }
  $('dg-reboot').addEventListener('click', reboot);
  $('ac-restart-btn').addEventListener('click', reboot);

  /* ------------------------------------------------------------------ header */
  $('btn-extend').addEventListener('click', () => act('portal/extend', {}, 'Đã gia hạn thời gian cấu hình'));
  $('btn-exit').addEventListener('click', async () => {
    if (!confirm('Thoát cấu hình và tắt Wi-Fi cấu hình? Thiết bị tiếp tục chạy qua Wi-Fi nhà.')) return;
    if (await act('portal/close', {}, 'Đã thoát cấu hình')) {
      closed = true;
      clearTimeout(timer);
      show('banner-closed', true);
      show('banner-offline', false);
      text('portal-left', '');
    }
  });

  /* ------------------------------------------------------------------ init */
  function initStatic() {
    fillSelect($('lf-mode'), AC_MODES.map((n, i) => [i, n]), 0);
    fillSelect($('lf-temp'), learnTempOptions(2), 50);
    for (const s of document.querySelectorAll('select.sel-fan')) fillSelect(s, AC_FANS.map((n, i) => [i, n]), 0);
    for (const s of document.querySelectorAll('select.sel-swing')) fillSelect(s, SWINGS.map((n, i) => [i, n]), 0);
    for (const b of document.querySelectorAll('#tabs button')) b.addEventListener('click', () => go(b.dataset.page));
    ['ace-form', 'ctl-form', 'algo-form'].forEach(trackDirty);
    updateLearnForm();
  }

  async function init() {
    initStatic();
    try {
      await getToken();
    } catch (e) {
      setOffline(true);
    }
    const h = location.hash.slice(1);
    go(loaders[h] ? h : 'overview');
  }

  init();
})();
