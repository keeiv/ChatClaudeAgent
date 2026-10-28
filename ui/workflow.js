/* ChatClaudeAgent workflow view. The host supplies actual dispatches and states. */
(function (root, factory) {
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.CCAWorkflow = api;
})(typeof window === 'object' ? window : globalThis, function () {
  'use strict';
  const COLORS = { claude: '#e8946f', codex: '#3bc495', gemini: '#88b6ff', local1: '#f47fbd', local2: '#b8a6f5' };
  const VENDORS = { claude: 'Claude', codex: 'GPT', gemini: 'Gemini', local1: '本地 1', local2: '本地 2' };
  const CENTERS = { claude: 185, codex: 815, gemini: 500, local1: 405, local2: 595 };
  const ACTIVE = new Set(['thinking', 'writing', 'reviewing', 'tool', 'preparing', 'loading', 'retry']);
  function edgePoints(from, to, positions = null) {
    if (positions) {
      const a = positions[from], b = positions[to];
      if (!a || !b || from === to) return [];
      if (a.y === b.y) { const d = a.x < b.x ? 1 : -1; return [[a.x + 120 * d, a.y], [b.x - 120 * d, b.y]]; }
      return [[a.x, a.y + 30], [a.x, (a.y + b.y) / 2], [b.x, (a.y + b.y) / 2], [b.x, b.y - 30]];
    }
    if (!(from in CENTERS) || !(to in CENTERS) || from === to) return [];
    const local = id => id.startsWith('local');
    if (!local(from) && !local(to)) {
      const direction = CENTERS[from] < CENTERS[to] ? 1 : -1;
      return [[CENTERS[from] + 105 * direction, 42], [CENTERS[to] - 105 * direction, 42]];
    }
    const startY = local(from) ? 100 : 72, endY = local(to) ? 100 : 72;
    return [[CENTERS[from], startY], [CENTERS[from], 85], [CENTERS[to], 85], [CENTERS[to], endY]];
  }
  function pipelineLayout(nodes) {
    const columns = Math.min(3, Math.max(1, nodes.length)), positions = {};
    nodes.forEach((node, i) => {
      const row = Math.floor(i / columns), column = row % 2 ? columns - 1 - i % columns : i % columns;
      positions[node.id] = { x: (column + .5) * 1000 / columns, y: 42 + row * 86 };
    });
    return { positions, height: Math.ceil(nodes.length / columns) * 86 + 2 };
  }
  function nodeName(node, names) {
    const agent = node.agent || node.id, current = names(agent);
    const name = current && current !== agent ? current : node.profile || current || agent;
    return node.name && node.name !== name ? node.name + ' · ' + name : name;
  }
  function packetFrames(points) {
    const distances = [0];
    for (let i = 1; i < points.length; i++) distances.push(distances[i - 1] + Math.hypot(points[i][0] - points[i - 1][0], points[i][1] - points[i - 1][1]));
    const total = distances.at(-1) || 1;
    return points.map(([x, y], i) => ({ transform: `translate(${x}px, ${y}px)`, offset: distances[i] / total }));
  }
  function stateText(flow, baseNames) {
    return (flow.nodes || []).map(n => `${nodeName(n, baseNames)}：${n.label}`).join('；');
  }
  function canAnimate(flow) { return flow.status === 'running' && (flow.nodes || []).length >= 2; }
  function el(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text != null) node.textContent = text;
    return node;
  }
  function svg(tag, attrs) {
    const node = document.createElementNS('http://www.w3.org/2000/svg', tag);
    for (const [key, value] of Object.entries(attrs)) node.setAttribute(key, value);
    return node;
  }
  function text(node, value) { if (node.textContent !== value) node.textContent = value; }

  class View {
    constructor(host, { onMode, onMotion } = {}) {
      this.host = host;
      this.onMode = onMode || (() => {});
      this.onMotion = onMotion || (() => {});
      this.seen = new Set();
      this.animations = new Set();
      this.nodes = new Map();
      this.media = typeof matchMedia === 'function' ? matchMedia('(prefers-reduced-motion: reduce)') : { matches: false };
      this.root = el('section', 'wf');
      this.title = el('span', 'wf-title', '協作流程');
      this.status = el('span', 'wf-status');
      this.status.setAttribute('role', 'status');
      this.summary = el('span', 'wf-stats');
      this.motionButton = el('button', 'wf-control');
      this.motionButton.type = 'button';
      this.motionButton.onclick = () => this.onMotion(!this.options.motion);
      this.collapseButton = el('button', 'wf-control');
      this.collapseButton.type = 'button';
      this.collapseButton.onclick = () => this.onMode(this.options.mode === 'compact' ? 'auto' : 'compact');
      const head = el('div', 'wf-head');
      head.append(this.title, this.status, this.summary, this.motionButton, this.collapseButton);
      this.graph = el('div', 'wf-graph');
      this.wires = svg('svg', { viewBox: '0 0 1000 158', preserveAspectRatio: 'none', 'aria-hidden': 'true', class: 'wf-wires' });
      this.mainWire = svg('path', { d: 'M290 42 H710', class: 'wf-wire' });
      this.pipelineWires = svg('g', {});
      this.wires.append(this.mainWire, this.pipelineWires);
      this.bus = svg('path', { d: '', class: 'wf-wire' });
      this.wires.append(this.bus);
      this.packetLayer = svg('g', { class: 'wf-packets' });
      this.wires.append(this.packetLayer);
      this.graph.append(this.wires);
      this.compact = el('div', 'wf-compact');
      this.caption = el('div', 'wf-caption');
      this.root.append(head, this.graph, this.compact, this.caption);
      host.append(this.root);
      this.changed = () => { if (this.flow) this.update(this.flow, this.options); };
      this.media.addEventListener?.('change', this.changed);
      document.addEventListener('visibilitychange', this.changed);
    }
    stopPackets() {
      for (const animation of this.animations) animation.cancel();
      this.animations.clear();
      this.packetLayer.replaceChildren();
    }
    update(flow, options) {
      this.flow = flow || { nodes: [] };
      this.options = options;
      flow = this.flow;
      const baseNames = options.names;
      const names = id => { const node = (flow.nodes || []).find(n => n.id === id); return node ? nodeName(node, baseNames) : baseNames(id); };
      const pipeline = flow.layout === 'pipeline';
      const layout = pipeline ? pipelineLayout(flow.nodes || []) : null;
      const motion = options.motion !== false && !this.media.matches && !document.hidden;
      const runKey = options.conversationId + ':' + (flow.runId || '');
      if (runKey !== this.runKey) { this.seen.clear(); this.runKey = runKey; }
      const signature = runKey + ':' + flow.layout + ':' + (flow.nodes || []).map(n => n.id + '/' + n.agent).join(',');
      if (signature !== this.signature) {
        this.signature = signature;
        this.stopPackets();
        for (const entry of this.nodes.values()) entry.node.remove();
        this.nodes.clear();
        for (const data of flow.nodes || []) {
          const node = el('div', 'wf-node');
          node.dataset.agent = data.agent || data.id;
          node.dataset.nodeId = data.id;
          const lamp = el('span', 'wf-lamp');
          lamp.setAttribute('aria-hidden', 'true');
          const name = el('span', 'wf-name');
          const vendor = el('span', 'wf-vendor', VENDORS[data.agent || data.id] || data.agent || data.id);
          const label = el('span', 'wf-label');
          const top = el('div', 'wf-node-head');
          top.append(lamp, name, vendor);
          node.append(top, label);
          this.graph.append(node);
          this.nodes.set(data.id, { node, lamp, name, vendor, label });
        }
      }
      // A direct message with one agent has no diagram (the host sends no nodes).
      this.root.hidden = options.mode === 'off' || (flow.nodes || []).length < 2;
      this.root.dataset.mode = options.mode;
      this.root.classList.toggle('wf-still', !motion);
      this.root.classList.toggle('wf-pipeline', pipeline);
      this.graph.style.height = layout ? layout.height + 'px' : '';
      this.mainWire.style.display = pipeline ? 'none' : '';
      this.pipelineWires.replaceChildren();
      if (pipeline) {
        const items = flow.nodes || [];
        items.slice(1).forEach((item, i) => {
          const points = edgePoints(items[i].id, item.id, layout.positions);
          this.pipelineWires.append(svg('path', { d: points.map((p, n) => (n ? 'L' : 'M') + p.join(' ')).join(' '), class: 'wf-wire' }));
        });
      }
      const hasLocals = !pipeline && (flow.nodes || []).some(n => n.id.startsWith('local'));
      this.root.classList.toggle('wf-with-locals', hasLocals);
      this.bus.style.display = hasLocals ? '' : 'none';
      const busPath = hasLocals ? 'M185 72 V85 H815 V72' + (flow.nodes || [])
        .filter(n => n.id === 'local1' || n.id === 'local2').map(n => ` M${CENTERS[n.id]} 85 V100`).join('') : '';
      if (this.bus.getAttribute('d') !== busPath) this.bus.setAttribute('d', busPath);
      this.wires.setAttribute('viewBox', '0 0 1000 ' + (layout ? layout.height : hasLocals ? 158 : 88));
      text(this.status, flow.label || '等待工作');
      text(this.summary, [flow.round ? `第 ${flow.round} 輪` : '', flow.files ? `${flow.files} 檔案 +${flow.added} −${flow.removed}` : ''].filter(Boolean).join(' · '));
      text(this.motionButton, motion ? '動態開啟' : '減少動態');
      this.motionButton.setAttribute('aria-pressed', String(options.motion !== false));
      this.motionButton.title = this.media.matches ? '依照系統設定減少動態效果' : '切換呼吸燈、轉圈與交接圓點';
      text(this.collapseButton, options.mode === 'compact' ? '展開' : '收合');
      this.collapseButton.setAttribute('aria-expanded', String(options.mode !== 'compact'));
      text(this.compact, stateText(flow, baseNames));
      for (const data of flow.nodes || []) {
        const entry = this.nodes.get(data.id);
        entry.node.dataset.status = data.status;
        const position = layout?.positions[data.id];
        entry.node.style.left = position ? (position.x / 10 - 12) + '%' : '';
        entry.node.style.top = position ? (position.y - 30) + 'px' : '';
        entry.node.style.width = position ? '24%' : '';
        entry.node.classList.toggle('wf-active', ACTIVE.has(data.status));
        entry.node.title = [names(data.id), data.role, data.label, data.detail, data.model].filter(Boolean).join(' · ');
        text(entry.name, names(data.id));
        entry.vendor.hidden = names(data.id) === VENDORS[data.agent || data.id];
        text(entry.label, data.label);
        text(entry.lamp, data.status === 'done' ? '✓' : data.status === 'failed' ? '×' : '');
      }
      const events = flow.transfers || [];
      const last = events.at(-1);
      text(this.caption, last ? `${names(last.from)} → ${names(last.to)} · ${last.label}` : '思考與撰寫顯示在節點，送審與交付建議才沿線傳送。');
      if (!motion || !canAnimate(flow) || options.mode !== 'auto') this.stopPackets();
      for (const event of events) {
        if (this.seen.has(event.id)) continue;
        this.seen.add(event.id);
        const age = Date.now() - event.ts;
        if (!motion || !canAnimate(flow) || options.mode !== 'auto' || age < 0 || age >= 1300) continue;
        const points = edgePoints(event.from, event.to, layout?.positions);
        if (points.length < 2) continue;
        const target = (flow.nodes || []).find(n => n.id === event.to);
        const agent = target?.agent || event.to;
        const fill = agent in COLORS ? 'var(--' + agent + '-name)' : 'var(--signal)';
        const packet = svg('circle', { r: 4.5, fill, 'data-transfer': event.id });
        this.packetLayer.append(packet);
        if (typeof packet.animate !== 'function') { packet.remove(); continue; }
        const animation = packet.animate(packetFrames(points), { duration: 1300, easing: 'linear', fill: 'none' });
        animation.currentTime = age;
        this.animations.add(animation);
        animation.onfinish = () => { packet.remove(); this.animations.delete(animation); };
      }
      // The host already caps dispatches at eight; do not retain earlier event IDs.
      this.seen = new Set(events.map(e => e.id));
    }
    destroy() {
      this.stopPackets();
      this.media.removeEventListener?.('change', this.changed);
      document.removeEventListener('visibilitychange', this.changed);
      this.root.remove();
    }
  }
  return { View, edgePoints, packetFrames, stateText, canAnimate, pipelineLayout };
});
