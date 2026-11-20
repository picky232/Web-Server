CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -g
CFLAGS  += -D_POSIX_C_SOURCE=200809L
SAN     ?=
BUILD   := build
SRCS    := src/event.c src/server.c src/http_parser.c src/mime.c src/static_file.c \
           src/config.c src/router.c src/proxy_msg.c src/cache.c
OBJS    := $(SRCS:src/%.c=$(BUILD)/%.o)

UNITY   := vendor/unity/unity.c
TESTS   := $(patsubst tests/test_%.c,$(BUILD)/test_%,$(wildcard tests/test_*.c))

.PHONY: all clean asan run test itest

all: $(BUILD)/webserver $(BUILD)/backend

# 통합 테스트용 백엔드 (C). strncasecmp 때문에 strings.h 가 필요해 POSIX 기본 정의로 빌드한다.
$(BUILD)/backend: tools/backend.c | $(BUILD)
	$(CC) -std=c11 -Wall -Wextra -O2 -g -o $@ $<

$(BUILD)/webserver: $(OBJS) $(BUILD)/main.o
	$(CC) $(CFLAGS) $(SAN) -o $@ $^

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

# 주소/누수 검사 빌드: make asan
asan: clean
	$(MAKE) SAN="-fsanitize=address,undefined -fno-omit-frame-pointer"

# 단위 테스트: 모듈별 test_*.c 를 해당 소스와 Unity 로 빌드해 실행 (ASan/UBSan 포함)
# 테스트 바이너리는 서버 전체가 아니라 필요한 모듈만 링크한다.
TEST_SAN := -fsanitize=address,undefined -fno-omit-frame-pointer
TEST_CC = $(CC) $(CFLAGS) $(TEST_SAN) -Isrc -Ivendor/unity -o $@ $^

$(BUILD)/test_http_parser: tests/test_http_parser.c src/http_parser.c $(UNITY) | $(BUILD)
	$(TEST_CC)

$(BUILD)/test_mime: tests/test_mime.c src/mime.c $(UNITY) | $(BUILD)
	$(TEST_CC)

$(BUILD)/test_static_file: tests/test_static_file.c src/static_file.c $(UNITY) | $(BUILD)
	$(TEST_CC)

$(BUILD)/test_config: tests/test_config.c src/config.c $(UNITY) | $(BUILD)
	$(TEST_CC)

$(BUILD)/test_router: tests/test_router.c src/router.c src/config.c $(UNITY) | $(BUILD)
	$(TEST_CC)

$(BUILD)/test_cache: tests/test_cache.c src/cache.c src/proxy_msg.c src/router.c src/http_parser.c $(UNITY) | $(BUILD)
	$(TEST_CC)

$(BUILD)/test_proxy_msg: tests/test_proxy_msg.c src/proxy_msg.c src/router.c src/config.c src/http_parser.c $(UNITY) | $(BUILD)
	$(TEST_CC)

test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; ./$$t || exit 1; done

# 통합 테스트(프록시): ASan 빌드 후 실행
itest:
	$(MAKE) asan
	bash tests/itest_proxy.sh
	bash tests/itest_cache.sh

run: all
	./$(BUILD)/webserver -p 8080

clean:
	rm -rf $(BUILD)
