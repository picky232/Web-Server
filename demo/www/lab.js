'use strict';
/* 시험대. 부하 발생기(/lab/)에 시키고 결과를 그린다. app.js의 $, sleep, listen을 쓴다 */

const el = (tag, cls, text) => {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined) e.textContent = text;
  return e;
};
const ms = us => us < 10000 ? (us / 1000).toFixed(2) + 'ms' : Math.round(us / 1000) + 'ms';
const num = n => n.toLocaleString('ko-KR');
const tell = text => { $('tell').textContent = text; }; // 화면 낭독기에 알린다
const explain = e => e.name === 'AbortError' ? '멈췄습니다.' : e.name === 'TypeError' ? '서버에 닿지 않습니다. demo/run.sh가 켜져 있는지 확인해 주세요.' : e.message;

/* 지금 좌석판. 푸시로 받은 것은 늦을 수 있으므로 백엔드에 바로 묻는다 */
async function seatsNow() {
  const r = await fetch('/api/seats');
  if (!r.ok) throw new Error(`팔린 자리를 세지 못했습니다(${r.status}). 고장이 풀린 뒤 다시 눌러 주세요.`);
  return (await r.json()).board;
}
const soldIn = text => text.replaceAll('0', '').length;

/* 진행 줄의 한 글자 → 뜻 (crowd.c의 end_shot) */
const KINDS = {
  o: '200 성공', c: '409 다른 손님이 먼저', h: '408 시간 초과', x: '503 가득 참',
  b: '502 백엔드가 끊음', t: '504 백엔드가 답 없음', e: '응답 없이 닫힘', '?': '그 밖의 응답',
};
const CODE_KIND = { 0: 'e', 200: 'o', 408: 'h', 409: 'c', 502: 'b', 503: 'x', 504: 't' };

/* ---- 탭 ---- */

const tabs = [...document.querySelectorAll('[role=tab]')];
function pick(tab) {
  for (const t of tabs) {
    const on = t === tab;
    t.ariaSelected = String(on);
    t.tabIndex = on ? 0 : -1;
    $(t.getAttribute('aria-controls')).hidden = !on;
  }
}
tabs.forEach((t, i) => {
  t.onclick = () => pick(t);
  t.onkeydown = e => {
    const to = { ArrowRight: i + 1, ArrowLeft: i - 1, Home: 0, End: -1 }[e.key];
    if (to === undefined) return;
    e.preventDefault();
    const next = tabs.at(to % tabs.length);
    pick(next);
    next.focus();
  };
});

/* ---- 부하 발생기에 시키기 ---- */

/* 한 줄씩 받아 넘겨준다. 끝 줄(end)을 못 받으면 중간에 끊긴 것 */
async function* runLab({ what, n, c, rate = 0, retry = 0, seat = 1 }, signal) {
  const r = await fetch('/lab/run', { method: 'POST', signal, body: String(new URLSearchParams({ what, n, c, rate, retry, seat })) });
  if (r.status === 409) throw new Error('다른 시험이 돌고 있습니다. 끝난 뒤 다시 눌러 주세요.');
  if (r.status === 403) throw new Error('http://localhost:9000 으로 연 화면에서만 시험할 수 있습니다.');
  if (!r.ok) throw new Error(`부하 발생기가 시험을 시작하지 못했습니다(${r.status}). demo/run.sh가 켜져 있는지 확인해 주세요.`);
  const reader = r.body.getReader(), text = new TextDecoder();
  let rest = '', ended = false;
  for (;;) {
    const { value, done } = await reader.read();
    if (done) break;
    const parts = (rest + text.decode(value, { stream: true })).split('\n');
    rest = parts.pop();
    for (const p of parts) {
      const line = JSON.parse(p);
      ended ||= !!line.end;
      yield line;
    }
  }
  if (!ended) throw new Error('시험 도중에 연결이 끊겼습니다.');
}

/* 단추를 누르면 job을 돌린다. 시험끼리 방해하지 않게 한 번에 하나만 돌고, 오류는 out에 적는다 */
let running = null;
async function start(out, stop, job) {
  if (running) return tell('다른 시험이 도는 중입니다. 끝난 뒤 다시 눌러 주세요.');
  const me = running = new AbortController();
  const busy = on => {
    document.body.classList.toggle('busy', on);
    for (const b of document.querySelectorAll('[data-job]')) b.ariaDisabled = String(on);
    if (stop) {
      if (!on && document.activeElement === stop) stop.parentElement.querySelector('[data-job]')?.focus(); // 곧 사라질 단추에서 초점을 옮긴다
      stop.hidden = !on;
    }
  };
  busy(true);
  try {
    await job(me.signal);
  } catch (e) {
    verdict(out, e.name === 'AbortError' ? 'unsure' : false, explain(e));
  }
  me.abort(); // 오류로 끝났어도 뒤에서 돌던 나머지를 거둔다
  out.querySelectorAll('.live').forEach(p => p.remove());
  busy(false);
  running = null;
}
$('rush-stop').onclick = $('fault-stop').onclick = () => running?.abort();

