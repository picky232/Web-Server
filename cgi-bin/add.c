/* CGI 예제: /cgi-bin/add?3&4 → 7  (빌드: make cgi) */
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    const char *q = getenv("QUERY_STRING");
    long a = 0, b = 0;
    if (!q || sscanf(q, "%ld&%ld", &a, &b) != 2) {
        printf("Status: 400 Bad Request\r\nContent-Type: text/plain\r\n\r\nusage: ?3&4\n");
        return 0;
    }
    printf("Content-Type: text/plain\r\n\r\n%ld + %ld = %ld\n", a, b, a + b);
    return 0;
}
