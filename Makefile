CC ?= cc
PKG_CONFIG ?= pkg-config
GLIB_COMPILE_RESOURCES ?= glib-compile-resources
PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share
APPDIR ?= $(DATADIR)/mpv-gtk

PACKAGES := gtk4 mpv epoxy
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDFLAGS += -Wl,-rpath,'$$ORIGIN'
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PACKAGES)) -lm -ldl

TARGET := build/mpv-gtk
CONFIG := build/mpv.conf
INPUT_CONFIG := build/input.conf
LOCALE_RESOURCE := build/locales/en.ini
TRIM_CONFIG := build/script-opts/trim-mode.conf
COMMON_SOURCES := src/config.c src/preferences.c src/transport.c src/i18n.c src/input.c src/key_commands.c src/keybindings.c src/keys_page.c src/about.c src/startup_trace.c src/gl_api.c src/chapters.c src/chapter_marks.c src/track_notice.c src/trim.c src/trim_backend.c src/trim_export.c src/seek_preview.c
COMMON_OBJECTS := build/mpv-gtk-resources.o
ICON_RESOURCE_FILES := $(wildcard data/icons/scalable/actions/*.svg data/icons/scalable/apps/*.svg) data/icons/LICENSE.GPL3 data/icons/NOTICE.txt data/icons/SOURCES.tsv
HEADERS := src/timeline.h src/config.h src/preferences.h src/controls.h src/transport.h src/i18n.h src/input.h src/key_commands.h src/keybindings.h src/keys_page.h src/about.h src/startup_trace.h src/gl_api.h src/chapters.h src/chapter_marks.h src/track_notice.h src/trim.h src/trim_backend.h src/trim_export.h src/seek_preview.h
STOCK_LIB := build/libmpv.so.2
STOCK_ENGINE := build/mpv-engine
STOCK_DATA := build/stock-mpv.json build/stock-mpv-version.txt
STOCK_LICENSES := $(wildcard tools/stock-mpv-licenses/*)
STOCK_SOURCES := $(shell $(PKG_CONFIG) --variable=libdir mpv)/libmpv.so.2 $(shell command -v mpv)
EMBEDDED_ENGINE_TEST := build/test-embedded-engine
TIMELINE_TEST := build/test-timeline
CONFIG_TEST := build/test-config
CONTROLS_TEST := build/test-controls
CURSOR_TIMEOUT_TEST := build/test-cursor-timeout
LOCALE_TEST := build/test-locale
RENDER_TEST := build/test-render
PREFERENCES_TEST := build/test-preferences
PREFERENCES_UI_TEST := build/test-preferences-ui
SCRUB_TEST := build/test-scrub
I18N_TEST := build/test-i18n
DECODER_TEST := build/test-decoder
PLAYBACK_TEST := build/test-playback
AUTOPLAYLIST_TEST := build/test-autoplaylist
WATCH_LATER_TEST := build/test-watch-later
INPUT_TEST := build/test-input
KEY_COMMANDS_TEST := build/test-key-commands
KEYBINDINGS_TEST := build/test-keybindings
KEYS_INTEGRATION_TEST := build/test-keys-integration
KEYS_PAGE_UI_TEST := build/test-keys-page-ui
CHAPTERS_TEST := build/test-chapters
CHAPTERS_INTEGRATION_TEST := build/test-chapters-integration
CHAPTER_MARKS_TEST := build/test-chapter-marks
LAZY_RESOURCES_TEST := build/test-lazy-resources
STARTUP_TEST := build/test-startup
GL_API_TEST := build/test-gl-api
GL_STARTUP_TEST := build/test-gl-startup
TRIM_NATIVE_TEST := build/test-trim-native
SEEK_PREVIEW_TEST := build/test-seek-preview
DESKTOP := build/io.github.emless.mpvgtk.desktop

.PHONY: all clean test test-trim test-trim-native test-seek-preview install uninstall FORCE

all: $(TARGET) $(CONFIG) $(INPUT_CONFIG) $(LOCALE_RESOURCE) $(TRIM_CONFIG) $(STOCK_LIB) $(STOCK_ENGINE) $(STOCK_DATA)

$(TARGET): | $(TRIM_CONFIG)

$(TRIM_CONFIG): data/script-opts/trim-mode.conf | build
	@if [ ! -e "$@" ] && [ ! -L "$@" ]; then install -Dm600 "$<" "$@"; fi

$(TARGET) $(I18N_TEST) $(KEY_COMMANDS_TEST) $(KEYS_INTEGRATION_TEST) $(DECODER_TEST) $(PLAYBACK_TEST) $(AUTOPLAYLIST_TEST) $(CURSOR_TIMEOUT_TEST) $(LOCALE_TEST) $(RENDER_TEST) $(PREFERENCES_TEST) $(PREFERENCES_UI_TEST) $(SCRUB_TEST) $(LAZY_RESOURCES_TEST): | $(LOCALE_RESOURCE)

$(STARTUP_TEST): | $(LOCALE_RESOURCE)

build:
	mkdir -p "$@"

build/mpv-gtk-resources.c: data/mpv-gtk.gresource.xml $(ICON_RESOURCE_FILES) | build
	$(GLIB_COMPILE_RESOURCES) --sourcedir=data --generate-source --c-name=mpv_gtk_icons --target="$@" "$<"

build/mpv-gtk-resources.o: build/mpv-gtk-resources.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-overlength-strings -c -o "$@" "$<"

$(TARGET): $(STOCK_LIB) src/main.c $(COMMON_SOURCES) $(COMMON_OBJECTS) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" src/main.c $(COMMON_SOURCES) $(COMMON_OBJECTS) $(LDLIBS)

$(STOCK_LIB) $(STOCK_ENGINE) $(STOCK_DATA) &: tools/stage-stock-mpv.sh $(STOCK_LICENSES) $(STOCK_SOURCES) | build
	bash tools/stage-stock-mpv.sh

$(EMBEDDED_ENGINE_TEST): tests/embedded_engine.c src/main.c $(COMMON_SOURCES) $(COMMON_OBJECTS) $(HEADERS) $(STOCK_LIB) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" "$<" $(COMMON_SOURCES) $(COMMON_OBJECTS) $(LDLIBS)

$(STARTUP_TEST): tests/startup.c src/main.c $(COMMON_SOURCES) $(COMMON_OBJECTS) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" tests/startup.c $(COMMON_SOURCES) $(COMMON_OBJECTS) $(LDLIBS)

$(GL_API_TEST): tests/gl_api.c src/gl_api.c src/gl_api.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o "$@" tests/gl_api.c src/gl_api.c $(shell $(PKG_CONFIG) --libs glib-2.0)

$(GL_STARTUP_TEST): tests/gl_startup.c src/main.c $(COMMON_SOURCES) $(COMMON_OBJECTS) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" tests/gl_startup.c $(COMMON_SOURCES) $(COMMON_OBJECTS) $(LDLIBS)

$(CONFIG): data/mpv.conf | build
	@if [ ! -e "$@" ] && [ ! -L "$@" ]; then install -m600 "$<" "$@"; fi

$(INPUT_CONFIG): data/input.conf | build
	@if [ ! -e "$@" ] && [ ! -L "$@" ]; then install -m600 "$<" "$@"; fi

$(LOCALE_RESOURCE): data/locales/en.ini | build
	install -Dm644 "$<" "$@"

$(CONFIG_TEST): tests/config.c src/config.c src/config.h data/mpv.conf data/input.conf | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o "$@" tests/config.c src/config.c $(shell $(PKG_CONFIG) --libs glib-2.0)

$(I18N_TEST): tests/i18n.c src/i18n.c src/i18n.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o "$@" tests/i18n.c src/i18n.c $(shell $(PKG_CONFIG) --libs glib-2.0)

$(KEY_COMMANDS_TEST): tests/key_commands.c src/key_commands.c src/key_commands.h src/i18n.c src/i18n.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o "$@" tests/key_commands.c src/key_commands.c src/i18n.c $(shell $(PKG_CONFIG) --libs glib-2.0 mpv)

$(KEYBINDINGS_TEST): tests/keybindings.c src/keybindings.c src/keybindings.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o "$@" tests/keybindings.c src/keybindings.c $(shell $(PKG_CONFIG) --libs glib-2.0 mpv) -lm

$(KEYS_INTEGRATION_TEST): tests/keys_integration.c src/main.c $(COMMON_SOURCES) $(COMMON_OBJECTS) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" tests/keys_integration.c $(COMMON_SOURCES) $(COMMON_OBJECTS) $(LDLIBS)

$(KEYS_PAGE_UI_TEST): tests/keys_page_ui.c src/keys_page.c src/keys_page.h src/input.c src/input.h src/key_commands.c src/key_commands.h src/keybindings.c src/keybindings.h src/i18n.c src/i18n.h | build $(LOCALE_RESOURCE)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" tests/keys_page_ui.c src/input.c src/key_commands.c src/keybindings.c src/i18n.c $(LDLIBS)

$(CHAPTERS_TEST): tests/chapters.c src/chapters.c src/chapters.h src/i18n.c src/i18n.h | build $(LOCALE_RESOURCE)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" tests/chapters.c src/i18n.c $(LDLIBS)

$(CHAPTERS_INTEGRATION_TEST): tests/chapters_integration.c src/main.c $(COMMON_SOURCES) $(COMMON_OBJECTS) $(HEADERS) | build $(LOCALE_RESOURCE)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" "$<" $(COMMON_SOURCES) $(COMMON_OBJECTS) $(LDLIBS)

$(CHAPTER_MARKS_TEST): tests/chapter_marks.c src/chapter_marks.c src/chapter_marks.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" "$<" $(LDLIBS)

$(LAZY_RESOURCES_TEST): tests/lazy_resources.c src/main.c $(COMMON_SOURCES) $(COMMON_OBJECTS) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" tests/lazy_resources.c $(COMMON_SOURCES) $(COMMON_OBJECTS) $(LDLIBS)

$(DECODER_TEST): tests/decoder.c src/main.c $(COMMON_SOURCES) $(COMMON_OBJECTS) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" "$<" $(COMMON_SOURCES) $(COMMON_OBJECTS) $(LDLIBS)

$(PLAYBACK_TEST): tests/playback.c src/main.c $(COMMON_SOURCES) $(COMMON_OBJECTS) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" "$<" $(COMMON_SOURCES) $(COMMON_OBJECTS) $(LDLIBS)

$(AUTOPLAYLIST_TEST): tests/autoplaylist.c src/main.c $(COMMON_SOURCES) $(COMMON_OBJECTS) $(HEADERS) $(STOCK_LIB) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" "$<" $(COMMON_SOURCES) $(COMMON_OBJECTS) $(LDLIBS)

$(WATCH_LATER_TEST): tests/watch_later.c src/main.c $(COMMON_SOURCES) $(COMMON_OBJECTS) $(HEADERS) $(STOCK_LIB) | build $(LOCALE_RESOURCE)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" "$<" $(COMMON_SOURCES) $(COMMON_OBJECTS) $(LDLIBS)

$(INPUT_TEST): tests/input.c src/input.c src/input.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" tests/input.c src/input.c $(LDLIBS)

$(CONTROLS_TEST): tests/controls.c src/controls.h | build
	$(CC) $(CFLAGS) $(LDFLAGS) -Isrc -o "$@" "$<"

$(CURSOR_TIMEOUT_TEST): tests/cursor_timeout.c src/main.c $(COMMON_SOURCES) $(COMMON_OBJECTS) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" "$<" $(COMMON_SOURCES) $(COMMON_OBJECTS) $(LDLIBS)

$(TIMELINE_TEST): tests/timeline.c src/timeline.h | build
	$(CC) $(CFLAGS) $(LDFLAGS) -Isrc -o "$@" "$<" -lm

$(LOCALE_TEST): tests/locale.c src/main.c $(COMMON_SOURCES) $(COMMON_OBJECTS) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" "$<" $(COMMON_SOURCES) $(COMMON_OBJECTS) $(LDLIBS)

$(RENDER_TEST): tests/render.c src/main.c $(COMMON_SOURCES) $(COMMON_OBJECTS) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" "$<" $(COMMON_SOURCES) $(COMMON_OBJECTS) $(LDLIBS)

$(PREFERENCES_TEST): tests/preferences.c src/main.c $(COMMON_SOURCES) $(COMMON_OBJECTS) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" "$<" $(COMMON_SOURCES) $(COMMON_OBJECTS) $(LDLIBS)

$(PREFERENCES_UI_TEST): tests/preferences_ui.c $(COMMON_SOURCES) $(COMMON_OBJECTS) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" "$<" $(filter-out src/preferences.c,$(COMMON_SOURCES)) $(COMMON_OBJECTS) $(LDLIBS)

$(SCRUB_TEST): tests/scrub.c src/main.c $(COMMON_SOURCES) $(COMMON_OBJECTS) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" "$<" $(COMMON_SOURCES) $(COMMON_OBJECTS) $(LDLIBS)

$(DESKTOP): data/io.github.emless.mpvgtk.desktop.in FORCE | build
	sed 's|@BINDIR@|$(abspath $(BINDIR))|g' "$<" > "$@"
	@if command -v desktop-file-validate >/dev/null 2>&1; then \
		desktop-file-validate "$@"; \
	fi

$(TRIM_NATIVE_TEST): tests/trim_native.c src/trim_export.c src/trim_export.h $(STOCK_LIB) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" tests/trim_native.c src/trim_export.c $(shell $(PKG_CONFIG) --libs gio-2.0 mpv) -lm

test-trim-native: $(TRIM_NATIVE_TEST)
	@./$(TRIM_NATIVE_TEST); status=$$?; \
	if [ "$$status" -eq 77 ]; then printf '%s\n' 'Native Trim Mode check skipped (FFmpeg unavailable).'; \
	elif [ "$$status" -ne 0 ]; then exit "$$status"; fi

$(SEEK_PREVIEW_TEST): tests/seek_preview.c src/seek_preview.c src/seek_preview.h src/i18n.c src/i18n.h $(STOCK_LIB) | build $(LOCALE_RESOURCE)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" tests/seek_preview.c src/seek_preview.c src/i18n.c $(LDLIBS)

test-seek-preview: $(SEEK_PREVIEW_TEST)
	@./$(SEEK_PREVIEW_TEST); status=$$?; \
	if [ "$$status" -eq 77 ]; then printf '%s\n' 'Timeline preview check skipped (no usable Wayland display or FFmpeg).'; \
	elif [ "$$status" -ne 0 ]; then exit "$$status"; fi

test-trim: all test-trim-native
	@MPV_GTK_BINARY="$(abspath $(TARGET))" python3 tests/trim.py; \
	status=$$?; \
	if [ "$$status" -eq 77 ]; then \
		printf '%s\n' 'Trim Mode integration check skipped (no usable display or FFmpeg).'; \
	elif [ "$$status" -ne 0 ]; then exit "$$status"; fi

test: $(TARGET) $(CONFIG) $(INPUT_CONFIG) $(TIMELINE_TEST) $(CONFIG_TEST) $(CONTROLS_TEST) $(CURSOR_TIMEOUT_TEST) $(LOCALE_TEST) $(RENDER_TEST) $(PREFERENCES_TEST) $(PREFERENCES_UI_TEST) $(SCRUB_TEST) $(I18N_TEST) $(DECODER_TEST) $(PLAYBACK_TEST) $(AUTOPLAYLIST_TEST) $(INPUT_TEST) $(KEY_COMMANDS_TEST) $(KEYBINDINGS_TEST) $(KEYS_INTEGRATION_TEST) $(KEYS_PAGE_UI_TEST) $(CHAPTERS_TEST) $(CHAPTERS_INTEGRATION_TEST) $(CHAPTER_MARKS_TEST) $(LAZY_RESOURCES_TEST) $(STARTUP_TEST) $(GL_API_TEST) $(GL_STARTUP_TEST) $(EMBEDDED_ENGINE_TEST) $(STOCK_LIB) $(STOCK_ENGINE) $(STOCK_DATA) $(WATCH_LATER_TEST)
	./$(TIMELINE_TEST)
	./$(CONFIG_TEST)
	./$(CONTROLS_TEST)
	./$(CURSOR_TIMEOUT_TEST)
	./$(LOCALE_TEST)
	./$(RENDER_TEST)
	./$(PREFERENCES_TEST)
	@./$(PREFERENCES_UI_TEST); \
	status=$$?; \
	if [ "$$status" -eq 77 ]; then \
		printf '%s\n' 'Preferences UI test skipped (no usable display).'; \
	elif [ "$$status" -ne 0 ]; then \
		exit "$$status"; \
	fi
	./$(SCRUB_TEST)
	./$(I18N_TEST)
	./$(DECODER_TEST)
	./$(PLAYBACK_TEST)
	./$(AUTOPLAYLIST_TEST)
	./$(WATCH_LATER_TEST)
	./$(INPUT_TEST)
	./$(KEY_COMMANDS_TEST)
	./$(KEYBINDINGS_TEST)
	./$(KEYS_INTEGRATION_TEST)
	@./$(CHAPTERS_TEST); \
	status=$$?; \
	if [ "$$status" -eq 77 ]; then \
		printf '%s\n' 'Chapter GTK test skipped (no usable display).'; \
	elif [ "$$status" -ne 0 ]; then \
		exit "$$status"; \
	fi
	./$(CHAPTERS_INTEGRATION_TEST)
	@./$(CHAPTER_MARKS_TEST); \
	status=$$?; \
	if [ "$$status" -eq 77 ]; then \
		printf '%s\n' 'Chapter mark GTK test skipped (no usable display).'; \
	elif [ "$$status" -ne 0 ]; then \
		exit "$$status"; \
	fi
	@./$(KEYS_PAGE_UI_TEST); \
	status=$$?; \
	if [ "$$status" -eq 77 ]; then \
		printf '%s\n' 'Key editor GTK test skipped (no usable display).'; \
	elif [ "$$status" -ne 0 ]; then \
		exit "$$status"; \
	fi
	./$(LAZY_RESOURCES_TEST)
	./$(STARTUP_TEST)
	./$(GL_API_TEST)
	./$(GL_STARTUP_TEST)
	./$(EMBEDDED_ENGINE_TEST)
	MPV_GTK_BINARY="$(abspath $(TARGET))" python3 tests/engine.py
	@MPV_GTK_BINARY="$(abspath $(TARGET))" python3 tests/smoke.py; \
	status=$$?; \
	if [ "$$status" -eq 77 ]; then \
		printf '%s\n' 'Smoke test skipped (no usable display).'; \
	elif [ "$$status" -ne 0 ]; then \
		exit "$$status"; \
	fi

install: $(TARGET) $(CONFIG) $(INPUT_CONFIG) $(DESKTOP) $(LOCALE_RESOURCE) $(STOCK_LIB) $(STOCK_ENGINE) $(STOCK_DATA)
	install -Dm755 "$(TARGET)" "$(APPDIR)/mpv-gtk"
	install -Dm755 "$(STOCK_LIB)" "$(APPDIR)/libmpv.so.2"
	install -Dm755 "$(STOCK_ENGINE)" "$(APPDIR)/mpv-engine"
	install -Dm644 "build/stock-mpv.json" "$(APPDIR)/stock-mpv.json"
	install -Dm644 "build/stock-mpv-version.txt" "$(APPDIR)/stock-mpv-version.txt"
	@for license in $(STOCK_LICENSES); do install -Dm644 "$$license" "$(APPDIR)/stock-mpv-licenses/$${license##*/}"; done
	rm -f "$(APPDIR)/libmpv-gtk-vulkan.so.2" "$(APPDIR)/libmpv-gtk-presenter.so"
	install -Dm644 "$(LOCALE_RESOURCE)" "$(APPDIR)/locales/en.ini"
	rm -f "$(APPDIR)/trim-licenses/LICENSE.LGPL" "$(APPDIR)/trim-licenses/NOTICE.txt"
	@if [ -d "$(APPDIR)/trim-licenses" ]; then rmdir --ignore-fail-on-non-empty "$(APPDIR)/trim-licenses"; fi
	rm -f "$(APPDIR)/scripts/trim-mode.lua" "$(APPDIR)/scripts/LICENSE.LGPL" "$(APPDIR)/scripts/NOTICE.txt"
	@if [ ! -e "$(APPDIR)/script-opts/trim-mode.conf" ] && [ ! -L "$(APPDIR)/script-opts/trim-mode.conf" ]; then \
		install -Dm600 "$(TRIM_CONFIG)" "$(APPDIR)/script-opts/trim-mode.conf"; \
	fi
	install -Dm644 "data/icons/scalable/apps/multimedia-video-player.svg" "$(DATADIR)/icons/hicolor/scalable/apps/io.github.emless.mpvgtk.svg"
	@for license in LICENSE.GPL3 NOTICE.txt SOURCES.tsv; do install -Dm644 "data/icons/$$license" "$(APPDIR)/icon-licenses/$$license"; done
	@if [ ! -e "$(APPDIR)/mpv.conf" ] && [ ! -L "$(APPDIR)/mpv.conf" ]; then \
		legacy="$${XDG_CONFIG_HOME:-$(HOME)/.config}/mpv-gtk/mpv.conf"; \
		if [ -f "$$legacy" ]; then \
			install -m600 "$$legacy" "$(APPDIR)/mpv.conf"; \
		else \
			install -m600 "$(CONFIG)" "$(APPDIR)/mpv.conf"; \
		fi; \
	fi
	@if [ ! -e "$(APPDIR)/input.conf" ] && [ ! -L "$(APPDIR)/input.conf" ]; then \
		legacy="$${XDG_CONFIG_HOME:-$(HOME)/.config}/mpv-gtk/input.conf"; \
		if [ -f "$$legacy" ]; then \
			install -m600 "$$legacy" "$(APPDIR)/input.conf"; \
		else \
			install -m600 "$(INPUT_CONFIG)" "$(APPDIR)/input.conf"; \
		fi; \
	fi
	install -d "$(BINDIR)"
	ln -sfn "$(abspath $(APPDIR))/mpv-gtk" "$(BINDIR)/mpv-gtk"
	install -Dm644 "$(DESKTOP)" "$(DATADIR)/applications/io.github.emless.mpvgtk.desktop"
	@if command -v gtk4-update-icon-cache >/dev/null 2>&1; then \
		gtk4-update-icon-cache -q -f -t "$(DATADIR)/icons/hicolor"; \
	fi
	@if command -v update-desktop-database >/dev/null 2>&1; then \
		update-desktop-database "$(DATADIR)/applications"; \
	fi

