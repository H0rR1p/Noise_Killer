'use strict';
const { spawn } = require('node:child_process');
let executable;
try { executable = require('electron'); }
catch {
  console.error('Electron runtime is not installed. Run npm ci, then npx install-electron, inside electron/.');
  process.exit(1);
}
const env = { ...process.env };
delete env.ELECTRON_RUN_AS_NODE;
const child = spawn(executable, [__dirname, ...process.argv.slice(2)], { env, stdio: 'inherit', windowsHide: false, shell: false });
child.on('error', (error) => { console.error(error.message); process.exitCode = 1; });
child.on('exit', (code) => { process.exitCode = code ?? 1; });
