// Decode a ConPTY recording without opening a browser or attaching to a live UI.
// Dev-only setup: npm install --prefix build/cli-qa --ignore-scripts @xterm/headless@5.5.0 @xterm/addon-unicode11@0.8.0
// node tests/replay_cli.cjs <recording.vt> <cols> <rows> <snapshot name> [output prefix]
const fs = require('node:fs');
const path = require('node:path');
const { Terminal } = require('../build/cli-qa/node_modules/@xterm/headless');
const { Unicode11Addon } = require('../build/cli-qa/node_modules/@xterm/addon-unicode11');

async function main() {
  const [file, c, r, name, prefix = `${file}.${name}`] = process.argv.slice(2);
  if (!file || !name) throw new Error('Expected recording, columns, rows, and snapshot name');
  let cols = Number(c), rows = Number(r);
  const snapshots = Object.fromEntries(fs.readFileSync(file + '.snaps', 'utf8').trim().split(/\r?\n/).map(l => l.split('=')));
  if (!(name in snapshots)) throw new Error(`Unknown snapshot: ${name}`);
  const term = new Terminal({ cols, rows, scrollback: 5000, allowProposedApi: true });
  term.loadAddon(new Unicode11Addon());
  term.unicode.activeVersion = '11';
  const recording = fs.readFileSync(file).subarray(0, Number(snapshots[name]));
  const sizes = fs.existsSync(file + '.sizes')
    ? fs.readFileSync(file + '.sizes', 'utf8').trim().split(/\r?\n/).map(l => l.split(' ').map(Number)) : [];
  let offset = 0;
  for (const [at, w, h] of sizes) {
    if (at >= recording.length) break;
    await new Promise(resolve => term.write(new Uint8Array(recording.subarray(offset, at)), resolve));
    term.resize(w, h);
    offset = at;
  }
  await new Promise(resolve => term.write(new Uint8Array(recording.subarray(offset)), resolve));
  cols = term.cols;
  rows = term.rows;
  const buffer = term.buffer.active;
  const screen = [];
  const lines = [];
  for (let y = 0; y < rows; y++) {
    const line = buffer.getLine(buffer.baseY + y);
    lines.push(line?.translateToString(true) || '');
    const cells = [];
    for (let x = 0; x < cols; x++) {
      const cell = line?.getCell(x);
      cells.push(cell ? {
        chars: cell.getChars(), width: cell.getWidth(), bold: !!cell.isBold(),
        fg: cell.isFgRGB() ? cell.getFgColor() : 0xCCCCCC,
        bg: cell.isBgRGB() ? cell.getBgColor() : 0x0C0C0C,
      } : { chars: '', width: 1, fg: 0xCCCCCC, bg: 0x0C0C0C });
    }
    screen.push(cells);
  }
  fs.mkdirSync(path.dirname(prefix), { recursive: true });
  fs.writeFileSync(prefix + '.json', JSON.stringify({ cols, rows, cursor: [buffer.cursorX, buffer.cursorY], screen }));
  fs.writeFileSync(prefix + '.txt', lines.join('\n'), 'utf8');
  console.log(lines.join('\n'));
  term.dispose();
}
main().catch(e => { console.error(e.message); process.exitCode = 1; });
