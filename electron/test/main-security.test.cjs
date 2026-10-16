'use strict';

// Run the real main-process entry point against inert Electron/native mocks.
// These tests never launch Electron, a native executable, or an audio stream.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { EventEmitter } = require('node:events');

const APP_URL = 'nk-app://ui/index.html';
const turn = () => new Promise((resolve) => setImmediate(resolve));
const deferred = () => {
  let resolve;
  const promise = new Promise((done) => { resolve = done; });
  return { promise, resolve };
};

async function desktop({ argv } = {}) {
  const handlers = new Map(), schemes = [], protocols = new Map(), windows = [], hosts = [];
  const dialogs = [], reads = [], intervals = [], errors = [];
  const app = new EventEmitter();
  Object.assign(app, {
    isPackaged: false, quitCount: 0, setName() {}, requestSingleInstanceLock: () => true,
    whenReady: () => Promise.resolve(), getVersion: () => 'test-only', quit() { this.quitCount++; },
  });
  const powerMonitor = new EventEmitter();
  const desktopSession = { webRequest: {
    onBeforeRequest(callback) { desktopSession.requestPolicy = callback; },
  },
  setPermissionRequestHandler(callback) { this.permissionRequest = callback; },
  setPermissionCheckHandler(callback) { this.permissionCheck = callback; },
  };
  class MockWindow extends EventEmitter {
    constructor(options) {
      super(); this.options = options; this.sent = []; this.destroyed = false; this.showCount = 0;
      this.webContents = new EventEmitter();
      Object.assign(this.webContents, {
        mainFrame: { url: APP_URL }, isDestroyed: () => false,
        send: (channel, value) => this.sent.push({ channel, value }),
        setWindowOpenHandler: (callback) => { this.windowOpen = callback; },
      });
      windows.push(this);
    }
    isDestroyed() { return this.destroyed; }
    async loadURL(url) { this.loadedURL = url; }
    show() { this.showCount++; } minimize() {} maximize() {} unmaximize() {} restore() {} focus() {}
    isMaximized() { return false; } isMinimized() { return false; }
    close() { this.emit('close', { preventDefault() {} }); }
  }
  class MockBridge extends EventEmitter {
    constructor(options) {
      super(); this.options = options; this.commands = []; this.failed = false; this.closed = false;
      this.closeCount = 0; this.starts = 0;
      hosts.push(this);
    }
    async start() { this.starts++; return this; }
    async request(verb, args = []) {
      this.commands.push({ verb, args });
      if (verb === 'devices') return { inputs: [{ id: 'mic-1', name: '物理麦克风' }], outputs: [{ id: 'cable-1', name: 'CABLE Input' }] };
      if (verb === 'status' || verb === 'stop' || verb === 'start' || verb === 'record')
        return { session: { state: 'stopped', error: '' }, job: { busy: false }, profile: { registered: false } };
      return { done: true };
    }
    async close() { this.closeCount++; this.closed = true; }
  }
  const dialog = {
    showMessageBox: async (_window, options) => {
      const item = { type: 'message', options, ...deferred() }; dialogs.push(item); return item.promise;
    },
    showOpenDialog: async (_window, options) => {
      const item = { type: 'open', options, ...deferred() }; dialogs.push(item); return item.promise;
    },
    showSaveDialog: async (_window, options) => {
      const item = { type: 'save', options, ...deferred() }; dialogs.push(item); return item.promise;
    },
  };
  const electron = {
    app, BrowserWindow: MockWindow, ipcMain: { handle: (name, callback) => handlers.set(name, callback) },
    protocol: {
      registerSchemesAsPrivileged: (items) => schemes.push(...items),
      handle: (name, callback) => protocols.set(name, callback),
    },
    session: { defaultSession: desktopSession }, dialog, powerMonitor,
    Menu: { setApplicationMenu() {} }, nativeTheme: {},
  };
  const nativeFs = {
    existsSync: (file) => path.basename(file) === 'NoiseKiller.Host.exe',
    promises: { readFile: async (file) => { reads.push(file); return Buffer.from('test-only-local-asset'); } },
  };
  const context = {
    require: (name) => {
      if (name === 'electron') return electron;
      if (name === './bridge.cjs') return { NativeBridge: MockBridge };
      if (name === 'node:fs') return nativeFs;
      if (name === 'node:path') return path;
      throw new Error(`Unexpected mocked require: ${name}`);
    },
    __dirname: path.resolve(__dirname, '..'),
    process: { argv, env: { LOCALAPPDATA: path.resolve('build', 'test-only-user-data') }, resourcesPath: path.resolve('resources') },
    URL, Response, Buffer,
    console: { error: (...args) => errors.push(args), log() {} },
    setInterval: (callback) => { intervals.push(callback); return intervals.length; }, clearInterval() {},
  };
  vm.runInNewContext(fs.readFileSync(path.join(__dirname, '..', 'main.cjs'), 'utf8'), context, { filename: 'main.cjs' });
  for (let count = 0; count < 4; count++) await turn();
  assert.equal(windows.length, 1, 'main entry point reached its single local window');
  assert.equal(hosts.length, 1);
  assert.deepEqual(errors, []);
  const window = windows[0], host = hosts[0];
  const event = () => ({ sender: window.webContents, senderFrame: window.webContents.mainFrame });
  const invoke = (name, value, sender = event()) => handlers.get(`nk:${name}`)(sender, value);
  return { app, host, window, dialogs, handlers, schemes, protocols, desktopSession, powerMonitor, invoke, event, reads, intervals };
}

