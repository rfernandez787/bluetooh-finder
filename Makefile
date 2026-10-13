# btfind — no external dependencies, so this stays a plain Makefile.

CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic
LDFLAGS  ?=

BIN     := btfind
SRCDIR  := src
OBJDIR  := build
SOURCES := $(wildcard $(SRCDIR)/*.cpp)
OBJECTS := $(patsubst $(SRCDIR)/%.cpp,$(OBJDIR)/%.o,$(SOURCES))
DEPS    := $(OBJECTS:.o=.d)

PREFIX ?= /usr/local

.PHONY: all clean test run install uninstall setcap caps

all: $(BIN)

$(BIN): $(OBJECTS)
	$(CXX) $(OBJECTS) -o $@ $(LDFLAGS)

$(OBJDIR)/%.o: $(SRCDIR)/%.cpp | $(OBJDIR)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(OBJDIR):
	@mkdir -p $(OBJDIR)

-include $(DEPS)

test: $(BIN)
	./$(BIN) --selftest

run: $(BIN)
	./$(BIN) --demo

# Scanning on btfind's own terms needs raw HCI socket access. Without this the
# binary must run as root, which is a worse idea than granting exactly the two
# capabilities. (Observing an existing scan needs no privileges at all.)
#
# Capabilities are an xattr on the binary, so they vanish on every rebuild and
# cannot be stored at all on most network filesystems (SMB, NFS). When setcap
# fails for that reason, install the binary on a local disk and cap it there.
caps: $(BIN)
	@sudo setcap 'cap_net_raw,cap_net_admin+eip' ./$(BIN) || { \
	  echo ""; \
	  echo "setcap failed. If it said 'Operation not supported', this"; \
	  echo "filesystem cannot store capabilities (network mounts cannot)."; \
	  echo "Put the binary on a local disk instead:"; \
	  echo "  mkdir -p ~/.local/bin && cp $(BIN) ~/.local/bin/"; \
	  echo "  sudo setcap 'cap_net_raw,cap_net_admin+eip' ~/.local/bin/$(BIN)"; \
	  echo "or run:  sudo ./$(BIN)"; \
	  exit 1; \
	}

setcap: caps

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)
	@echo "Installed. Grant scanning rights with:"
	@echo "  sudo setcap 'cap_net_raw,cap_net_admin+eip' $(DESTDIR)$(PREFIX)/bin/$(BIN)"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)

clean:
	rm -rf $(OBJDIR) $(BIN)
