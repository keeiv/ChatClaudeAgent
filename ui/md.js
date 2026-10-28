// Discord-flavoured Markdown for AI replies. Every piece of source text is
// HTML-escaped before any tag is added, so model output can never inject markup.
const MD = (() => {
  const esc = (s) => String(s)
    .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;').replace(/'/g, '&#39;');

  function localTarget(value) {
    let path = value;
    if (/^file:\/\/\//i.test(path)) {
      try {
        const url = new URL(path);
        if (url.hostname) return null;
        path = decodeURIComponent(url.pathname) + url.hash;
      } catch { return null; }
    }
    path = path.replace(/^\/([a-z]:[\\/])/i, '$1');
    const position = path.match(/(?::(\d+)(?::(\d+))?|#L(\d+)(?:C(\d+))?)$/);
    const line = position ? Number(position[1] || position[3]) : 0;
    if (position) path = path.slice(0, position.index);
    if (!/^[a-z]:[\\/]/i.test(path) || /[\x00-\x1f<>"|?*]/.test(path) || path.slice(2).includes(':')) return null;
    if (path.length > 32700 || !Number.isSafeInteger(line) || line < 0) return null;
    return { path: path.replace(/\\/g, '/'), line };
  }

  function inline(text) {
    const tokens = [];
    const keep = html => { tokens.push(html); return `\u0000${tokens.length - 1}\u0000`; };
    let s = String(text).replace(/\u0000/g, '\ufffd').replace(/`([^`\n]+)`/g,
      (_, c) => keep(`<code class="inline">${esc(c)}</code>`));
    // Read destinations before escaping HTML; angle destinations can contain spaces and parentheses.
    s = s.replace(/\[([^\]\n]+)\]\((<[^>\n]+>|(?:[^()\n]|\([^()\n]*\))+)\)/g, (raw, label, destination) => {
      const target = destination.startsWith('<') ? destination.slice(1, -1) : destination;
      const local = localTarget(target);
      const caption = esc(label).replace(/\u0000(\d+)\u0000/g, (_, i) => tokens[+i] || '').replace(/\*\*([^*\n]+)\*\*/g, '<strong>$1</strong>');
      if (local) return keep(`<a href="#" class="link local-link" data-local-path="${esc(local.path)}" data-line="${local.line}" ` +
        `data-tip="${esc(local.path + (local.line ? ':' + local.line : ''))} · 開啟所在資料夾">${caption}</a>`);
      if (!/^https?:\/\//i.test(target) || /[\x00-\x20<>]/.test(target)) return raw;
      return keep(`<a href="${esc(target)}" class="link">${caption}</a>`);
    });
    s = s.replace(/(^|[\s(（])(https?:\/\/[^\s<\u0000]+[^\s<\u0000.,:;'"\)\]，。；：）])/g,
      (_, p, u) => p + keep(`<a href="${esc(u)}" class="link">${esc(u)}</a>`));
    s = esc(s);
    s = s.replace(/\*\*([^*\n]+?)\*\*/g, '<strong>$1</strong>');
    s = s.replace(/__([^_\n]+?)__/g, '<u>$1</u>');
    s = s.replace(/(^|[^*\w])\*([^*\n]+?)\*(?![*\w])/g, '$1<em>$2</em>');
    s = s.replace(/(^|[^_\w])_([^_\n]+?)_(?![_\w])/g, '$1<em>$2</em>');
    s = s.replace(/~~([^~\n]+?)~~/g, '<s>$1</s>');
    return s.replace(/\u0000(\d+)\u0000/g, (_, i) => tokens[+i] || '');
  }

  function codeBlock(code, lang) {
    const l = (lang || '').toLowerCase();
    let body;
    if (l === 'diff' || l === 'patch') {
      body = code.split('\n').map((line) => {
        const cls = line.startsWith('+') ? 'd-add' : line.startsWith('-') ? 'd-del' : line.startsWith('@@') ? 'd-hunk' : '';
        return `<span class="${cls}">${esc(line) || ' '}</span>`;
      }).join('\n');
    } else {
      body = esc(code);
    }
    return `<div class="codeblock"><pre><code>${body}</code></pre>` +
      `<button class="code-copy" type="button" title="è¤‡è£½">è¤‡è£½</button></div>`;
  }

  const isFence = (l) => /^\s*```/.test(l);
  const isList = (l) => /^\s*([-*+]|\d+[.)])\s+/.test(l);
  const isQuote = (l) => /^\s*>/.test(l);
  const isHeading = (l) => /^#{1,3}\s+\S/.test(l);
  const isRule = (l) => /^\s*(-{3,}|\*{3,}|_{3,})\s*$/.test(l);
  const isTableSep = (l) => /^\s*\|?\s*:?-{2,}:?\s*(\|\s*:?-{2,}:?\s*)+\|?\s*$/.test(l);
  const cells = (l) => l.trim().replace(/^\||\|$/g, '').split('|').map((c) => c.trim());

  function render(src) {
    const lines = String(src || '').replace(/\r\n/g, '\n').split('\n');
    const out = [];
    let i = 0;
    while (i < lines.length) {
      const line = lines[i];
      if (isFence(line)) {
        const lang = line.trim().slice(3).trim();
        const buf = [];
        i++;
        while (i < lines.length && !/^\s*```\s*$/.test(lines[i])) buf.push(lines[i++]);
        i++;
        out.push(codeBlock(buf.join('\n'), lang));
        continue;
      }
      if (!line.trim()) { i++; continue; }
      if (isHeading(line)) {
        const m = line.match(/^(#{1,3})\s+(.*)$/);
        out.push(`<div class="md-h md-h${m[1].length}">${inline(m[2])}</div>`);
        i++;
        continue;
      }
      if (isRule(line)) { out.push('<hr class="md-hr">'); i++; continue; }
      if (isQuote(line)) {
        const buf = [];
        while (i < lines.length && isQuote(lines[i])) buf.push(lines[i++].replace(/^\s*>\s?/, ''));
        out.push(`<blockquote class="md-quote">${render(buf.join('\n'))}</blockquote>`);
        continue;
      }
      if (line.includes('|') && i + 1 < lines.length && isTableSep(lines[i + 1])) {
        const head = cells(line);
        i += 2;
        const rows = [];
        while (i < lines.length && lines[i].includes('|') && lines[i].trim()) rows.push(cells(lines[i++]));
        out.push('<div class="md-table-wrap"><table class="md-table"><thead><tr>' +
          head.map((c) => `<th>${inline(c)}</th>`).join('') + '</tr></thead><tbody>' +
          rows.map((r) => '<tr>' + r.map((c) => `<td>${inline(c)}</td>`).join('') + '</tr>').join('') +
          '</tbody></table></div>');
        continue;
      }
      if (isList(line)) {
        const ordered = /^\s*\d+[.)]/.test(line);
        const items = [];
        while (i < lines.length && (isList(lines[i]) || (/^\s{2,}\S/.test(lines[i]) && items.length))) {
          const l = lines[i++];
          if (isList(l)) {
            const indent = l.match(/^\s*/)[0].length;
            items.push({ indent, text: l.replace(/^\s*([-*+]|\d+[.)])\s+/, '') });
          } else {
            items[items.length - 1].text += ' ' + l.trim();
          }
        }
        const base = items[0].indent;
        const tag = ordered ? 'ol' : 'ul';
        out.push(`<${tag} class="md-list">` + items.map((it) =>
          `<li class="${it.indent > base ? 'nested' : ''}">${inline(it.text)}</li>`).join('') + `</${tag}>`);
        continue;
      }
      const para = [];
      while (i < lines.length && lines[i].trim() && !isFence(lines[i]) && !isHeading(lines[i]) &&
             !isQuote(lines[i]) && !isList(lines[i]) && !isRule(lines[i]) &&
             !(lines[i].includes('|') && i + 1 < lines.length && isTableSep(lines[i + 1]))) {
        para.push(inline(lines[i++]));
      }
      out.push(`<p>${para.join('<br>')}</p>`);
    }
    return out.join('');
  }

  return { render, esc, localTarget };
})();