test('real main startup opens only local sandboxed UI and never requests record/start', async () => {
  const ui = await desktop();
  assert.equal(ui.window.loadedURL, APP_URL);
  const prefs = ui.window.options.webPreferences;
  assert.equal(prefs.contextIsolation, true);
  assert.equal(prefs.nodeIntegration, false);
  assert.equal(prefs.sandbox, true);
  assert.equal(prefs.webSecurity, true);
  assert.equal(ui.host.starts, 1);
  assert.deepEqual(ui.host.commands.map((command) => command.verb), ['status', 'devices']);
  assert.equal(ui.dialogs.length, 0, 'startup must not ask for or start microphone access');
  const boot = await ui.invoke('bootstrap');
  assert.equal(boot.backendAvailable, true);
});

test('--smoke-ui suppresses only the visible window, without relaxing audio or renderer security', async () => {
  const ui = await desktop({ argv: ['electron', '--smoke-ui'] });
  ui.window.emit('ready-to-show');
  assert.equal(ui.window.showCount, 0);
  assert.deepEqual(ui.host.commands.map((command) => command.verb), ['status', 'devices']);
  assert.equal(ui.window.options.webPreferences.sandbox, true);
  assert.equal(ui.window.options.webPreferences.contextIsolation, true);
  assert.equal(ui.window.options.webPreferences.nodeIntegration, false);
  assert.equal(ui.desktopSession.permissionCheck(ui.window.webContents, 'media'), false);
});

test('all main IPC handlers reject another webContents, subframes, and lookalike URLs', async () => {
  const ui = await desktop();
  await assert.rejects(ui.invoke('bootstrap', undefined, { sender: {}, senderFrame: ui.window.webContents.mainFrame }), /Untrusted/);
  await assert.rejects(ui.invoke('bootstrap', undefined, { sender: ui.window.webContents, senderFrame: { url: APP_URL } }), /Untrusted/);
  ui.window.webContents.mainFrame.url = 'nk-app://ui/index.html?untrusted=1';
  await assert.rejects(ui.invoke('bootstrap'), /Untrusted/);
  ui.window.webContents.mainFrame.url = APP_URL;
  assert.ok((await ui.invoke('bootstrap')).status);
});

