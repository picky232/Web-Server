'use strict';

const $ = id => document.getElementById(id);
const SVG = 'http://www.w3.org/2000/svg';
const SLOTS = 4; // front.conf의 max=4
const sleep = ms => new Promise(done => setTimeout(done, ms));
const calm = matchMedia('(prefers-reduced-motion: reduce)');

/* 번호표. 창마다 다른 손님이다. 탭을 복제하면 저장한 값도 복사되므로, 새로고침했을 때만 이어 쓴다 */
const who = (() => {
  let v = null;
  const reloaded = performance.getEntriesByType('navigation')[0]?.type === 'reload';
  try { if (reloaded) v = sessionStorage.getItem('who'); } catch { /* 저장이 막힌 브라우저 */ }
  if (!v) {
    v = Array.from(crypto.getRandomValues(new Uint8Array(8)), b => b.toString(16).padStart(2, '0')).join('');
    try { sessionStorage.setItem('who', v); } catch { /* 새로고침하면 다른 손님이 된다 */ }
  }
  return v;
})();

/* ---- 좌석판 ---- */

let board = '';
const waiting = new Set(); // 응답을 기다리는 자리
const seats = Array.from({ length: 60 }, (_, i) => {
  const b = document.createElement('button');
  b.type = 'button';
  b.className = 'seat';
  b.textContent = i + 1;
  b.onclick = () => b.ariaDisabled !== 'true' && book(i + 1); // disabled는 키보드 초점을 잃게 한다
  $('seats').append(b);
  return b;
});

function draw() {
  const count = { 0: 0, 1: 0, 2: 0 };
  seats.forEach((b, i) => {
    const s = board[i] ?? 'none';
    const state = waiting.has(i + 1) && s !== '1' ? 'wait' : s;
    if (board) count[s]++;
    b.dataset.state = state;
    b.ariaDisabled = String(state !== '0');
    b.ariaLabel = `${i + 1}번, ${{ 0: '빈자리', 1: '팔린 자리', 2: '내 자리', wait: '예매하는 중', none: '받는 중' }[state]}`;
  });
  for (const s in count) $('n' + s).textContent = count[s];
}

function say(text, kind = '') {
  $('say').textContent = text;
  $('say').dataset.kind = kind;
}

function log(what, status, note) {
  const li = document.createElement('li');
  [what, status || '끊김', note].forEach((t, i) => {
    const span = document.createElement('span');
    span.textContent = t;
    if (i === 1 && status !== 200) span.className = 'bad';
    li.append(span);
  });
  $('log').prepend(li);
  while ($('log').children.length > 6) $('log').lastChild.remove();
}

/* 같은 번호표로 몇 번을 보내도 한 번 예매한 것과 같다. 그래서 결과를 모를 때 다시 물어도 안전하다 */
async function book(seat) {
  if (waiting.has(seat)) return;
  waiting.add(seat);
  draw();
  say(`${seat}번 자리를 예매하는 중입니다.`, 'wait');
  let done = false;
  for (let turn = 1; turn <= 4 && !done; turn++) {
    const from = performance.now();
    let status = 0, data = null;
    try {
      const r = await fetch('/api/book', { method: 'POST', body: new URLSearchParams({ seat, who }) });
      status = r.status;
      if (status === 200 || status === 409) data = await r.json();
    } catch { /* 응답을 받지 못했다 */ }
    const took = ((performance.now() - from) / 1000).toFixed(2) + '초';
    done = data !== null;
    if (done && board) board = board.slice(0, seat - 1) + (status === 200 ? 2 : 1) + board.slice(seat); // 푸시를 기다리지 않는다
    log(`POST ${seat}번 예매`, status, done ? `백엔드 ${data.by}, ${took}` : took);
    travel(data?.by, status);
    if (done && status === 409) say(`${seat}번은 다른 손님이 먼저 예매했습니다.`);
    else if (done && data.again) say(`${seat}번은 내 자리입니다. 앞의 요청은 처리됐고 응답만 끊겼습니다.`, 'good');
    else if (done) say(`${seat}번 자리를 예매했습니다. 백엔드 ${data.by}가 처리했습니다.`, 'good');
    else if (turn === 4 && status === 503) say('서버가 계속 가득 찼습니다(503). 잠시 뒤 다시 눌러 주세요.');
    else if (turn === 4) say(`${seat}번의 결과를 아직 모릅니다. 잠시 뒤 다시 눌러 확인해 주세요.`);
    else if (status === 503) say('서버가 가득 찼습니다(503). 1초 뒤 다시 보냅니다.', 'wait');
    else say(`예매됐는지 모릅니다(${status || '연결 끊김'}). 1초 뒤 같은 번호표로 다시 묻습니다.`, 'wait');
    if (!done && turn < 4) await sleep(1000);
  }
  waiting.delete(seat);
  draw();
}

/* ---- 서버가 먼저 보내는 좌석판 ---- */