/* ---- 결과 그리기 ---- */

function verdict(out, ok, text) {
  const p = el('p', 'verdict', text);
  p.dataset.ok = ok; // true 맞음, false 틀림, 'unsure' 판정할 수 없음
  out.append(p);
  tell(text);
}

/* 코드별 개수를 색 이름표로 */
function counts(codes) {
  const ul = el('ul', 'codes');
  for (const [code, n] of Object.entries(codes)) {
    const li = el('li'), dot = el('i'), kind = CODE_KIND[code] ?? '?';
    dot.dataset.k = kind;
    li.append(dot, `${kind === '?' ? code : KINDS[kind]} `, el('b', '', num(n)));
    ul.append(li);
  }
  return ul;
}

function facts(pairs) {
  const dl = el('dl', 'nums');
  for (const [name, value] of pairs) {
    const div = el('div');
    div.append(el('dt', '', name), el('dd', '', value));
    dl.append(div);
  }
  return dl;
}

/* 요청마다 칸 하나. 끝난 순서대로 찬다 */
function grid(n) {
  const box = el('div', 'grid');
  box.setAttribute('aria-hidden', 'true'); // 같은 내용을 개수로도 적는다
  for (let i = 0; i < n; i++) box.append(el('i'));
  let at = 0;
  return { box, add: s => { for (const k of s) box.children[at++].dataset.k = k; } };
}

/* 결과 칸을 만들고 줄이 올 때마다 채운다. 끝 줄을 돌려준다 */
async function watchRun(out, opt, signal, draw = true) {
  const g = draw ? grid(opt.n) : null;
  const live = el('p', 'live'), sums = el('div');
  out.append(...(g ? [g.box] : []), live, sums);
  let end;
  for await (const line of runLab(opt, signal)) {
    g?.add(line.new);
    live.textContent = line.end ? '' : `${num(line.done)} / ${num(line.n)}개 끝남, 열린 연결 ${num(line.live)}` + (line.waiting ? `, 다시 보내려고 기다리는 손님 ${num(line.waiting)}` : '');
    sums.replaceChildren(counts(line.codes));
    end = line;
  }
  return end;
}

/* ---- 몰아 보내기 ---- */

const form = $('rush');
const EXPECT = {
  static: () => '모두 200. 연결 수백 개를 이벤트 루프 스레드들이 나눠 맡습니다.',
  show: () => '거의 모두 캐시가 답합니다. 백엔드의 요청 수는 거의 늘지 않습니다.',
  book: again => '백엔드마다 동시에 4개, 모두 8개까지만 받습니다. 넘치면 503' + (again ? '을 받고 1초쯤 뒤 다시 보내고, 이미 팔린 자리는 409입니다. 빈자리만큼 팔리고 두 번 팔린 자리는 없습니다.' : ', 이미 팔린 자리는 409입니다. 팔린 자리 수는 성공 수와 같습니다.'),
  same: () => '성공은 한 명뿐입니다. 나머지는 409(다른 손님이 먼저)나 503(가득 참)입니다.',
};
form.onchange = () => {
  const f = new FormData(form), book = ['book', 'same'].includes(f.get('what'));
  form.retry.disabled = !book;
  $('rush-expect').textContent = EXPECT[f.get('what')](book && form.retry.checked);
};
form.onchange();

form.onsubmit = e => {
  e.preventDefault();
  const out = $('rush-out');
  start(out, $('rush-stop'), async signal => {
    out.replaceChildren();
    const what = form.what.value, n = Number(form.n.value), c = Math.min(n, Number(form.c.value));
    const book = what === 'book' || what === 'same';
    const opt = { what, n, c, retry: Number(book && form.retry.checked) };
    if (what === 'show') { // 갓 만든 사본을 두고 시작. 곧 만료될 사본이면 도중에 모두 백엔드로 몰린다
      await fetch('/api/show', { method: 'POST' });
      await fetch('/api/show');
    }
    const before = book ? await seatsNow() : '';
    if (what === 'same') opt.seat = before.indexOf('0') + 1;
    if (book && !before.includes('0')) throw new Error('빈자리가 없습니다. 좌석 비우기를 먼저 눌러 주세요.');
    const r = await watchRun(out, opt, signal);
    const ok = r.codes[200] ?? 0;
    out.append(facts([
      ['걸린 시간', (r.t / 1000).toFixed(2) + '초'],
      ['초당', num(Math.round(r.done / Math.max(r.t, 1) * 1000)) + '개'],
      ['지연 가운데 값', ms(r.p50)], ['느린 1%', ms(r.p99)],
      ...(what === 'static' ? [] : [['9001이 답함', num(r.by[0])], ['9002가 답함', num(r.by[1])]]),
      ...(what === 'show' ? [['캐시가 답함', num(r.hits)]] : []),
      ...(opt.retry ? [['다시 보낸 횟수', num(r.retried)]] : []),
    ]));
    if (r.done < r.n) return verdict(out, false, `틀림: 30초 안에 ${num(r.n - r.done)}개가 끝나지 않았습니다.`);
    if (what === 'static') return verdict(out, ok === n, ok === n ? `맞음: ${num(n)}개가 모두 200입니다.` : `틀림: 200이 ${num(ok)}개뿐입니다.`);
    if (what === 'show') return verdict(out, r.hits > n * 0.9, `${r.hits > n * 0.9 ? '맞음' : '틀림'}: ${num(n)}개 가운데 ${num(r.hits)}개를 캐시가 답했습니다.`);
    const sold = soldIn(await seatsNow()) - soldIn(before), lost = r.n - ok - (r.codes[409] ?? 0) - (r.codes[503] ?? 0);
    const score = `성공 ${ok}명, 새로 팔린 자리 ${sold}개, 두 번 팔린 자리 ${r.dup}개`;
    if (r.dup || (what === 'same' && ok > 1)) return verdict(out, false, `틀림: ${score}`);
    if (!ok) return verdict(out, 'unsure', `판정할 수 없음: 예매가 한 건도 처리되지 않았습니다. ${score}`);
    if (sold < ok || sold - ok > lost) return verdict(out, 'unsure', `판정할 수 없음: 시험하는 동안 다른 예매나 비우기가 있었습니다. ${score}`);
    verdict(out, true, `맞음: ${score}` + (sold > ok ? ` (차이 ${sold - ok}개는 팔렸지만 응답을 못 받은 예매)` : ''));
  });
};

