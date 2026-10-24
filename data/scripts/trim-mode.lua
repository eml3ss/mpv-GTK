-- Trim Mode backend adapted for the mpv GTK frontend.
-- Original controls, keyframe navigation, export verification, and recovery are preserved.
-- SPDX-License-Identifier: LGPL-2.1-or-later
local mp = require("mp")
local utils = require("mp.utils")
local options = {
    ffmpeg_path = "ffmpeg",
    ffprobe_path = "ffprobe",
    remember = false,
    ["open-replace"] = true,
    ["open-copy"] = true,
}
local gtk_config = mp.get_property_native("user-data/mpv-gtk-trim-config")
if not gtk_config then
    require("mp.options").read_options(options, "trim-mode")
end

local export_request
local update_shortcuts

-- config=no isolates the GTK player from the system mpv configuration.
-- Avoid mp.options reapplying its default snapshot on its first observer event.
local gtk_directory = gtk_config and utils.split_path(gtk_config)
if gtk_directory then
    local conf = io.open(utils.join_path(gtk_directory, "script-opts/trim-mode.conf"), "r")
    if conf then
        for line in conf:lines() do
            local key, value = line:match("^%s*([%w_-]+)%s*=%s*([^#]-)%s*$")
            if key and options[key] ~= nil then
                if type(options[key]) == "boolean" then
                    options[key] = value == "yes" or value == "true"
                elseif value ~= "" then
                    options[key] = value
                end
            end
        end
        conf:close()
    end
end
local state_path = gtk_directory and utils.join_path(gtk_directory, "trim-mode-state.json")
    or mp.command_native({ "expand-path", "~~home/trim-mode-state.json" })
local enabled = false
if options.remember and state_path then
    local file = io.open(state_path, "rb")
    if file then
        local state = utils.parse_json(file:read(256) or "")
        file:close()
        enabled = type(state) == "table" and state.enabled == true
    end
end

local function save_mode()
    if not options.remember or not state_path then
        return
    end
    local file, error = io.open(state_path, "wb")
    if not file then
        mp.msg.warn("Could not save trim mode: " .. tostring(error))
        return
    end
    local written, write_error = file:write(utils.format_json({ enabled = enabled }) .. "\n")
    local closed, close_error = file:close()
    if not written or not closed then
        mp.msg.warn("Could not save trim mode: " .. tostring(write_error or close_error))
    end
end

local loaded = false
local keyframes = {}
local cached_file = nil
local scan_job = nil
local revision = 0
local cache = {}
local cache_order = {}
local capture_limit = 64 * 1024 * 1024
local start_ms, end_ms, duration_ms = 0, 0, 0
local end_follows_duration = true
local editor = nil
local gtk_editing = false
local file_serial = 0
local keyframe_seek = nil
local keyframe_target = nil

local function available()
    local track = mp.get_property_native("current-tracks/video")
    return loaded and duration_ms > 0 and track ~= nil and not track.image and not track.albumart
end

local function format_boundary(ms)
    return string.format(
        "%02d:%02d:%02d.%03d",
        math.floor(ms / 3600000),
        math.floor(ms / 60000) % 60,
        math.floor(ms / 1000) % 60,
        ms % 1000
    )
end

local function publish_controls()
    if update_shortcuts then
        update_shortcuts()
    end
    mp.set_property_native("user-data/trim-controls", {
        available = available(),
        start_ms = start_ms,
        end_ms = end_ms,
        duration_ms = duration_ms,
        file_serial = file_serial,
        start_text = format_boundary(start_ms),
        end_text = format_boundary(end_ms),
        editor = editor,
    })
end

