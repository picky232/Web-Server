#ifndef MIME_H
#define MIME_H

/* 확장자 → Content-Type. 모르는 확장자는 application/octet-stream */
const char *mime_type(const char *path);

#endif
