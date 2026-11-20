#ifndef PROXY_MSG_H
#define PROXY_MSG_H

#include "http_parser.h"

#include <stddef.h>

/* ---------------- 백엔드 응답 헤더 파서 ---------------- */

typedef struct {
    int       status, minor;
    size_t    reason_off, reason_len;
    hp_header headers[HP_MAX_HEADERS];
    int       nheaders;
    size_t    head_len;          /* 마지막 빈 줄까지 포함한 헤더 길이 */
    int       has_cl;
    size_t    content_length;
    int       chunked;
    int       conn_close;
} pm_resp;

/* 1: 헤더 완성, 0: 더 필요, -1: 형식 오류/지원하지 않음 (호출자는 502 로 처리) */
int pm_resp_parse(const char *buf, size_t len, pm_resp *r);

typedef enum { BM_NONE, BM_LENGTH, BM_CHUNKED, BM_CLOSE } pm_body_mode;

/* RFC 7230 3.3.3: HEAD 응답, 1xx/204/304 는 본문 없음. TE 가 CL 보다 우선. 둘 다 없으면 연결 종료까지. */
pm_body_mode pm_resp_body_mode(const pm_resp *r, int req_is_head);

/* ---------------- chunked 종료 감지기 ----------------
 * 본문을 해석해 변환하지 않고, "마지막 청크와 트레일러 끝"이 어디인지만 찾는다
 * (바이트는 그대로 클라이언트에 전달). */
typedef struct {
    int    st;
    size_t remain;
    int    digits;
} chunk_dec;

void chunk_init(chunk_dec *d);

/* 반환 1: 종료 (used 는 종료 지점까지 소비한 바이트, 이후 바이트는 소비하지 않음),
 *      0: 더 필요 (n 바이트 모두 소비), -1: 형식 오류 */
int chunk_feed(chunk_dec *d, const char *buf, size_t n, size_t *used);

/* ---------------- 헤더 재작성 ---------------- */

/* 클라이언트 요청 -> 백엔드 요청.
 *  - hop-by-hop 헤더와 Expect 를 제거하고 Connection 을 새로 쓴다 (upstream_ka 0: close, 1: keep-alive)
 *  - X-Forwarded-For 에 peer_ip 를 덧붙인다
 *  - strip_len > 0 이면 요청 경로 앞의 strip_len 바이트를 제거한다 ("/api/x" -> "/x")
 *  - 본문(Content-Length)을 헤더 뒤에 붙인다
 * 반환: malloc 된 버퍼 (호출자가 free), 실패 시 NULL */
char *pm_build_request(const hp_parser *p, const char *rbuf, const char *peer_ip,
                       size_t strip_len, int upstream_ka, size_t *out_len);

/* 백엔드 응답 헤더 -> 클라이언트 응답 헤더 (hop-by-hop 제거, Via 추가, Connection 새로 씀)
 * extra: "Name: value\r\n" 형태의 추가 헤더들 (NULL 가능, 예: "X-Cache: MISS\r\n") */
char *pm_build_client_head(const pm_resp *r, const char *buf, int client_ka, const char *extra, size_t *out_len);

/* 캐시에 저장할 헤더: 상태 라인 + 전달 가능한 헤더들 (Connection/Via/Age 제외, 끝의 빈 줄 없음).
 * 꺼내 쓸 때 Age/X-Cache/Via/Connection 을 새로 붙이고 빈 줄로 끝낸다. */
char *pm_build_stored_head(const pm_resp *r, const char *buf, size_t *out_len);

#endif
