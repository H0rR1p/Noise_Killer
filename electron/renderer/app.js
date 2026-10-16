'use strict';

(() => {
  const api = window.noisekiller;
  const routes = { workspace: '工作台', profile: '语音注册', diagnostics: '运行诊断', settings: '设置与帮助' };
  const stateLabels = { stopped: '未启动', starting: '准备中', enrolling: '注册采集中', filtering: '降噪运行中', bypass: '原声旁路中', enrollmentReady: '注册采集完成', fault: '引擎异常' };
  const state = { status: null, devices: { inputs: [], outputs: [] }, available: false, pending: false, stopping: false, lastError: '', route: 'workspace', appVersion: '—' };
  let toastTimer;
  const $ = (selector) => document.querySelector(selector);
  const all = (selector) => [...document.querySelectorAll(selector)];
  const values = (key, value) => all(`[data-value="${key}"]`).forEach((node) => { node.textContent = String(value); });
  const finite = (value, digits = 0) => typeof value === 'number' && Number.isFinite(value) ? value.toLocaleString('zh-CN', { maximumFractionDigits: digits }) : '—';
  const session = () => state.status?.session || {};
  const running = () => ['starting', 'filtering', 'bypass', 'enrolling'].includes(session().state);

  function notify(message) {
    if (!message) return;
    const toast = $('#toast');
    toast.querySelector('span').textContent = String(message);
    toast.hidden = false;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => { toast.hidden = true; }, 5500);
  }

  function reportError(error) {
    state.lastError = typeof error === 'string' ? error : error?.message || '操作未完成，请检查音频设备后重试。';
    $('#error-message').textContent = state.lastError;
    $('#error-banner').hidden = false;
    values('lastError', state.lastError);
  }

  function navigate(route, updateHash = true) {
    if (!routes[route]) route = 'workspace';
    state.route = route;
    all('.page-panel').forEach((panel) => { panel.hidden = panel.id !== `panel-${route}`; });
    all('.nav-item').forEach((button) => {
      const active = button.dataset.route === route;
      button.classList.toggle('active', active);
      if (active) button.setAttribute('aria-current', 'page'); else button.removeAttribute('aria-current');
    });
    $('#route-label').textContent = routes[route];
    if (updateHash && location.hash !== `#${route}`) history.replaceState(null, '', `#${route}`);
    document.title = `${routes[route]} · NoiseKiller`;
  }

  function applyTheme(theme) {
    document.documentElement.dataset.theme = theme === 'dark' ? 'dark' : 'light';
    values('themeLabel', theme === 'dark' ? '切换浅色' : '切换深色');
    all('.theme-toggle').forEach((button) => button.setAttribute('aria-label', theme === 'dark' ? '切换到浅色外观' : '切换到深色外观'));
    try { localStorage.setItem('noisekiller-theme', theme); } catch { /* Storage may be disabled; appearance still works. */ }
  }

  function populateDevices() {
    for (const [kind, selector, empty] of [['inputs', '#input-device', '未找到输入设备'], ['outputs', '#output-device', '未找到输出设备']]) {
      const select = $(selector);
      const previous = select.value;
      select.replaceChildren();
      const devices = Array.isArray(state.devices?.[kind]) ? state.devices[kind] : [];
      if (!devices.length) {
        select.add(new Option(state.available ? empty : '未连接音频引擎', ''));
      } else {
        select.add(new Option(kind === 'inputs' ? '请选择输入麦克风' : '请选择输出设备', ''));
        for (const device of devices) select.add(new Option(device.name || '未命名音频设备', device.id));
        if (devices.some((device) => device.id === previous)) select.value = previous;
      }
    }
    updateControls();
  }

  function updateControls() {
    const active = running();
    const canStop = active || session().state === 'fault';
    const busy = state.pending || Boolean(state.status?.job?.busy);
    const registered = Boolean(state.status?.profile?.registered);
    const hasInput = Boolean($('#input-device').value);
    const hasOutput = Boolean($('#output-device').value);
    const blocked = !state.available || busy;
    $('#cancel-button').disabled = !state.available || state.stopping || !(canStop || busy);
    $('#cancel-button').textContent = state.stopping ? '正在停止…' : '停止 / 取消';
    $('#input-device').disabled = blocked || active || !state.devices.inputs?.length;
    $('#output-device').disabled = blocked || active || !state.devices.outputs?.length;
    $('#refresh-button').disabled = blocked || active;
    $('#refresh-button').classList.toggle('loading-button', state.pending && state.pendingAction === 'refresh');
    $('#start-button').disabled = blocked || (canStop ? session().state === 'starting' : !(registered && hasInput && hasOutput));
    all('[data-action="bypass"]').forEach((button) => {
      button.disabled = blocked || active || !(registered && hasInput && hasOutput);
      button.title = registered ? '发送未过滤输入；需要再次确认' : '请先注册本人参考语音';
    });
    all('[data-action="record"]').forEach((button) => { button.disabled = blocked || active || !hasInput; });
    all('[data-action="import"]').forEach((button) => { button.disabled = blocked || active; });
    all('[data-action="delete"]').forEach((button) => { button.disabled = blocked || active || !registered; });
    all('[data-action="process"]').forEach((button) => { button.disabled = blocked || active || !registered; });
    values('startLabel', state.pending && state.pendingAction === 'toggle-session' ? (canStop ? '正在停止…' : '正在启动…') : session().state === 'fault' ? '重置 / 静音' : active ? (session().state === 'enrolling' ? '停止注册采集' : '停止处理') : '开启降噪');
    values('recordLabel', state.pending && state.pendingAction === 'record' ? '正在准备采集…' : registered ? '重新注册语音' : '注册本人语音');
    const reason = !state.available ? '未连接音频引擎' : !registered ? '请先注册本人参考语音' : !hasInput || !hasOutput ? '请选择输入麦克风与输出设备' : '';
    $('#start-button').title = canStop ? '停止当前采集与处理，清除故障状态' : reason;
  }

  function renderStatus(next) {
    if (next && typeof next === 'object') state.status = next;
    const s = session();
    const p = state.status?.profile || {};
    const job = state.status?.job || {};
    const label = stateLabels[s.state] || '未连接';
    document.body.dataset.session = s.state || 'disconnected';
    values('stateLabel', label);
    const fullVersion = state.status?.algorithmVersion || '—';
    values('algorithmVersion', fullVersion.match(/\d+\.\d+\.\d+/)?.[0] || fullVersion);
    all('[data-value="algorithmVersion"]').forEach((node) => { node.title = fullVersion; });
    values('algorithmVersionFull', fullVersion);
    values('appVersion', state.appVersion);
    values('profileLabel', p.registered ? '已注册 · 本地档案' : '尚未注册');
    values('acceptedSeconds', p.registered ? finite(p.acceptedSeconds, 1) : '—');
    values('prototypeCount', p.registered ? finite(p.prototypeCount) : '—');
    values('profileSummary', p.registered ? `已建立本人参考档案 · ${finite(p.acceptedSeconds, 1)} 秒有效语音。\n参考档案只在本机用于声源匹配。` : '录制约 15 秒独自讲话的语音，\n用于生成本地声学参考档案。');
    all('[data-value="profileSummary"]').forEach((node) => { node.style.whiteSpace = 'pre-line'; });
    const heads = { stopped: '降噪未启动', filtering: 'MECAG 降噪运行中', bypass: '原声旁路已开启', enrolling: '正在采集注册语音', starting: '正在启动音频会话', enrollmentReady: '注册语音采集完成', fault: '音频会话异常' };
    values('engineHeadline', heads[s.state] || '音频引擎未连接');
    const descriptions = {
      filtering: '麦克风 → MECAG → 输出设备。\n重叠语音效果仍在持续验证。',
      bypass: '未进行本人筛选或降噪。\n旁人与环境声音可能一同传出。',
      enrolling: '请自然、连续地独自讲话。\n完成后将自动结束采集。',
      starting: '正在打开选定的音频设备，\n请稍候。',
      enrollmentReady: '注册语音已采集。\n请等待参考档案生成后开启降噪。',
      fault: '请查看错误提示和运行诊断，\n停止会话后检查设备配置。'
    };
    const description = $('#engine-description');
    description.textContent = descriptions[s.state] || (p.registered ? '本人参考档案已加载。\n请选择输入与输出设备后开启降噪。' : '请先注册本人语音，\n并选择输入与输出设备。');
    description.style.whiteSpace = 'pre-line';
    const hasProcessed = typeof s.processedHops === 'number' && s.processedHops > 0;
    for (const key of ['processedHops', 'computeUs', 'maximumComputeUs', 'deadlineMisses']) values(key, hasProcessed ? finite(s[key], key === 'computeUs' || key === 'maximumComputeUs' ? 1 : 0) : '—');
    values('underflowEvents', hasProcessed ? finite(s.output?.underflowEvents) : '—');
    values('inputRate', s.inputRate > 0 && running() ? finite(s.inputRate) : '—');
    values('outputRate', s.output?.rate > 0 && running() ? finite(s.output.rate) : '—');
    values('queuedInputSamples', hasProcessed ? finite(s.output?.queuedInputSamples) : '—');
    const goal = typeof s.enrollmentGoal === 'number' && s.enrollmentGoal > 0 ? s.enrollmentGoal : 240000;
    const samples = typeof s.enrollmentSamples === 'number' ? Math.max(0, s.enrollmentSamples) : 0;
    all('.record-progress').forEach((node) => { node.hidden = s.state !== 'enrolling'; });
    all('.progress-track>span').forEach((node) => { node.style.width = `${Math.min(100, 100 * samples / goal)}%`; });
    values('enrollmentProgress', `${finite(samples / 16000, 1)} / ${finite(goal / 16000, 1)} 秒`);
    values('jobMessage', job.message || (job.busy ? '任务正在处理…' : '暂无运行任务。'));
    const currentError = job.error || s.error || s.output?.error;
    if (currentError && currentError !== state.lastError) reportError(currentError);
    values('lastError', state.lastError || '没有已报告的异常。');
    updateControls();
  }

  async function perform(action, task, success) {
    if (state.pending) return;
    if (!state.available) return notify('未连接音频引擎，请使用 Electron 桌面程序。');
    state.pending = true;
    state.pendingAction = action;
    updateControls();
    try {
      const result = await task();
      if (result && typeof result === 'object' && ('session' in result || 'profile' in result)) renderStatus(result);
      if (result !== null && success) notify(typeof success === 'function' ? success(result) : success);
    } catch (error) {
      reportError(error);
    } finally {
      state.pending = false;
      state.pendingAction = '';
      updateControls();
    }
  }

  // Stop is independent of pending operations: even a file-picker or a busy
  // enrollment job must never hide the user's ability to request cancellation.
  async function stopNow() {
    if (!state.available || state.stopping) return;
    state.stopping = true;
    updateControls();
    try {
      const result = await api.stop();
      if (result) renderStatus(result);
      notify('已发送停止 / 取消请求，请以引擎状态为准。');
    } catch (error) { reportError(error); }
    finally { state.stopping = false; updateControls(); }
  }

  const actions = {
    refresh: () => perform('refresh', async () => { const devices = await api.refreshDevices(); if (devices) state.devices = devices; populateDevices(); return devices; }, '音频设备已刷新。'),
    'toggle-session': () => {
      const stop = running() || session().state === 'fault';
      return perform('toggle-session', () => stop ? api.stop() : api.start({ inputId: $('#input-device').value, outputId: $('#output-device').value, mode: 'filter' }), stop ? '已请求停止音频采集与处理。' : '已提交降噪启动请求，请以引擎状态为准。');
    },
    bypass: () => perform('bypass', () => api.start({ inputId: $('#input-device').value, outputId: $('#output-device').value, mode: 'bypass' })),
    record: () => perform('record', () => api.record({ inputId: $('#input-device').value })),
    import: () => perform('import', () => api.importReference()),
    delete: () => perform('delete', () => api.deleteProfile()),
    process: () => perform('process', () => api.processFile()),
    cancel: stopNow,
    theme: () => applyTheme(document.documentElement.dataset.theme === 'dark' ? 'light' : 'dark'),
    guide: async () => {
      if (!api?.openGuide) return notify('浏览器预览不支持打开本地教程，请使用桌面程序或直接打开 docs/tutorial/index.html。');
      try { const opened = await api.openGuide(); if (opened === false) notify('未找到本地图文教程，请检查 docs/tutorial/index.html。'); } catch (error) { reportError(error); }
    },
    'dismiss-error': () => { $('#error-banner').hidden = true; }
  };

  document.addEventListener('click', (event) => {
    const routeButton = event.target.closest('[data-route]');
    if (routeButton) { event.preventDefault(); navigate(routeButton.dataset.route); return; }
    const windowButton = event.target.closest('[data-window]');
    if (windowButton) { if (api?.windowControl) api.windowControl(windowButton.dataset.window); return; }
    const actionButton = event.target.closest('[data-action]');
    if (actionButton && !actionButton.disabled && actions[actionButton.dataset.action]) actions[actionButton.dataset.action]();
  });
  $('#input-device').addEventListener('change', updateControls);
  $('#output-device').addEventListener('change', updateControls);
  window.addEventListener('hashchange', () => navigate(location.hash.slice(1), false));

  async function initialize() {
    let theme = 'light';
    try { theme = localStorage.getItem('noisekiller-theme') || 'light'; } catch { /* Ignore unavailable storage. */ }
    applyTheme(theme);
    navigate(location.hash.slice(1) || 'workspace', false);
    $('#preview-notice').hidden = Boolean(api);
    all('[data-window]').forEach((button) => { button.disabled = !api?.windowControl; });
    if (!api) { populateDevices(); renderStatus(); return; }
    if (api.onStatus) api.onStatus((next) => renderStatus(next));
    if (api.onBackendError) api.onBackendError((error) => {
      state.available = false;
      reportError(error);
      updateControls();
    });
    try {
      const data = await api.bootstrap();
      state.available = Boolean(data?.backendAvailable);
      state.appVersion = data?.appVersion || '—';
      state.devices = data?.devices || { inputs: [], outputs: [] };
      populateDevices();
      renderStatus(data?.status);
      if (data?.error) reportError(data.error);
      else if (!state.available) reportError('本地音频引擎不可用。界面已启动，但采集与处理尚未连接。');
    } catch (error) { reportError(error); populateDevices(); renderStatus(); }
  }
  initialize();
})();
