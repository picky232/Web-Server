/* 그림 마감 검사: 글자가 잘리거나, 선·테두리·다른 글자와 겹치거나, 바깥 여백이 들쭉날쭉하면 찾아 준다.
   사용: 페이지에서 auditSvg(svg, 이름) → [{type, msg}] */
const SAFE = { left: 100, right: 100, top: 60, bottom: 60 }, PAD = 8;   // 모든 그림이 지킬 안전 여백과 글자-선 최소 간격(px, 1280×720 기준)
function auditSvg(svg, name) {
  const [, , VW, VH] = svg.getAttribute('viewBox').split(' ').map(Number);
  const issues = [], push = (type, msg) => issues.push({ name, type, msg });
  const shrink = (b) => ({ x1: b.x + 1, x2: b.x + b.width - 1, y1: b.y + b.height * 0.2, y2: b.y + b.height * 0.8 });
  const texts = [...svg.querySelectorAll('text')].map((t) => { const b = t.getBBox(); return { t: t.textContent.trim(), b, r: shrink(b) }; }).filter((o) => o.t);
  const isBg = (e) => +e.getAttribute('width') >= VW - 1 && +e.getAttribute('height') >= VH - 1;
  const rects = [...svg.querySelectorAll('rect')].filter((e) => !isBg(e)).map((e) => ({ x1: +e.getAttribute('x'), y1: +e.getAttribute('y'), x2: +e.getAttribute('x') + +e.getAttribute('width'), y2: +e.getAttribute('y') + +e.getAttribute('height'), e }));
  /* 선(직선·꺾은선·곡선)을 점으로 쪼갠다 */
  const pts = [];
  svg.querySelectorAll('line,path').forEach((e) => { if (e.closest('marker') || e.closest('defs')) return;
    if (e.tagName === 'line') { const a = [+e.getAttribute('x1'), +e.getAttribute('y1')], b = [+e.getAttribute('x2'), +e.getAttribute('y2')], n = Math.ceil(Math.hypot(b[0] - a[0], b[1] - a[1]) / 3);
      for (let i = 0; i <= n; i++) pts.push([a[0] + (b[0] - a[0]) * i / n, a[1] + (b[1] - a[1]) * i / n, e]); }
    else if (e.getAttribute('fill') === 'none' || !e.getAttribute('fill')) { const L = e.getTotalLength(); for (let d = 0; d <= L; d += 3) { const p = e.getPointAtLength(d); pts.push([p.x, p.y, e]); } } });
  texts.forEach(({ t, b, r }) => {
    const label = `"${t.slice(0, 18)}"`;
    /* 1) 잘림·안전 여백 */
    if (b.x < 0 || b.x + b.width > VW || b.y < 0 || b.y + b.height > VH) push('잘림', `${label} 화면 밖으로 나감`);
    else if (b.x < SAFE.left - 1 || b.x + b.width > VW - SAFE.right + 1) push('여백', `${label} 좌우 안전 여백(${SAFE.left}px) 안으로 들어옴: x ${Math.round(b.x)}~${Math.round(b.x + b.width)}`);
    else if (b.y < SAFE.top - 1 || b.y + b.height > VH - SAFE.bottom + 1) push('여백', `${label} 상하 안전 여백(${SAFE.top}px) 안으로 들어옴: y ${Math.round(b.y)}~${Math.round(b.y + b.height)}`);
    /* 2) 글자 ↔ 선 */
    const hit = pts.find(([x, y]) => x > r.x1 && x < r.x2 && y > r.y1 && y < r.y2), near = !hit && pts.find(([x, y]) => x > r.x1 - PAD && x < r.x2 + PAD && y > r.y1 - PAD && y < r.y2 + PAD);
    if (hit) push('선 겹침', `${label} 위로 선이 지나감 (${Math.round(hit[0])}, ${Math.round(hit[1])})`); else if (near) push('선 근접', `${label} 선과 ${PAD}px 안으로 붙음 (${Math.round(near[0])}, ${Math.round(near[1])})`);
    /* 3) 글자 ↔ 상자 테두리 */
    rects.forEach((q) => { const inter = r.x1 < q.x2 && r.x2 > q.x1 && r.y1 < q.y2 && r.y2 > q.y1; if (!inter) return;
      const inside = r.x1 >= q.x1 + 2 && r.x2 <= q.x2 - 2 && r.y1 >= q.y1 + 2 && r.y2 <= q.y2 - 2;
      if (!inside) push('상자 걸침', `${label} 상자(${Math.round(q.x1)},${Math.round(q.y1)} ${Math.round(q.x2 - q.x1)}×${Math.round(q.y2 - q.y1)}) 테두리에 걸침/넘침`); });
  });
  /* 4) 글자 ↔ 글자 */
  for (let i = 0; i < texts.length; i++) for (let j = i + 1; j < texts.length; j++) { const a = texts[i].r, c = texts[j].r;
    const w = Math.min(a.x2, c.x2) - Math.max(a.x1, c.x1), h = Math.min(a.y2, c.y2) - Math.max(a.y1, c.y1);
    if (w > 2 && h > 2) push('글자 겹침', `"${texts[i].t.slice(0, 14)}" ↔ "${texts[j].t.slice(0, 14)}"`); }
  /* 5) 바깥 여백이 그림마다 같은가: 내용 전체의 경계 */
  const all = [...texts.map((o) => ({ x1: o.b.x, x2: o.b.x + o.b.width, y1: o.b.y + o.b.height * 0.15, y2: o.b.y + o.b.height * 0.85 })), ...rects.filter((q) => q.x2 - q.x1 < VW - 1).map((q) => ({ x1: q.x1, x2: q.x2, y1: q.y1, y2: q.y2 })), ...pts.map(([x, y]) => ({ x1: x, x2: x, y1: y, y2: y }))];
  const m = { left: Math.min(...all.map((o) => o.x1)), right: VW - Math.max(...all.map((o) => o.x2)), top: Math.min(...all.map((o) => o.y1)), bottom: VH - Math.max(...all.map((o) => o.y2)) };
  Object.keys(m).forEach((k) => { m[k] = Math.round(m[k]); });
  return { issues, margins: m };
}
function auditAll() {
  const res = [];
  document.querySelectorAll('.tile').forEach((tile, i) => { const cap = tile.querySelector('.cap').firstChild.textContent.trim(), r = auditSvg(tile.querySelector('svg'), cap); res.push({ name: cap, margins: r.margins, issues: r.issues }); });
  const pre = document.createElement('pre'); pre.id = 'audit'; pre.textContent = JSON.stringify(res); document.body.appendChild(pre);
}
