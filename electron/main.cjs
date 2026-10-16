'use strict';

const { app, BrowserWindow, ipcMain, protocol, session, dialog, Menu, nativeTheme, powerMonitor, shell } = require('electron');
const fs = require('node:fs');
const path = require('node:path');
const { NativeBridge } = require('./bridge.cjs');

const APP_URL = 'nk-app://ui/index.html';
const smokeUi = process.argv?.includes('--smoke-ui') === true;
const CSP = "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; font-src 'self'; connect-src 'none'; media-src 'none'; object-src 'none'; frame-src 'none'; base-uri 'none'; form-action 'none'";
protocol.registerSchemesAsPrivileged([{ scheme: 'nk-app', privileges: { standard: true, secure: true, supportFetchAPI: true } }]);
app.setName('NoiseKiller');
if (smokeUi) {
  const dataArgument = process.argv?.find((value) => value.startsWith('--user-data-dir='));
  const isolatedData = dataArgument?.slice('--user-data-dir='.length);
  if (isolatedData && path.isAbsolute(isolatedData)) app.setPath('userData', isolatedData);
}

let window = null;
let bridge = null;
let poll = null;
let polling = false;
let closing = false;
let exitAllowed = false;
let backendError = '';
let status = null;
let devices = { inputs: [], outputs: [] };
let ready = Promise.resolve();
let mutation = false;
let dialogOpen = false;
let operationEpoch = 0;

