#ifndef STR_H
#define STR_H

#include <stdbool.h>
#include <stddef.h>

/* 문자열 조각(slice). 복사하지 않고 원본 버퍼의 일부를 가리킨다 → 원본보다 오래 쓰면 안 된다 */
struct sl {
    const char *p;
    size_t n;
};

bool sl_eq(struct sl a, const char *b);
bool sl_eqi(struct sl a, const char *b);               /* 대소문자 무시 */
bool sl_list_has(struct sl list, const char *token);   /* "a, b ,c" 목록에 token이 있나 */
bool sl_to_ll(struct sl s, long long *out);            /* 0 이상 10진수만. 부호·공백·넘침은 false */
struct sl sl_trim(struct sl s);
bool sl_copy(struct sl s, char *out, size_t cap);      /* '\0'을 붙여 복사. 안 들어가면 false */

/* 뒤로 가지 않는 시계. 제한 시간 계산용 */
long long now_us(void);
long long now_ms(void);

#endif
