// Markdown destinations stay inert until a deliberate, native-validated folder action.
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const load = path => vm.runInNewContext(fs.readFileSync(path, 'utf8') + '\nMD;', { URL });
const md = load('ui/md.js');
let checks = 0;
const check = (value, label) => { assert.ok(value, label); checks++; };
for (const input of [
  '[開發文件](<C:/Users/dev/CCA/docs/workbench.md:47>)',
  '[桌面版](<C:/My Project (測試)/CCA-1.0.0-terra.exe>)',
  '[說明](/C:/MyProject/main.cpp:123)',
  '[測試](C:\\MyProject\\main.cpp:42:3)',
  '[文件](file:///C:/My%20Project/readme.md#L23)',
  '[文件](<C:/My Project/a & b.md>)',
]) {
  const html = md.render(input);
  check(html.includes('class="link local-link"') && !html.includes('](<'), 'local destination renders: ' + input);
}
check(md.localTarget('C:/main.cpp:47').line === 47, 'line suffix retained separately');
check(md.localTarget('file:///C:/My%20Project/readme.md#L23').line === 23, 'file URL fragment retained');
for (const target of ['javascript:alert(1)', 'file://server/share/a.exe', '\\\\server\\share\\a.exe', 'C:/a.exe:stream',
  'C:/bad\nfile.md', 'C:/file.md:99999999999999999', '//server/share', 'C:/file?.md']) {
  check(md.localTarget(target) === null, 'unsupported target rejected: ' + target);
}
const dangerous = md.render('[<img src=x onerror=alert(1)>](<C:/good.txt>) <script>alert(1)</script>');
check(!dangerous.includes('<img') && !dangerous.includes('<script>'), 'HTML stays escaped in labels and prose');
check(!md.render('[bad](javascript:alert(1))').includes('<a '), 'script scheme stays noninteractive');
check(md.render('[**文件**](<C:/docs/readme.md>)').includes('<strong>文件</strong>'), 'label emphasis preserved');
const tick = String.fromCharCode(96);
check(md.render(tick + '[文件](<C:/file.md>)' + tick).includes('<code class="inline">[文件]'), 'code is not linkified');
check(md.render('https://example.test ' + tick + 'x' + tick).includes('<code class="inline">x</code>'), 'autolinks do not consume code tokens');
check(md.render('[網頁](https://example.test/a(b)?a=1&b=2)').includes('href="https://example.test/a(b)?a=1&amp;b=2"'), 'balanced web destination and query preserved');
check(!md.render('\u00000\u0000').includes('<a'), 'source cannot forge internal placeholders');
console.log(checks + ' Markdown link checks passed');
