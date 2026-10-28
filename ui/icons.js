// Line icons drawn for CCA on a 24px grid. Stroke icons inherit currentColor.
const ICONS = {
  // The 1.0 handoff mark is generated from tools/make_logo.py.
  logo: '<g transform="scale(.75)"><g fill="currentColor" stroke="none"><path d="M20 3L8 3L3 8L3 16L8 21L20 21L20 16L10 16L8 14L8 10L10 8L20 8Z"/><path d="M12 29L24 29L29 24L29 16L24 11L12 11L12 16L22 16L24 18L24 22L22 24L12 24Z"/></g></g>',
  puzzle: '<path d="M10 4.5a2 2 0 1 1 4 0V6h3.5A1.5 1.5 0 0 1 19 7.5V11h-1.5a2 2 0 1 0 0 4H19v3.5a1.5 1.5 0 0 1-1.5 1.5H14v-1.5a2 2 0 1 0-4 0V20H6.5A1.5 1.5 0 0 1 5 18.5V15h1.5a2 2 0 1 0 0-4H5V7.5A1.5 1.5 0 0 1 6.5 6H10z"/>',
  plus: '<path d="M12 5v14M5 12h14"/>',
  sliders: '<path d="M4 7h9M17 7h3M4 17h3M11 17h9"/><circle cx="15" cy="7" r="2"/><circle cx="9" cy="17" r="2"/>',
  x: '<path d="M6.5 6.5l11 11M17.5 6.5l-11 11"/>',
  min: '<path d="M6 12.5h12" stroke-width="1.3"/>',
  max: '<rect x="6.5" y="6.5" width="11" height="11" rx="1" stroke-width="1.3"/>',
  restore: '<rect x="6.5" y="9" width="8.5" height="8.5" rx="1" stroke-width="1.3"/><path d="M9 9V7.5A1 1 0 0 1 10 6.5h6.5a1 1 0 0 1 1 1V14a1 1 0 0 1-1 1H15" stroke-width="1.3"/>',
  close: '<path d="M7 7l10 10M17 7L7 17" stroke-width="1.3"/>',
  folder: '<path d="M3.5 7.5a2 2 0 0 1 2-2h3.6l2 2h7.4a2 2 0 0 1 2 2v7a2 2 0 0 1-2 2h-13a2 2 0 0 1-2-2z"/>',
  stop: '<rect x="7" y="7" width="10" height="10" rx="2" fill="currentColor" stroke="none"/>',
  chevron: '<path d="M7 10l5 5 5-5"/>',
  check: '<path d="M5.5 12.5l4 4 9-9"/>',
  alert: '<path d="M12 4.5l8.5 15h-17z"/><path d="M12 10v4.2"/><circle cx="12" cy="17" r=".6" fill="currentColor"/>',
  info: '<circle cx="12" cy="12" r="8.5"/><path d="M12 11v5"/><circle cx="12" cy="8" r=".6" fill="currentColor"/>',
  terminal: '<rect x="3.5" y="5" width="17" height="14" rx="2"/><path d="M7.5 10l2.5 2-2.5 2M12.5 15h4"/>',
  file: '<path d="M6.5 3.5h7l4 4v13h-11z"/><path d="M13.5 3.5v4h4"/>',
  // A page with one added and one removed line: the review pane.
  diff: '<path d="M6.5 3.5h7l4 4v13h-11z"/><path d="M13.5 3.5v4h4"/><path d="M9.5 12.5h5M12 10v5M9.5 17.5h5"/>',
  search: '<circle cx="11" cy="11" r="6"/><path d="M19.5 19.5l-4-4"/>',
  globe: '<circle cx="12" cy="12" r="8.5"/><path d="M3.5 12h17M12 3.5c2.5 2.5 3.5 5.5 3.5 8.5s-1 6-3.5 8.5c-2.5-2.5-3.5-5.5-3.5-8.5s1-6 3.5-8.5z"/>',
  wrench: '<path d="M14.5 5.5a4 4 0 0 0-4.9 5.2L4.5 15.8a1.8 1.8 0 0 0 2.6 2.6l5.1-5.1a4 4 0 0 0 5.2-4.9l-2.6 2.6-2.4-.6-.6-2.4z"/>',
  users: '<circle cx="9" cy="8.5" r="3.2"/><path d="M3.5 19c.6-3.2 2.8-5 5.5-5s4.9 1.8 5.5 5"/><path d="M15.5 5.6a3.2 3.2 0 0 1 0 5.8M17.5 14.4c1.6.7 2.6 2.3 3 4.6"/>',
  trash: '<path d="M5 7h14M10 7V4.5h4V7M7 7l.8 12.5h8.4L17 7"/>',
  refresh: '<path d="M19.5 12a7.5 7.5 0 1 1-2.2-5.3"/><path d="M19.5 4.5v4.5H15"/>',
  shield: '<path d="M12 3.5l7 2.8v5.4c0 4.4-3 7.4-7 8.8-4-1.4-7-4.4-7-8.8V6.3z"/>',
  arrow: '<path d="M5 12h13M13 7l5 5-5 5"/>',
  copy: '<rect x="8.5" y="8.5" width="11" height="11" rx="2"/><path d="M15.5 8.5V6a1.5 1.5 0 0 0-1.5-1.5H6A1.5 1.5 0 0 0 4.5 6v8A1.5 1.5 0 0 0 6 15.5h2.5"/>',
  external: '<path d="M14 4.5h5.5V10M19.5 4.5L11 13M17.5 14v5.5h-13v-13H10"/>',
  inbox: '<path d="M4.5 13.5l2-8h11l2 8v5h-15z"/><path d="M4.5 13.5h4.5l1 2h4l1-2h4.5"/>',
  help: '<circle cx="12" cy="12" r="8.5"/><path d="M9.6 9.5a2.5 2.5 0 1 1 3.3 2.4c-.6.2-.9.7-.9 1.3v.6"/><circle cx="12" cy="16.6" r=".6" fill="currentColor"/>',
  chart: '<path d="M5 19.5V12M10 19.5V5M15 19.5v-9M20 19.5v-5"/>',
  eye: '<path d="M2.5 12S6 5.5 12 5.5 21.5 12 21.5 12 18 18.5 12 18.5 2.5 12 2.5 12z"/><circle cx="12" cy="12" r="2.8"/>',
  pencil: '<path d="M4.5 19.5l1-4.2L15.8 5a1.8 1.8 0 0 1 2.6 0l.6.6a1.8 1.8 0 0 1 0 2.6L8.7 18.5z"/><path d="M13.8 7l3.2 3.2"/>',
  bolt: '<path d="M13 3L5.5 13.5H11L10 21l8.5-11H13z"/>',
  at: '<circle cx="12" cy="12" r="3.6"/><path d="M15.6 12v1.3a2.4 2.4 0 0 0 4.8 0V12a8.4 8.4 0 1 0-3.3 6.7"/>',
  hash: '<path d="M9 3L6 21M18 3l-3 18M3 9h18M2 15h18"/>',
  branch: '<circle cx="7" cy="6" r="2"/><circle cx="7" cy="18" r="2"/><circle cx="17" cy="8" r="2"/><path d="M7 8v8M17 10c0 4-4 4-8.6 6.6"/>',
  list: '<path d="M9 7h11M9 12h11M9 17h11"/><circle cx="5" cy="7" r=".9" fill="currentColor"/><circle cx="5" cy="12" r=".9" fill="currentColor"/><circle cx="5" cy="17" r=".9" fill="currentColor"/>',
  circle: '<circle cx="12" cy="12" r="7"/>',
  dot: '<circle cx="12" cy="12" r="3.5" fill="currentColor" stroke="none"/>',
  question: '<path d="M4.5 6.5a2 2 0 0 1 2-2h11a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H11l-4.5 3.5v-3.5h0a2 2 0 0 1-2-2z"/><path d="M10.4 8.6a1.7 1.7 0 1 1 2.3 1.6c-.4.2-.7.5-.7 1v.3"/><circle cx="12" cy="13.6" r=".55" fill="currentColor"/>',
  clock: '<circle cx="12" cy="12" r="8.5"/><path d="M12 7.5V12l3 2"/>',
  code: '<path d="M8.5 7.5L4 12l4.5 4.5M15.5 7.5L20 12l-4.5 4.5"/>',
  key: '<circle cx="8" cy="15" r="3.5"/><path d="M10.5 12.5l8-8M15.5 7.5l2 2M17 6l2 2"/>',
  download: '<path d="M12 4.5v10M7.5 10l4.5 4.5 4.5-4.5M5 19.5h14"/>',
};

function icon(name, size = 20, cls = '') {
  const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  svg.setAttribute('viewBox', '0 0 24 24');
  svg.setAttribute('width', size);
  svg.setAttribute('height', size);
  svg.setAttribute('fill', 'none');
  svg.setAttribute('stroke', 'currentColor');
  svg.setAttribute('stroke-width', '1.8');
  svg.setAttribute('stroke-linecap', 'round');
  svg.setAttribute('stroke-linejoin', 'round');
  svg.setAttribute('aria-hidden', 'true');
  if (cls) svg.setAttribute('class', cls);
  svg.innerHTML = ICONS[name] || '';
  return svg;
}
