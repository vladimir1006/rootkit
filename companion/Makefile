CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -O2 -g
LDFLAGS += -static
LDLIBS ?=

CPPFLAGS += -I../include
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
          -Wstrict-prototypes -Wmissing-prototypes -Wformat=2

ifeq ($(STRICT),1)
CFLAGS += -Werror
endif

TARGET := companion
SOURCE := companion.c
PROTOCOL := ../include/rk2600_protocol.h

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(SOURCE) $(PROTOCOL)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCE) $(LDLIBS)

test: $(TARGET)
	sh ./tests/test_cli.sh ./$(TARGET)
	sh ./tests/test_name_bounds.sh ./$(TARGET)

clean:
	rm -f $(TARGET)
