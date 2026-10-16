'use strict';

// Control-plane contract tests: no executable, microphone, or speaker is opened.
const test = require('node:test');
const assert = require('node:assert/strict');
const path = require('node:path');
const { EventEmitter } = require('node:events');
const { PassThrough } = require('node:stream');
const { NativeBridge, COMMANDS } = require('../bridge.cjs');

const executable = path.resolve('build', 'test-only-native-host.exe');
const profilePath = path.resolve('build', 'test-only-本人档案.nkp');
const tick = () => new Promise((resolve) => setImmediate(resolve));

function mockHost({ autoShutdown = true, spawnError } = {}) {
  const child = new EventEmitter();
  Object.assign(child, {
    stdin: new PassThrough(), stdout: new PassThrough(), stderr: new PassThrough(),
    pid: 424242, exitCode: null, killed: false, kills: [],
  });
  const calls = [], requests = [], faults = [];
  let input = '', exited = false;
  const finish = (code = 0, signal = null) => {
    if (exited) return;
    exited = true;
    child.exitCode = code;
    child.emit('exit', code, signal);
  };
  child.kill = (signal) => {
    child.killed = true;
    child.kills.push(signal);
    queueMicrotask(() => finish(null, signal || 'SIGTERM'));
    return true;
  };
  const reply = (id, result = {}, ok = true) => {
    const message = ok ? { id, ok, result } : { id, ok, error: result };
    child.stdout.write(JSON.stringify(message) + '\n');
  };
  child.stdin.setEncoding('utf8');
  child.stdin.on('data', (chunk) => {
    input += chunk;
    let end;
    while ((end = input.indexOf('\n')) !== -1) {
      const line = input.slice(0, end);
      input = input.slice(end + 1);
      const [protocol, id, verb, ...encoded] = line.split('\t');
      requests.push({ line, protocol, id, verb, encoded,
        args: encoded.map((value) => Buffer.from(value, 'base64').toString('utf8')) });
      if (verb === 'shutdown' && autoShutdown) queueMicrotask(() => {
        reply(id, { stopped: true });
        queueMicrotask(() => finish());
      });
    }
  });
  const spawn = (...args) => {
    calls.push(args);
    if (spawnError) throw spawnError;
    queueMicrotask(() => child.emit('spawn'));
    return child;
  };
  const bridge = new NativeBridge({ executable, profilePath, spawn });
  bridge.on('fault', (error) => faults.push(error));
  return { bridge, child, calls, requests, faults, reply, finish };
}

async function started(t, options) {
  const host = mockHost(options);
  await host.bridge.start();
  t.after(() => host.bridge.close());
  return host;
}

test('constructor requires absolute native executable and profile paths', () => {
  for (const options of [undefined, {}, { executable: 'relative.exe' },
    { executable, profilePath: 'relative.nkp' }, { executable, profilePath: 10 }]) {
    assert.throws(() => new NativeBridge(options), TypeError);
  }
  assert.doesNotThrow(() => new NativeBridge({ executable }));
});

test('startup launches only the fixed host, without shell or automatic microphone commands', async (t) => {
  const host = await started(t);
  assert.equal(await host.bridge.start(), host.bridge);
  assert.equal(host.calls.length, 1);
  const [file, args, options] = host.calls[0];
  assert.equal(file, executable);
  assert.deepEqual(args, ['--profile', profilePath]);
  assert.equal(options.shell, false);
  assert.equal(options.windowsHide, true);
  assert.deepEqual(options.stdio, ['pipe', 'pipe', 'pipe']);
  assert.equal(host.requests.length, 0, 'startup must not record, start, or bypass');
});

test('request before explicit startup rejects without spawning a host', async () => {
  const host = mockHost();
  await assert.rejects(host.bridge.request('status'), /unavailable/i);
  assert.equal(host.calls.length, 0);
  await host.bridge.close();
});

test('concurrent replies correlate by ID, including canonical string IDs from native host', async (t) => {
  const host = await started(t);
  const first = host.bridge.request('status');
  const second = host.bridge.request('devices');
  assert.equal(host.requests.length, 2);
  assert.match(host.requests[0].id, /^[1-9]\d*$/);
  assert.notEqual(host.requests[0].id, host.requests[1].id);
  host.reply(host.requests[1].id, { inputs: ['second'] });
  host.reply(Number(host.requests[0].id), { session: { state: 'Stopped' } });
  assert.deepEqual(await second, { inputs: ['second'] });
  assert.deepEqual(await first, { session: { state: 'Stopped' } });
});

