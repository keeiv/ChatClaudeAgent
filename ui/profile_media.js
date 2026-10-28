// Crop geometry and canvas lifecycle. Images stay local; saving sends only the cropped PNG.
(function (root, factory) {
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.CCAProfileMedia = api;
})(typeof window === 'object' ? window : globalThis, function () {
  'use strict';
  function cropRect(width, height, zoom = 1, x = width / 2, y = height / 2) {
    const side = Math.min(width, height) / Math.max(1, Math.min(4, zoom));
    const cx = Math.max(side / 2, Math.min(width - side / 2, x));
    const cy = Math.max(side / 2, Math.min(height - side / 2, y));
    return { x: cx - side / 2, y: cy - side / 2, side, cx, cy };
  }
  function modelInfo(model, table, aliases) {
    let id = (model || '').trim().toLowerCase();
    const colon = id.indexOf(':');
    if (colon >= 0) {
      const base = id.slice(0, colon), tag = id.slice(colon + 1);
      const size = tag.split(/[-_]/)[0], suffix = tag.slice(size.length).replace(/^-instruct/, '');
      const ordinary = !suffix || /^[-_](?:q2_k|q3_k_[sml]|q4_[01k]|q4_k_[sm]|q5_[01]|q5_k_[sm]|q6_k|q8_0|f16|fp16|bf16|f32)$/.test(suffix);
      if (ordinary) {
        if (base === 'qwen2.5-coder') id = base + ':' + size;
        else if ((base === 'llama3.1' && ['latest', '8b', '70b', '405b'].includes(size)) ||
          (base === 'qwen3' && ['latest', '0.6b', '1.7b', '4b', '8b', '14b', '30b', '32b', '235b'].includes(size)) ||
          (base === 'dolphin3' && ['latest', '8b'].includes(size))) id = base;
      }
    }
    id = aliases[id] || id;
    return table.find(e => e.id === id || (id.startsWith(e.id + '-') && /^\d{8}$/.test(id.slice(e.id.length + 1)))) || null;
  }
  class Cropper {
    constructor(host, h, source, onReady, onError) {
      this.dead = false;
      this.zoom = 1;
      this.canvas = h('canvas', { width: 512, height: 512, tabindex: 0, 'aria-label': '裁切預覽，可拖曳或用方向鍵移動圖片' });
      this.slider = h('input', { type: 'range', min: 1, max: 4, step: 0.01, value: 1, 'aria-label': '縮放圖片', disabled: true });
      this.hint = h('div', { class: 'hint', role: 'status' }, '正在讀取圖片…');
      const stage = h('div', { class: 'crop-stage' }, this.canvas, h('div', { class: 'crop-circle', 'aria-hidden': 'true' }));
      host.append(stage, h('label', { class: 'crop-zoom' }, '縮放', this.slider), this.hint);
      this.slider.addEventListener('input', () => { this.zoom = Number(this.slider.value); this.draw(); });
      const canvas = this.canvas;
      canvas.addEventListener('pointerdown', e => {
        if (!this.ready || e.button !== 0) return;
        this.drag = { x: e.clientX, y: e.clientY, id: e.pointerId };
        canvas.setPointerCapture?.(e.pointerId);
        canvas.focus();
      });
      canvas.addEventListener('pointermove', e => {
        if (!this.drag || this.drag.id !== e.pointerId) return;
        const box = canvas.getBoundingClientRect(), ratio = this.rect.side / box.width;
        this.x -= (e.clientX - this.drag.x) * ratio;
        this.y -= (e.clientY - this.drag.y) * ratio;
        this.drag.x = e.clientX; this.drag.y = e.clientY;
        this.draw();
      });
      const end = () => { this.drag = null; };
      canvas.addEventListener('pointerup', end);
      canvas.addEventListener('pointercancel', end);
      canvas.addEventListener('lostpointercapture', end);
      canvas.addEventListener('keydown', e => {
        if (!this.ready) return;
        const moves = { ArrowLeft: [-1, 0], ArrowRight: [1, 0], ArrowUp: [0, -1], ArrowDown: [0, 1] };
        if (moves[e.key]) {
          e.preventDefault();
          const step = this.rect.side * (e.shiftKey ? 0.1 : 0.02);
          this.x += moves[e.key][0] * step; this.y += moves[e.key][1] * step;
          this.draw();
        }
      });
      this.image = new Image();
      this.image.crossOrigin = 'anonymous';
      this.image.onload = () => {
        if (this.dead) return;
        const width = this.image.naturalWidth, height = this.image.naturalHeight;
        if (!width || !height || width * height > 40000000) { onError('圖片尺寸太大或無法讀取，請使用 4,000 萬像素以內的圖片'); return; }
        this.x = width / 2; this.y = height / 2; this.ready = true;
        this.slider.disabled = false;
        this.hint.textContent = '拖曳或使用方向鍵調整位置；圓框是頭像顯示範圍。GIF 裁切後會變成靜態 PNG。';
        this.draw(); onReady();
      };
      this.image.onerror = () => { if (!this.dead) onError('讀不到圖片，請改選 PNG、JPG、GIF 或 WebP'); };
      this.image.src = source;
    }
    draw() {
      if (!this.ready || this.dead) return;
      const image = this.image;
      this.rect = cropRect(image.naturalWidth, image.naturalHeight, this.zoom, this.x, this.y);
      this.x = this.rect.cx; this.y = this.rect.cy;
      const ctx = this.canvas.getContext('2d');
      ctx.clearRect(0, 0, 512, 512);
      ctx.drawImage(image, this.rect.x, this.rect.y, this.rect.side, this.rect.side, 0, 0, 512, 512);
    }
    png() { return this.ready && !this.dead ? this.canvas.toDataURL('image/png') : ''; }
    destroy() {
      this.dead = true; this.ready = false; this.drag = null;
      this.image.onload = this.image.onerror = null;
      this.image.src = '';
      this.canvas.width = this.canvas.height = 0;
    }
  }
  return { cropRect, modelInfo, Cropper };
});
