// Writes the resource scripts.
//   node tools/gen_resources.js       build/resources.rc for ChatClaudeAgent.exe: the manifest, the icon,
//                                     the version info, and every file under ui/ as RCDATA named
//                                     UI_<PATH> (upper-case, non-alphanumerics -> '_'), the same
//                                     rule main.cpp uses to look them up.
//   node tools/gen_resources.js cli   build/cli.rc for ChatClaudeAgent-CLI.exe: manifest, icon, version info.
const fs = require('fs');
const path = require('path');

const root = path.resolve(__dirname, '..');
const uiDir = path.join(root, 'ui');
const cli = process.argv[2] === 'cli';
const out = path.join(root, 'build', cli ? 'cli.rc' : 'resources.rc');

function walk(dir) {
  const files = [];
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) files.push(...walk(full));
    else if (entry.name !== 'mock.js') files.push(full);  // the browser mock never ships
  }
  return files;
}

const esc = (p) => p.replace(/\\/g, '\\\\');
const description = cli ? 'ChatClaudeAgent CLI' : 'ChatClaudeAgent';
const fileName = cli ? 'ChatClaudeAgent-CLI.exe' : 'ChatClaudeAgent.exe';
const lines = ['#include <windows.h>', `#include "${esc(path.join(root, 'src', 'version.hpp'))}"`, ''];
lines.push(`RUNTIME_AGENT_PY RCDATA "${esc(path.join(root, 'tools', 'agent_runtime.py'))}"`);
lines.push(`GEMINI_ACCOUNT_JS RCDATA "${esc(path.join(root, 'tools', 'gemini_account.cjs'))}"`);
lines.push(`SANDBOX_MCP_PY RCDATA "${esc(path.join(root, 'tools', 'sandbox_mcp.py'))}"`);
lines.push(`QUALITY_WORKER_PY RCDATA "${esc(path.join(root, 'tools', 'quality_worker.py'))}"`);
lines.push(`BROWSER_WORKER_JS RCDATA "${esc(path.join(root, 'tools', 'browser_worker.cjs'))}"`);
lines.push(`1 RT_MANIFEST "${esc(path.join(root, 'src', 'app.manifest'))}"`);
// A special edition has its own product icon (assets/editions/<id>/<id>.ico); the ordinary edition keeps the CCA mark.
const edition = (process.env.CCA_EDITION || '').toLowerCase();
const icon = edition ? path.join(root, 'assets', 'editions', edition, edition + '.ico') : path.join(root, 'assets', 'cca.ico');
if (fs.existsSync(icon)) lines.push(`101 ICON "${esc(icon)}"`);
lines.push('',
  // Task Manager and the file's Properties show these names.
  '1 VERSIONINFO',
  'FILEVERSION CCA_VERSION_NUM',
  'PRODUCTVERSION CCA_VERSION_NUM',
  'FILEOS VOS_NT_WINDOWS32',
  `FILETYPE VFT_APP`,
  'BEGIN',
  '  BLOCK "StringFileInfo"',
  '  BEGIN',
  '    BLOCK "040404B0"',
  '    BEGIN',
  `      VALUE "FileDescription", "${description}"`,
  '      VALUE "ProductName", CCA_PRODUCT',
  '      VALUE "FileVersion", CCA_VERSION',
  '      VALUE "ProductVersion", CCA_VERSION',
  // Empty for the ordinary edition; the updater tells editions apart by this field and the manifest.
  '      VALUE "CCAEdition", CCA_EDITION',
  `      VALUE "OriginalFilename", "${fileName}"`,
  '    END',
  '  END',
  '  BLOCK "VarFileInfo"',
  '  BEGIN',
  '    VALUE "Translation", 0x0404, 1200',
  '  END',
  'END',
  '');
if (!cli) {
  for (const file of walk(uiDir)) {
    const rel = path.relative(uiDir, file).split(path.sep).join('/');
    const name = 'UI_' + rel.toUpperCase().replace(/[^A-Z0-9]/g, '_');
    lines.push(`${name} RCDATA "${esc(file)}"`);
  }
  lines.push('');
}

fs.mkdirSync(path.dirname(out), { recursive: true });
// rc.exe reads UTF-16 with a BOM, which keeps non-ASCII folder names intact.
fs.writeFileSync(out, '﻿' + lines.join('\r\n'), 'utf16le');
console.log(cli ? 'cli.rc written' : `resources.rc: ${lines.filter((l) => l.includes('RCDATA')).length} UI files`);