uninstall:
	rm -f "$(APPDIR)/trim-licenses/LICENSE.LGPL" "$(APPDIR)/trim-licenses/NOTICE.txt"
	rm -f "$(APPDIR)/scripts/trim-mode.lua" "$(APPDIR)/scripts/LICENSE.LGPL" "$(APPDIR)/scripts/NOTICE.txt"
	rm -f "$(APPDIR)/mpv-gtk" "$(APPDIR)/libmpv.so.2" "$(APPDIR)/mpv-engine" "$(APPDIR)/stock-mpv.json" "$(APPDIR)/stock-mpv-version.txt" "$(APPDIR)/locales/en.ini" "$(BINDIR)/mpv-gtk" "$(DATADIR)/applications/io.github.emless.mpvgtk.desktop"
	rm -f "$(APPDIR)/stock-mpv-licenses/Copyright" "$(APPDIR)/stock-mpv-licenses/LICENSE.GPL" "$(APPDIR)/stock-mpv-licenses/LICENSE.LGPL"
	rm -f "$(DATADIR)/icons/hicolor/scalable/apps/io.github.emless.mpvgtk.svg" "$(APPDIR)/icon-licenses/LICENSE.GPL3" "$(APPDIR)/icon-licenses/NOTICE.txt" "$(APPDIR)/icon-licenses/SOURCES.tsv"
	@if command -v gtk4-update-icon-cache >/dev/null 2>&1 && [ -d "$(DATADIR)/icons/hicolor" ]; then \
		gtk4-update-icon-cache -q -f -t "$(DATADIR)/icons/hicolor"; \
	fi
	@if command -v update-desktop-database >/dev/null 2>&1 && [ -d "$(DATADIR)/applications" ]; then \
		update-desktop-database "$(DATADIR)/applications"; \
	fi

