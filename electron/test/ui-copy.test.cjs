'use strict';

// Copy-only regression checks; no browser, native process or audio stream.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const renderer = path.resolve(__dirname, '..', 'renderer');
const html = fs.readFileSync(path.join(renderer, 'index.html'), 'utf8');
const script = fs.readFileSync(path.join(renderer, 'app.js'), 'utf8');

test('all page headings describe their function', () => {
  for (const [id, text] of [['workspace-title', '实时降噪'], ['profile-title', '语音注册'],
    ['diagnostics-title', '运行诊断'], ['settings-title', '设置与帮助']]) {
    assert.ok(html.includes(`id="${id}">${text}</h1>`), id);
  }
});

test('static and dynamic UI copy contain no previous slogans or figurative prompts', () => {
  for (const text of ['只留下，你的声音', '把嘈杂留在这里', '让表达自在发生', '安静，准备就绪',
    '连接你的声音', '先认识你的声音', '一条清晰的路径', '让引擎，认识你', '好好说话',
    '每一帧，都有据可查', '简单一点，连接起来', '黑白之间', '开始一次清晰表达',
    '正在认识你的声音', '需要一点检查', '此刻，原声直通', '轻松一点',
    'A little less noise', 'YOUR VOICE. NOTHING ELSE.', 'LESS NOISE. MORE YOU.',
    'A REFERENCE, NOT A RECORDING LIBRARY.', "OBSERVE. DON'T GUESS.",
    'SMALL DETAILS. CLEAR CONNECTIONS.', 'FROM ZERO TO YOUR FIRST SESSION']) {
    assert.equal((html + script).includes(text), false, text);
  }
});

test('every runtime engine headline describes the actual session state', () => {
  const match = script.match(/const heads = (\{[^\n]+\});/);
  assert.ok(match, 'runtime status text map exists');
  const headlines = vm.runInNewContext(`(${match[1]})`);
  assert.deepEqual(JSON.parse(JSON.stringify(headlines)), {
    stopped: '降噪未启动', filtering: 'MECAG 降噪运行中', bypass: '原声旁路已开启',
    enrolling: '正在采集注册语音', starting: '正在启动音频会话',
    enrollmentReady: '注册语音采集完成', fault: '音频会话异常',
  });
});

test('the existing local desktop result uses the same UI text as the source', () => {
  const packaged = path.resolve(__dirname, '..', '..', 'dist', 'NoiseKiller-Desktop-0.3.0-preview', 'resources', 'app', 'renderer');
  if (!fs.existsSync(packaged)) return; // A source-only checkout need not contain a preview.
  for (const name of ['index.html', 'app.js']) {
    assert.ok(fs.readFileSync(path.join(renderer, name)).equals(fs.readFileSync(path.join(packaged, name))), name);
  }
});
