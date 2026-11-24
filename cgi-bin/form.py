#!/usr/bin/env python3
"""CGI 예제: POST로 받은 폼을 해석해 돌려준다"""
import os
import sys
from urllib.parse import parse_qs

body = sys.stdin.read(int(os.environ.get("CONTENT_LENGTH") or 0))
print("Content-Type: text/plain; charset=utf-8\n")
for name, values in parse_qs(body).items():
    print(f"{name} = {', '.join(values)}")
print(f"({os.environ['REQUEST_METHOD']}, 본문 {len(body)}글자)")