/* 연결을 열어 두기 전, 열어 둔 동안, 끊긴 뒤의 정상 요청을 차례로 잰다 */
for (const b of document.querySelectorAll('[data-hold]')) {
  b.onclick = () => {
    const out = $('hold-out');
    start(out, null, async signal => {
      out.replaceChildren();
      const probe = async () => {
        let end;
        for await (const line of runLab({ what: 'static', n: 300, c: 1 }, signal)) end = line;
        return end;
      };
      const before = await probe();
      const status = el('p', 'live');
      out.append(status);
      let open = 0, hold;
      const holding = (async () => {
        for await (const line of runLab({ what: b.dataset.hold, n: 1000, c: 1000 }, signal)) {
          open = Math.max(open, line.live);
          status.textContent = `열린 연결 ${num(line.live)}, 서버가 끊은 연결 ${num(line.done)}`;
          hold = line;
        }
      })();
      holding.catch(() => {}); // 아래 await에서 다룬다
      for (let i = 0; i < 40 && open < 1000; i++) await sleep(50);
      const during = await probe();
      await holding;
      const after = await probe();
      out.append(counts(hold.codes), facts([
        ['열기 전 지연', ms(before.p50)], [`${num(open)}개 열린 동안`, ms(during.p50)], ['끊긴 뒤', ms(after.p50)],
        ['느린 1% (전, 동안, 뒤)', `${ms(before.p99)}, ${ms(during.p99)}, ${ms(after.p99)}`], ['버틴 시간', (hold.t / 1000).toFixed(1) + '초'],
      ]));
      const fine = during.codes[200] === 300 && during.p50 < Math.max(before.p50 * 3, 2000);
      verdict(out, fine, fine ? `맞음: 손님 ${num(open)}명이 말없이 있어도 정상 요청 300개가 모두 200이고, 느려지지 않았습니다.` : '틀림: 정상 요청이 실패했거나 3배 넘게 느려졌습니다.');
    });
  };
}

/* ---- 고장 내기 ---- */

const FAULTS = [
  ['slow', '느림', '1.5초 늦게 답합니다. 살아 있으므로 빼지 않습니다.', '9001과 9002가 모두 정상으로 남고 실패가 없습니다. 예매라면 그쪽 칸 4개가 오래 차 있습니다.'],
  ['hang', '멈춤', '아무 답도 하지 않습니다. 헬스 체크에도.', '이미 보낸 요청은 3초 뒤 504. 헬스 체크가 3번 연속 실패하면 꺼짐이 되고 나머지는 다른 쪽이 받습니다.'],
  ['busy', '503', '살아 있다고 답하지만 일은 모두 거절합니다.', '503이 5번 이어지면 10초 동안 제외됩니다. 헬스 체크로는 알 수 없는 고장입니다.'],
  ['drop', '답 없이 끊기', '요청을 받고 답 없이 연결을 닫습니다.', 'GET은 다른 백엔드로 한 번 더 보내 모두 200, POST는 두 번 처리될 수 있어 다시 보내지 않고 502. 5번 이어지면 제외됩니다.'],
  ['die', '죽이기', '프로세스를 바로 끝냅니다(kill -9).', '연결이 거절되는 순간 제외하고 다른 쪽으로 보냅니다. 4초쯤 뒤 다시 켜지면 돌아옵니다. 좌석은 그대로입니다.'],
];
const KIND_NAME = Object.fromEntries(FAULTS.map(([k, name]) => [k, name]));
const sick = {}; // 포트 → { mode, until }