test('only the four fixed internal navigation fragments retain trusted desktop IPC', async () => {
  const ui = await desktop();
  for (const fragment of ['workspace', 'profile', 'diagnostics', 'settings']) {
    ui.window.webContents.mainFrame.url = `${APP_URL}#${fragment}`;
    assert.equal((await ui.invoke('bootstrap')).backendAvailable, true, fragment);
    let prevented = false;
    ui.window.webContents.emit('will-navigate', { preventDefault: () => { prevented = true; } }, `${APP_URL}#${fragment}`);
    assert.equal(prevented, false, `fixed internal fragment ${fragment} may navigate`);
  }
  for (const suffix of ['?query=1', '?query=1#workspace', '#unknown', '#', '#workspace?query=1',
    '#workspace/other', '#work%73pace', '#Workspace', '#workspace#settings']) {
    ui.window.webContents.mainFrame.url = APP_URL + suffix;
    await assert.rejects(ui.invoke('bootstrap'), /Untrusted/, suffix);
    let prevented = false;
    ui.window.webContents.emit('will-navigate', { preventDefault: () => { prevented = true; } }, APP_URL + suffix);
    assert.equal(prevented, true, `${suffix} cannot navigate with desktop privileges`);
  }
});

test('custom protocol serves only fixed bundled assets with restrictive response CSP', async () => {
  const ui = await desktop();
  const serve = ui.protocols.get('nk-app');
  assert.ok(serve);
  const response = await serve({ url: APP_URL, method: 'GET' });
  assert.equal(response.status, 200);
  assert.equal(response.headers.get('X-Content-Type-Options'), 'nosniff');
  assert.match(response.headers.get('Content-Security-Policy'), /connect-src 'none'/);
  assert.match(response.headers.get('Content-Security-Policy'), /media-src 'none'/);
  assert.match(response.headers.get('Content-Security-Policy'), /script-src 'self'/);
  assert.equal(ui.reads[0], path.resolve(__dirname, '..', 'renderer', 'index.html'));
  for (const request of [
    { url: 'nk-app://evil/index.html', method: 'GET' },
    { url: 'nk-app://ui:444/index.html', method: 'GET' },
    { url: 'nk-app://ui/main.cjs', method: 'GET' },
    { url: 'nk-app://ui/%2e%2e/package.json', method: 'GET' },
    { url: APP_URL, method: 'POST' },
  ]) assert.equal((await serve(request)).status, 404);
  assert.equal(ui.reads.length, 1, 'rejected requests cannot read arbitrary local files');
});

test('permissions, remote loads, popups, webviews, and external navigation are denied', async () => {
  const ui = await desktop();
  let allowed = null;
  ui.desktopSession.permissionRequest(ui.window.webContents, 'media', (value) => { allowed = value; });
  assert.equal(allowed, false);
  assert.equal(ui.desktopSession.permissionCheck(ui.window.webContents, 'media'), false);
  for (const url of ['https://example.com/', 'file:///C:/private.txt', 'http://localhost:3000/', 'nk-app://ui.attacker/index.html']) {
    let blocked = null;
    ui.desktopSession.requestPolicy({ url }, (value) => { blocked = value.cancel; });
    assert.equal(blocked, true, url);
  }
  assert.equal(ui.window.windowOpen({ url: 'https://example.com/' }).action, 'deny');
  let prevented = 0;
  ui.window.webContents.emit('will-navigate', { preventDefault: () => prevented++ }, 'https://example.com/');
  ui.window.webContents.emit('will-attach-webview', { preventDefault: () => prevented++ });
  assert.equal(prevented, 2);
});

