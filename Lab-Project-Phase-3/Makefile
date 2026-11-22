# Build the C11 application from its commented source modules.
CC = gcc
CPPFLAGS = -Iinclude
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror
SOURCES = $(wildcard src/*.c)
HEADERS = $(wildcard include/*.h)

# Test builds go to build/. The wrap options let the unit tests fail and count malloc/free.
LIB_SOURCES = $(filter-out src/main.c,$(SOURCES))
DEBUG_FLAGS = -std=c11 -O0 -g -Wall -Wextra -Wpedantic -Werror
SAN_FLAGS = -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror -fno-omit-frame-pointer \
	-fsanitize=address,undefined,float-divide-by-zero,float-cast-overflow -fno-sanitize-recover=all
UNIT_FLAGS = -fno-builtin -Wl,--wrap=malloc -Wl,--wrap=free
UNIT_SOURCES = $(LIB_SOURCES) tests/test_core.c

.PHONY: all run clean test sanitize valgrind
all: minios

minios: $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) -o $@

# Runtime directories are created when the application is launched this way.
run: minios
	mkdir -p data/notes
	./minios

# Functional, unit, reference-model, and experiment-regression tests.
test: minios build/test_core
	bash scripts/run_tests.sh --binary ./minios --unit build/test_core

# The same suite under AddressSanitizer, LeakSanitizer, and UndefinedBehaviorSanitizer.
sanitize: build/minios-san build/test_core-san
	bash scripts/run_tests.sh --mode sanitize --binary build/minios-san --unit build/test_core-san

# The same suite under Valgrind; any memory error or unfreed block is a failure.
valgrind: build/minios-debug build/test_core
	bash scripts/run_tests.sh --mode valgrind --binary build/minios-debug --unit build/test_core

build/minios-debug: $(SOURCES) $(HEADERS) Makefile
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(DEBUG_FLAGS) $(SOURCES) -o $@

build/minios-san: $(SOURCES) $(HEADERS) Makefile
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(SAN_FLAGS) $(SOURCES) -o $@

build/test_core: $(UNIT_SOURCES) $(HEADERS) Makefile
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(DEBUG_FLAGS) $(UNIT_FLAGS) $(UNIT_SOURCES) -o $@

build/test_core-san: $(UNIT_SOURCES) $(HEADERS) Makefile
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(SAN_FLAGS) $(UNIT_FLAGS) $(UNIT_SOURCES) -o $@

# Remove only generated executables; preserve saved notes and runtime data.
clean:
	rm -f minios
	rm -rf build