function listen() {
  const es = new EventSource('/api/events?who=' + who);
  es.onmessage = e => {
    const d = JSON.parse(e.data);
    if (!board) show(); // 처음이거나 백엔드가 이제 켜졌다
    else if (board !== d.board) travel(d.by, 'push');
    board = d.board; // 바뀐 자리만이 아니라 전체가 오므로, 끊긴 사이의 것을 놓쳐도 맞춰진다
    $('pushed').textContent = `푸시 연결됨 (백엔드 ${d.by})`;
    draw();
  };
  es.onerror = () => {
    $('pushed').textContent = '푸시 다시 연결하는 중';
    /* 연결이 끊기면 브라우저가 알아서 다시 잇지만, 5xx 응답을 받으면 포기한다 */
    if (es.readyState === EventSource.CLOSED) setTimeout(listen, 1000);
  };
}

/* ---- 공연 정보 (캐시) ---- */

async function show() {
  const from = performance.now();
  try {
    const r = await fetch('/api/show');
    if (!r.ok) throw r.status;
    const d = await r.json();
    const took = ((performance.now() - from) / 1000).toFixed(2) + '초';
    const hit = r.headers.get('X-Cache') === 'HIT';
    $('title').textContent = d.title;
    $('when').textContent = `${d.place}, ${d.time}`;
    log('GET 공연 정보', 200, hit ? `캐시, ${r.headers.get('Age')}초 전 사본, ${took}` : `백엔드 ${d.by}, ${took}`);
    travel(hit ? null : d.by, hit ? 'cache' : 200);
  } catch (status) {
    log('GET 공연 정보', Number(status) || 0, '');
    $('when').textContent = '공연 정보를 받지 못했습니다. 백엔드가 켜져 있는지 확인해 주세요.';
  }
}
$('again').onclick = show;

/* ---- 요청이 지나간 길 ---- */

async function along(dot, path, back) {
  const len = path.getTotalLength();
  const from = performance.now();
  for (let t = 0; t < 1;) {
    t = Math.min(1, (await new Promise(requestAnimationFrame) - from) / 260);
    const p = path.getPointAtLength(len * Math.max(0, back ? 1 - t : t));
    dot.setAttribute('cx', p.x);
    dot.setAttribute('cy', p.y);
  }
}

/* kind: 상태 코드, 'cache'(서버 프로그램에서 되돌아감), 'push'(좌석 파일에서 시작) */
async function travel(by, kind) {
  if (calm.matches || document.hidden) return;
  const client = $('w-client'), wire = $('w-' + by), file = $('f-' + by);
  const booked = kind === 200 || kind === 409;
  const go = [[client], wire && [wire], wire && booked && [file]];
  const back = [wire && booked && [file, true], wire && [wire, true], [client, true]];
  const steps = kind === 'push' ? [file && [file, true], wire && [wire, true], [client, true]] : [...go, ...back];
  const dot = document.createElementNS(SVG, 'circle');
  dot.setAttribute('r', 7);
  dot.setAttribute('class', kind === 'push' ? 'dot push' : 'dot');
  $('dots').append(dot);
  for (const step of steps) if (step) await along(dot, ...step);
  dot.remove();
}

/* ---- 서버 안쪽 ---- */

for (const port of [9001, 9002]) {
  const g = $('b-' + port);
  const x = Number(g.querySelector('rect').getAttribute('x'));
  for (let i = 0; i < SLOTS; i++) {
    const r = document.createElementNS(SVG, 'rect');
    Object.entries({ x: x + 16 + i * 26, y: 280, width: 20, height: 18, rx: 3 }).forEach(a => r.setAttribute(...a));
    g.querySelector('.slots').append(r);
  }
}

function drawServer(s) {
  const pool = name => s.pools.find(p => p.name === name).backends;
  $('alive').textContent = `서버 프로그램이 켜진 지 ${s.uptime_s}초`;
  $('alive').className = 'alive';
  for (const b of pool('seats')) {
    const g = $('b-' + b.name.split(':').pop());
    g.dataset.state = b.state;
    g.querySelector('.state').textContent = { up: '정상', down: '꺼짐', ejected: '잠시 제외' }[b.state];
    g.querySelectorAll('.slots rect').forEach((r, i) => r.classList.toggle('on', i < b.inflight));
    g.querySelector('.count').textContent = `요청 ${b.requests}, 연결 재사용 ${b.reused}`;
    g.querySelector('.fail').textContent = `실패 ${b.failures}`;
  }
  const open = pool('push').map(b => b.inflight);
  $('pushes').textContent = open.reduce((a, b) => a + b, 0);
  $('hits').textContent = s.cache ? s.cache.hits : 0;
  $('misses').textContent = s.cache ? s.cache.misses : 0;
  const most = Math.max(4, ...s.loops.map(l => l.connections));
  $('loops').replaceChildren(...s.loops.map(l => {
    const bar = document.createElement('div');
    const fill = document.createElement('i');
    bar.textContent = l.connections;
    fill.style.height = (l.connections / most * 2.4) + 'rem';
    bar.append(fill);
    return bar;
  }));
}

async function watch() {
  for (;;) {
    try {
      drawServer(await (await fetch('/status.json', { cache: 'no-store' })).json());
    } catch {
      $('alive').textContent = '서버 프로그램에 닿지 않습니다. demo/run.sh가 켜져 있는지 확인해 주세요.';
      $('alive').className = 'alive off';
    }
    await sleep(document.hidden ? 2000 : 300);
  }
}

draw();
listen();
watch();
