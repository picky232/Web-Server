#!/bin/sh
# CGI 예제: 요청 정보를 그대로 보여 준다
echo "Content-Type: text/plain; charset=utf-8"
echo
echo "안녕하세요, 프로세스 $$ 입니다"
echo "메서드: $REQUEST_METHOD"
echo "쿼리:   $QUERY_STRING"
echo "경로:   $PATH_INFO"
echo "브라우저: $HTTP_USER_AGENT"