clean:
	rm -f "$(SEEK_PREVIEW_TEST)"
	rm -f "$(TRIM_NATIVE_TEST)"
	rm -f build/trim-licenses/LICENSE.LGPL build/trim-licenses/NOTICE.txt build/scripts/trim-mode.lua build/scripts/LICENSE.LGPL build/scripts/NOTICE.txt
	rm -f "build/mpv-gtk-resources.c" $(COMMON_OBJECTS)
	rm -f "$(WATCH_LATER_TEST)"
	rm -f "$(AUTOPLAYLIST_TEST)"
	rm -f "$(CHAPTER_MARKS_TEST)"
	rm -f "$(CHAPTERS_TEST)" "$(CHAPTERS_INTEGRATION_TEST)"
	rm -f "$(KEYS_PAGE_UI_TEST)"
	rm -f "$(STOCK_LIB)" "$(STOCK_ENGINE)" $(STOCK_DATA) "$(EMBEDDED_ENGINE_TEST)"
	rm -f "$(GL_API_TEST)" "$(GL_STARTUP_TEST)"
	rm -f "$(STARTUP_TEST)"
	rm -f "$(TARGET)" "$(TIMELINE_TEST)" "$(CONFIG_TEST)" "$(CONTROLS_TEST)" "$(CURSOR_TIMEOUT_TEST)" "$(LOCALE_TEST)" "$(RENDER_TEST)" "$(PREFERENCES_TEST)" "$(PREFERENCES_UI_TEST)" "$(SCRUB_TEST)" "$(I18N_TEST)" "$(DECODER_TEST)" "$(PLAYBACK_TEST)" "$(INPUT_TEST)" "$(KEY_COMMANDS_TEST)" "$(KEYBINDINGS_TEST)" "$(KEYS_INTEGRATION_TEST)" "$(LAZY_RESOURCES_TEST)" "$(LOCALE_RESOURCE)" "$(DESKTOP)"

FORCE:
