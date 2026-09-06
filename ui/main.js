import './compost/components/compost-slider.js';

const encoder = new TextEncoder();
const decoder = new TextDecoder();
const send = text => parent.postMessage(encoder.encode(text).buffer, '*');
const $ = selector => document.querySelector(selector);
const controls = new Map([...document.querySelectorAll('compost-slider')]
  .map(control => [Number(control.getAttribute('parameter-id')), control]));
const hold = $('#hold');
const orders = [...document.querySelectorAll('[data-order]')];
const values = {0: 1, 5: 1, 6: 65, 8: 0, 9: 0};
let state = {clock: 0, bpm: 120, held: 0, pitch: -1, key: -1, notes: []};
let connected = false;
const noteName = n => ['c', 'c♯', 'd', 'd♯', 'e', 'f', 'f♯', 'g', 'g♯', 'a', 'a♯', 'b'][n % 12] + (Math.floor(n / 12) - 1);

function sync() {
  for (const [id, control] of controls)
    if (!control.hasAttribute('data-editing')) control.value = values[id];
  hold.setAttribute('aria-pressed', String(values[8] === 1));
  for (const button of orders) button.setAttribute('aria-pressed', String(+button.dataset.order === values[9]));
  renderStatus();
}
function edit(id, value) {
  values[id] = value;
  send(`begin:${id}`); send(`value:${id}:${value}`); send(`end:${id}`);
  sync();
}
hold.addEventListener('click', () => edit(8, 1 - values[8]));
for (const button of orders) button.addEventListener('click', () => edit(9, +button.dataset.order));
$('#clear').addEventListener('click', () => send('clear'));
addEventListener('parameter-begin', ({detail: {parameterID}}) => {
  controls.get(Number(parameterID))?.setAttribute('data-editing', '');
  send(`begin:${parameterID}`);
});
addEventListener('parameter-edit', ({detail: {parameterID, value}}) => {
  values[parameterID] = Number(value);
  send(`value:${parameterID}:${value}`);
});
addEventListener('parameter-end', ({detail}) => {
  if (detail.cancelled) {
    values[detail.parameterID] = Number(detail.value);
    send(`value:${detail.parameterID}:${detail.value}`);
  }
  controls.get(Number(detail.parameterID))?.removeAttribute('data-editing');
  send(`end:${detail.parameterID}`);
  sync();
});

function renderStatus() {
  const pitches = [];
  for (let octave = 0; octave < values[5]; ++octave)
    for (const key of state.notes) pitches.push(Math.min(127, key + octave * 12));
  const notes = values[9] === 1 ? [...pitches].reverse()
    : values[9] === 2 && pitches.length > 1 ? [...pitches, ...pitches.slice(1, -1).reverse()]
    : pitches;
  const low = Math.min(...pitches);
  const high = Math.max(...pitches);
  const range = Math.max(1, high - low);
  $('#notes').replaceChildren(...notes.map((key, index) => {
    const dot = document.createElement('span');
    dot.className = 'note';
    dot.style.left = `${notes.length === 1 ? 50 : 6 + index / (notes.length - 1) * 88}%`;
    dot.style.top = `${pitches.length === 1 ? 50 : 8 + (high - key) / range * 84}%`;
    dot.dataset.name = noteName(key);
    dot.title = noteName(key);
    dot.setAttribute('aria-label', noteName(key));
    dot.classList.toggle('active', state.clock === 2 && state.pitch === index);
    return dot;
  }));
  $('#tempo').textContent = state.clock ? `${Number(state.bpm.toFixed(2))} bpm` : '—';
  $('#hint').textContent = !connected ? 'connecting' : !state.held ? 'hold a chord' : !state.clock
    ? 'waiting for host clock' : state.clock !== 2 ? 'start transport' : '';
}
addEventListener('message', ({data, source}) => {
  // The native CHOC bridge dispatches a MessageEvent without a source window.
  if (source !== parent && !(source === null && parent === window)) return;
  if (!(data instanceof ArrayBuffer) && !ArrayBuffer.isView(data)) return;
  const text = decoder.decode(data);
  if (text.startsWith('values:')) {
    for (const pair of text.slice(7).split(';')) {
      const [id, value] = pair.split('=').map(Number);
      if (Object.hasOwn(values, id) && Number.isFinite(value)) values[id] = value;
    }
    connected = true;
    sync();
  } else if (text.startsWith('status:')) {
    const parts = text.split(':');
    const [clock, bpm, held, , pitch, key] = parts.slice(1, 8).map(Number);
    if (![clock, bpm, held, pitch, key].every(Number.isFinite)) return;
    const notes = parts[8] ? parts[8].split(',').map(Number).filter(n => Number.isInteger(n) && n >= 0 && n <= 127) : [];
    state = {clock, bpm, held, pitch, key, notes};
    connected = true;
    renderStatus();
  }
});
sync();
send('ready');
