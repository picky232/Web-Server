#!/bin/sh
# figures/diagrams.html 의 그림을 img/*.png 로 뽑는다 (Chrome 필요).
cd "$(dirname "$0")" || exit 1
CHROME="${CHROME:-/Applications/Google Chrome.app/Contents/MacOS/Google Chrome}"
mkdir -p img
for spec in "flow:01-flow" "pin:02-pinning" "silent:03-silent" "thru:04-throughput" "fail:05-failover" "sup:06-supervisor" "arch:07-architecture" "tests:08-tests" "compare:09-compare" "banner:00-banner"; do
  id=${spec%%:*}; name=${spec#*:}
  url="file://$PWD/figures/diagrams.html?fig=$id"
  h=$("$CHROME" --headless=new --disable-gpu --hide-scrollbars --window-size=1280,1400 \
        --virtual-time-budget=2000 --dump-dom "$url" 2>/dev/null | grep -o 'data-h="[0-9]*"' | grep -o '[0-9]*')
  "$CHROME" --headless=new --disable-gpu --hide-scrollbars --force-device-scale-factor=2 \
    --window-size=1280,"${h:-720}" --virtual-time-budget=2000 --screenshot="$PWD/img/$name.png" "$url" >/dev/null 2>&1
  echo "$name height=$h"
done