-- Decimal fractions are seconds, never frame numbers. No expression evaluation.
local function parse_time(text)
    text = text:match("^%s*(.-)%s*$")
    if text == "" or #text > 256 then
        return nil
    end
    local negative = text:sub(1, 1) == "-"
    if negative or text:sub(1, 1) == "+" then
        text = text:sub(2)
    end
    if text == "" or text:find("[^%d:%.]") then
        return nil
    end

    local fields = {}
    for field in (text .. ":"):gmatch("(.-):") do
        fields[#fields + 1] = field
    end
    if #fields < 1 or #fields > 3 then
        return nil
    end
    local last = fields[#fields]
    if not (last:match("^%d+$") or last:match("^%d+%.%d*$") or last:match("^%.%d+$")) then
        return nil
    end
    local seconds = tonumber(last)
    if not seconds or seconds ~= seconds or seconds == math.huge then
        return nil
    end
    if #fields > 1 and seconds >= 60 then
        return nil
    end
    local total = seconds
    for i = 1, #fields - 1 do
        if not fields[i]:match("^%d+$") then
            return nil
        end
        local value = tonumber(fields[i])
        if not value or value == math.huge or (#fields == 3 and i == 2 and value >= 60) then
            return nil
        end
        total = total + value * 60 ^ (#fields - i)
    end
    if total ~= total or total == math.huge then
        return nil
    end
    if negative then
        return 0
    end
    -- Validate the exact input before rounding it to the displayed millisecond.
    if total > duration_ms / 1000 + 0.000000001 then
        return nil
    end
    return total * 1000
end

local function valid_boundary(boundary, ms)
    -- Decimal-to-binary conversion can put an exact displayed millisecond a
    -- few ulps beyond itself (e.g. 0.069 * 1000). Tolerate only that noise.
    local epsilon = 0.0000001
    return ms ~= nil
        and ms >= 0
        and ms <= duration_ms + epsilon
        and ((boundary == "start" and ms < end_ms) or (boundary == "end" and ms > start_ms))
end

local function apply_boundary(boundary, ms)
    if not valid_boundary(boundary, ms) then
        return false
    end
    ms = math.floor(ms + 0.5)
    if not valid_boundary(boundary, ms) then
        return false
    end
    if boundary == "start" then
        start_ms = ms
    else
        end_ms, end_follows_duration = ms, false
    end
    return true
end

local function finish_edit(commit, keep_invalid)
    if not editor then
        return true
    end
    if commit and not apply_boundary(editor.boundary, parse_time(editor.text)) and keep_invalid then
        return false
    end
    editor = nil
    publish_controls()
    return true
end

local function previous_char(text, pos)
    pos = math.max(0, pos - 1)
    while pos > 0 and text:byte(pos + 1) >= 128 and text:byte(pos + 1) < 192 do
        pos = pos - 1
    end
    return pos
end

local function next_char(text, pos)
    pos = math.min(#text, pos + 1)
    while pos < #text and text:byte(pos + 1) >= 128 and text:byte(pos + 1) < 192 do
        pos = pos + 1
    end
    return pos
end

local function mouse_cursor(text, pos)
    pos = math.max(0, math.min(#text, math.floor(tonumber(pos) or #text)))
    while pos > 0 and pos < #text and text:byte(pos + 1) >= 128 and text:byte(pos + 1) < 192 do
        pos = pos - 1
    end
    return pos
end

local function begin_edit(boundary, pos, extend)
    if not enabled or not available() or (boundary ~= "start" and boundary ~= "end") then
        return
    end
    if not editor or editor.boundary ~= boundary then
        finish_edit(true, false)
        local text = format_boundary(boundary == "start" and start_ms or end_ms)
        editor = { boundary = boundary, text = text, cursor = #text, anchor = #text }
    end
    editor.cursor = mouse_cursor(editor.text, pos)
    if not extend then
        editor.anchor = editor.cursor
    end
    publish_controls()
end

local function replace_selection(text)
    if not editor then
        return
    end
    text = text:gsub("[%z\1-\31\127]", " ")
    local first, last = math.min(editor.cursor, editor.anchor), math.max(editor.cursor, editor.anchor)
    if #editor.text - (last - first) + #text > 256 then
        return
    end
    editor.text = editor.text:sub(1, first) .. text .. editor.text:sub(last + 1)
    editor.cursor = first + #text
    editor.anchor = editor.cursor
    publish_controls()
end

local function move_cursor(direction, extend, whole)
    if not editor then
        return
    end
    local pos = editor.cursor
    if whole then
        pos = direction < 0 and 0 or #editor.text
    elseif not extend and editor.anchor ~= pos then
        pos = direction < 0 and math.min(pos, editor.anchor) or math.max(pos, editor.anchor)
    else
        pos = direction < 0 and previous_char(editor.text, pos) or next_char(editor.text, pos)
    end
    editor.cursor = pos
    if not extend then
        editor.anchor = pos
    end
    publish_controls()
end

local function erase(direction, whole)
    if not editor then
        return
    end
    if editor.cursor == editor.anchor then
        editor.anchor = whole and (direction < 0 and 0 or #editor.text)
            or (direction < 0 and previous_char(editor.text, editor.cursor) or next_char(editor.text, editor.cursor))
    end
    replace_selection("")
end

local function copy_selection(cut)
    if not editor or editor.cursor == editor.anchor then
        return
    end
    local first, last = math.min(editor.cursor, editor.anchor), math.max(editor.cursor, editor.anchor)
    mp.set_property("clipboard/text", editor.text:sub(first + 1, last))
    if cut then
        replace_selection("")
    end
end

-- Bindings have no global keys: ModernZ activates its exclusive input section
-- only while a field is focused. Playback shortcuts cannot leak into the edit.
local function bind(name, callback, flags)
    mp.add_key_binding(nil, "trim-" .. name, callback, flags or { repeatable = true })
end

bind("text", function(info)
    if info.key_text and (info.event == "press" or info.event == "down" or info.event == "repeat") then
        replace_selection(info.key_text)
    end
end, { complex = true })

bind("enter", function()
    finish_edit(true, true)
end)

bind("escape", function()
    finish_edit(false, false)
end)

bind("tab", function()
    if not editor then
        return
    end
    local boundary = editor.boundary == "start" and "end" or "start"
    finish_edit(true, false)
    begin_edit(boundary)
end)

bind("select-all", function()
    if not editor then
        return
    end
    editor.anchor, editor.cursor = 0, #editor.text
    publish_controls()
end)

bind("copy", function()
    copy_selection(false)
end)

bind("cut", function()
    copy_selection(true)
end)

bind("paste", function()
    if not editor then
        return
    end
    mp.commandv("update-clipboard", "text")
    replace_selection(mp.get_property("clipboard/text", ""))
end)

for _, direction in ipairs({ -1, 1 }) do
    local dir = direction
    local name = dir < 0 and "left" or "right"
    bind(name, function()
        move_cursor(dir, false, false)
    end)

    bind("select-" .. name, function()
        move_cursor(dir, true, false)
    end)

    bind(dir < 0 and "home" or "end", function()
        move_cursor(dir, false, true)
    end)

    bind(dir < 0 and "select-home" or "select-end", function()
        move_cursor(dir, true, true)
    end)

    bind(dir < 0 and "backspace" or "delete", function()
        erase(dir, false)
    end)

    bind(dir < 0 and "erase-before" or "erase-after", function()
        erase(dir, true)
    end)
end
for i = 0, 9 do
    local digit = tostring(i)
    bind("kp" .. digit, function()
        replace_selection(digit)
    end)
end
bind("decimal", function()
    replace_selection(".")
end)

mp.register_script_message("edit-click", function(boundary, pos, extend)
    begin_edit(boundary, pos, extend == "yes")
end)

mp.register_script_message("edit-drag", function(boundary, pos)
    if editor and editor.boundary == boundary then
        editor.cursor = mouse_cursor(editor.text, pos)
        publish_controls()
    end
end)

mp.register_script_message("edit-blur", function()
    finish_edit(true, false)
end)

local function set_boundary(boundary)
    if not enabled or not available() then
        return
    end
    finish_edit(true, false)
    local pos = mp.get_property_number("time-pos")
    if pos then
        local ms = math.min(duration_ms, math.max(0, math.floor(pos * 1000 + 0.5)))
        if apply_boundary(boundary, ms) then
            publish_controls()
        end
    end
end

mp.register_script_message("set-boundary", set_boundary)

local function cancel_keyframe_seek()
    keyframe_seek, keyframe_target = nil, nil
end

local function start_export(mode)
    if not enabled or not available() or (mode ~= "keep" and mode ~= "replace") then
        return
    end
    finish_edit(true, false)
    cancel_keyframe_seek()
    if export_request then
        export_request(mode)
    end
end

mp.register_script_message("export", start_export)

local function seek_keyframe(index, exact)
    local target = keyframes[index]
    -- Fast absolute seeks round backwards. A small guard inside this keyframe's
    -- interval prevents rounded timestamps from selecting the preceding key.
    local following = keyframes[index + 1] or duration_ms / 1000
    local point = exact and target or target + math.min(0.01, math.max(0, following - target) / 2)
    keyframe_seek = { index = index, point = point, exact = exact, started = false, issued = mp.get_time() }
    mp.commandv("seek", point, exact and "absolute+exact" or "absolute+keyframes")
end

mp.register_event("seek", function()
    if not keyframe_seek then
        return
    end
    local pos = mp.get_property_number("time-pos")
    if
        keyframe_seek.started
        or not pos
        or (math.abs(pos - keyframe_seek.point) > 0.002 and math.abs(pos - keyframes[keyframe_seek.index]) > 0.002)
    then
        -- A timeline/keyboard seek supersedes navigation with the key buttons.
        cancel_keyframe_seek()
    else
        keyframe_seek.started = true
    end
end)

mp.register_event("playback-restart", function()
    local request = keyframe_seek
    if not request or not request.started then
        return
    end
    -- The event may have been queued before a newer, external seek started.
    if mp.get_property_bool("seeking") then
        cancel_keyframe_seek()
        return
    end
    if keyframe_target ~= request.index then
        -- Count every click, but skip rendering obsolete intermediate requests.
        seek_keyframe(keyframe_target, false)
        return
    end
    local pos = mp.get_property_number("time-pos")
    local allowance = 0.002
    if not mp.get_property_bool("pause") then
        -- Playback can advance while this script receives the restart event.
        allowance = allowance
            + (mp.get_time() - request.issued) * mp.get_property_number("speed", 1)
            + 1 / math.max(1, (mp.get_property_number("container-fps", 25)))
    end
    local target = keyframes[request.index]
    if pos and (pos < target - 0.002 or pos > target + allowance) and not request.exact then
        -- Some container indexes/preroll rules need a precise fallback. Do this
        -- only on an actual mismatch, rather than decoding a whole GOP per click.
        seek_keyframe(request.index, true)
    else
        cancel_keyframe_seek()
    end
end)

local function jump_keyframe(direction)
    if not enabled or not available() or #keyframes == 0 then
        return
    end
    local pos = keyframe_target and keyframes[keyframe_target] or mp.get_property_number("time-pos")
    if not pos then
        return
    end
    -- Exclude the current keyframe, allowing for rounded container timestamps.
    local epsilon = math.min(0.002, 0.2 / math.max(1, (mp.get_property_number("container-fps", 25))))
    local low, high = 1, #keyframes
    if direction == "previous" then
        while low <= high do
            local mid = math.floor((low + high) / 2)
            if keyframes[mid] < pos - epsilon then
                low = mid + 1
            else
                high = mid - 1
            end
        end
        if high >= 1 then
            keyframe_target = high
        else
            return
        end
    elseif direction == "next" then
        while low <= high do
            local mid = math.floor((low + high) / 2)
            if keyframes[mid] <= pos + epsilon then
                low = mid + 1
            else
                high = mid - 1
            end
        end
        if low <= #keyframes and keyframes[low] <= duration_ms / 1000 then
            keyframe_target = low
        else
            return
        end
    else
        return
    end
    if not keyframe_seek then
        seek_keyframe(keyframe_target, false)
    end
end

mp.register_script_message("keyframe", jump_keyframe)

-- Defaults are scoped to trim mode; input.conf keeps priority over these keys.
local shortcut_section = "trim-mode-shortcuts"
local shortcut_active

local function shortcut(callback)
    return function()
        if not editor then
            callback()
        end
    end
end

local function add_shortcut(name, callback, flags, alias)
    callback = shortcut(callback)
    -- Public names use dashes; keep the earlier client/name form compatible.
    for _, binding_name in ipairs({ name, alias }) do
        mp.add_key_binding(nil, "trim-mode-" .. binding_name, callback, flags)
        mp.add_key_binding(nil, binding_name, callback, flags)
    end
end

add_shortcut("prev-keyframe", function()
    jump_keyframe("previous")
end, { repeatable = true }, "previous-keyframe")

add_shortcut("next-keyframe", function()
    jump_keyframe("next")
end, { repeatable = true })

add_shortcut("set-start", function()
    set_boundary("start")
end)

add_shortcut("set-end", function()
    set_boundary("end")
end)

add_shortcut("export-copy", function()
    start_export("keep")
end)

add_shortcut("export-replace", function()
    start_export("replace")
end)

local shortcut_prefix = "script-binding trim-mode-"
mp.set_key_bindings({
    { "[", shortcut_prefix .. "prev-keyframe" },
    { "]", shortcut_prefix .. "next-keyframe" },
    { "i", shortcut_prefix .. "set-start" },
    { "o", shortcut_prefix .. "set-end" },
    { "Ctrl+e", shortcut_prefix .. "export-copy" },
    { "Ctrl+E", shortcut_prefix .. "export-replace" },
}, shortcut_section, "force")

update_shortcuts = function()
    local active = enabled and editor == nil and not gtk_editing
    if active == shortcut_active then
        return
    end
    shortcut_active = active
    if active then
        mp.enable_key_bindings(shortcut_section, "allow-hide-cursor+allow-vo-dragging")
        -- User bindings outrank trim defaults; trim defaults outrank player defaults.
        mp.commandv("enable-section", "mpv-gtk-edits")
        mp.commandv("enable-section", "default")
    else
        mp.disable_key_bindings(shortcut_section)
    end
end

local function publish()
    update_shortcuts()
    mp.set_property_native("user-data/trim-mode", {
        enabled = enabled,
        keyframes = keyframes,
    })
end

local function stop_scan()
    revision = revision + 1
    if scan_job then
        mp.abort_async_command(scan_job)
        scan_job = nil
        cached_file = nil
    end
end

local function remember(file_key, times)
    cache[file_key] = times
    cache_order[#cache_order + 1] = file_key
    if #cache_order > 4 then
        cache[table.remove(cache_order, 1)] = nil
    end
end

local function scan_keyframes()
    if not loaded then
        return
    end

    local track = mp.get_property_native("current-tracks/video")
    local path = track and (track["external-filename"] or mp.get_property("path"))
    if not track or track.image or track.albumart or not path then
        stop_scan()
        cached_file = nil
        keyframes = {}
        publish()
        return
    end

    path = utils.join_path(mp.get_property("working-directory", ""), path)
    local stream = tostring(track["ff-index"] or "v:0")
    local offset = mp.get_property_bool("rebase-start-time", true) and mp.get_property_number("demuxer-start-time", 0)
        or 0
    -- File metadata prevents reusing markers after the file has changed.
    local info = utils.file_info(path)
    local file_key = table.concat(
        { path, stream, tostring(offset), tostring(info and info.size), tostring(info and info.mtime) },
        "\n"
    )
    if cached_file == file_key then
        return
    end

    stop_scan()
    cached_file = file_key
    keyframes = cache[file_key] or {}
    publish()

    -- Reuse recent files, and never probe network streams or fabricate markers.
    if cache[file_key] or not info or not info.is_file then
        return
    end

    local request_revision = revision
    local started = mp.get_time()
    scan_job = mp.command_native_async({
        name = "subprocess",
        playback_only = true,
        capture_stdout = true,
        capture_stderr = true,
        capture_size = capture_limit,
        args = {
            options.ffprobe_path,
            "-v",
            "error",
            "-select_streams",
            stream,
            "-show_packets",
            "-show_entries",
            "packet=pts_time,dts_time,flags:packet_side_data=",
            "-of",
            "csv=p=0",
            "-i",
            path,
        },
    }, function(success, result)
        if request_revision ~= revision then
            return
        end
        scan_job = nil
        if not success or not result or result.status ~= 0 then
            cached_file = nil
            mp.msg.warn("Could not read keyframe markers with ffprobe.")
            return
        end
        local output = result.stdout or ""
        if #output >= capture_limit then
            cached_file = nil
            mp.msg.warn("Keyframe scan output exceeded its capture limit.")
            return
        end

        -- Packet flags identify keyframes without decoding video frames.
        local times = {}
        for line in output:gmatch("[^\r\n]+") do
            local pts, dts, flags = line:match("^([^,]*),([^,]*),([^,]*)")
            if flags and flags:find("K", 1, true) and not flags:find("D", 1, true) then
                local timestamp = tonumber(pts) or tonumber(dts)
                if timestamp then
                    timestamp = timestamp - offset
                    if timestamp >= -0.001 then
                        times[#times + 1] = math.max(0, timestamp)
                    end
                end
            end
        end
        table.sort(times)
        keyframes = {}
        for _, timestamp in ipairs(times) do
            if #keyframes == 0 or timestamp ~= keyframes[#keyframes] then
                keyframes[#keyframes + 1] = timestamp
            end
        end
        remember(file_key, keyframes)
        mp.msg.verbose(string.format("Read %d keyframe markers in %.3f seconds.", #keyframes, mp.get_time() - started))
        publish()
    end)
end

local function toggle_mode()
    if enabled then
        finish_edit(true, false)
    end
    cancel_keyframe_seek()
    enabled = not enabled
    save_mode()
    publish()
    -- Keep preloading when the user returns to player mode.
    scan_keyframes()
end

mp.register_script_message("toggle", toggle_mode)
mp.add_key_binding(nil, "toggle", toggle_mode)
mp.add_key_binding(nil, "trim-mode-toggle", toggle_mode)
-- Ctrl+T is registered by the GTK frontend so loading remains lazy.
mp.register_script_message("set-enabled", function(value)
    if enabled ~= (value == "yes") then toggle_mode() end
end)
mp.register_script_message("gtk-focus", function(value)
    gtk_editing = value == "yes"
    update_shortcuts()
end)
mp.register_script_message("set-time", function(boundary, text, serial)
    local ok = enabled and available() and (boundary == "start" or boundary == "end")
        and apply_boundary(boundary, parse_time(text or "")) or false
    publish_controls()
    mp.set_property_native("user-data/trim-field-result", {
        boundary = boundary, serial = serial, valid = ok,
        text = format_boundary(boundary == "start" and start_ms or end_ms),
    })
end)

local function reset_file()
    cancel_keyframe_seek()
    loaded = false
    editor = nil
    start_ms, end_ms, duration_ms = 0, 0, 0
    end_follows_duration = true
    file_serial = file_serial + 1
    stop_scan()
    cached_file = nil
    keyframes = {}
    publish()
    publish_controls()
end

local function refresh_duration()
    local duration = mp.get_property_number("duration", 0)
    local updated = math.max(0, math.floor(duration * 1000 + 0.000001))
    if end_follows_duration then
        end_ms = updated
    else
        end_ms = math.min(end_ms, updated)
    end
    duration_ms = updated
    -- Reset a selection that is completely outside a shortened file.
    if duration_ms > 0 and start_ms >= end_ms then
        start_ms, end_ms = 0, duration_ms
    else
        start_ms = math.min(start_ms, end_ms)
    end
    if editor and not available() then
        editor = nil
    end
    publish_controls()
end

mp.register_event("start-file", reset_file)
mp.register_event("end-file", reset_file)
mp.register_event("file-loaded", function()
    loaded = true
    refresh_duration()
    scan_keyframes()
end)

mp.observe_property("duration", "number", refresh_duration)
mp.observe_property("vid", "native", function()
    cancel_keyframe_seek()
    refresh_duration()
    scan_keyframes()
end)

mp.observe_property("focused", "bool", function(_, focused)
    if focused == false then
        finish_edit(true, false)
    end
end)

mp.register_event("shutdown", function()
    stop_scan()
    mp.del_property("user-data/trim-mode")
    mp.del_property("user-data/trim-controls")
end)

publish()
publish_controls()

-- Native Windows operations; POSIX links/rename on Linux, including plain Lua mpv builds.
local file_ops = {}
do
    local is_windows = package.config:sub(1, 1) == "\\"
    local ffi_ok, ffi = pcall(require, "ffi")
    local native, native_error
    local serial = 0
    local platform = ffi_ok and ffi.os or (is_windows and "Windows" or "Linux")
    file_ops.windows = is_windows
    file_ops.platform = platform

    function file_ops.normalize(path)
        local absolute = utils.join_path(mp.get_property("working-directory", ""), path)
        return mp.command_native({ "normalize-path", absolute }) or absolute
    end

    function file_ops.same(first, second)
        if not first or not second then
            return false
        end
        first, second = file_ops.normalize(first), file_ops.normalize(second)
        if is_windows then
            first, second = first:lower(), second:lower()
        end
        return first == second
    end

    if ffi_ok and is_windows then
        local ok, error = pcall(function()
            ffi.cdef([[
                typedef struct { uint32_t low, high; } trim_filetime;
                typedef struct {
                    uint32_t attributes;
                    trim_filetime creation, access, write;
                    uint32_t size_high, size_low;
                } trim_file_attributes;
                int __stdcall MultiByteToWideChar(unsigned int, unsigned long, const char*, int, uint16_t*, int);
                int __stdcall GetFileAttributesExW(const uint16_t*, int, trim_file_attributes*);
                int __stdcall MoveFileW(const uint16_t*, const uint16_t*);
                int __stdcall ReplaceFileW(const uint16_t*, const uint16_t*, const uint16_t*, unsigned long, void*, void*);
                int __stdcall DeleteFileW(const uint16_t*);
                int __stdcall CreateDirectoryW(const uint16_t*, void*);
                void* __stdcall CreateFileW(const uint16_t*, unsigned long, unsigned long, void*, unsigned long, unsigned long, void*);
                int __stdcall WriteFile(void*, const void*, unsigned long, unsigned long*, void*);
                int __stdcall CloseHandle(void*);
                unsigned long __stdcall GetLastError(void);
                unsigned long __stdcall GetCurrentProcessId(void);
            ]])

            local function wide(path)
                -- All file operations receive normalized absolute paths.
                path = path:gsub("/", "\\")
                if path:sub(1, 4) ~= "\\\\?\\" then
                    if path:sub(1, 2) == "\\\\" then
                        path = "\\\\?\\UNC\\" .. path:sub(3)
                    else
                        path = "\\\\?\\" .. path
                    end
                end
                local count = ffi.C.MultiByteToWideChar(65001, 8, path, #path, nil, 0)
                if count == 0 then
                    error("The filename is not valid UTF-8.")
                end
                local result = ffi.new("uint16_t[?]", count + 1)
                if ffi.C.MultiByteToWideChar(65001, 8, path, #path, result, count) == 0 then
                    error("Could not convert the filename to UTF-16.")
                end
                return result
            end

            local function failure()
                local code = tonumber(ffi.C.GetLastError())
                return nil, "Windows file operation failed (error " .. code .. ").", code
            end

            native = { pid = tonumber(ffi.C.GetCurrentProcessId()) }

            function native.metadata(path)
                local attr = ffi.new("trim_file_attributes[1]")
                if ffi.C.GetFileAttributesExW(wide(path), 0, attr) == 0 then
                    return failure()
                end
                local value = attr[0]
                if math.floor(tonumber(value.attributes) / 16) % 2 == 1 then
                    return nil, "Only local video files can be exported."
                end
                return {
                    size = tonumber(value.size_high) * 4294967296 + tonumber(value.size_low),
                    signature = string.format(
                        "%u:%u:%u:%u",
                        value.size_high,
                        value.size_low,
                        value.write.high,
                        value.write.low
                    ),
                }
            end

            function native.move(from, to)
                if ffi.C.MoveFileW(wide(from), wide(to)) == 0 then
                    return failure()
                end
                return true
            end

            function native.replace(source, replacement, backup)
                if ffi.C.ReplaceFileW(wide(source), wide(replacement), wide(backup), 2, nil, nil) == 0 then
                    return failure()
                end
                return true
            end

            function native.remove(path)
                if ffi.C.DeleteFileW(wide(path)) == 0 then
                    local ok, error, code = failure()
                    if code == 2 or code == 3 then
                        return true
                    end
                    return ok, error, code
                end
                return true
            end

            function native.mkdir(path)
                local info = utils.file_info(path)
                if info then
                    return info.is_dir or nil,
                        not info.is_dir and "The configuration directory is occupied by a file." or nil
                end
                local parent = utils.split_path(path)
                parent = parent:gsub("[\\/]+$", "")
                if parent:match("^%a:$") then
                    parent = parent .. "\\"
                end
                if parent ~= "" and not file_ops.same(parent, path) then
                    local ok, error = native.mkdir(parent)
                    if not ok then
                        return nil, error
                    end
                end
                if ffi.C.CreateDirectoryW(wide(path), nil) ~= 0 then
                    return true
                end
                local ok, error, code = failure()
                if code == 183 then
                    info = utils.file_info(path)
                    if info and info.is_dir then
                        return true
                    end
                end
                return ok, error
            end

            function native.create_config(path, contents)
                -- CREATE_NEW is exclusive: an existing user file can never be truncated.
                local handle = ffi.C.CreateFileW(wide(path), 1073741824, 0, nil, 1, 128, nil)
                if handle == ffi.cast("void*", -1) then
                    local ok, error, code = failure()
                    if code == 80 or code == 183 then
                        return true
                    end
                    return ok, error
                end
                local count = ffi.new("unsigned long[1]")
                local written = ffi.C.WriteFile(handle, contents, #contents, count, nil) ~= 0
                local error = not written
                        and ("Windows configuration write failed (error " .. tonumber(ffi.C.GetLastError()) .. ").")
                    or nil
                ffi.C.CloseHandle(handle)
                if not written or tonumber(count[0]) ~= #contents then
                    return nil, error or "The configuration could not be written completely."
                end
                return true
            end
        end)

        if not ok then
            native = nil
            native_error = tostring(error)
        end
    elseif ffi_ok and platform == "Linux" then
        local ok, error = pcall(function()
            ffi.cdef([[int link(const char*, const char*); int chmod(const char*, unsigned int); int getpid(void);]])
            native = { pid = tonumber(ffi.C.getpid()) }

            function native.link(from, to)
                if ffi.C.link(from, to) ~= 0 then
                    return nil, "Could not create a hard link (errno " .. ffi.errno() .. ")."
                end
                return true
            end

            function native.chmod(path, mode)
                if ffi.C.chmod(path, mode) ~= 0 then
                    return nil, "Could not preserve file permissions (errno " .. ffi.errno() .. ")."
                end
                return true
            end
        end)

        if not ok then
            native = nil
            native_error = tostring(error)
        end
    end

    local function exists(path)
        return path and utils.file_info(path) ~= nil
    end

    local function remove(path)
        if is_windows then
            return native.remove(path)
        end
        if not exists(path) then
            return true
        end
        return os.remove(path)
    end

    local function assert_owned(j)
        local directory = utils.split_path(j.source)
        for _, entry in ipairs({ { j.temp, "partial" }, { j.backup, "original" } }) do
            local parent, name = utils.split_path(entry[1] or "")
            local token, suffix = name:match("^%.mpv%-trim%-(%x+)%.(.*)$")
            if
                not file_ops.same(parent, directory)
                or not token
                or #token ~= 32
                or not (suffix == entry[2] or suffix:sub(1, #entry[2] + 1) == entry[2] .. ".")
                or file_ops.same(entry[1], j.source)
            then
                return nil, "Unrecognized export temporary path."
            end
        end
        return true
    end

    local function format_time(value)
        return string.format(
            "%02d.%02d.%02d.%03d",
            math.floor(value / 3600000),
            math.floor(value / 60000) % 60,
            math.floor(value / 1000) % 60,
            value % 1000
        )
    end

    local function split_extension(name)
        local stem, extension = name:match("^(.+)(%.[^%.]+)$")
        return stem or name, extension or ""
    end

    local function process(run, j, args, callback)
        run(j, args, function(ok, result, error)
            if ok and result.status == 0 then
                callback(true, result.stdout or "")
            else
                callback(
                    false,
                    (result.stderr and result.stderr ~= "" and result.stderr)
                        or error
                        or "System file operation failed."
                )
            end
        end)
    end

    local function metadata(run, j, callback)
        if is_windows then
            local data, error = native.metadata(j.source)
            callback(data ~= nil, data or {}, error)
        else
            local info = utils.file_info(j.source)
            if not info or not info.is_file then
                callback(false, {}, "Only local video files can be exported.")
                return
            end
            -- GNU stat provides nanosecond timestamps on plain Lua as well as LuaJIT.
            -- Do not include ctime: adding the backup hard link changes it.
            process(run, j, { "stat", "-L", "-c", "%s|%y|%i|%d", "--", j.source }, function(ok, value)
                callback(
                    ok,
                    ok and { size = info.size, mode = info.mode % 512, signature = value:gsub("[\r\n]+$", "") } or {},
                    not ok and value or nil
                )
            end)
        end
    end

    local function link(run, j, from, to, callback)
        if native and native.link then
            local ok, error = native.link(from, to)
            callback(ok, error)
        else
            process(run, j, { "ln", "-P", "-T", "--", from, to }, callback)
        end
    end

    local function chmod(run, j, path, mode, callback)
        if native and native.chmod then
            local ok, error = native.chmod(path, mode)
            callback(ok, error)
        else
            process(run, j, { "chmod", string.format("%o", mode), "--", path }, callback)
        end
    end

    local function retry(j, operation, callback)
        local count = 0

        local function attempt()
            j.file_retry = nil
            local ok, error, code = operation()
            if not ok and (code == 5 or code == 32 or code == 33 or code == 1175) and count < 19 then
                count = count + 1
                j.file_retry = mp.add_timeout(0.1, attempt)
            else
                callback(ok, error)
            end
        end

        attempt()
    end

    local function check_original(run, j, callback)
        metadata(run, j, function(ok, data, error)
            if not ok then
                callback(false, error)
            elseif data.signature ~= j.signature then
                callback(false, "The original changed during export; it was not replaced.")
            else
                callback(true, data)
            end
        end)
    end

    function file_ops.ensure_config(contents)
        if mp.get_property_native("options/config") == false then
            return
        end
        if mp.find_config_file("script-opts/trim-mode.conf") then
            return
        end
        local home = mp.command_native({ "expand-path", "~~home/" })
        if is_windows and mp.get_property("options/config-dir", "") == "" then
            -- Windows also searches <mpv.exe directory>/mpv, while ~~home/ may still be AppData.
            local main_config = mp.find_config_file("mpv.conf")
            if main_config then
                home = utils.split_path(main_config)
            else
                local script = mp.find_config_file("scripts/trim-mode.lua")
                if script then
                    home = utils.split_path((utils.split_path(script):gsub("[\\/]+$", "")))
                end
            end
        end
        if not home or home == "" then
            return
        end
        local directory = file_ops.normalize(utils.join_path(home, "script-opts"))
        local path = utils.join_path(directory, "trim-mode.conf")
        if exists(path) then
            return
        end

        local function warn(error)
            mp.msg.warn("Could not create trim-mode.conf: " .. tostring(error))
        end

        if is_windows then
            if not native then
                warn(native_error or "Native Windows file operations are unavailable.")
                return
            end
            local ok, error = native.mkdir(directory)
            if ok then
                ok, error = native.create_config(path, contents:gsub("\r?\n", "\r\n"))
            end
            if not ok then
                warn(error)
            end
            return
        end
        if platform ~= "Linux" then
            return
        end
        local task = {}

        local function run_config(_, args, callback)
            task.process = mp.command_native_async({
                name = "subprocess",
                args = args,
                playback_only = false,
                capture_stdout = true,
                capture_stderr = true,
                capture_size = 65536,
            }, function(ok, result, error)
                task.process = nil
                if not task.stopped then
                    callback(ok, result or {}, error)
                end
            end)
        end

        local function cleanup()
            if task.temp then
                os.remove(task.temp)
                task.temp = nil
            end
        end

        mp.register_event("shutdown", function()
            task.stopped = true
            if task.process then
                mp.abort_async_command(task.process)
            end
            cleanup()
        end)

        process(run_config, task, { "mkdir", "-p", "--", directory }, function(ok, error)
            if not ok then
                warn(error)
                return
            end
            if exists(path) then
                return
            end
            serial = serial + 1
            task.temp = utils.join_path(
                directory,
                string.format(".trim-mode-config-%d-%d-%d.tmp", mp.get_property_number("pid", 0), os.time(), serial)
            )
            if exists(task.temp) then
                warn("The configuration temporary path is occupied.")
                task.temp = nil
                return
            end
            local file, error = io.open(task.temp, "wb")
            if not file then
                warn(error)
                task.temp = nil
                return
            end
            local written, error = file:write(contents)
            local closed, close_error = file:close()
            if not written or not closed then
                cleanup()
                warn(error or close_error)
                return
            end
            if exists(path) then
                cleanup()
                return
            end
            link(run_config, task, task.temp, path, function(ok, error)
                if ok or exists(path) then
                    cleanup()
                    return
                end
                -- Support filesystems without hard links; -T prevents copying into a directory.
                process(run_config, task, { "cp", "-T", "--no-clobber", "--", task.temp, path }, function(ok, error)
                    cleanup()
                    if not ok then
                        warn(error)
                    end
                end)
            end)
        end)
    end

    function file_ops.action(run, j, mode, cleanup_backup, callback)
        if is_windows and not native then
            callback(false, {}, "This Windows mpv build needs LuaJIT for file operations. " .. (native_error or ""))
            return
        end
        if platform ~= "Windows" and platform ~= "Linux" then
            callback(false, {}, "Export is supported on Windows and Linux.")
            return
        end
        if mode == "Prepare" then
            j.source = file_ops.normalize(j.source or j.input_path)
            metadata(run, j, function(ok, data, error)
                if not ok then
                    callback(false, {}, error)
                    return
                end
                local directory, name = utils.split_path(j.source)
                local stem, extension = split_extension(name)
                local temp, backup
                repeat
                    serial = serial + 1
                    local token = string.format(
                        "%08x%08x%08x%08x",
                        os.time() % 4294967296,
                        math.floor(mp.get_time() * 1000000) % 4294967296,
                        mp.get_property_number("pid", native and native.pid or 0) % 4294967296,
                        serial
                    )
                    temp = utils.join_path(directory, ".mpv-trim-" .. token .. ".partial" .. extension)
                    backup = utils.join_path(directory, ".mpv-trim-" .. token .. ".original" .. extension)
                until not exists(temp) and not exists(backup)
                callback(true, {
                    source = j.source,
                    temp = temp,
                    backup = backup,
                    signature = data.signature,
                    mode = data.mode,
                    target = utils.join_path(
                        directory,
                        stem .. "_" .. format_time(j.start_ms) .. "-" .. format_time(j.end_ms) .. extension
                    ),
                })
            end)

            return
        end
        local owned, error = assert_owned(j)
        if not owned then
            callback(false, {}, error)
            return
        end
        if mode == "Cleanup" then
            local ok, error = remove(j.temp)
            if ok and cleanup_backup then
                ok, error = remove(j.backup)
            end
            callback(ok, {}, error)
            return
        end
        if mode == "Rollback" then
            if not exists(j.backup) then
                callback(false, {}, "The original backup is missing.")
                return
            end
            if is_windows then
                retry(j, function()
                    if exists(j.source) then
                        return native.replace(j.source, j.backup, j.temp)
                    end
                    return native.move(j.backup, j.source)
                end, function(ok, error)
                    callback(ok, { path = j.source }, error)
                end)
            else
                local ok, error = os.rename(j.backup, j.source)
                callback(ok, { path = j.source }, error)
            end
            return
        end
        check_original(run, j, function(ok, data)
            if not ok then
                callback(false, {}, data)
                return
            end
            local info = utils.file_info(j.temp)
            if not info or not info.is_file or info.size == 0 then
                callback(false, {}, "The exported file is empty or missing.")
                return
            end
            if mode == "Copy" then
                local directory, name = utils.split_path(j.target)
                if not file_ops.same(directory, utils.split_path(j.source)) or file_ops.same(j.target, j.source) then
                    callback(false, {}, "Invalid export destination.")
                    return
                end
                local stem, extension = split_extension(name)
                local suffix = 0

                local function next_name()
                    suffix = suffix + 1
                    if suffix > 10000 then
                        callback(false, {}, "Too many exports already use this filename.")
                        return
                    end
                    local target =
                        utils.join_path(directory, stem .. (suffix == 1 and "" or "_" .. suffix) .. extension)

                    local function moved(ok, error)
                        if ok then
                            callback(true, { path = target })
                        elseif exists(target) then
                            mp.add_timeout(0, next_name)
                        else
                            callback(false, {}, error)
                        end
                    end

                    if is_windows then
                        local ok, error = native.move(j.temp, target)
                        moved(ok, error)
                    else
                        -- link() never overwrites an existing file, even during simultaneous exports.
                        link(run, j, j.temp, target, function(ok, error)
                            if ok or exists(target) then
                                moved(ok, error)
                                return
                            end
                            -- Some removable filesystems have no hard links. mv -n still refuses overwrite.
                            process(run, j, { "mv", "-n", "-T", "--", j.temp, target }, function(ok, error)
                                moved(ok and not exists(j.temp) and exists(target), error)
                            end)
                        end)
                    end
                end

                next_name()
            elseif mode == "Replace" then
                if exists(j.backup) then
                    callback(false, {}, "The original backup path is occupied.")
                    return
                end
                if is_windows then
                    retry(j, function()
                        local info, error = native.metadata(j.source)
                        if not info then
                            return nil, error
                        end
                        if info.signature ~= j.signature then
                            return nil, "The original changed during export; it was not replaced."
                        end
                        return native.replace(j.source, j.temp, j.backup)
                    end, function(ok, error)
                        callback(ok, { path = j.source }, error)
                    end)
                else
                    chmod(run, j, j.temp, data.mode, function(ok, error)
                        if not ok then
                            callback(false, {}, error)
                            return
                        end

                        -- Keep the original inode under a backup name, then atomically replace its directory entry.
                        local function backed_up(ok, error)
                            if not ok then
                                callback(false, {}, error)
                                return
                            end
                            check_original(run, j, function(ok, error)
                                if not ok then
                                    remove(j.backup)
                                    callback(false, {}, error)
                                    return
                                end
                                local renamed, error = os.rename(j.temp, j.source)
                                if not renamed then
                                    remove(j.backup)
                                end
                                callback(renamed, { path = j.source }, error)
                            end)
                        end

                        link(run, j, j.source, j.backup, function(ok, error)
                            if ok or exists(j.backup) then
                                backed_up(ok, error)
                                return
                            end
                            -- Keep a byte-for-byte backup when hard links are unavailable.
                            process(
                                run,
                                j,
                                { "cp", "-T", "--no-clobber", "--preserve=mode,timestamps", "--", j.source, j.backup },
                                function(ok, error)
                                    local backup_info = utils.file_info(j.backup)
                                    if not ok or not backup_info or backup_info.size ~= data.size then
                                        remove(j.backup)
                                        backed_up(false, error or "Could not create the original backup.")
                                    else
                                        backed_up(true)
                                    end
                                end
                            )
                        end)
                    end)
                end
            else
                callback(false, {}, "Unknown export file operation.")
            end
        end)
    end
end

file_ops.ensure_config([[# remember the last trim mode on/off state between mpv launches
remember=no

# open the exported video in the current mpv window after replacing the original
open-replace=yes

# open the exported copy in the current mpv window
open-copy=yes
]])

-- Export service shares the same script, options, and message handler.
do
    local job, deferred_restore

    local function publish(phase)
        if not job then
            return
        end
        job.phase = phase or job.phase
        mp.set_property_native("user-data/trim-export", {
            busy = true,
            phase = job.phase,
            mode = job.mode,
            source = job.source or job.input_path,
            start_ms = job.start_ms,
            end_ms = job.end_ms,
            path = job.target,
        })
    end

    local function restore_options(saved)
        if not saved then
            return
        end
        mp.set_property("idle", saved.idle)
        mp.set_property("force-window", saved.force_window)
    end

    local function failure_message(detail)
        mp.msg.verbose("Export diagnostic: " .. tostring(detail))
        local text = tostring(detail or ""):lower()
        local code = text:match("windows file operation failed %(error (%d+)%)")
        if code == "32" or code == "33" or code == "1224" then
            return "Export failed: file is in use."
        end
        for _, phrase in ipairs({
            "file is in use",
            "used by another process",
            "sharing violation",
            "lock violation",
            "device or resource busy",
            "text file busy",
        }) do
            if text:find(phrase, 1, true) then
                return "Export failed: file is in use."
            end
        end
        return "Export failed: unexpected error."
    end

    local function finish(j, success, message, path, backup)
        if job ~= j then
            return
        end
        if j.timeout then
            j.timeout:kill()
        end
        if j.saved then
            if mp.get_property_bool("idle-active", false) then
                -- With automatic opening disabled, leave this window available for a new file.
                deferred_restore = j.saved
                mp.set_property_bool("pause", j.saved.pause)
            else
                restore_options(j.saved)
            end
        end
        if not success then
            message = failure_message(message)
        end
        job = nil
        mp.set_property_native(
            "user-data/trim-export",
            { busy = false, success = success, path = path, error = not success and message or nil, backup = backup }
        )
        if success then
            mp.msg.info(message)
        else
            mp.msg.error(message)
        end
    end

    local function run(j, args, callback)
        j.process = mp.command_native_async({
            name = "subprocess",
            args = args,
            playback_only = false,
            capture_stdout = true,
            capture_stderr = true,
            capture_size = 1024 * 1024,
        }, function(ok, result, error)
            if job ~= j then
                return
            end
            j.process = nil
            callback(ok, result or {}, error)
        end)
    end

    local function file_action(j, mode, extra, callback)
        file_ops.action(run, j, mode, extra and extra[1] == "-CleanupBackup", function(ok, data, error)
            if job == j then
                callback(ok, data, error)
            end
        end)
    end

    local function error_text(result, fallback)
        local text = result.stderr or ""
        if text == "" then
            return fallback
        end
        return text
    end

    local function clean_failure(j, message)
        if not j.temp then
            finish(j, false, "Export failed: " .. message)
            return
        end
        file_action(j, "Cleanup", nil, function(ok, _, error)
            if not ok then
                mp.msg.verbose("Export cleanup: " .. tostring(error))
            end
            finish(j, false, "Export failed: " .. message, j.mode == "keep" and j.output or nil)
        end)
    end

    local function complete(j)
        publish("cleanup")
        file_action(j, "Cleanup", j.committed and { "-CleanupBackup" } or nil, function(ok, _, error)
            local _, name = utils.split_path(j.output)
            local message = "Exported: " .. name
            if not ok then
                mp.msg.verbose("Export cleanup: " .. tostring(error))
                if utils.file_info(j.backup) then
                    mp.msg.verbose("Export recovery: Original backup: " .. j.backup)
                end
            end
            finish(j, true, message, j.output, not ok and utils.file_info(j.backup) and j.backup or nil)
        end)
    end

    local rollback, open_video, commit, recover_open

    local function recovery_notice(j)
        return utils.file_info(j.backup) and (" Original backup: " .. j.backup) or (" Original file: " .. j.source)
    end

    local same_path = file_ops.same

    local function same_source(j)
        return same_path(mp.get_property("path"), j.source)
    end

    local function save_playback(j)
        j.saved = {
            idle = mp.get_property("idle", "no"),
            force_window = mp.get_property("force-window", "no"),
            pause = mp.get_property_bool("pause"),
            position = mp.get_property_number("time-pos", 0),
            index = mp.get_property_number("playlist-pos", 0),
        }
        mp.set_property("idle", "yes")
        mp.set_property("force-window", "yes")
        mp.set_property_bool("pause", true)
    end

    open_video = function(j, restoring)
        if j.timeout then
            j.timeout:kill()
            j.timeout = nil
        end
        publish(restoring and "restoring" or "opening")
        j.open_path = restoring and j.source or j.output
        j.open_target = restoring and j.saved.position or 0
        j.seek_issued = false
        j.expected_load = true
        j.timeout = mp.add_timeout(15, function()
            if job ~= j then
                return
            end
            recover_open(j, "Could not open the exported video.")
        end)

        if j.mode == "keep" and not restoring then
            -- Keep the original playlist entry and open the copy right after it.
            mp.commandv("loadfile", j.output, "insert-at", j.saved.index + 1)
            mp.commandv("playlist-play-index", j.saved.index + 1)
        else
            -- Replacement and recovery reuse the original playlist entry.
            mp.commandv("playlist-play-index", j.saved.index)
        end
    end

    rollback = function(j, message)
        if j.timeout then
            j.timeout:kill()
            j.timeout = nil
        end
        j.failure = "Export failed: " .. message
        publish("rollback")
        -- Unload any failed reload before restoring the original bytes.
        mp.commandv("stop", "keep-playlist")
        mp.add_timeout(0.05, function()
            if job ~= j then
                return
            end
            file_action(j, "Rollback", nil, function(ok, _, error)
                if not ok then
                    finish(
                        j,
                        false,
                        j.failure .. " Original backup: " .. j.backup .. " (" .. error .. ")",
                        nil,
                        j.backup
                    )
                    return
                end
                j.committed = false
                open_video(j, true)
            end)
        end)
    end

    recover_open = function(j, message)
        if j.phase == "opening" then
            if j.committed then
                rollback(j, message)
            else
                j.failure = "Export failed: " .. message .. " Saved copy: " .. j.output
                open_video(j, true)
            end
        else
            finish(
                j,
                false,
                (j.failure or "Could not reopen the original.") .. recovery_notice(j),
                nil,
                utils.file_info(j.backup) and j.backup or nil
            )
        end
    end

    commit = function(j)
        publish("committing")
        file_action(j, j.mode == "replace" and "Replace" or "Copy", nil, function(ok, data, error)
            if not ok then
                if j.saved and not j.user_changed then
                    j.failure = "Export failed: " .. error
                    open_video(j, true)
                else
                    clean_failure(j, error)
                end
                return
            end
            j.output = data.path
            j.committed = j.mode == "replace"
            if j.mode == "keep" and j.open_result and same_source(j) then
                save_playback(j)
            end
            if j.saved and not j.user_changed and j.open_result then
                open_video(j, false)
            else
                complete(j)
            end
        end)
    end

    local function validated(j)
        if j.mode ~= "replace" or not same_source(j) then
            commit(j)
            return
        end
        save_playback(j)
        publish("closing")
        mp.commandv("stop", "keep-playlist")
    end

    local function verify(j)
        publish("verifying")
        run(j, {
            options.ffprobe_path,
            "-v",
            "error",
            "-show_entries",
            "stream=index,codec_type,width,height:stream_disposition=attached_pic:format=duration",
            "-of",
            "json",
            j.temp,
        }, function(ok, result)
            local data = utils.parse_json(result.stdout or "")
            local video
            for _, stream in ipairs(data and data.streams or {}) do
                if
                    stream.codec_type == "video"
                    and (stream.width or 0) > 0
                    and (stream.height or 0) > 0
                    and not (stream.disposition and stream.disposition.attached_pic == 1)
                then
                    video = stream.index
                    break
                end
            end
            if not ok or result.status ~= 0 or not video then
                clean_failure(j, error_text(result, "The export has no video stream."))
                return
            end
            -- Decode one frame to reject empty cuts or broken output before replacement.
            run(j, {
                options.ffmpeg_path,
                "-hide_banner",
                "-loglevel",
                "error",
                "-nostdin",
                "-xerror",
                "-i",
                j.temp,
                "-map",
                "0:" .. video,
                "-frames:v",
                "1",
                "-an",
                "-sn",
                "-f",
                "framehash",
                "pipe:1",
            }, function(decoded, frame)
                if not decoded or frame.status ~= 0 or not (frame.stdout or ""):match("\n%d+,%s*%-?%d+,") then
                    clean_failure(j, error_text(frame, "The selected interval has no decodable video frame."))
                    return
                end
                validated(j)
            end)
        end)
    end

    export_request = function(mode)
        if job or (mode ~= "keep" and mode ~= "replace") then
            return
        end
        local controls = mp.get_property_native("user-data/trim-controls") or {}
        local trim = mp.get_property_native("user-data/trim-mode") or {}
        local input_path = mp.get_property("path")
        if
            not trim.enabled
            or not controls.available
            or not input_path
            or not controls.start_ms
            or not controls.end_ms
            or controls.end_ms <= controls.start_ms
        then
            return
        end
        local path = utils.join_path(mp.get_property("working-directory", ""), input_path)
        local info = utils.file_info(path)
        if not info or not info.is_file then
            local message = failure_message("Only local video files can be exported.")
            mp.msg.error(message)
            mp.set_property_native("user-data/trim-export", {
                busy = false, success = false, error = message, serial = mp.get_time(),
            })
            return
        end
        job = {
            mode = mode,
            input_path = input_path,
            start_ms = controls.start_ms,
            end_ms = controls.end_ms,
            phase = "preparing",
            started_at = mp.get_time(),
            open_result = mode == "replace" and options["open-replace"] or (mode == "keep" and options["open-copy"]),
        }
        local j = job
        publish()
        j.source = path
        file_action(
            j,
            "Prepare",
            { "-StartMs", tostring(j.start_ms), "-EndMs", tostring(j.end_ms) },
            function(ok, data, error)
                if not ok then
                    clean_failure(j, error)
                    return
                end
                for _, key in ipairs({ "source", "temp", "backup", "target", "signature" }) do
                    j[key] = data[key]
                end
                publish("exporting")
                run(j, {
                    options.ffmpeg_path,
                    "-hide_banner",
                    "-loglevel",
                    "error",
                    "-nostdin",
                    "-n",
                    "-ss",
                    string.format("%.3f", j.start_ms / 1000),
                    "-i",
                    j.source,
                    "-t",
                    string.format("%.3f", (j.end_ms - j.start_ms) / 1000),
                    "-map",
                    "0",
                    "-map_metadata",
                    "0",
                    "-map_chapters",
                    "0",
                    -- A subtitle spanning the end of the cut must not extend its duration.
                    "-c",
                    "copy",
                    "-bsf:s",
                    string.format("setts=duration='max(0,min(DURATION,%.3f/TB-PTS))'", (j.end_ms - j.start_ms) / 1000),
                    "-avoid_negative_ts",
                    "make_zero",
                    j.temp,
                }, function(exported, result)
                    if not exported or result.status ~= 0 then
                        clean_failure(j, error_text(result, "The export process failed."))
                        return
                    end
                    verify(j)
                end)
            end
        )
    end

    mp.register_event("end-file", function(event)
        local j = job
        if not j then
            return
        end
        if j.phase == "closing" then
            mp.add_timeout(0.03, function()
                if job == j and j.phase == "closing" then
                    commit(j)
                end
            end)
        elseif (j.phase == "opening" or j.phase == "restoring") and event.reason == "error" then
            recover_open(j, "mpv could not open the exported video.")
        end
    end)

    mp.register_event("file-loaded", function()
        if deferred_restore then
            restore_options(deferred_restore)
            deferred_restore = nil
        end
        local j = job
        if not j or not j.expected_load or (j.phase ~= "opening" and j.phase ~= "restoring") then
            return
        end
        if not same_path(mp.get_property("path"), j.open_path) then
            -- An explicit user file change takes priority over automatic reopening.
            j.expected_load = false
            if j.committed or (j.mode == "keep" and j.output and not j.failure) then
                complete(j)
            else
                clean_failure(j, j.failure or "Could not reopen the original.")
            end
            return
        end
        mp.commandv("seek", j.open_target, "absolute+exact")
        j.seek_issued = true
    end)

    mp.register_event("start-file", function()
        if job and job.saved and not job.expected_load then
            job.user_changed = true
        end
    end)

    mp.register_event("playback-restart", function()
        local j = job
        if not j or not j.seek_issued or mp.get_property_bool("seeking") then
            return
        end
        j.seek_issued = false
        if j.timeout then
            j.timeout:kill()
            j.timeout = nil
        end
        mp.set_property_bool("pause", j.saved.pause)
        if j.phase == "restoring" then
            clean_failure(j, (j.failure or "Export failed."):gsub("^Export failed:%s*", ""))
        elseif j.phase == "opening" then
            complete(j)
        end
    end)

    mp.register_event("shutdown", function()
        if job and job.process then
            mp.abort_async_command(job.process)
        end
        if job and job.file_retry then
            job.file_retry:kill()
        end
        -- A committed original backup is deliberately retained if shutdown interrupts reopening.
        mp.del_property("user-data/trim-export")
    end)

    mp.set_property_native("user-data/trim-export", { busy = false })
end

-- A lazy-loaded script may join an already playing file after file-loaded.
loaded = mp.get_property_number("time-pos") ~= nil and not mp.get_property_bool("idle-active", true)
refresh_duration()
if enabled then scan_keyframes() end
publish()