async function setFault(port, mode) {
  let r;
  try {
    r = await fetch('/fault/' + port, { method: 'POST', body: 'mode=' + mode });
  } catch { /* 아래에서 */ }
  if (mode === 'die') return void (sick[port] = { mode, until: performance.now() + 6000 }); // 답하기 전에 죽는 것이 정상
  if (!r?.ok) throw new Error(`백엔드 ${port}에 고장을 내지 못했습니다(${r?.status ?? '끊김'}). 다시 켜지는 중이면 잠시 뒤 눌러 주세요.`);
  const d = await r.json();
  sick[port] = { mode: d.mode, until: performance.now() + d.left_ms };
}

setInterval(() => {
  for (const s of document.querySelectorAll('.mode')) {
    const f = sick[s.dataset.port], left = f ? Math.ceil((f.until - performance.now()) / 1000) : 0;
    s.textContent = left > 0 && f.mode !== 'ok' ? `${s.dataset.port} ${KIND_NAME[f.mode]} ${left}초` : s.dataset.port;
    s.classList.toggle('on', left > 0 && f.mode !== 'ok');
  }
}, 250);
for (const port of [9001, 9002]) // 새로고침해도 걸려 있던 고장을 보여 준다
  fetch('/fault/' + port).then(r => r.json()).then(d => { sick[port] = { mode: d.mode, until: performance.now() + d.left_ms }; }).catch(() => {});

/* 10초를 0.5초씩 20칸으로. 칸마다 결과를 쌓고, 고장 낸 순간에 금을 긋는다 */
const BIN = 500, BINS = 20;
function timeline(out) {
  const box = el('div', 'time'), cols = [];
  for (let i = 0; i < BINS; i++) {
    const col = el('div');
    cols.push(col);
    box.append(col);
  }
  box.setAttribute('aria-hidden', 'true');
  const axis = el('p', 'axis');
  axis.append(el('span', '', '0초'), el('span', '', '5초'), el('span', '', '10초'));
  out.append(box, axis);
  return {
    add: (t, s) => {
      const col = cols[Math.min(BINS - 1, Math.floor(t / BIN))];
      for (const k of s) {
        const i = el('i');
        i.dataset.k = k;
        col.append(i);
      }
    },
    mark: (t, text) => {
      const m = el('span', 'mark', text);
      m.style.left = Math.min(100, t / (BIN * BINS) * 100) + '%';
      box.append(m);
    },
  };
}

/* 10초 동안 요청을 보내면서 1초쯤에 act를 한다. 예매는 실패하면 다시 보내므로 실패가 최종 개수에 남지 않는다 */
function story(title, expect, act, judge) {
  const out = $('fault-out'), traffic = document.querySelector('[name=traffic]:checked').value;
  start(out, $('fault-stop'), async signal => {
    out.replaceChildren();
    $('fault-expect').textContent = expect + (traffic === 'book' ? ' 예매는 실패하면 같은 번호표로 다시 보내므로, 실패는 "다시 보낸 횟수"로 나타납니다.' : '');
    if (traffic === 'book' && !(await seatsNow()).includes('0')) throw new Error('빈자리가 없어 예매가 모두 409가 됩니다. 좌석 비우기를 먼저 눌러 주세요.');
    const opt = traffic === 'book' ? { what: 'book', n: 60, c: 60, rate: 6, retry: 1 } : { what: traffic, n: 80, c: 80, rate: 8 };
    const line = timeline(out), sums = el('div'), from = performance.now();
    out.append(sums);
    let end, acting = null, at = 0;
    for await (const r of runLab(opt, signal)) {
      line.add(r.t, r.new);
      sums.replaceChildren(counts(r.codes));
      if (!acting && performance.now() - from > 1000) {
        line.mark(at = r.t, title);
        tell(title);
        acting = act();
        acting.catch(() => {}); // 아래 await에서 다룬다
      }
      end = r;
    }
    await acting;
    const ok = end.codes[200] ?? 0;
    out.append(facts([[title, (at / 1000).toFixed(1) + '초에'], ['성공', `${num(ok)} / ${num(end.n)}`], ['9001이 답함', num(end.by[0])], ['9002가 답함', num(end.by[1])], ['느린 1%', ms(end.p99)],
      ...(opt.retry ? [['다시 보낸 횟수', num(end.retried)], ['두 번 팔린 자리', num(end.dup)]] : [])]));
    if (judge) judge(out, end);
    else tell(`끝났습니다. ${num(end.n)}개 가운데 ${num(ok)}개 성공`);
  });
}

