import assert from 'node:assert/strict';
import {createRequire} from 'node:module';
import {fileURLToPath} from 'node:url';
import {mkdir, writeFile} from 'node:fs/promises';
import {resolve} from 'node:path';

const root = fileURLToPath(new URL('..', import.meta.url));
const dawRoot = process.env.DAW_ROOT ?? resolve(root, '../../wclap-browser-daw');
const {chromium} = createRequire(`${dawRoot}/package.json`)('playwright');
const artifacts = resolve(root, 'build-native/browser-test');
await mkdir(artifacts, {recursive: true});
const browser = await chromium.launch({channel: 'chrome', headless: true,
  args: ['--autoplay-policy=no-user-gesture-required']});
const page = await browser.newPage({viewport: {width: 1440, height: 960}});
const errors = [];
page.on('pageerror', e => errors.push(e.message));
try {
  await page.goto(process.env.DAW_URL ?? 'http://127.0.0.1:8470/');
  await page.locator('html[data-boot="ready"]').waitFor();
  await page.getByRole('button', {name: /^Start audio/}).click();
  await page.locator('body[data-engine="ready"]').waitFor({timeout: 30000});
  await page.getByRole('button', {name: 'Plugins', exact: true}).click();
  await page.locator('#plugin-file').setInputFiles(resolve(root, 'build-wclap/artifacts/Mote.wclap.tar.gz'));
  await page.locator('#plugin-inspection-status').filter({hasText: 'Added 1 plug-in'}).waitFor({timeout: 30000});
  await page.locator('#plugin-tree .plugin-row').filter({hasText: 'Mote'}).dblclick();
  const ui = page.frameLocator('iframe[title="Mote interface"]');
  await ui.getByRole('slider', {name: 'rate', exact: true}).waitFor({timeout: 30000});
  await page.screenshot({path: `${artifacts}/loaded.png`});
  const frame = page.frames().find(f => f.url().includes('/_wclap/resource/'));
  assert(frame, 'Mote resource frame was not created');
  await frame.evaluate(() => {
    window.moteTest = {messages: [], latest: null};
    addEventListener('message', ({data}) => {
      if (!(data instanceof ArrayBuffer)) return;
      const text = new TextDecoder().decode(data);
      if (!text.startsWith('status:')) return;
      const p = text.split(':');
      const snapshot = {at: performance.now(), clock: +p[1], bpm: +p[2], held: +p[3], tick: +p[4], key: +p[6], emitted: +p[7]};
      window.moteTest.latest = snapshot;
      window.moteTest.messages.push(snapshot);
    });
  });
  const meters = () => page.locator('daw-session-channel').evaluateAll(els => els.map(e => ({
    name: e.getAttribute('label'), levels: e._levels,
  })));
  const outputCount = () => frame.evaluate(() => window.moteTest.latest?.emitted ?? 0);
  const chord = async () => {
    await page.locator('#host-keyboard').focus();
    for (const key of ['a', 'd', 'g']) await page.keyboard.down(key);
    await frame.waitForFunction(() => window.moteTest.latest?.held === 3);
    for (const key of ['a', 'd', 'g']) await page.keyboard.up(key);
  };
  await ui.getByRole('button', {name: 'Hold notes', exact: true}).click();
  await page.getByRole('button', {name: 'Keyboard', exact: true}).click();
  await chord();
  assert.equal(await outputCount(), 0, 'Generated notes with transport stopped');
  assert.equal(await ui.locator('#hint').innerText(), 'start transport');
  console.log('INPUT', await ui.locator('#notes').innerText());
  await page.getByRole('button', {name: 'Play', exact: true}).click();
  await frame.waitForFunction(() => window.moteTest.latest?.emitted >= 6);
  await page.waitForFunction(() => [...document.querySelectorAll('daw-session-channel')]
    .some(e => e._levels?.some(n => n > -60)));
  console.log('OUTPUT', await frame.evaluate(() => window.moteTest.latest), 'METERS', await meters());
  await page.screenshot({path: `${artifacts}/playing.png`});
  await page.locator('iframe[title="Mote interface"]').screenshot({path: `${artifacts}/mote.png`});

  await page.getByRole('spinbutton', {name: 'Tempo', exact: true}).press('Enter');
  await page.locator('#bpmBox input').fill('90.5');
  await page.locator('#bpmBox input').press('Enter');
  await frame.waitForFunction(() => window.moteTest.latest?.bpm === 90.5);
  console.log('TEMPO', await ui.locator('#tempo').innerText());

  const observedOrders = {};
  for (const [name, expected] of [
    ['down', [55, 52, 48, 55, 52, 48]],
    ['up/down', [48, 52, 55, 52, 48, 52]],
    ['up', [48, 52, 55, 48, 52, 55]],
  ]) {
    await ui.getByRole('button', {name, exact: true}).click();
    const before = await outputCount();
    if (name === 'down') {
      await frame.waitForFunction(before => window.moteTest.latest?.emitted > before
        && window.moteTest.latest?.key >= 0, before);
      await ui.locator('.note.active').waitFor();
    }
    await frame.waitForFunction(before => window.moteTest.latest?.emitted >= before + 6, before);
    const keys = await frame.evaluate(before => {
      const seen = new Set();
      return window.moteTest.messages.filter(m => m.emitted > before && m.key >= 0
        && !seen.has(m.emitted) && seen.add(m.emitted)).map(m => m.key).slice(0, 6);
    }, before);
    assert.deepEqual(keys, expected, `${name} output order`);
    observedOrders[name] = keys;
    console.log('ORDER', name, keys);
  }
  const speed = ui.getByRole('slider', {name: 'rate', exact: true});
  await speed.focus();
  await page.keyboard.press('Home');
  await ui.locator('compost-slider[aria-valuenow="0"]').waitFor();
  const rateStart = await outputCount();
  await frame.waitForFunction(n => window.moteTest.latest?.emitted >= n + 4, rateStart);
  const intervals = await frame.evaluate(n => {
    const seen = new Set();
    const onsets = window.moteTest.messages.filter(m => m.emitted > n && m.key >= 0
      && !seen.has(m.emitted) && seen.add(m.emitted));
    return onsets.slice(1).map((m, i) => m.at - onsets[i].at);
  }, rateStart);
  assert(intervals.length >= 3 && intervals.every(ms => ms > 250 && ms < 1200), 'Rate change stopped output');
  console.log('RATE', intervals);

  const octaveControl = ui.getByRole('slider', {name: 'octaves', exact: true});
  await octaveControl.focus();
  await page.keyboard.press('ArrowRight');
  await ui.locator('compost-slider[parameter-id="5"][aria-valuenow="2"]').waitFor();
  const octaveStart = await outputCount();
  await frame.waitForFunction(n => window.moteTest.latest?.emitted >= n + 6, octaveStart);
  const octaveKeys = await frame.evaluate(before => {
    const seen = new Set();
    return window.moteTest.messages.filter(m => m.emitted > before && m.key >= 0
      && !seen.has(m.emitted) && seen.add(m.emitted)).map(m => m.key).slice(0, 6);
  }, octaveStart);
  const octaveCycle = [48, 52, 55, 60, 64, 67];
  const octavePhase = octaveCycle.indexOf(octaveKeys[0]);
  assert(octavePhase >= 0 && octaveKeys.every((key, i) => key === octaveCycle[(octavePhase + i) % octaveCycle.length]),
    'Two-octave output');
  const gateControl = ui.getByRole('slider', {name: 'gate', exact: true});
  await gateControl.focus();
  await page.keyboard.press('Home');
  await ui.locator('compost-slider[parameter-id="6"][aria-valuenow="5"]').waitFor();

  await ui.getByRole('button', {name: 'Hold notes', exact: true}).click();
  await frame.waitForFunction(() => window.moteTest.latest?.held === 0);
  await ui.getByRole('button', {name: 'Hold notes', exact: true}).click();
  await chord();
  const restartCount = await outputCount();
  await frame.waitForFunction(n => window.moteTest.latest?.emitted > n, restartCount);
  await page.getByRole('button', {name: 'Stop', exact: true}).click();
  const stopCount = await outputCount();
  await page.waitForTimeout(600);
  const hostStopped = await frame.evaluate(() => window.moteTest.latest?.clock !== 2);
  const hostResumedAfterStop = await outputCount() !== stopCount;
  const stopTrace = await frame.evaluate(() => window.moteTest.messages.slice(-12));
  await ui.getByRole('button', {name: 'Clear held notes', exact: true}).click();
  await frame.waitForFunction(() => window.moteTest.latest?.held === 0);
  const clearCount = await outputCount();
  await page.waitForTimeout(400);
  assert.equal(await outputCount(), clearCount, 'Clear still generated notes');
  const win = page.locator('compost-window').filter({has: page.locator('iframe[title="Mote interface"]')});
  await win.evaluate(w => w.setContentSize(320, 260));
  assert(await ui.locator('main').evaluate(e => e.scrollWidth <= innerWidth && e.scrollHeight <= innerHeight), 'Compact UI overflows');
  await page.locator('iframe[title="Mote interface"]').screenshot({path: `${artifacts}/compact.png`});
  const report = {verified: ['MIDI input', 'downstream audio meters', 'three note orders', 'BPM updates',
    'rate change', 'octave range', 'gate control', 'latch release', 'clear', '320 × 260 UI'],
    observedOrders, intervals, octaveKeys, hostStopped, hostResumedAfterStop, stopTrace, errors};
  await writeFile(`${artifacts}/report.json`, JSON.stringify(report, null, 2));
  console.log('VERIFIED', report.verified.join(', '));
  assert.equal(errors.length, 0, errors.join('\n'));
} catch (e) {
  await page.screenshot({path: `${artifacts}/failure.png`}).catch(() => {});
  console.error('ERRORS', errors);
  console.error((await page.locator('body').innerText()).slice(-7000));
  throw e;
} finally { await browser.close(); }
