'use strict';

const { EventEmitter } = require('node:events');
const { spawn: nativeSpawn } = require('node:child_process');
const path = require('node:path');

const COMMANDS = Object.freeze({
  status: 0, devices: 0, enrollFile: 1, record: 1, start: 3,
  stop: 0, deleteProfile: 0, processFile: 2, shutdown: 0,
});
const MAX_LINE = 1024 * 1024;
const MAX_REQUEST = 65536;

// This is control-plane IPC only. PCM never enters Electron or this pipe.
class NativeBridge extends EventEmitter {
  constructor({ executable, profilePath, spawn = nativeSpawn } = {}) {
    super();
    if (typeof executable !== 'string' || !path.isAbsolute(executable))
      throw new TypeError('An absolute native host executable is required');
    if (profilePath !== undefined && (typeof profilePath !== 'string' || !path.isAbsolute(profilePath)))
      throw new TypeError('The profile location must be absolute');
    this.executable = executable;
    this.profilePath = profilePath;
    this.spawn = spawn;
    this.child = null;
    this.pending = new Map();
    this.nextId = 1;
    this.buffer = '';
    this.failed = false;
    this.closing = false;
    this.closed = false;
    this.startPromise = null;
    this.closePromise = null;
  }

  start() {
    if (this.failed || this.closing || this.closed) return Promise.reject(new Error('Native host is unavailable'));
    if (this.startPromise) return this.startPromise;
    this.startPromise = new Promise((resolve, reject) => {
      try {
        this.child = this.spawn(this.executable, this.profilePath ? ['--profile', this.profilePath] : [], {
          shell: false, windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'],
        });
      } catch (error) {
        this._fail(error); reject(error); return;
      }
      const child = this.child;
      child.once('spawn', () => resolve(this));
      child.once('error', (error) => { this._fail(error); reject(error); });
      child.once('exit', (code, signal) => {
        this.closed = true;
        if (!this.closing) this._fail(new Error(`Native audio host exited (${code ?? signal ?? 'unknown'})`));
        else this._rejectPending(new Error('Native host closed'));
        this.emit('closed');
      });
      child.stdin.on('error', (error) => { if (!this.closing) this._fail(error); });
      child.stdout.setEncoding('utf8');
      child.stdout.on('data', (chunk) => this._receive(chunk));
      child.stdout.on('error', (error) => this._fail(error));
      // Drain errors without persisting device IDs, file paths, or acoustic data.
      child.stderr?.resume();
    });
    return this.startPromise;
  }

  request(verb, args = [], timeoutMs = 8000) {
    return this._request(verb, args, timeoutMs, false);
  }

  _request(verb, args, timeoutMs, allowClosing) {
    if (!Object.hasOwn(COMMANDS, verb) || !Array.isArray(args) || args.length !== COMMANDS[verb])
      return Promise.reject(new TypeError('Invalid native command or argument count'));
    if (args.some((arg) => typeof arg !== 'string' || arg.includes('\0') || Buffer.byteLength(arg, 'utf8') > 16384))
      return Promise.reject(new TypeError('Native arguments must be bounded UTF-8 strings'));
    if (verb === 'start' && !['filter', 'bypass'].includes(args[2]))
      return Promise.reject(new TypeError('An explicit filter or bypass mode is required'));
    if (!Number.isFinite(timeoutMs) || timeoutMs < 1 || timeoutMs > 60000)
      return Promise.reject(new TypeError('Invalid native request timeout'));
    if (!this.child || this.failed || this.closed || (this.closing && !allowClosing))
      return Promise.reject(new Error('Native host is unavailable; restart the application to reconnect'));
    if (this.pending.size >= 32) return Promise.reject(new Error('Too many native requests'));
    const id = this.nextId++;
    const line = ['NK1', id, verb, ...args.map((arg) => Buffer.from(arg, 'utf8').toString('base64'))].join('\t') + '\n';
    if (Buffer.byteLength(line) > MAX_REQUEST) return Promise.reject(new TypeError('Native request is too large'));
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        // Never allow a late start/record reply to leave a hidden live session.
        this._fail(new Error(`Native command timed out: ${verb}`));
      }, timeoutMs);
      this.pending.set(id, { resolve, reject, timer, verb });
      try {
        this.child.stdin.write(line, 'utf8', (error) => { if (error) this._fail(error); });
      } catch (error) { this._fail(error); }
    });
  }

  _receive(chunk) {
    if (this.failed || this.closed) return;
    this.buffer += chunk;
    if (Buffer.byteLength(this.buffer, 'utf8') > MAX_LINE) {
      this._fail(new Error('Native protocol response exceeded its size limit')); return;
    }
    let end;
    while ((end = this.buffer.indexOf('\n')) !== -1) {
      const line = this.buffer.slice(0, end).replace(/\r$/, '');
      this.buffer = this.buffer.slice(end + 1);
      if (!line) continue;
      let reply;
      try { reply = JSON.parse(line); }
      catch { this._fail(new Error('Malformed native protocol response')); return; }
      const replyId = typeof reply?.id === 'string' && /^[1-9][0-9]{0,15}$/.test(reply.id) ? Number(reply.id) : reply?.id;
      if (!reply || !Number.isSafeInteger(replyId) || replyId < 1 || typeof reply.ok !== 'boolean' ||
          (reply.ok ? !Object.hasOwn(reply, 'result') : typeof reply.error !== 'string')) {
        this._fail(new Error('Invalid native protocol response')); return;
      }
      const pending = this.pending.get(replyId);
      if (!pending) { this._fail(new Error('Unexpected native protocol response ID')); return; }
      this.pending.delete(replyId);
      clearTimeout(pending.timer);
      if (reply.ok) {
        pending.resolve(reply.result);
        if (pending.verb !== 'devices' && reply.result?.session) this.emit('status', reply.result);
      } else pending.reject(new Error(reply.error.slice(0, 4000)));
    }
  }

  _rejectPending(error) {
    for (const entry of this.pending.values()) { clearTimeout(entry.timer); entry.reject(error); }
    this.pending.clear();
  }

  _fail(error) {
    if (this.failed) return;
    this.failed = true;
    this._rejectPending(error);
    // Terminating our own host also terminates its WASAPI streams. No raw fallback.
    try { this.child?.stdin.end(); } catch { /* already closed */ }
    try { this.child?.kill(); } catch { /* already exited */ }
    this.emit('fault', error instanceof Error ? error : new Error(String(error)));
  }

  close() {
    if (this.closePromise) return this.closePromise;
    this.closing = true;
    this.closePromise = (async () => {
      if (!this.child || this.closed) return;
      if (!this.failed) {
        try { await this._request('shutdown', [], 2000, true); } catch { /* fail closed */ }
      }
      try { this.child.stdin.end(); } catch { /* already closed */ }
      if (this.closed) return;
      await new Promise((resolve) => {
        const finish = () => { clearTimeout(timer); this.removeListener('closed', finish); resolve(); };
        const timer = setTimeout(() => {
          try { this.child.kill(); } catch { /* already exited */ }
          finish();
        }, 2500);
        this.once('closed', finish);
      });
    })();
    return this.closePromise;
  }
}

module.exports = { NativeBridge, COMMANDS };