for (const [mode, name, what, expect] of FAULTS) {
  const tr = el('tr');
  const th = el('th', '', name);
  th.scope = 'row';
  tr.append(th, el('td', '', what));
  for (const port of [9001, 9002]) {
    const td = el('td'), b = el('button', 'plain', '내기');
    b.type = 'button';
    b.ariaLabel = `${port}에 ${name} 고장 내기`;
    b.dataset.job = '';
    b.onclick = () => story(`${port} ${name}`, expect, () => setFault(port, mode));
    td.append(b);
    tr.append(td);
  }
  $('faults').append(tr);
}
$('heal').onclick = async () => {
  const done = await Promise.allSettled([9001, 9002].map(p => setFault(p, 'ok')));
  const bad = done.find(d => d.status === 'rejected');
  if (bad) verdict($('fault-out'), false, bad.reason.message);
};
$('reload').onclick = () => story('설정 다시 읽기', '새 워커가 뜬 뒤 옛 워커가 물러나므로 실패는 0건입니다. 서버 안쪽의 "켜진 지"가 0초로 돌아갑니다. 푸시는 새 워커로 다시 잇습니다.',
  async () => {
    const r = await fetch('/lab/reload', { method: 'POST' });
    if (!r.ok) throw new Error(`설정을 다시 읽히지 못했습니다(${r.status}).`);
    await sleep(1500);
    listen();
  },
  (out, end) => {
    const lost = end.n - (end.codes[200] ?? 0) - (end.codes[409] ?? 0) + end.retried;
    verdict(out, lost === 0, lost ? `틀림: 워커를 바꾸는 동안 ${lost}건이 실패했습니다.` : `맞음: 워커를 바꾸는 동안 ${end.n}건 가운데 실패가 0건입니다.`);
  });

$('compare').onclick = () => {
  const out = $('compare-out');
  start(out, null, async signal => {
    out.replaceChildren();
    const rows = [];
    await setFault(9002, 'ok');
    for (const [what, name] of [['rr', '돌아가며'], ['lc', '한가한 곳']]) {
      const live = el('p', 'live', `"${name}" 방식으로 보내는 중입니다.`);
      out.append(live);
      let end;
      try {
        await setFault(9001, 'slow'); // 두 방식 모두 같은 조건에서 시작
        for await (const line of runLab({ what, n: 60, c: 12 }, signal)) end = line;
      } finally {
        await setFault(9001, 'ok').catch(() => {}); // 못 풀어도 10초 뒤 저절로 풀린다
      }
      await sleep(600); // 미뤄 둔 요청이 다 빠지기를 기다린다
      live.remove();
      rows.push(end);
      const row = el('div', 'share');
      const bar = el('div', 'bar');
      for (const [i, port] of [[0, 9001], [1, 9002]]) {
        const part = el('span', '', end.by[i] ? `${port} ${end.by[i]}개` : '');
        part.style.flexGrow = end.by[i];
        part.dataset.port = port;
        bar.append(part);
      }
      row.append(el('b', '', name), bar, el('span', 'took', (end.t / 1000).toFixed(1) + '초'));
      out.append(row);
    }
    const [rr, lc] = rows, fair = rows.every(r => r.codes[200] === 60), good = lc.t < rr.t && lc.by[0] < rr.by[0];
    if (!fair) return verdict(out, 'unsure', '판정할 수 없음: 실패한 요청이 있습니다. 다른 고장이 풀린 뒤 다시 눌러 주세요.');
    verdict(out, good, `${good ? '맞음' : '틀림'}: 느린 9001로 간 요청이 ${rr.by[0]}개에서 ${lc.by[0]}개로, 전체 시간이 ${(rr.t / 1000).toFixed(1)}초에서 ${(lc.t / 1000).toFixed(1)}초로 바뀌었습니다.`);
  });
};

/* ---- 이상한 요청 ---- */

