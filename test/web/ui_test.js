// Headless browser test of the portal UI against test/web/mock_server.py.
// Usage: node test/web/ui_test.js http://127.0.0.1:8080/ <screenshot-dir>
// Fails on any JavaScript error, failed check or unexpected HTML injection.
const { chromium } = require('playwright');

const base = process.argv[2] || 'http://127.0.0.1:8080/';
const shots = process.argv[3] || null;

function check(cond, what) {
  if (!cond) throw new Error('check failed: ' + what);
  console.log('  ok  ' + what);
}

/* Minimal application image as the device checks it: esp_image_header_t magic, esp_app_desc_t magic,
 * version and project name; padded to a plausible size. */
function fwImage(project, version) {
  const b = Buffer.alloc(200 * 1024, 0xff);
  b[0] = 0xe9;
  b.writeUInt32LE(0xabcd5432, 32);
  b.fill(0, 36, 112);
  b.write(version, 48, 'ascii');
  b.write(project, 80, 'ascii');
  return b;
}

(async () => {
  const browser = await chromium.launch();
  const page = await browser.newPage({ viewport: { width: 390, height: 844 }, deviceScaleFactor: 2, locale: 'vi-VN' });
  const errors = [];
  page.on('pageerror', (e) => errors.push('pageerror: ' + e.message));
  // 4xx answers are provoked on purpose (invalid input must be rejected); everything else fails.
  page.on('console', (m) => {
    if (m.type() === 'error' && !/Failed to load resource: the server responded with a status of 4\d\d/.test(m.text())) {
      errors.push('console: ' + m.text());
    }
  });
  page.on('dialog', (d) => d.accept());
  const txt = (sel) => page.locator(sel).innerText();
  const shot = async (name) => { if (shots) await page.screenshot({ path: shots + '/' + name + '.png', fullPage: true }); };

  try {
    await page.goto(base);
    await page.waitForFunction(() => document.getElementById('ov-temp').textContent !== '--');
    check((await txt('#dev-name')) === 'May lanh IR v1.3.0', 'versioned device name from /api/session');
    check(await page.locator('#banner-test').isVisible(), 'test-mode banner shown');
    check((await txt('#ov-temp')) === 'Không khả dụng', 'no sensor -> temperature unavailable (not invented)');
    check((await txt('#portal-left')).startsWith('Tự tắt sau'), 'portal countdown');
    await shot('01-overview');

    // ---------------------------------------------------------------- Wi-Fi
    await page.click('#tabs button[data-page=wifi]');
    await page.click('#wf-scan');
    await page.waitForFunction(() => document.querySelectorAll('#wf-list li').length === 3, null, { timeout: 8000 });
    check((await page.locator('#wf-list b').count()) === 0, 'SSID with HTML is rendered as text');
    check((await txt('#wf-list li:nth-child(2)')).includes('<b>5G</b>'), 'SSID text kept verbatim');
    await page.click('#wf-list li:nth-child(1)');
    check((await page.inputValue('#wf-in-ssid')) === 'NhaToi', 'click on a scan result fills the SSID');
    await page.fill('#wf-in-pass', 'short');
    await page.click('#wf-form button[type=submit]');
    await page.waitForSelector('#toast.error');
    check((await txt('#toast')).includes('8-63'), 'short password rejected in the UI');
    await page.fill('#wf-in-pass', 'matkhau-dai-123');
    await page.click('#wf-form button[type=submit]');
    await page.waitForFunction(() => document.getElementById('wf-job').textContent.includes('Đã kết nối'), null, { timeout: 9000 });
    check(true, 'station change job reaches "connected"');
    await shot('02-wifi');

    // ---------------------------------------------------------------- Home
    await page.click('#tabs button[data-page=home]');
    await page.waitForFunction(() => document.querySelectorAll('#hm-minutes option').length > 0);
    await page.click('#hm-show');
    await page.waitForSelector('#hm-qr svg path');
    check((await txt('#hm-manual')) === '3497-011-2332', 'manual code formatted');
    check((await txt('#hm-product')) === 'May lanh IR v1.3.0', 'Matter product name with version suffix');
    check((await txt('#hm-ble')).startsWith('sẵn sàng'), 'BLE ready while the window is closed');
    await page.click('#hm-open');
    await page.waitForFunction(() => document.getElementById('hm-window').textContent.startsWith('Đang bật'));
    await page.waitForFunction(() => document.getElementById('hm-ble').textContent.includes('AC-1A2B-v1.3.0'));
    check((await txt('#hm-ble')).startsWith('đang phát'), 'BLE advertising while the window is open');
    check((await page.locator('#hm-print').isVisible()), 'printable pairing label offered');
    check(true, 'commissioning window opened');
    check((await page.locator('#hm-fabrics li').count()) === 2, 'two home ecosystems listed');
    await page.click('#hm-fabrics li:nth-child(2) button');
    await page.waitForFunction(() => document.querySelectorAll('#hm-fabrics li').length === 1);
    check(!(await txt('#hm-fabrics')).includes('Nhà cũ'), 'one ecosystem removed on the device side');
    check(await page.locator('#hm-remove-all').isVisible(), '"leave all" offered while an ecosystem is left');
    await shot('03-home');

    // ---------------------------------------------------------------- Sensor
    await page.click('#tabs button[data-page=sensor]');
    await page.fill('#pair-code', '12');
    await page.click('#pair-form button[type=submit]');
    await page.waitForSelector('#toast.error');
    check((await txt('#toast')).includes('không hợp lệ'), 'invalid share code rejected');
    await page.fill('#pair-code', '3497-011-2332');
    await page.click('#pair-form button[type=submit]');
    await page.waitForFunction(() => document.getElementById('pg-state').textContent === 'Hoàn tất', null, { timeout: 12000 });
    check((await page.locator('#pg-endpoints input[type=radio]').count()) === 2, 'two endpoints listed');
    check(await page.locator('#pg-endpoints input[value="2"]').isDisabled() === false, 'null endpoint still listed');
    check((await txt('#pg-endpoints')).includes('26,51 °C'), 'endpoint value in 0.01 C');
    await shot('04-sensor-paired');
    await page.click('#pg-bind');
    await page.waitForSelector('#sn-info:not([hidden])');
    check((await txt('#sn-state')).includes('Đã subscribe'), 'sensor bound and subscribed');
    await shot('05-sensor-bound');

    // ---------------------------------------------------------------- AC
    await page.click('#tabs button[data-page=ac]');
    await page.waitForFunction(() => document.querySelectorAll('#ac-profiles tr').length === 4);
    await page.waitForFunction(() => document.getElementById('ace-count').textContent === '4');
    check(await page.locator('#learn-card').isVisible(), 'learning available on an unlocked profile');
    await page.selectOption('#lf-mode', '0');
    await page.selectOption('#lf-temp', '52');
    await page.click('#learn-start');
    await page.waitForSelector('#learn-result:not([hidden])', { timeout: 8000 });
    await page.click('#learn-test');
    await page.waitForSelector('#toast:not(.error)');
    await page.click('#learn-save');
    await page.waitForFunction(() => document.getElementById('ace-count').textContent === '5', null, { timeout: 8000 });
    check(true, 'learned code saved (5 codes)');
    const first = await txt('#ace-codes li:first-child');
    check(first.startsWith('Tắt'), 'codes sorted, Off first');
    await page.click('#ace-lock');
    await page.waitForFunction(() => document.getElementById('ace-lockinfo').textContent.startsWith('Đã khóa'));
    check(!(await page.locator('#learn-card').isVisible()), 'learning hidden once locked');
    check((await page.locator('#ace-codes button.danger').count()) === 0, 'no delete buttons on a locked profile');
    await shot('06-ac');
    await page.selectOption('#ctl-mode', '3');
    await page.click('#ctl-form button[type=submit]');
    await page.waitForSelector('#toast:not(.error)');
    check(true, 'manual control sent through the thermostat');
    await page.waitForFunction(() => document.getElementById('ctl-remote').textContent.startsWith('Lạnh 26'));
    check((await txt('#ctl-remote')).includes('bỏ qua lệnh cuối: giao thức NEC'), 'original remote state and last ignored command shown');

    // ---------------------------------------------------------------- Remote recognition (IR library)
    await page.click('#det-start');
    await page.waitForSelector('#det-result:not([hidden])', { timeout: 12000 });
    check((await txt('#det-proto')).startsWith('DAIKIN (280 bit)'), 'remote recognised as DAIKIN');
    check((await txt('#det-state')).startsWith('Lạnh 25'), 'decoded state shown');
    check(!(await page.locator('#det-model-wrap').isVisible()), 'no model choice for a single-model protocol');
    check((await page.locator('#det-t-fan option').count()) === 4, 'test fan list = Auto + 3 levels');
    await page.selectOption('#det-levels', '5');
    check((await page.locator('#det-t-fan option').count()) === 6, 'fan list follows the number of levels');
    await page.click('#det-test');
    await page.waitForFunction(() => document.getElementById('toast').textContent.startsWith('Đã phát thử'));
    check(true, 'library test command sent');
    await page.fill('#det-name', 'Daikin phòng khách');
    await page.click('#det-save');
    await page.waitForFunction(() => document.getElementById('ac-profiles').textContent.includes('thư viện DAIKIN'));
    check(true, 'library profile saved and listed');
    await page.waitForSelector('#ace-lib:not([hidden])');
    check((await txt('#ace-lib')).includes('5 mức quạt'), 'library settings shown on the profile');
    check(!(await page.locator('#learn-card').isVisible()), 'no learning on a library profile');
    await shot('06b-detect');

    // ---------------------------------------------------------------- Algorithm
    await page.click('#tabs button[data-page=algo]');
    await page.waitForFunction(() => document.querySelector('#algo-form input[name=deadband]').value !== '');
    check((await page.inputValue('#algo-form input[name=deadband]')) === '0.5', 'deadband shown in C');
    check(await page.isChecked('#algo-form input[name=remote_sync]'), 'remote sync on by default');
    await page.fill('#algo-form input[name=deadband]', '0.6');
    await page.uncheck('#algo-form input[name=remote_sync]');
    await page.click('#algo-form button[type=submit]');
    await page.waitForFunction(() => document.getElementById('toast').textContent.includes('Đã lưu thuật toán'));
    check(true, 'algorithm settings saved');
    check(!(await page.isChecked('#algo-form input[name=remote_sync]')), 'remote sync switched off');
    await page.click('#tabs button[data-page=ac]');
    await page.waitForFunction(() => document.getElementById('ctl-remote').textContent.startsWith('không theo dõi'), null, { timeout: 8000 });
    check(true, 'control card shows that the remote is not followed');
    await page.click('#tabs button[data-page=algo]');
    await shot('07-algo');

    // ---------------------------------------------------------------- Diagnostics
    await page.click('#tabs button[data-page=diag]');
    await page.waitForFunction(() => document.querySelectorAll('#dg-errors li').length === 1);
    check((await txt('#dg-fw')).includes('1.6.1'), 'esp_matter version shown');
    check((await txt('#dg-matter')).includes('5542'), 'commissioner UDP port shown');
    check((await txt('#dg-ota')).includes('ota_1'), 'update slot shown');
    // firmware update: refused files (nothing changes on the device)
    await page.click('#ota-start');
    check((await txt('#ota-msg')).startsWith('Hãy chọn file'), 'update without a file refused');
    await page.setInputFiles('#ota-file', { name: 'ac_matter_ir_v9.9.9_full_0x0.bin', mimeType: 'application/octet-stream', buffer: fwImage('ac_matter_ir', '9.9.9') });
    await page.click('#ota-start');
    check((await txt('#ota-msg')).startsWith('Đây là file full'), 'full image refused before uploading');
    await page.setInputFiles('#ota-file', { name: 'other_app_0x40000.bin', mimeType: 'application/octet-stream', buffer: fwImage('other_project', '1.0.0') });
    await page.click('#ota-start');
    await page.waitForFunction(() => document.getElementById('ota-msg').textContent.includes('không phải firmware của thiết bị này'));
    check(await page.locator('#ota-start').isEnabled(), 'firmware of another project refused by the device, page usable again');
    await shot('08-diag');

    // ---------------------------------------------------------------- Overview after binding
    await page.click('#tabs button[data-page=overview]');
    await page.waitForFunction(() => document.getElementById('ov-temp').textContent.includes('26,5'));
    check(true, 'overview shows the external temperature');

    // ---------------------------------------------------------------- Exit
    await page.click('#btn-exit');
    await page.waitForSelector('#banner-closed:not([hidden])');
    check(true, 'exit closes the portal');
    await shot('09-exit');

    // ---------------------------------------------------------------- Leave all homes, update, factory reset
    // (each of these ends the page's polling, so the page is loaded again in between)
    await page.goto(base);
    await page.click('#tabs button[data-page=home]');
    await page.waitForFunction(() => document.querySelectorAll('#hm-fabrics li button').length === 1);
    await page.click('#hm-remove-all');
    await page.waitForFunction(() => document.getElementById('hm-fabrics').textContent.includes('Chưa thuộc hệ sinh thái nào'));
    check(!(await page.locator('#hm-remove-all').isVisible()), 'left all home ecosystems');

    await page.click('#tabs button[data-page=diag]');
    await page.waitForFunction(() => document.getElementById('dg-ota').textContent.includes('ota_1'));
    await page.setInputFiles('#ota-file', { name: 'ac_matter_ir_v9.9.9_app_0x40000.bin', mimeType: 'application/octet-stream', buffer: fwImage('ac_matter_ir', '9.9.9') });
    await page.click('#ota-start');
    await page.waitForFunction(() => document.getElementById('ota-msg').textContent.startsWith('Đã ghi firmware 9.9.9'), null, { timeout: 15000 });
    check((await txt('#ota-msg')).includes('v9.9.9'), 'firmware uploaded, new version and new Wi-Fi name announced');
    await shot('10-update');

    await page.goto(base);
    await page.click('#tabs button[data-page=diag]');
    await page.waitForFunction(() => document.getElementById('dg-fw').textContent.includes('9.9.9'));
    check(true, 'device reports the new firmware version');
    await page.click('#dg-freset'); /* two confirmations (a sensor is paired): accepted by the dialog handler */
    await page.waitForFunction(() => document.getElementById('toast').textContent.startsWith('Đang đặt lại gốc'));
    check(true, 'factory reset requested (with a paired sensor: second confirmation)');
  } catch (e) {
    errors.push(e.message);
  }
  await browser.close();
  if (errors.length) {
    console.error('UI TEST FAILED\n' + errors.join('\n'));
    process.exit(1);
  }
  console.log('UI TEST PASSED');
})();
