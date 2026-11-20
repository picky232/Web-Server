# WEEK13 C Web Server
#
#   make              서버 빌드 → build/webserver
#   make test         단위 + 통합(Python) + curl 스모크 테스트
#   make unit         C 단위 테스트만
#   make integration  Python 통합 테스트만 (서버/백엔드 자동 기동)
#   make smoke        curl 기반 스모크 테스트
#   make asan / tsan  sanitizer 빌드 (build/asan, build/tsan)
#   make test-asan    ASan+UBSan(+LSan) 빌드로 단위+통합 테스트
#   make test-tsan    TSan 빌드로 단위+통합 테스트
#   make load         간단 부하 테스트 (Python 동시 클라이언트)
#   make run          ./www 로 서버 실행 (백엔드 127.0.0.1:9000)
#   make clean

CC      ?= cc
BUILD   ?= build
SAN     ?=
OPT     ?= -O2

# C11 + POSIX.1-2008 API 만 사용한다 (GNU 확장 함수에 기대지 않기 위해)
CPPFLAGS := -Iinclude -D_POSIX_C_SOURCE=200809L
CFLAGS   := -std=c11 $(OPT) -g -pthread \
            -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wstrict-prototypes \
            -Wmissing-prototypes -Wvla -Werror=implicit-function-declaration \
            -MMD -MP $(SAN)
LDFLAGS  := -pthread $(SAN)

SRCS      := $(wildcard src/*.c)
LIB_SRCS  := $(filter-out src/main.c,$(SRCS))
OBJS      := $(SRCS:src/%.c=$(BUILD)/obj/%.o)
LIB_OBJS  := $(LIB_SRCS:src/%.c=$(BUILD)/obj/%.o)
TEST_OBJ  := $(BUILD)/obj/test_unit.o

SERVER    := $(BUILD)/webserver
UNIT_BIN  := $(BUILD)/unit_tests
PYTHON    ?= python3

.PHONY: all unit integration smoke test asan tsan test-asan test-tsan load run clean

all: $(SERVER)

$(SERVER): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

$(UNIT_BIN): $(TEST_OBJ) $(LIB_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/obj/%.o: src/%.c | $(BUILD)/obj
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(TEST_OBJ): tests/unit/test_unit.c | $(BUILD)/obj
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-missing-prototypes -c -o $@ $<

$(BUILD)/obj:
	mkdir -p $@

unit: $(UNIT_BIN)
	./$(UNIT_BIN)

integration: $(SERVER)
	WEBSERVER_BIN=$(SERVER) $(PYTHON) tests/integration/test_server.py

smoke: $(SERVER)
	WEBSERVER_BIN=$(SERVER) bash tests/integration/curl_smoke.sh

test: unit integration smoke

asan:
	$(MAKE) BUILD=build/asan OPT=-O1 SAN="-fsanitize=address,undefined -fno-omit-frame-pointer" all build/asan/unit_tests

tsan:
	$(MAKE) BUILD=build/tsan OPT=-O1 SAN="-fsanitize=thread" all build/tsan/unit_tests

test-asan: asan
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ./build/asan/unit_tests
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 WEBSERVER_BIN=build/asan/webserver SANITIZER=asan $(PYTHON) tests/integration/test_server.py

test-tsan: tsan
	TSAN_OPTIONS=halt_on_error=0 ./build/tsan/unit_tests
	TSAN_OPTIONS=halt_on_error=0 WEBSERVER_BIN=build/tsan/webserver SANITIZER=tsan $(PYTHON) tests/integration/test_server.py

load: $(SERVER)
	WEBSERVER_BIN=$(SERVER) $(PYTHON) tests/load/stress.py

run: $(SERVER)
	./$(SERVER) --port 8080 --root ./www --threads 4 --backend 127.0.0.1:9000

clean:
	rm -rf build

-include $(OBJS:.o=.d) $(TEST_OBJ:.o=.d)