const E = '\\r\\n';
const req = (...lines) => lines.map(l => l + E).join('\n') + '\n' + E;
const host = 'Host: localhost:9000';
/* [묶음, 이름, 기대하는 상태 코드들, 보낼 글자, 왜] */
const RAWS = [
  ['정상', '평범한 GET', [200], req('GET /api/seats HTTP/1.1', host), '프록시를 거쳐 백엔드가 답합니다. 연결은 열어 둡니다.'],
  ['정상', 'HEAD', [200], req('HEAD /style.css HTTP/1.1', host), '길이는 알려 주되 본문은 한 바이트도 보내지 않습니다.'],
  ['정상', '바뀌었으면 주세요', [304], req('GET /style.css HTTP/1.1', host, 'If-None-Match: *'), '손님이 이미 가진 것과 같으면 본문 없이 304입니다.'],
  ['정상', '한 연결에 세 개를 한꺼번에', [200, 200, 200], [req('GET /api/seats HTTP/1.1', host), req('HEAD /style.css HTTP/1.1', host), req('GET /api/work HTTP/1.1', host)].join('\n'), '응답을 기다리지 않고 이어 보내도 보낸 순서대로 답합니다.'],
  ['정상', 'HTTP/1.0', [200], req('GET /api/seats HTTP/1.0'), 'Host가 없어도 되고, 답한 뒤 서버가 연결을 닫습니다.'],
  ['정상', 'Connection: close', [200], req('GET /api/seats HTTP/1.1', host, 'Connection: close'), '답한 뒤 서버가 연결을 닫습니다.'],
  ['정상', '절대형 주소', [200], req('GET http://localhost:9000/api/seats HTTP/1.1', host), '프록시에 보내는 꼴의 주소도 읽습니다.'],
  ['정상', '조각으로 보낸 본문', [200], req('POST /api/work HTTP/1.1', host, 'Transfer-Encoding: chunked') + '\n5' + E + '\nhello' + E + '\n0' + E + '\n' + E, '길이를 모를 때 쓰는 chunked 본문을 다 받아 백엔드로 넘깁니다.'],
  ['정상', '보내도 되나요?', [100], req('POST /api/work HTTP/1.1', host, 'Content-Length: 5', 'Expect: 100-continue'), '본문을 보내기 전에 물으면 100 Continue로 먼저 답합니다.'],
  ['정상', '서버 전체에 OPTIONS', [204], req('OPTIONS * HTTP/1.1', host), '백엔드에 보내지 않고 서버가 직접 답합니다.'],
  ['없는 것', '없는 파일', [404], req('GET /nope.html HTTP/1.1', host), ''],
  ['없는 것', '정적 파일에 POST', [405], req('POST / HTTP/1.1', host, 'Content-Length: 0'), '받을 수 있는 메서드를 Allow에 적어 줍니다.'],
  ['없는 것', '숨김 파일', [403], req('GET /.git/config HTTP/1.1', host), '점으로 시작하는 파일은 내주지 않습니다.'],
  ['없는 것', '모르는 메서드', [501], req('BREW /api/seats HTTP/1.1', host), ''],
  ['없는 것', 'CONNECT', [501], req('CONNECT example.com:443 HTTP/1.1', 'Host: example.com:443'), '포워드 프록시가 아니므로 터널을 만들지 않습니다.'],
  ['없는 것', 'HTTP/2.0', [505], req('GET / HTTP/2.0', host), ''],
  ['없는 것', '모르는 Expect', [417], req('GET / HTTP/1.1', host, 'Expect: something-else'), ''],
  ['없는 것', '자기에게 되돌아오는 프록시', [508], req('GET /loop/ HTTP/1.1', host), '거쳐 온 이름(Via)에 자기가 있으면 끊습니다. 안 보면 연결이 다 찰 때까지 돕니다.'],
  ['크기와 시간', '너무 긴 주소', [414], req('GET /{A*9000} HTTP/1.1', host), '주소는 8,000자까지.'],
  ['크기와 시간', '너무 큰 머리', [431], req('GET / HTTP/1.1', host, 'X-Big: {A*17000}'), '요청 머리는 16KB까지.'],
  ['크기와 시간', '너무 큰 본문', [413], req('POST /api/work HTTP/1.1', host, 'Content-Length: 20971520'), '길이만 보고 본문을 받기 전에 거절합니다.'],
  ['크기와 시간', '백엔드가 거절하는 본문', [413], req('POST /api/work HTTP/1.1', host, 'Content-Length: 5000') + '\n{A*5000}', '백엔드의 상한(4KB)을 넘습니다. 백엔드의 답을 그대로 전합니다.'],
  ['크기와 시간', '머리를 보내다 말기', [408], 'GET / HTTP/1.1' + E + '\nHost: lo', '5초까지 기다리고 408. 안 하면 침묵하는 연결이 자리를 영영 차지합니다.'],
  ['속이려는 것', '경로 탈출 /../', [400], req('GET /../etc/passwd HTTP/1.1', host), '공개 폴더 밖으로 나가는 경로입니다.'],
  ['속이려는 것', '경로 탈출 %2e%2e', [400], req('GET /%2e%2e/etc/passwd HTTP/1.1', host), '점을 숫자로 바꿔 검사를 피하려는 것입니다.'],
  ['속이려는 것', '주소 속 %2f', [400], req('GET /a%2fb HTTP/1.1', host), '앞단과 백엔드가 경로를 다르게 읽게 만드는 글자입니다.'],
  ['속이려는 것', '주소 속 %00', [400], req('GET /index.html%00.png HTTP/1.1', host), 'C 문자열의 끝으로 읽히는 글자입니다.'],
  ['속이려는 것', '길이가 두 개', [400], req('POST /api/work HTTP/1.1', host, 'Content-Length: 5', 'Content-Length: 6') + '\nhello', '어느 쪽을 믿느냐에 따라 다음 요청의 시작이 달라집니다.'],
  ['속이려는 것', '길이와 조각을 같이', [400], req('POST /api/work HTTP/1.1', host, 'Content-Length: 4', 'Transfer-Encoding: chunked') + '\n0' + E + '\n' + E + '\n' + req('GET /api/seats HTTP/1.1', host), '본문 뒤에 다른 요청을 숨겨 들이는 수법(요청 밀반입). 숨긴 GET의 답이 오면 안 됩니다.'],
  ['속이려는 것', 'Host가 두 개', [400], req('GET / HTTP/1.1', 'Host: a', 'Host: b'), ''],
  ['속이려는 것', 'Host가 없음', [400], req('GET / HTTP/1.1', 'X-A: 1'), ''],
  ['속이려는 것', '\\n 하나로 끝낸 줄', [400], 'GET / HTTP/1.1\\n\nHost: localhost\\n\n\\n', '줄의 끝은 \\r\\n이어야 합니다. 서버마다 다르게 읽으면 틈이 생깁니다.'],
  ['속이려는 것', '다음 줄로 이어 쓴 헤더', [400], req('GET / HTTP/1.1', host, 'X-A: one', ' two'), ''],
  ['속이려는 것', '콜론 앞 빈칸', [400], req('GET / HTTP/1.1', host, 'X-A : one'), ''],
  ['속이려는 것', '잘못된 조각 길이', [400], req('POST /api/work HTTP/1.1', host, 'Transfer-Encoding: chunked') + '\nzz' + E + '\nhello' + E + '\n0' + E + '\n' + E, ''],
];

/* 적은 글자 → 보낼 바이트. 실제 줄바꿈은 읽기 좋으라고 넣은 것이라 버린다 */
function bytes(text) {
  const out = [], plain = new TextEncoder();
  const more = text.replace(/\r?\n/g, '').replace(/\{(.)\*(\d{1,5})\}/g, (_, ch, n) => ch.repeat(Math.min(n, 20001)));
  for (const [part] of more.matchAll(/\\[rnt0\\]|\\x[0-9a-fA-F]{2}|[^\\]+|\\/g)) {
    if (part[0] !== '\\' || part.length === 1) out.push(...plain.encode(part));
    else out.push({ r: 13, n: 10, t: 9, 0: 0, '\\': 92 }[part[1]] ?? parseInt(part.slice(2), 16));
  }
  return new Uint8Array(out);
}

async function sendRaw(text, signal) {
  const body = bytes(text);
  if (body.length < 1 || body.length > 20000) throw new Error(`1~20,000바이트만 보낼 수 있습니다. 지금은 ${num(body.length)}바이트입니다.`);
  const r = await fetch('/lab/raw', { method: 'POST', body, signal });
  if (r.status === 409) throw new Error('동시에 보낼 수 있는 수(8개)를 넘었습니다. 잠시 뒤 다시 눌러 주세요.');
  if (!r.ok) throw new Error(`보내지 못했습니다(${r.status}). demo/run.sh가 켜져 있는지 확인해 주세요.`);
  const got = new Uint8Array(await r.arrayBuffer());
  const cut = got.lastIndexOf(0); // 받은 바이트 뒤에 0과 결과가 붙어 온다
  const meta = JSON.parse(new TextDecoder().decode(got.subarray(cut + 1)));
  const reply = new TextDecoder().decode(got.subarray(0, cut));
  return { reply, ...meta, codes: [...reply.matchAll(/HTTP\/1\.1 (\d{3}) [A-Z][A-Za-z ]*\r\n/g)].map(m => Number(m[1])) };
}

/* 응답 칸에는 마지막에 보낸 것의 응답만. 5초 걸리는 앞 요청이 나중 것을 덮지 않게 */
let turn = 0;
async function showRaw(sending) {
  const me = ++turn;
  let text;
  $('raw-out').textContent = '응답을 기다리는 중입니다.';
  try {
    const r = await sending;
    text = (r.reply || '(받은 바이트가 없습니다)') + `\n\n── ${r.closed ? '서버가 연결을 닫았습니다' : '연결은 열려 있습니다'}. ${ms(r.us)}`;
    if (me === turn) tell(`받은 상태 코드 ${r.codes.join(' ') || '없음'}`);
  } catch (e) {
    text = explain(e);
    if (me === turn) tell(text);
  }
  if (me === turn) $('raw-out').textContent = text;
}

const rawRows = RAWS.map(([group, name, expect, text, why], i) => {
  if (group !== RAWS[i - 1]?.[0]) {
    const tr = el('tr', 'group'), th = el('th', '', group);
    th.colSpan = 4;
    th.scope = 'rowgroup';
    tr.append(th);
    $('raws').append(tr);
  }
  const tr = el('tr'), b = el('button', 'link', name), actual = el('td'), judged = el('td');
  const first = el('td');
  b.type = 'button';
  first.append(b);
  tr.append(first, el('td', '', expect.join(' ')), actual, judged);
  $('raws').append(tr);
  const row = {
    async send() {
      actual.textContent = '기다리는 중';
      judged.textContent = '';
      try {
        const r = await sendRaw(text);
        row.ok = r.codes.join() === expect.join();
        actual.textContent = r.codes.join(' ') || '응답 없음';
        judged.textContent = row.ok ? '맞음' : '틀림';
        judged.dataset.ok = row.ok;
        return r;
      } catch (e) {
        row.ok = false;
        actual.textContent = explain(e);
        throw e;
      }
    },
  };
  b.onclick = () => {
    $('raw-text').value = text;
    $('raw-why').textContent = why;
    showRaw(row.send());
  };
  return row;
});
$('raw-text').value = RAWS[0][3];

$('raw').onsubmit = e => {
  e.preventDefault();
  $('raw-why').textContent = '';
  showRaw(sendRaw($('raw-text').value));
};

