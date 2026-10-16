'use strict';

// Real native-host integration, using only generated files and read-only inventory.
// Never issues a record command or a valid start command.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { NativeBridge } = require('../bridge.cjs');

const root = path.resolve(__dirname, '..', '..');
const executable = path.join(root, 'build', 'Release', 'NoiseKiller.Host.exe');
fs.mkdirSync(path.join(root, 'build'), { recursive: true });
const folder = fs.mkdtempSync(path.join(root, 'build', 'electron-host-test-'));
const profilePath = path.join(folder, '本人测试档案.nkp');
const bridge = new NativeBridge({ executable, profilePath });
let assertions = 0;
const check = (condition, message) => { assert.ok(condition, message); assertions++; };

async function completeJob() {
  const deadline = Date.now() + 15000;
  let state;
  do {
    state = await bridge.request('status');
    if (!state.job.busy) return state;
    await new Promise((resolve) => setTimeout(resolve, 25));
  } while (Date.now() < deadline);
  throw new Error('Native synthetic job failed to complete');
}

function eofTest() {
  return new Promise((resolve, reject) => {
    const child = spawn(executable, ['--profile', path.join(folder, 'unused.nkp')], { shell: false, windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'] });
    const timer = setTimeout(() => { child.kill(); reject(new Error('EOF did not terminate the native host')); }, 5000);
    child.on('error', reject);
    child.on('exit', (code) => { clearTimeout(timer); try { check(code === 0, 'clean exit on stdin EOF'); resolve(); } catch (error) { reject(error); } });
    child.stdout.resume(); child.stderr.resume(); child.stdin.end();
  });
}

(async () => {
  await bridge.start();
  let state = await bridge.request('status');
  check(state.session.state === 'stopped', 'startup does not capture');
  check(!state.profile.registered && !fs.existsSync(profilePath), 'startup does not create a profile');
  check(state.session.processedHops === 0 && state.session.capturedPackets === 0, 'startup has zero real audio frames');
  const inventory = await bridge.request('devices');
  check(Array.isArray(inventory.inputs) && Array.isArray(inventory.outputs), 'read-only device inventory');
  state = await bridge.request('status');
  check(state.session.state === 'stopped' && state.session.capturedPackets === 0, 'inventory does not activate audio');
  await assert.rejects(bridge.request('start', ['', '', 'filter'])); assertions++;
  check((await bridge.request('status')).session.state === 'stopped', 'invalid route cannot activate audio');

  const reference = path.join(root, 'build', 'test-artifacts', 'synthetic-reference.wav');
  const mixture = path.join(root, 'build', 'test-artifacts', 'synthetic-mixture.wav');
  if (!fs.existsSync(reference) || !fs.existsSync(mixture))
    throw new Error('Run build/Release/noisekiller_tests.exe from build/ first to generate synthetic fixtures');
  await bridge.request('enrollFile', [reference]);
  state = await completeJob();
  check(!state.job.error && state.profile.registered, `synthetic enrollment: ${state.job.error}`);
  check(fs.existsSync(profilePath) && state.profile.prototypeCount > 0, 'encrypted private test profile exists');
  check(state.session.state === 'stopped' && state.session.capturedPackets === 0, 'enrollment from file never captures');

  const resultPath = path.join(folder, '合成降噪结果.wav');
  await bridge.request('processFile', [mixture, resultPath]);
  state = await completeJob();
  check(!state.job.error && fs.existsSync(resultPath), `synthetic offline processing: ${state.job.error}`);
  check(fs.readFileSync(resultPath).subarray(0, 4).toString('ascii') === 'RIFF', 'valid WAV output');
  const original = fs.readFileSync(resultPath);
  await assert.rejects(bridge.request('processFile', [mixture, resultPath])); assertions++;
  check(fs.readFileSync(resultPath).equals(original), 'existing output is never overwritten');
  check(!fs.readdirSync(folder).some((file) => file.includes('.tmp.')), 'no abandoned temporary outputs');

  // Test cancel/stop on a longer synthetic input without opening an audio stream.
  await bridge.request('processFile', [mixture, path.join(folder, 'cancelled.wav')]);
  await bridge.request('stop');
  state = await completeJob();
  check(state.session.state === 'stopped' && state.session.capturedPackets === 0, 'stop/cancel cannot leave an audio stream');
  state = await bridge.request('deleteProfile');
  check(!state.profile.registered && !fs.existsSync(profilePath), 'explicit delete affects only the isolated test profile');
  await bridge.close();
  check(bridge.closed, 'graceful shutdown terminates the host');
  await eofTest();
  console.log(JSON.stringify({ passed: assertions, noCapture: true, noPlayback: true, folder }, null, 2));
})().catch(async (error) => {
  await bridge.close(); console.error(error.stack); process.exitCode = 1;
});