test('arguments use separate bounded UTF-8 base64 fields, not command interpolation', async (t) => {
  const host = await started(t);
  const input = '麦克风\t设备\n甲';
  const output = '线路 & $(不执行) "乙"';
  const result = host.bridge.request('start', [input, output, 'filter']);
  const request = host.requests[0];
  assert.equal(request.protocol, 'NK1');
  assert.equal(request.verb, 'start');
  assert.deepEqual(request.args, [input, output, 'filter']);
  assert.equal(request.line.split('\t').length, 6);
  assert.ok(request.encoded.every((value) => /^[A-Za-z0-9+/]*={0,2}$/.test(value)));
  assert.equal(host.requests.length, 1, 'newlines in arguments cannot create commands');
  host.reply(request.id, { session: { state: 'Filtering' } });
  await result;
});

test('split JSON and split multibyte UTF-8 responses are reassembled', async (t) => {
  const host = await started(t);
  const promise = host.bridge.request('status');
  const bytes = Buffer.from(JSON.stringify({ id: host.requests[0].id, ok: true,
    result: { session: { state: 'Stopped' }, label: '本人甲' } }) + '\n');
  const chineseStart = bytes.indexOf(Buffer.from('本人甲'));
  host.child.stdout.write(bytes.subarray(0, 6));
  host.child.stdout.write(bytes.subarray(6, chineseStart + 1));
  host.child.stdout.write(bytes.subarray(chineseStart + 1, chineseStart + 5));
  host.child.stdout.write(bytes.subarray(chineseStart + 5));
  assert.equal((await promise).label, '本人甲');
  assert.equal(host.faults.length, 0);
});

test('multiple replies in a single chunk allow CRLF and blank lines', async (t) => {
  const host = await started(t);
  const first = host.bridge.request('status');
  const second = host.bridge.request('devices');
  host.child.stdout.write('\n' + JSON.stringify({ id: host.requests[0].id, ok: true, result: 1 }) +
    '\r\n' + JSON.stringify({ id: host.requests[1].id, ok: true, result: 2 }) + '\n');
  assert.deepEqual(await Promise.all([first, second]), [1, 2]);
});

test('status events expose successful session results, not arbitrary devices/error results', async (t) => {
  const host = await started(t);
  const seen = [];
  host.bridge.on('status', (value) => seen.push(value));
  const promises = [host.bridge.request('status'), host.bridge.request('devices'), host.bridge.request('stop')];
  const status = { session: { state: 'Stopped' }, profile: { registered: false } };
  host.reply(host.requests[0].id, status);
  host.reply(host.requests[1].id, { session: { unexpected: true } });
  host.reply(host.requests[2].id, { stopped: true });
  await Promise.all(promises);
  assert.deepEqual(seen, [status]);
});

test('a native operation error rejects only that operation and leaves control available', async (t) => {
  const host = await started(t);
  const rejected = assert.rejects(host.bridge.request('enrollFile', [profilePath]), /Reference rejected/);
  host.reply(host.requests[0].id, 'Reference rejected', false);
  await rejected;
  assert.equal(host.child.killed, false);
  const status = host.bridge.request('status');
  host.reply(host.requests[1].id, { session: { state: 'Stopped' } });
  await status;
  assert.equal(host.faults.length, 0);
});

for (const [name, response] of [
  ['invalid JSON', '{not-json}\n'],
  ['missing OK discriminator', '{"id":1,"result":{}}\n'],
  ['nonboolean OK discriminator', '{"id":1,"ok":1,"result":{}}\n'],
  ['success without result', '{"id":1,"ok":true}\n'],
  ['error without string message', '{"id":1,"ok":false,"error":{}}\n'],
  ['noncanonical numeric ID', '{"id":"01","ok":true,"result":{}}\n'],
  ['unknown response ID', '{"id":999999,"ok":true,"result":{}}\n'],
]) {
  test(`protocol ${name} fails closed and rejects all pending operations`, async (t) => {
    const host = await started(t);
    const first = assert.rejects(host.bridge.request('status'));
    const second = assert.rejects(host.bridge.request('devices'));
    host.child.stdout.write(response);
    await Promise.all([first, second]);
    await tick();
    assert.equal(host.child.killed, true);
    assert.equal(host.faults.length, 1);
    await assert.rejects(host.bridge.request('status'), /unavailable/i);
    await assert.rejects(host.bridge.start(), /unavailable|closed/i);
    assert.equal(host.calls.length, 1, 'protocol faults must never auto-restart audio');
  });
}

test('oversized response without a newline is bounded and fails closed', async (t) => {
  const host = await started(t);
  const pending = assert.rejects(host.bridge.request('status'), /size|large|limit/i);
  host.child.stdout.write('x'.repeat(1024 * 1024 + 1));
  await pending;
  await tick();
  assert.equal(host.child.killed, true);
  assert.equal(host.faults.length, 1);
});

test('a duplicate successful response is a protocol fault, never an unsolicited state change', async (t) => {
  const host = await started(t);
  const promise = host.bridge.request('status');
  const id = host.requests[0].id;
  host.reply(id, { session: { state: 'Stopped' } });
  await promise;
  host.reply(id, { session: { state: 'Filtering' } });
  await tick();
  assert.equal(host.child.killed, true);
  assert.equal(host.faults.length, 1);
});