/* 브라우저 연결 6개 중 푸시와 상태 조회가 하나씩 쓰므로 동시에 세 개까지만 */
$('raw-all').onclick = () => start($('raw-err'), null, async () => {
  $('raw-err').replaceChildren();
  const todo = [...rawRows];
  let left = todo.length;
  const worker = async () => {
    for (let row; (row = todo.shift());) {
      await row.send().catch(() => {}); // 줄에 적혀 있다
      $('raw-sum').textContent = `${--left}개 남았습니다.`;
    }
  };
  await Promise.all([worker(), worker(), worker()]);
  const ok = rawRows.filter(r => r.ok).length;
  $('raw-sum').textContent = `${rawRows.length}개 가운데 ${ok}개가 맞습니다.`;
  $('raw-sum').dataset.ok = ok === rawRows.length;
  tell($('raw-sum').textContent);
});

/* ---- 캐시 ---- */

function cacheCell(kind, note) {
  const li = el('li');
  li.dataset.kind = kind;
  li.append(el('b', '', { HIT: '사본', MISS: '새로 받음', POST: '고침', WAIT: '기다림' }[kind]), el('span', '', note));
  $('cache-out').append(li);
  while ($('cache-out').children.length > 24) $('cache-out').firstChild.remove();
}
async function ask() {
  const r = await fetch('/api/show');
  if (!r.ok) throw new Error(`공연 정보를 받지 못했습니다(${r.status}).`);
  const d = await r.json(), kind = r.headers.get('X-Cache') === 'HIT' ? 'HIT' : 'MISS';
  cacheCell(kind, `${d.made}, ${d.by}`);
  return { kind, made: d.made };
}
async function edit() {
  const r = await fetch('/api/show', { method: 'POST' });
  if (!r.ok) throw new Error(`고쳤다고 알리지 못했습니다(${r.status}).`);
  cacheCell('POST', '사본이 지워짐');
}
const cacheJob = job => start($('cache-say'), null, async () => {
  $('cache-say').replaceChildren();
  await job();
});
$('ask').onclick = () => cacheJob(async () => {
  const { kind, made } = await ask();
  tell(`${kind === 'HIT' ? '사본' : '새로 받음'}, 만든 시각 ${made}`);
});
$('edit').onclick = () => cacheJob(async () => {
  await edit();
  tell('사본이 지워졌습니다.');
});
$('cycle').onclick = () => cacheJob(async () => {
  $('cache-out').replaceChildren();
  const got = [];
  await edit(); // 남아 있던 사본을 지우고 시작
  for (let i = 0; i < 4; i++) got.push((await ask()).kind);
  await edit();
  for (let i = 0; i < 2; i++) got.push((await ask()).kind);
  cacheCell('WAIT', '5초 넘게');
  await sleep(5600);
  for (let i = 0; i < 2; i++) got.push((await ask()).kind);
  const ok = got.join(' ') === 'MISS HIT HIT HIT MISS HIT MISS HIT';
  verdict($('cache-say'), ok, ok ? '맞음: 처음과 고친 뒤, 5초가 지난 뒤에만 백엔드에서 새로 받았습니다.' : '틀림: 기대한 순서와 다릅니다. 다른 창이 공연 정보를 묻는 중이면 달라질 수 있습니다.');
});

/* ---- 올리기 ---- */

/* FNV-1a 32비트. crowd.c의 upload_pump와 같은 식 */
function fnv(data) {
  let h = 2166136261;
  for (const b of data) h = Math.imul(h ^ b, 16777619);
  return (h >>> 0).toString(16).padStart(8, '0');
}
for (const b of document.querySelectorAll('[data-size]')) {
  b.onclick = async () => {
    const size = Number(b.dataset.size), data = new Uint8Array(size);
    for (let i = 0; i < size; i += 65536) crypto.getRandomValues(data.subarray(i, i + 65536)); // 한 번에 64KB까지만 채워 준다
    const tr = el('tr');
    const put = cells => tr.replaceChildren(...cells.map(t => el('td', '', t)));
    $('ups').prepend(tr);
    put([num(size) + '바이트', fnv(data), '', '', '보내는 중']);
    const from = performance.now();
    try {
      const r = await fetch('/lab/upload', { method: 'POST', body: data });
      const took = Math.round(performance.now() - from) + 'ms';
      if (!r.ok) throw new Error(`실패(${r.status})`);
      const d = await r.json(), ok = d.size === size && d.sum === fnv(data);
      put([num(size) + '바이트', fnv(data), d.sum, took, ok ? '맞음' : '틀림']);
      tr.lastChild.dataset.ok = ok;
      tell(`${num(size)}바이트 올리기 ${ok ? '맞음' : '틀림'}`);
    } catch (e) {
      put([num(size) + '바이트', fnv(data), '', '', explain(e)]);
      tr.lastChild.dataset.ok = 'false';
      tell(explain(e));
    }
    while ($('ups').children.length > 6) $('ups').lastChild.remove();
  };
}
