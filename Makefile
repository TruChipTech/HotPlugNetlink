CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -O2 -g \
            -D_GNU_SOURCE \
            -I./include
LDFLAGS :=

TARGET  := hotplug_monitor
SRCDIR  := src
OBJDIR  := build

SRCS    := $(wildcard $(SRCDIR)/*.c)
OBJS    := $(patsubst $(SRCDIR)/%.c, $(OBJDIR)/%.o, $(SRCS))

.PHONY: all clean install

all: $(OBJDIR) $(TARGET)

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

install: all
	install -m 0755 $(TARGET) /usr/local/bin/$(TARGET)

clean:
	rm -rf $(OBJDIR) $(TARGET)
