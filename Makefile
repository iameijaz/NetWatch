CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=c99
TARGET  = netwatch
SRC     = netwatch.c
PREFIX  = /usr/local
BINDIR  = $(PREFIX)/bin

# Windows detection
ifeq ($(OS),Windows_NT)
    TARGET  := netwatch.exe
    LDFLAGS := -lws2_32
    RM      := del /Q
else
    LDFLAGS :=
    RM      := rm -f
endif

.PHONY: all clean install uninstall test

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "Built: $@"

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	@echo "Installed to $(DESTDIR)$(BINDIR)/$(TARGET)"

uninstall:
	$(RM) $(DESTDIR)$(BINDIR)/$(TARGET)

test: $(TARGET)
	@echo "── Running tests ──────────────────────"
	@./$(TARGET) --version
	@./$(TARGET) --help > /dev/null
	@./$(TARGET) -1 && echo "  one-shot: online" || echo "  one-shot: offline"
	@echo "── Tests passed ───────────────────────"

clean:
	$(RM) $(TARGET) netwatch.exe *.o