test('main accepts only enumerated device IDs and explicit modes, never renderer paths', async () => {
  const ui = await desktop();
  for (const value of [
    { inputId: 'unknown', outputId: 'cable-1', mode: 'filter' },
    { inputId: 'mic-1', outputId: 'unknown', mode: 'filter' },
    { inputId: 'mic-1', outputId: 'cable-1', mode: 'auto' },
    { inputId: 'mic-1', outputId: 'cable-1', mode: 'filter', path: 'C:/injected.exe' },
    { inputId: 'mic-1\0', outputId: 'cable-1', mode: 'filter' },
  ]) await assert.rejects(ui.invoke('start', value));
  assert.equal(ui.host.commands.filter((command) => command.verb === 'start').length, 0);
  await ui.invoke('start', { inputId: 'mic-1', outputId: 'cable-1', mode: 'filter' });
  const start = ui.host.commands.find((command) => command.verb === 'start');
  assert.deepEqual(Array.from(start.args), ['mic-1', 'cable-1', 'filter']);
});

test('Stop cancels a bypass awaiting confirmation, rather than allowing a later hidden restart', async () => {
  const ui = await desktop();
  const starting = ui.invoke('start', { inputId: 'mic-1', outputId: 'cable-1', mode: 'bypass' });
  await turn();
  assert.equal(ui.dialogs.length, 1);
  await ui.invoke('stop');
  ui.dialogs[0].resolve({ response: 1 });
  assert.equal(await starting, null);
  assert.equal(ui.host.commands.filter((command) => command.verb === 'start').length, 0);
  assert.equal(ui.host.commands.filter((command) => command.verb === 'stop').length, 1);
});

test('bypass defaults to cancel and never starts without affirmative main-process confirmation', async () => {
  const ui = await desktop();
  const starting = ui.invoke('start', { inputId: 'mic-1', outputId: 'cable-1', mode: 'bypass' });
  await turn();
  assert.equal(ui.dialogs[0].options.defaultId, 0);
  assert.equal(ui.dialogs[0].options.cancelId, 0);
  ui.dialogs[0].resolve({ response: 0 });
  assert.equal(await starting, null);
  assert.equal(ui.host.commands.filter((command) => command.verb === 'start').length, 0);
});

test('suspend cancels pending start confirmation and stops the native session', async () => {
  const ui = await desktop();
  const starting = ui.invoke('start', { inputId: 'mic-1', outputId: 'cable-1', mode: 'bypass' });
  await turn();
  ui.powerMonitor.emit('suspend');
  await turn();
  ui.dialogs[0].resolve({ response: 1 });
  assert.equal(await starting, null);
  assert.equal(ui.host.commands.filter((command) => command.verb === 'start').length, 0);
  assert.ok(ui.host.commands.some((command) => command.verb === 'stop'));
});

test('Stop invalidates pending reference-file selection instead of beginning a new enrollment', async () => {
  const ui = await desktop();
  const importing = ui.invoke('importReference');
  await turn();
  assert.equal(ui.dialogs[0].type, 'open');
  await ui.invoke('stop');
  ui.dialogs[0].resolve({ canceled: false, filePaths: [path.resolve('build', '本人-reference.wav')] });
  assert.equal(await importing, null);
  assert.equal(ui.host.commands.filter((command) => command.verb === 'enrollFile').length, 0);
});

test('Stop invalidates a pending delete confirmation', async () => {
  const ui = await desktop();
  const deleting = ui.invoke('deleteProfile');
  await turn();
  await ui.invoke('stop');
  ui.dialogs[0].resolve({ response: 1 });
  assert.equal(await deleting, null);
  assert.equal(ui.host.commands.filter((command) => command.verb === 'deleteProfile').length, 0);
});

test('Stop during the save picker cancels offline processing before any output is written', async () => {
  const ui = await desktop();
  const processing = ui.invoke('processFile');
  await turn();
  ui.dialogs[0].resolve({ canceled: false, filePaths: [path.resolve('build', 'mix.wav')] });
  await turn();
  assert.equal(ui.dialogs[1].type, 'save');
  await ui.invoke('stop');
  ui.dialogs[1].resolve({ canceled: false, filePath: path.resolve('build', 'out.wav') });
  assert.equal(await processing, null);
  assert.equal(ui.host.commands.filter((command) => command.verb === 'processFile').length, 0);
});

