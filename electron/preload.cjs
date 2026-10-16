'use strict';
const { contextBridge, ipcRenderer } = require('electron');

const invoke = (name, payload) => ipcRenderer.invoke(`nk:${name}`, payload);
const subscribe = (name, callback) => {
  if (typeof callback !== 'function') throw new TypeError('A callback is required');
  const listener = (_event, value) => callback(value); // Never expose the IPC event.
  ipcRenderer.on(`nk:${name}`, listener);
  return () => ipcRenderer.removeListener(`nk:${name}`, listener);
};

contextBridge.exposeInMainWorld('noisekiller', Object.freeze({
  bootstrap: () => invoke('bootstrap'),
  refreshDevices: () => invoke('refreshDevices'),
  start: (options) => invoke('start', options),
  stop: () => invoke('stop'),
  record: (options) => invoke('record', options),
  importReference: () => invoke('importReference'),
  deleteProfile: () => invoke('deleteProfile'),
  processFile: () => invoke('processFile'),
  onStatus: (callback) => subscribe('status', callback),
  onBackendError: (callback) => subscribe('backendError', callback),
  windowControl: (action) => invoke('windowControl', action),
  openGuide: () => invoke('openGuide'),
}));
