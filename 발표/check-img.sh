#!/bin/sh
# 그림 9장의 마감을 측정한다(글자 잘림·선 겹침·선 근접·상자 걸침·글자 겹침·안전 여백). 문제가 있으면 목록을 보여 주고 1로 끝난다.
cd "$(dirname "$0")" || exit 1
CHROME="${CHROME:-/Applications/Google Chrome.app/Contents/MacOS/Google Chrome}"
bad=0
ids="${*:-flow pin silent thru fail sup arch tests compare lb rec}"
for id in $ids; do
  "$CHROME" --headless=new --disable-gpu --window-size=1280,1400 --virtual-time-budget=2000 --dump-dom "file://$PWD/figures/diagrams.html?fig=$id&audit=1" 2>/dev/null |
  python3 -I -c '
import sys, re, json, html
t = sys.stdin.read()
m = re.search(r"<pre id=\"audit\">(.*?)</pre>", t, re.S)
if not m:
    print("%-8s 검사 결과 없음" % "'$id'"); sys.exit(2)
r = json.loads(html.unescape(m.group(1))); g = r["margins"]
print("%-8s 여백 좌%3d 우%3d 상%3d 하%3d | 문제 %d건" % ("'$id'", g["left"], g["right"], g["top"], g["bottom"], len(r["issues"])))
for i in r["issues"]: print("    [%s] %s" % (i["type"], i["msg"]))
sys.exit(1 if r["issues"] else 0)' || bad=1
done
exit $bad