function send(name, value) {
  if (window && !window.isDestroyed() && !window.webContents.isDestroyed()) window.webContents.send(`nk:${name}`, value);
}
function fault(error) {
  ++operationEpoch;
  backendError = error instanceof Error ? error.message : String(error);
  if (status) {
    status = { ...status, session: { ...status.session, state: 'fault', error: backendError }, job: { ...status.job, busy: false } };
    send('status', status);
  }
  send('backendError', backendError);
}
function hostExecutable() {
  return app.isPackaged
    ? path.join(process.resourcesPath, 'native', 'NoiseKiller.Host.exe')
    : path.join(__dirname, '..', 'build', 'Release', 'NoiseKiller.Host.exe');
}
function profilePath() {
  if (!process.env.LOCALAPPDATA || !path.isAbsolute(process.env.LOCALAPPDATA))
    throw new Error('Windows current-user profile directory is unavailable');
  // Retain compatibility with the native GUI. Resolving the path does not create it.
  return path.join(process.env.LOCALAPPDATA, 'NoiseKiller', 'active-profile.nkp');
}
function backend() {
  if (!bridge || bridge.failed || bridge.closed || closing) throw new Error(backendError || '音频引擎未连接，请构建后重新启动应用。');
  return bridge;
}
function payload(value, keys) {
  if (!value || typeof value !== 'object' || Array.isArray(value) ||
      Object.keys(value).length !== keys.length || !keys.every((key) => Object.hasOwn(value, key)))
    throw new TypeError('Invalid desktop command');
  for (const key of keys) if (typeof value[key] !== 'string' || value[key].includes('\0') || value[key].length > 8192)
    throw new TypeError('Invalid desktop argument');
  return value;
}
function trustedLocation(value) {
  return typeof value === 'string' && /^nk-app:\/\/ui\/index\.html(?:#(?:workspace|profile|diagnostics|settings))?$/.test(value);
}
function validateSender(event) {
  if (!window || event.sender !== window.webContents || event.senderFrame !== window.webContents.mainFrame || !trustedLocation(event.senderFrame.url))
    throw new Error('Untrusted desktop IPC sender');
}
function handle(name, callback) {
  ipcMain.handle(`nk:${name}`, async (event, value) => { validateSender(event); return callback(value); });
}
async function command(callback) {
  await ready;
  backend();
  if (mutation || dialogOpen) throw new Error('另一个操作尚未完成，请稍候。');
  mutation = true;
  try { return await callback(); } finally { mutation = false; }
}
async function withDialog(callback) {
  dialogOpen = true;
  try { return await callback(); } finally { dialogOpen = false; }
}
async function stopForFileOperation() {
  const current = await backend().request('status');
  if (current.job.busy) throw new Error('后台任务仍在运行，请先停止并等待完成。');
  if (!['stopped', 'fault'].includes(current.session.state)) await backend().request('stop');
}

function registerHandlers() {
  handle('bootstrap', async () => {
    await ready;
    return { status, devices, appVersion: app.getVersion(), backendAvailable: Boolean(bridge && !bridge.failed && !bridge.closed && !backendError), error: backendError };
  });
  handle('refreshDevices', () => command(async () => { devices = await backend().request('devices'); return devices; }));
  handle('start', (value) => command(async () => {
    const generation = operationEpoch;
    const { inputId, outputId, mode } = payload(value, ['inputId', 'outputId', 'mode']);
    if (!['filter', 'bypass'].includes(mode)) throw new Error('Invalid processing mode');
    if (!devices.inputs.some((device) => device.id === inputId) || !devices.outputs.some((device) => device.id === outputId))
      throw new Error('请先明确选择物理麦克风和虚拟线路播放端点。');
    if (mode === 'bypass') {
      const answer = await withDialog(() => dialog.showMessageBox(window, {
        type: 'warning', title: '确认发送原始声音', message: '原声旁路不会过滤旁人的声音。',
        detail: '所选麦克风的未处理音频会发送到虚拟线路，包括旁人和环境声。只有你确认后才会启动。',
        buttons: ['取消', '确认发送原声'], defaultId: 0, cancelId: 0, noLink: true,
      }));
      if (answer.response !== 1) return null;
    }
    if (generation !== operationEpoch || closing) return null;
    return backend().request('start', [inputId, outputId, mode]);
  }));
  // Stop is never queued behind a file-picker or a pending start operation.
  handle('stop', async () => { ++operationEpoch; await ready; return backend().request('stop'); });
  handle('record', (value) => command(async () => {
    const { inputId } = payload(value, ['inputId']);
    if (!devices.inputs.some((device) => device.id === inputId)) throw new Error('请先明确选择物理麦克风。');
    return backend().request('record', [inputId]);
  }));
  handle('importReference', () => command(async () => {
    const generation = operationEpoch;
    await stopForFileOperation();
    if (closing || generation !== operationEpoch) return null;
    const selected = await withDialog(() => dialog.showOpenDialog(window, {
      title: '导入本人单独讲话的参考音（10–30 秒）', properties: ['openFile'], filters: [{ name: 'WAV 音频', extensions: ['wav'] }],
    }));
    if (selected.canceled || selected.filePaths.length !== 1 || closing || generation !== operationEpoch) return null;
    return backend().request('enrollFile', [selected.filePaths[0]]);
  }));
  handle('deleteProfile', () => command(async () => {
    const generation = operationEpoch;
    const answer = await withDialog(() => dialog.showMessageBox(window, {
      type: 'warning', title: '删除本人档案', message: '删除本机的加密声学档案？',
      detail: '删除后需要重新注册才能开启 MECAG。当前音频会话将停止；原始录音默认未保存。',
      buttons: ['取消', '删除档案'], defaultId: 0, cancelId: 0, noLink: true,
    }));
    if (answer.response !== 1 || closing || generation !== operationEpoch) return null;
    await backend().request('stop');
    if (closing || generation !== operationEpoch) return null;
    return backend().request('deleteProfile');
  }));
  handle('processFile', () => command(async () => {
    const generation = operationEpoch;
    await stopForFileOperation();
    if (closing || generation !== operationEpoch) return null;
    const source = await withDialog(() => dialog.showOpenDialog(window, {
      title: '选择待处理的混合音频', properties: ['openFile'], filters: [{ name: 'WAV 音频', extensions: ['wav'] }],
    }));
    if (source.canceled || source.filePaths.length !== 1 || closing || generation !== operationEpoch) return null;
    const file = source.filePaths[0];
    const destination = await withDialog(() => dialog.showSaveDialog(window, {
      title: '另存降噪结果（不覆盖已有文件）', defaultPath: path.join(path.dirname(file), `${path.basename(file, path.extname(file))}-filtered.wav`),
      filters: [{ name: 'WAV 音频', extensions: ['wav'] }], properties: ['showOverwriteConfirmation'],
    }));
    if (destination.canceled || !destination.filePath || closing || generation !== operationEpoch) return null;
    if (fs.existsSync(destination.filePath)) throw new Error('输出文件已存在，请选择新文件名；不会覆盖旧文件。');
    return backend().request('processFile', [file, destination.filePath]);
  }));
  handle('windowControl', (action) => {
    if (!['minimize', 'maximize', 'close'].includes(action)) throw new TypeError('Invalid window action');
    if (action === 'minimize') window.minimize();
    else if (action === 'maximize') window.isMaximized() ? window.unmaximize() : window.maximize();
    else window.close();
    return true;
  });
  handle('openGuide', async () => {
    // Only a fixed, local teaching artifact may be opened; never accept a path
    // or URL from the renderer. Private docs are not bundled in the preview.
    const tutorialRoot = app.isPackaged ? path.resolve(process.resourcesPath, '..', '..', '..') : path.resolve(__dirname, '..');
    const tutorial = path.join(tutorialRoot, 'docs', 'tutorial', 'index.html');
    if (!fs.existsSync(tutorial)) return false;
    const failure = await shell.openPath(tutorial);
    if (failure) throw new Error('无法打开本地图文教程，请直接打开 docs/tutorial/index.html。');
    return true;
  });
}

async function quitSafely() {
  if (closing) return;
  closing = true;
  ++operationEpoch;
  clearInterval(poll);
  try { await bridge?.close(); } finally { exitAllowed = true; app.quit(); }
}

if (!app.requestSingleInstanceLock()) app.quit();
else {
  app.on('second-instance', () => { if (window) { if (window.isMinimized()) window.restore(); window.focus(); } });
  app.on('before-quit', (event) => { if (!exitAllowed) { event.preventDefault(); void quitSafely(); } });
  app.on('window-all-closed', () => { void quitSafely(); });
  app.whenReady().then(async () => {
    nativeTheme.themeSource = 'light';
    Menu.setApplicationMenu(null);
    const allowedFiles = new Map([
      ['/index.html', ['index.html', 'text/html; charset=utf-8']],
      ['/styles.css', ['styles.css', 'text/css; charset=utf-8']],
      ['/app.js', ['app.js', 'text/javascript; charset=utf-8']],
      ['/icons.svg', ['icons.svg', 'image/svg+xml']],
    ]);
    protocol.handle('nk-app', async (request) => {
      const url = new URL(request.url);
      const asset = allowedFiles.get(url.pathname);
      if (url.host !== 'ui' || !asset || !['GET', 'HEAD'].includes(request.method)) return new Response('Not found', { status: 404 });
      try {
        const body = await fs.promises.readFile(path.join(__dirname, 'renderer', asset[0]));
        return new Response(request.method === 'HEAD' ? null : body, { headers: { 'Content-Type': asset[1], 'Content-Security-Policy': CSP, 'X-Content-Type-Options': 'nosniff' } });
      } catch { return new Response('Not found', { status: 404 }); }
    });
    const desktopSession = session.defaultSession;
    desktopSession.setPermissionRequestHandler((_contents, _permission, callback) => callback(false));
    desktopSession.setPermissionCheckHandler(() => false);
    desktopSession.webRequest.onBeforeRequest((details, callback) => {
      callback({ cancel: !details.url.startsWith('nk-app://ui/') && !details.url.startsWith('devtools://') });
    });
    registerHandlers();
    try {
      const executable = hostExecutable();
      if (!fs.existsSync(executable)) throw new Error('未找到原生音频引擎。请运行 scripts/build.ps1 -SkipTests 后重启。');
      bridge = new NativeBridge({ executable, profilePath: profilePath() });
      bridge.on('status', (value) => { status = value; send('status', status); });
      bridge.on('fault', fault);
      ready = (async () => { await bridge.start(); status = await bridge.request('status'); devices = await bridge.request('devices'); })().catch(fault);
    } catch (error) { fault(error); }
    window = new BrowserWindow({
      width: 1280, height: 860, minWidth: 780, minHeight: 640, show: false, frame: false,
      title: 'NoiseKiller', backgroundColor: '#f6f6f4', autoHideMenuBar: true,
      webPreferences: { preload: path.join(__dirname, 'preload.cjs'), contextIsolation: true, nodeIntegration: false, sandbox: true, webSecurity: true, spellcheck: false },
    });
    window.webContents.setWindowOpenHandler(() => ({ action: 'deny' }));
    window.webContents.on('will-navigate', (event, url) => { if (!trustedLocation(url)) event.preventDefault(); });
    window.webContents.on('will-attach-webview', (event) => event.preventDefault());
    window.webContents.on('render-process-gone', () => { void quitSafely(); });
    window.on('close', (event) => { if (!exitAllowed) { event.preventDefault(); void quitSafely(); } });
    window.on('unresponsive', () => { clearInterval(poll); fault(new Error('界面无响应，已关闭音频引擎。')); void bridge?.close(); });
    window.once('ready-to-show', () => { if (!closing && !smokeUi) window.show(); });
    await window.loadURL(APP_URL);
    poll = setInterval(async () => {
      if (polling || closing || !bridge || bridge.failed || bridge.closed) return;
      polling = true;
      try { await bridge.request('status'); } catch (error) { if (!bridge.failed) fault(error); }
      finally { polling = false; }
    }, 500);
    powerMonitor.on('suspend', () => { ++operationEpoch; if (bridge && !bridge.failed && !closing) void bridge.request('stop').catch(fault); });
  }).catch((error) => { console.error(error.message); void quitSafely(); });
}
