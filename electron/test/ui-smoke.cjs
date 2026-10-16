'use strict';

// Actual Electron + actual host, with an isolated empty profile. No audio actions.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { createRequire } = require('node:module');
function automation() {
  try { return require('playwright'); }
  catch {
    const runtime = process.env.NOISEKILLER_PLAYWRIGHT_ROOT;
    if (!runtime) throw new Error('Install Playwright for UI testing, or set NOISEKILLER_PLAYWRIGHT_ROOT to an existing node_modules directory.');
    return createRequire(path.join(runtime, '__resolver.cjs'))('playwright');
  }
}
const root = path.resolve(__dirname, '..', '..');
const output = path.join(root, 'build', 'electron-ui-qa');
fs.mkdirSync(output, { recursive: true });
const isolated = fs.mkdtempSync(path.join(root, 'build', 'electron-ui-user-'));
const packaged = process.argv.includes('--packaged');
const env = { ...process.env, LOCALAPPDATA: isolated };
delete env.ELECTRON_RUN_AS_NODE;
let desktop;

(async () => {
  desktop = await automation()._electron.launch({
    executablePath: packaged ? path.join(root, 'dist', 'NoiseKiller-Desktop-0.3.0-preview', 'NoiseKiller.exe') : require('electron'),
    args: [...(packaged ? [] : [path.resolve(__dirname, '..')]), '--smoke-ui', `--user-data-dir=${path.join(isolated, 'electron-profile')}`],
    cwd: path.resolve(__dirname, '..'), env, timeout: 30000,
  });
  const page = await desktop.firstWindow();
  const errors = [];
  page.on('pageerror', (error) => errors.push(error.message));
  await page.waitForFunction(() => window.noisekiller && document.querySelector('#refresh-button')?.disabled === false, null, { timeout: 15000 });
  const initial = await page.evaluate(() => window.noisekiller.bootstrap());
  assert.equal(initial.backendAvailable, true, initial.error);
  assert.equal(initial.status.session.state, 'stopped');
  assert.equal(initial.status.session.capturedPackets, 0);
  assert.equal(initial.status.session.processedHops, 0);
  assert.equal(initial.status.profile.registered, false);
  assert.equal(await page.locator('#input-device').inputValue(), '');
  assert.equal(await page.locator('#output-device').inputValue(), '');
  assert.equal(await page.locator('#preview-notice').isVisible(), false);
  assert.equal(await page.locator('#start-button').isDisabled(), true);
  assert.equal(await page.evaluate(() => typeof window.require), 'undefined');
  const preferences = await desktop.evaluate(({ BrowserWindow }) => BrowserWindow.getAllWindows()[0].webContents.getLastWebPreferences());
  assert.equal(preferences.sandbox, true);
  assert.equal(preferences.contextIsolation, true);
  assert.equal(preferences.nodeIntegration, false);

  if (await page.locator('html').getAttribute('data-theme') === 'dark') await page.locator('[data-action="theme"]').first().click();
  await page.screenshot({ path: path.join(output, 'desktop-light.png'), fullPage: true });
  for (const route of ['profile', 'diagnostics', 'settings', 'workspace']) {
    await page.locator(`.nav-item[data-route="${route}"]`).click();
    assert.equal(await page.locator(`#panel-${route}`).isVisible(), true);
    // Routing changes the fragment; the trusted-frame check must still work.
    const observed = await page.evaluate(() => window.noisekiller.bootstrap());
    assert.equal(observed.backendAvailable, true);
    assert.equal(observed.status.session.capturedPackets, 0);
  }
  await page.locator('#refresh-button').click();
  await page.waitForFunction(() => !document.querySelector('#refresh-button').disabled);
  assert.equal(await page.locator('#error-banner').isVisible(), false);
  await page.locator('[data-action="theme"]').first().click();
  assert.equal(await page.locator('html').getAttribute('data-theme'), 'dark');
  await page.screenshot({ path: path.join(output, 'desktop-dark.png'), fullPage: true });
  await page.locator('[data-action="theme"]').first().click();
  const state = await page.evaluate(() => window.noisekiller.bootstrap());
  assert.equal(state.status.session.capturedPackets, 0);
  assert.equal(state.status.session.state, 'stopped');
  assert.equal(fs.existsSync(path.join(isolated, 'NoiseKiller', 'active-profile.nkp')), false);
  assert.deepEqual(errors, []);
  await desktop.close(); desktop = null;
  console.log(JSON.stringify({ passed: true, packaged, electron: require('electron/package.json').version, noCapture: true, noPlayback: true, routes: 4, errors, screenshots: output }, null, 2));
})().catch(async (error) => {
  console.error(error.stack);
  if (desktop) await desktop.close();
  process.exitCode = 1;
});
