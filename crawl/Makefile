# Linux / macOS build. On Windows use .\build.ps1 (no make required).
#
# Needs libcurl dev headers here; Windows uses WinHTTP instead and needs
# nothing installed. See decisions.md D4.

CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter
CPPFLAGS = -Isrc -Ivendor -D_DEFAULT_SOURCE
SQLFLAGS = -DSQLITE_ENABLE_FTS5 -DSQLITE_THREADSAFE=0 \
           -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_OMIT_DEPRECATED
LDLIBS   = -lcurl -lpthread -lm -ldl

SRC = src/main.c src/util.c src/asset.c src/classify.c src/jsonutil.c \
      src/http.c src/limiter.c src/store.c src/fetch.c src/robots.c src/adapter.c \
      src/adapters/polyhaven.c src/adapters/ambientcg.c
VENDOR = vendor/sqlite3.c vendor/yyjson.c

OBJ = $(SRC:.c=.o) $(VENDOR:.c=.o)

all: magpie

magpie: $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDLIBS)

# Vendored code is built without our warning flags - not our code.
vendor/%.o: vendor/%.c
	$(CC) -std=c99 -O2 $(CPPFLAGS) $(SQLFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(SQLFLAGS) -MMD -MP -c $< -o $@

-include $(SRC:.c=.d)

deps:
	@mkdir -p vendor
	@echo "yyjson..."
	@curl -fsSL -o vendor/yyjson.c https://raw.githubusercontent.com/ibireme/yyjson/master/src/yyjson.c
	@curl -fsSL -o vendor/yyjson.h https://raw.githubusercontent.com/ibireme/yyjson/master/src/yyjson.h
	@echo "sqlite3..."
	@curl -fsSL https://sqlite.org/download.html \
	  | grep -o 'PRODUCT,[0-9.]*,[0-9]\{4\}/sqlite-amalgamation-[0-9]*\.zip' \
	  | head -1 | cut -d, -f3 \
	  | xargs -I{} curl -fsSL -o /tmp/sqlite.zip https://sqlite.org/{}
	@cd /tmp && unzip -oq sqlite.zip && cp sqlite-amalgamation-*/sqlite3.[ch] $(CURDIR)/vendor/
	@echo "deps ready"

clean:
	rm -f $(OBJ) magpie

.PHONY: all deps clean