test('unexpected child exit rejects pending requests and does not reconnect', async (t) => {
  const host = await started(t);
  const pending = assert.rejects(host.bridge.request('status'), /exit/i);
  host.finish(12);
  await pending;
  await assert.rejects(host.bridge.start(), /unavailable|closed/i);
  await assert.rejects(host.bridge.request('devices'), /unavailable/i);
  assert.equal(host.calls.length, 1);
  assert.equal(host.faults.length, 1);
});

test('request timeout terminates the host and rejects all work, including late start replies', async (t) => {
  const host = await started(t);
  const start = assert.rejects(host.bridge.request('start', ['mic', 'cable', 'filter'], 20), /timed out/i);
  const status = assert.rejects(host.bridge.request('status'), /timed out/i);
  await Promise.all([start, status]);
  await tick();
  assert.equal(host.child.killed, true);
  host.reply(host.requests[0].id, { session: { state: 'Filtering' } });
  await assert.rejects(host.bridge.start(), /unavailable|closed/i);
  assert.equal(host.calls.length, 1);
  assert.equal(host.faults.length, 1);
});

test('startup spawn failure is observable and never retries implicitly', async () => {
  const host = mockHost({ spawnError: new Error('host missing') });
  await assert.rejects(host.bridge.start(), /host missing/);
  await assert.rejects(host.bridge.start());
  assert.equal(host.calls.length, 1);
  assert.equal(host.faults.length, 1);
  await host.bridge.close();
});

test('request validates the closed command vocabulary, arity, explicit mode, and bounded fields', async (t) => {
  const host = await started(t);
  const invalid = [
    ['exec', []], ['__proto__', []], ['constructor', []], ['status', ['extra']],
    ['status', 'not-an-array'], ['record', []], ['start', ['mic', 'cable']],
    ['start', ['mic', 'cable', 'auto']], ['start', ['mic', 'cable', '']],
    ['enrollFile', [undefined]], ['enrollFile', [10]], ['enrollFile', ['a\0b']],
    ['enrollFile', ['界'.repeat(5462)]],
    ['status', [], 0], ['status', [], -1], ['status', [], Infinity],
    ['status', [], NaN], ['status', [], 60001], ['status', [], '100'],
  ];
  for (const args of invalid) await assert.rejects(host.bridge.request(...args), TypeError);
  assert.equal(host.requests.length, 0, 'invalid operations must not reach native stdin');
  assert.equal(host.child.killed, false, 'caller validation errors do not kill a healthy idle host');
  assert.equal(host.faults.length, 0);
  assert.deepEqual(Object.keys(COMMANDS).sort(),
    ['status', 'devices', 'enrollFile', 'record', 'start', 'stop', 'deleteProfile', 'processFile', 'shutdown'].sort());
});

test('legal arguments at the UTF-8 byte boundary are encoded without truncation', async (t) => {
  const host = await started(t);
  const args = ['x'.repeat(16384), 'y'.repeat(16384), 'filter'];
  const pending = host.bridge.request('start', args);
  assert.deepEqual(host.requests[0].args, args);
  assert.ok(Buffer.byteLength(host.requests[0].line + '\n') < 65536);
  host.reply(host.requests[0].id, { session: { state: 'Filtering' } });
  await pending;
});

test('at most 32 concurrent requests can occupy the control pipe', async (t) => {
  const host = await started(t);
  const pending = Array.from({ length: 32 }, () => host.bridge.request('status'));
  await assert.rejects(host.bridge.request('devices'), /many|limit|pending/i);
  assert.equal(host.requests.length, 32);
  for (const request of host.requests.slice()) host.reply(request.id, { session: { state: 'Stopped' } });
  await Promise.all(pending);
});

test('close requests shutdown, ends stdin, waits for exit, and is idempotent', async () => {
  const host = mockHost();
  await host.bridge.start();
  const closing = host.bridge.close();
  assert.equal(host.bridge.close(), closing);
  await closing;
  assert.deepEqual(host.requests.map((value) => value.verb), ['shutdown']);
  assert.equal(host.child.stdin.writableEnded, true);
  assert.equal(host.child.killed, false, 'a cooperating native host needs no forced termination');
  assert.equal(host.faults.length, 0);
  await assert.rejects(host.bridge.request('status'), /unavailable/i);
  await assert.rejects(host.bridge.start(), /unavailable|closed/i);
});

test('closing an unstarted bridge never spawns the native host', async () => {
  const host = mockHost();
  await host.bridge.close();
  assert.equal(host.calls.length, 0);
  await assert.rejects(host.bridge.start(), /unavailable|closed/i);
});
