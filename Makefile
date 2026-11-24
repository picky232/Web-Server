# make            서버 빌드 → build/webserver
# make test       단위 테스트 + 기능 테스트
# make unit/e2e   각각 따로
# make asan/tsan  sanitizer 빌드 (build/asan, build/tsan)
# make cgi        CGI 예제 프로그램
# make clean

CC     ?= cc
BUILD  ?= build
OPT    ?= -O2
SAN    ?=
PYTHON ?= python3

# OS마다 다른 것은 두 가지뿐이다: 확장 함수를 여는 매크로, 이벤트 장치
ifeq ($(shell uname -s),Darwin)
  OSDEF := -D_DARWIN_C_SOURCE
  EV    := kqueue
else
  OSDEF := -D_GNU_SOURCE
  EV    := epoll
endif

CPPFLAGS := -Iinclude $(OSDEF)
CFLAGS   := -std=c11 $(OPT) -g -pthread -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 \
            -Wstrict-prototypes -Wmissing-prototypes -Wvla \
            -Werror=implicit-function-declaration -MMD -MP $(SAN) $(EXTRA)
LDFLAGS  := -pthread $(SAN)

SRCS := $(filter-out src/ev_%.c,$(wildcard src/*.c)) src/ev_$(EV).c
OBJS := $(SRCS:src/%.c=$(BUILD)/obj/%.o)
UNIT_OBJS := $(filter-out $(BUILD)/obj/main.o,$(OBJS)) \
             $(patsubst tests/unit/%.c,$(BUILD)/unit/%.o,$(wildcard tests/unit/*.c))

.PHONY: all cgi unit e2e test fault asan tsan clean
all: $(BUILD)/webserver

# CGI 예제 프로그램
cgi: cgi-bin/add
cgi-bin/add: cgi-bin/add.c
	$(CC) $(CFLAGS) -o $@ $<
	@rm -f cgi-bin/*.d

$(BUILD)/webserver: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/unit_tests: $(UNIT_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/obj/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD)/unit/%.o: tests/unit/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

unit: $(BUILD)/unit_tests
	$(BUILD)/unit_tests

e2e: $(BUILD)/webserver fault
	WEBSERVER=$(BUILD)/webserver FAULT=build/fault/webserver $(PYTHON) -W ignore tests/e2e.py

# 좀처럼 나지 않는 실패(디스크 오류, fd 부족)를 일부러 내는 시험 전용 빌드
fault:
	@$(MAKE) -s BUILD=build/fault EXTRA=-DFAULT_INJECT build/fault/webserver

test: unit e2e

asan:
	$(MAKE) BUILD=build/asan OPT=-O1 SAN="-fsanitize=address,undefined -fno-omit-frame-pointer" all build/asan/unit_tests

tsan:
	$(MAKE) BUILD=build/tsan OPT=-O1 SAN="-fsanitize=thread" all build/tsan/unit_tests

clean:
	rm -rf build

-include $(OBJS:.o=.d) $(UNIT_OBJS:.o=.d)
