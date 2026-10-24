#include "input.h"

#include <string.h>

static const mpv_node *field(const mpv_node *node, const char *name)
{
    if (!node || node->format != MPV_FORMAT_NODE_MAP || !node->u.list)
        return NULL;
    for (int i = 0; i < node->u.list->num; ++i)
        if (g_str_equal(node->u.list->keys[i], name))
            return &node->u.list->values[i];
    return NULL;
}

GHashTable *input_bindings_new(void)
{
    return g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}

void input_bindings_update(GHashTable *bindings, const mpv_node *node)
{
    g_return_if_fail(bindings != NULL);
    g_hash_table_remove_all(bindings);
    if (!node || node->format != MPV_FORMAT_NODE_ARRAY || !node->u.list)
        return;
    for (int i = 0; i < node->u.list->num; ++i) {
        const mpv_node *item = &node->u.list->values[i];
        const mpv_node *key = field(item, "key");
        const mpv_node *weak = field(item, "is_weak");
        const mpv_node *priority = field(item, "priority");
        if (!key || key->format != MPV_FORMAT_STRING || !key->u.string || !*key->u.string ||
            !weak || weak->format != MPV_FORMAT_FLAG || weak->u.flag ||
            !priority || priority->format != MPV_FORMAT_INT64 || priority->u.int64 < 0)
            continue;
        /* Keys already use mpv's canonical spelling and modifier order.
         * In particular, an explicit "ignore" command is still a binding. */
        g_hash_table_add(bindings, g_strdup(key->u.string));
    }
}

gboolean input_bindings_contains(GHashTable *bindings, const char *key_name)
{
    return bindings && key_name && g_hash_table_contains(bindings, key_name);
}

static const char *named_key(guint keyval)
{
    switch (keyval) {
    case GDK_KEY_space: return "SPACE";
    case GDK_KEY_numbersign: return "SHARP";
    case GDK_KEY_BackSpace: return "BS";
    case GDK_KEY_Tab: case GDK_KEY_ISO_Left_Tab: return "TAB";
    case GDK_KEY_Return: return "ENTER";
    case GDK_KEY_Escape: return "ESC";
    case GDK_KEY_Insert: return "INS";
    case GDK_KEY_Delete: return "DEL";
    case GDK_KEY_Home: return "HOME";
    case GDK_KEY_End: return "END";
    case GDK_KEY_Page_Up: return "PGUP";
    case GDK_KEY_Page_Down: return "PGDWN";
    case GDK_KEY_Left: return "LEFT";
    case GDK_KEY_Right: return "RIGHT";
    case GDK_KEY_Up: return "UP";
    case GDK_KEY_Down: return "DOWN";
    case GDK_KEY_Print: return "PRINT";
    case GDK_KEY_Pause: return "PAUSE";
    case GDK_KEY_KP_Enter: return "KP_ENTER";
    case GDK_KEY_KP_Insert: return "KP_INS";
    case GDK_KEY_KP_Delete: return "KP_DEL";
    case GDK_KEY_KP_Decimal: case GDK_KEY_KP_Separator: return "KP_DEC";
    case GDK_KEY_KP_Home: return "KP_HOME";
    case GDK_KEY_KP_End: return "KP_END";
    case GDK_KEY_KP_Page_Up: return "KP_PGUP";
    case GDK_KEY_KP_Page_Down: return "KP_PGDWN";
    case GDK_KEY_KP_Left: return "KP_LEFT";
    case GDK_KEY_KP_Right: return "KP_RIGHT";
    case GDK_KEY_KP_Up: return "KP_UP";
    case GDK_KEY_KP_Down: return "KP_DOWN";
    case GDK_KEY_KP_Begin: return "KP_BEGIN";
    case GDK_KEY_KP_Add: return "KP_ADD";
    case GDK_KEY_KP_Subtract: return "KP_SUBTRACT";
    case GDK_KEY_KP_Multiply: return "KP_MULTIPLY";
    case GDK_KEY_KP_Divide: return "KP_DIVIDE";
    case GDK_KEY_Menu: return "MENU";
    case GDK_KEY_AudioPlay: return "PLAY";
    case GDK_KEY_AudioPause: return "PAUSE";
    case GDK_KEY_AudioStop: return "STOP";
    case GDK_KEY_AudioPrev: return "PREV";
    case GDK_KEY_AudioNext: return "NEXT";
    case GDK_KEY_AudioRewind: return "REWIND";
    case GDK_KEY_AudioForward: return "FORWARD";
    case GDK_KEY_AudioMute: return "MUTE";
    case GDK_KEY_AudioRaiseVolume: return "VOLUME_UP";
    case GDK_KEY_AudioLowerVolume: return "VOLUME_DOWN";
    default: return NULL;
    }
}

char *input_key_name(guint keyval, GdkModifierType modifiers)
{
    gunichar unicode = gdk_keyval_to_unicode(keyval);
    gboolean printable = unicode >= 32 && unicode != 127;
    gboolean shift = (modifiers & GDK_SHIFT_MASK) != 0;
    g_autofree char *key = NULL;
    const char *name = named_key(keyval);
    if (name)
        key = g_strdup(name);
    else if (keyval >= GDK_KEY_F1 && keyval <= GDK_KEY_F24)
        key = g_strdup_printf("F%u", keyval - GDK_KEY_F1 + 1);
    else if (keyval >= GDK_KEY_KP_0 && keyval <= GDK_KEY_KP_9)
        key = g_strdup_printf("KP%u", keyval - GDK_KEY_KP_0);
    else if (printable) {
        if (unicode == 0x3000)
            key = g_strdup("IDEOGRAPHIC_SPACE");
        else {
            char utf8[7];
            /* mpv folds Shift+ASCII letters into uppercase. GDK normally
             * already supplies the shifted character; other Unicode is
             * preserved exactly, including EurKEY-produced characters. */
            if (shift && unicode >= 'a' && unicode <= 'z')
                unicode -= 'a' - 'A';
            int length = g_unichar_to_utf8(unicode, utf8);
            utf8[length] = '\0';
            key = g_strdup(utf8);
        }
    } else {
        return NULL;
    }
    /* mpv strips Shift for printable characters (including SPACE/SHARP)
     * and retains it for arrows, function keys, and other special keys. */
    if (printable && !(keyval >= GDK_KEY_KP_0 && keyval <= GDK_KEY_KP_9) &&
        !(keyval >= GDK_KEY_KP_Multiply && keyval <= GDK_KEY_KP_Divide))
        shift = FALSE;
    if (keyval == GDK_KEY_ISO_Left_Tab)
        shift = TRUE;
    GString *result = g_string_new(NULL);
    if (shift) g_string_append(result, "Shift+");
    if (modifiers & GDK_CONTROL_MASK) g_string_append(result, "Ctrl+");
    if (modifiers & GDK_ALT_MASK) g_string_append(result, "Alt+");
    if (modifiers & (GDK_SUPER_MASK | GDK_META_MASK)) g_string_append(result, "Meta+");
    g_string_append(result, key);
    return g_string_free(result, FALSE);
}
