#ifndef CACHE_H
#define CACHE_H

#include "http_parser.h"
#include "proxy_msg.h"

#include <stddef.h>

/* 리버스 프록시용 공유 캐시 (RFC 7234 의 보수적인 부분집합).
 *
 * 저장 조건 (모두 만족해야 함):
 *   - 응답 상태 200/301 (기본 TTL 적용) 또는 404 (명시적 신선도가 있을 때만)
 *   - Cache-Control 에 no-store / no-cache / private 이 없고, Set-Cookie 가 없고, Vary: * 가 아님
 *   - 신선도: s-maxage > max-age > 설정의 기본 TTL.  Expires 만 있는 응답은 날짜를 해석하지 않으므로 저장하지 않음
 *   - 요청에 Authorization / Range / Cache-Control: no-store 가 없음
 * 조회 조건: 위에 더해 요청에 no-cache, max-age=0, Pragma: no-cache 가 없음 (있으면 BYPASS: 새로 받아 갱신)
 *
 * 시간은 항상 인자(now_ms)로 받는다: 테스트에서 시계를 제어하기 위해서다.
 * 단일 스레드(이벤트 루프)에서만 사용한다. */

typedef struct cache cache;

typedef struct {
    unsigned long hits, misses, stores, evictions, expired, invalidations;
    size_t entries, bytes;
} cache_stats;

/* max_bytes: 전체 메모리 상한 (키/헤더/본문/관리 구조 포함), max_object: 본문 하나의 최대 크기 */
cache *cache_create(size_t max_bytes, size_t max_object);
void   cache_destroy(cache *c);
void   cache_get_stats(const cache *c, cache_stats *out);

/* 캐시 키: "<host 소문자>\n<origin-form URI>" (쿼리 포함). 반환: 길이, 버퍼가 작으면 -1 */
int cache_make_key(const hp_parser *p, const char *rbuf, char *out, size_t cap);

/* 요청이 캐시 조회/저장을 허용하는가 */
void cache_request_policy(const hp_parser *p, const char *rbuf, int *allow_lookup, int *allow_store);

typedef struct {
    long ttl_s;
    char vary[128];   /* 소문자로 정규화된 Vary 헤더 이름들 ("accept-language,accept-encoding"), 없으면 "" */
} cache_resp_info;

/* 응답을 저장해도 되는가. 1: 저장 가능 (out 채움), 0: 저장하지 않음 */
int cache_response_policy(const pm_resp *r, const char *buf, long default_ttl_s, cache_resp_info *out);

/* Vary 에 나열된 헤더의 요청 값을 이어 붙인 비교용 문자열. 반환: 길이, 버퍼가 작으면 -1 */
int cache_vary_key(const hp_parser *p, const char *rbuf, const char *vary, char *out, size_t cap);

typedef struct {
    const char *head;  size_t head_len;   /* 상태 라인 + 헤더들 (Connection/Via/Age 제외, 끝의 빈 줄 없음) */
    const char *body;  size_t body_len;   /* 원본 바이트 그대로 (chunked 면 chunk 형식 그대로) */
    long age_s;
} cache_hit;

/* 1: 적중 (out 의 포인터는 다음 캐시 변경 전까지 유효), 0: 미스 (만료된 항목은 여기서 제거) */
int cache_lookup(cache *c, const char *key, size_t klen, const hp_parser *p, const char *rbuf,
                 long long now_ms, cache_hit *out);

/* 저장. 1: 저장됨, 0: 크기 제한 등으로 저장하지 않음. 같은 키가 있으면 교체한다. */
int cache_store(cache *c, const char *key, size_t klen, const cache_resp_info *info, const char *vary_key,
                const char *head, size_t head_len, const char *body, size_t body_len, long long now_ms);

void cache_invalidate(cache *c, const char *key, size_t klen);

#endif
