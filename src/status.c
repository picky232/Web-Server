/* 상태 페이지: 서버가 자기 상태를 스스로 보여 준다. 별도 포트(기본 꺼짐)에서만 응답한다 */
#include "status.h"
#include "conn.h"
#include "loop.h"
#include "server.h"
#include "str.h"

#include <string.h>

static const char page[] =
    "<!doctype html><html lang=ko><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>서버 상태</title><style>body{font:15px system-ui,sans-serif;max-width:60rem;margin:2rem auto;padding:0 1rem}"
    "table{border-collapse:collapse;width:100%;margin:.5rem 0 1.5rem}th,td{text-align:right;padding:.35rem .6rem;border-bottom:1px solid #ccc}"
    "th:first-child,td:first-child{text-align:left}.up{color:#087f23}.down,.ejected{color:#b00020;font-weight:600}</style>"
    "<h1>서버 상태</h1><p id=top></p><h2>백엔드</h2><table id=be></table><h2>이벤트 루프</h2><table id=lo></table>"
    "<script>const row=(t,a,c)=>{const r=t.insertRow();a.forEach((v,i)=>{const d=r.appendChild(document.createElement(c||'td'));"
    "d.textContent=v;if(v==='up'||v==='down'||v==='ejected')d.className=v})};"
    "async function go(){try{const s=await(await fetch('status.json')).json();"
    "top_.textContent=`가동 ${s.uptime_s}초 · 요청 ${s.requests} · CGI ${s.cgi_running} · 업로드 임시 파일 ${s.upload_disk_bytes}B`;"
    "be.innerHTML='';row(be,['묶음','백엔드','상태','처리 중','요청','실패','연결 재사용'],'th');"
    "s.pools.forEach(p=>p.backends.forEach(b=>row(be,[p.name+' ('+p.policy+')',b.name,b.state,b.inflight,b.requests,b.failures,b.reused])));"
    "lo.innerHTML='';row(lo,['루프','연결','받은 연결','요청','가장 오래 붙잡힌 시간(ms)'],'th');"
    "s.loops.forEach(l=>row(lo,[l.id,l.connections,l.accepted,l.requests,(l.max_busy_us/1000).toFixed(1)]))}"
    "catch(e){top_.textContent='연결 끊김'}setTimeout(go,1000)}const top_=document.getElementById('top'),be=document.getElementById('be'),"
    "lo=document.getElementById('lo');go()</script></html>\n";

static const char *state_of(const struct backend *b, long long now)
{
    if (atomic_load(&b->down))
        return "down";
    return atomic_load(&b->ejected_until) > now ? "ejected" : "up";
}

static bool build_json(struct server *s, long long now, struct buf *o)
{
    long long requests = 0;
    for (int i = 0; i < s->nloops; i++)
        requests += atomic_load(&s->loops[i].requests);
    bool ok = buf_printf(o, "{\"uptime_s\":%lld,\"requests\":%lld,\"cgi_running\":%d,\"upload_disk_bytes\":%lld,\"pools\":[",
                         (now - s->started_ms) / 1000, requests, atomic_load(&s->cgi_running), atomic_load(&s->spool_bytes));
    for (int i = 0; ok && i < s->cfg->npools; i++) {
        const struct pool *p = &s->cfg->pools[i];
        ok = buf_printf(o, "%s{\"name\":\"%s\",\"policy\":\"%s\",\"max\":%d,\"backends\":[", i ? "," : "", p->name,
                        p->policy == LEAST_CONN ? "least_conn" : "round_robin", p->max_inflight);
        for (int k = 0; ok && k < p->n; k++) {
            const struct backend *b = &p->be[k];
            ok = buf_printf(o, "%s{\"name\":\"%s\",\"state\":\"%s\",\"inflight\":%d,\"requests\":%lld,\"failures\":%lld,\"reused\":%lld}",
                            k ? "," : "", b->name, state_of(b, now), atomic_load(&b->inflight),
                            atomic_load(&b->requests), atomic_load(&b->failures), atomic_load(&b->reused));
        }
        ok = ok && buf_printf(o, "]}");
    }
    ok = ok && buf_printf(o, "],\"loops\":[");
    for (int i = 0; ok && i < s->nloops; i++) {
        struct loop *l = &s->loops[i];
        ok = buf_printf(o, "%s{\"id\":%d,\"connections\":%d,\"accepted\":%lld,\"requests\":%lld,\"bytes\":%lld,\"max_busy_us\":%lld,\"log_dropped\":%lld}",
                        i ? "," : "", i, atomic_load(&l->nconn), atomic_load(&l->accepted), atomic_load(&l->requests),
                        atomic_load(&l->bytes), atomic_load(&l->max_busy_us), atomic_load(&l->log_dropped));
    }
    return ok && buf_printf(o, "]}\n");
}

static void status_start(struct conn *c)
{
    struct exchange *ex = c->ex;
    bool ok;
    if (!strcmp(ex->path, "/status.json")) {
        struct buf o;
        buf_init(&o, 60000);
        ok = build_json(c->loop->srv, c->loop->now, &o) &&
             reply_text(&ex->reply, &c->out, 200, "application/json", buf_head(&o), buf_len(&o), "Cache-Control: no-store\r\n");
        buf_free(&o);
    } else if (!strcmp(ex->path, "/")) {
        ok = reply_text(&ex->reply, &c->out, 200, "text/html; charset=utf-8", page, sizeof page - 1, "Cache-Control: no-store\r\n");
    } else {
        exchange_error(c, 404, NULL);
        return;
    }
    if (ok)
        exchange_finish(c);
    else
        exchange_error(c, 500, NULL);
}

const struct handler status_handler = {.name = "status", .start = status_start};