test('window close terminates the backend and invalidates outstanding start consent', async () => {
  const ui = await desktop();
  const starting = ui.invoke('start', { inputId: 'mic-1', outputId: 'cable-1', mode: 'bypass' });
  await turn();
  let prevented = false;
  ui.window.emit('close', { preventDefault: () => { prevented = true; } });
  await turn();
  assert.equal(prevented, true);
  assert.equal(ui.host.closeCount, 1);
  assert.equal(ui.app.quitCount, 1);
  ui.dialogs[0].resolve({ response: 1 });
  assert.equal(await starting, null);
  assert.equal(ui.host.commands.filter((command) => command.verb === 'start').length, 0);
});

test('renderer crash closes the backend rather than leaving an invisible audio session', async () => {
  const ui = await desktop();
  ui.window.webContents.emit('render-process-gone', {}, { reason: 'crashed' });
  await turn();
  assert.equal(ui.host.closeCount, 1);
  assert.equal(ui.app.quitCount, 1);
});

test('preload exposes a frozen task-specific API, not Electron, filesystem, or generic IPC', async () => {
  const exposed = [], calls = [];
  const ipc = new EventEmitter();
  ipc.invoke = async (...args) => { calls.push(args); return { stopped: true }; };
  vm.runInNewContext(fs.readFileSync(path.join(__dirname, '..', 'preload.cjs'), 'utf8'), {
    require: (name) => {
      assert.equal(name, 'electron');
      return { contextBridge: { exposeInMainWorld: (name, api) => exposed.push({ name, api }) }, ipcRenderer: ipc };
    },
  }, { filename: 'preload.cjs' });
  assert.equal(exposed.length, 1);
  assert.equal(exposed[0].name, 'noisekiller');
  const api = exposed[0].api;
  assert.equal(Object.isFrozen(api), true);
  assert.deepEqual(Object.keys(api).sort(), ['bootstrap', 'refreshDevices', 'start', 'stop', 'record',
    'importReference', 'deleteProfile', 'processFile', 'onStatus', 'onBackendError', 'windowControl', 'openGuide'].sort());
  assert.equal(api.invoke, undefined);
  assert.equal(api.send, undefined);
  assert.equal(api.fs, undefined);
  assert.equal(api.exec, undefined);
  assert.equal(calls.length, 0, 'loading preload must never initiate audio');
  await api.stop();
  assert.equal(calls[0][0], 'nk:stop');
  let seen = null;
  const unsubscribe = api.onStatus((...values) => { seen = values; });
  const secretEvent = { sender: 'privileged IPC event' }, status = { session: { state: 'stopped' } };
  ipc.emit('nk:status', secretEvent, status);
  assert.deepEqual(seen, [status], 'renderer callback receives data only, never the privileged IPC event');
  unsubscribe();
  assert.equal(ipc.listenerCount('nk:status'), 0);
  assert.throws(() => api.onStatus(undefined), /callback/i);
});

test('backend fault cancels outstanding consent and cannot be mistaken for filtered output', async () => {
  const ui = await desktop();
  const starting = ui.invoke('start', { inputId: 'mic-1', outputId: 'cable-1', mode: 'bypass' });
  await turn();
  ui.host.failed = true;
  ui.host.emit('fault', new Error('test-only native failure'));
  ui.dialogs[0].resolve({ response: 1 });
  assert.equal(await starting, null);
  assert.equal(ui.host.commands.filter((command) => command.verb === 'start').length, 0);
  const boot = await ui.invoke('bootstrap');
  assert.equal(boot.backendAvailable, false);
  assert.equal(boot.status.session.state, 'fault');
  assert.match(boot.error, /test-only native failure/);
});
