/* The model checks run without a display. GTK checks drive the real editor
 * controller signals; compositor shortcut delivery is a separate concern. */
#include <gtk/gtk.h>

static void observe_inhibit(GdkToplevel *toplevel, GdkEvent *event);
static void observe_restore(GdkToplevel *toplevel);
#define gdk_toplevel_inhibit_system_shortcuts observe_inhibit
#define gdk_toplevel_restore_system_shortcuts observe_restore
#include "../src/keys_page.c"
#undef gdk_toplevel_restore_system_shortcuts
#undef gdk_toplevel_inhibit_system_shortcuts

static guint inhibits, restores;
static GdkToplevel *inhibited_surface;

/* Observe request ownership without grabbing shortcuts in the user's
 * compositor when this test is run in a graphical session. */
static void observe_inhibit(GdkToplevel *toplevel, GdkEvent *event)
{
    g_assert_true(GDK_IS_TOPLEVEL(toplevel));
    g_assert_null(event);
    g_assert_null(inhibited_surface);
    inhibited_surface = toplevel;
    inhibits++;
}

static void observe_restore(GdkToplevel *toplevel)
{
    g_assert_true(toplevel == inhibited_surface);
    inhibited_surface = NULL;
    restores++;
}

static void check_model(void)
{
    const struct { const char *key, *label; } cases[] = {
        {"KP_ADD", "Num +"}, {"Alt+KP_MULTIPLY", "Alt+Num ×"},
        {"Shift+KP_ENTER", "Shift+Num Enter"}, {"Alt+LEFT", "Alt+LEFT"},
        {"F", "F"}, {"Alt+ESC", "Alt+Esc"}
    };
    for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_autofree char *label = display_key(cases[i].key);
        g_assert_cmpstr(label, ==, cases[i].label);
    }
    KeyEditor e = {.capturing = TRUE};
    const guint modifiers[] = {GDK_KEY_Shift_L, GDK_KEY_Shift_R, GDK_KEY_Alt_L, GDK_KEY_Alt_R};
    for (guint i = 0; i < G_N_ELEMENTS(modifiers); i++) {
        g_assert_true(capture_key(NULL, modifiers[i], 0, 0, &e));
        g_assert_true(e.capturing);
    }
    e.capturing = FALSE;
    g_assert_false(capture_key(NULL, GDK_KEY_F, 0, GDK_SHIFT_MASK, &e));
    puts("PASS key editor model: pure modifiers keep recording, idle keys propagate, keypad display names preserve modifiers");
}

static GtkEventController *capture_controller(GtkWidget *window)
{
    g_autoptr(GListModel) controllers = gtk_widget_observe_controllers(window);
    for (guint i = 0; i < g_list_model_get_n_items(controllers); i++) {
        GtkEventController *controller = g_list_model_get_item(controllers, i);
        if (GTK_IS_EVENT_CONTROLLER_KEY(controller) &&
            gtk_event_controller_get_propagation_phase(controller) == GTK_PHASE_CAPTURE)
            return controller;
        g_object_unref(controller);
    }
    g_assert_not_reached();
    return NULL;
}

static gboolean press(GtkEventController *controller, guint key, GdkModifierType state)
{
    gboolean handled = FALSE;
    g_signal_emit_by_name(controller, "key-pressed", key, 0u, state, &handled);
    return handled;
}

static void begin_capture(KeyEditor *e)
{
    g_assert_false(e->capturing);
    g_signal_emit_by_name(e->capture, "clicked");
    g_assert_true(e->capturing);
    g_assert_nonnull(e->capture_toplevel);
    g_assert_cmpuint(inhibits, ==, restores + 1);
}

static void assert_stopped(KeyEditor *e)
{
    g_assert_false(e->capturing);
    g_assert_null(e->capture_toplevel);
    g_assert_null(inhibited_surface);
    g_assert_cmpuint(inhibits, ==, restores);
}

static void check_columns(KeysPage *p)
{
    const KeyBindingDefault defaults[] = {
        {"a", "cycle pause", ""},
        {"Alt+LEFT", "seek -5", ""},
        {"Shift+Ctrl+Alt+KP_MULTIPLY", "set speed 1", ""}
    };
    p->rows = keybindings_snapshot(NULL, defaults, G_N_ELEMENTS(defaults));
    rebuild_rows(p);
    gtk_widget_allocate(p->page, 900, 650, -1, NULL);
    float left = -1;
    for (guint i = 0; i < p->rows->len; i++) {
        GtkListBoxRow *row = gtk_list_box_get_row_at_index(p->list, i);
        GtkWidget *box = gtk_list_box_row_get_child(row);
        GtkWidget *column = gtk_widget_get_first_child(box);
        GtkWidget *labels = gtk_widget_get_next_sibling(column);
        graphene_point_t origin = GRAPHENE_POINT_INIT(0, 0), position;
        g_assert_true(gtk_widget_compute_point(labels, p->page, &origin, &position));
        g_assert_cmpint(gtk_widget_get_width(column), >, 0);
        if (i == 0)
            left = position.x;
        else
            g_assert_cmpfloat(position.x, ==, left);
        GtkWidget *badge = gtk_widget_get_first_child(column);
        const KeyBinding *binding = g_ptr_array_index(p->rows, i);
        const char *tooltip = gtk_widget_get_tooltip_text(badge);
        g_assert_cmpstr(tooltip, ==, binding->key);
    }
}

int main(void)
{
    check_model();
    if (!gtk_init_check()) {
        puts("SKIP key editor GTK: no usable display; compositor delivery was not tested");
        return 77;
    }
    GtkWidget *page = keys_page_new();
    g_object_ref_sink(page);
    KeysPage *p = page_data(page);
    check_columns(p);
    open_editor(p, NULL, NULL);
    KeyEditor *e = g_object_get_data(G_OBJECT(p->editor), "mpv-key-editor");
    g_autoptr(GtkEventController) controller = capture_controller(GTK_WIDGET(e->window));
    const struct { guint modifier, key; GdkModifierType state; const char *name; } cases[] = {
        {GDK_KEY_Shift_L, GDK_KEY_F, GDK_SHIFT_MASK, "F"},
        {GDK_KEY_Alt_L, GDK_KEY_f, GDK_ALT_MASK, "Alt+f"},
        {GDK_KEY_Shift_R, GDK_KEY_Left, GDK_SHIFT_MASK, "Shift+LEFT"},
        {GDK_KEY_Alt_R, GDK_KEY_KP_Add, GDK_ALT_MASK, "Alt+KP_ADD"}
    };
    for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
        begin_capture(e);
        g_assert_true(press(controller, cases[i].modifier, 0));
        g_assert_true(e->capturing);
        g_assert_true(press(controller, cases[i].key, cases[i].state));
        g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(e->key)), ==, cases[i].name);
        assert_stopped(e);
    }
    begin_capture(e);
    g_assert_true(press(controller, GDK_KEY_Escape, GDK_ALT_MASK));
    g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(e->key)), ==, "Alt+ESC");
    assert_stopped(e);
    begin_capture(e);
    g_assert_true(press(controller, GDK_KEY_Escape, GDK_LOCK_MASK));
    g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(e->key)), ==, "Alt+ESC");
    assert_stopped(e);
    begin_capture(e);
    g_signal_emit_by_name(e->capture, "clicked");
    assert_stopped(e);
    begin_capture(e);
    /* Exercise the focus-loss handler with a deterministically inactive
     * window; this does not depend on the host compositor's focus policy. */
    GtkWindow *inactive = GTK_WINDOW(gtk_window_new());
    g_assert_false(gtk_window_is_active(inactive));
    capture_active_changed(G_OBJECT(inactive), NULL, e);
    gtk_window_destroy(inactive);
    assert_stopped(e);
    begin_capture(e);
    gtk_widget_set_visible(GTK_WIDGET(e->window), FALSE);
    gtk_widget_unrealize(GTK_WIDGET(e->window));
    assert_stopped(e);
    gtk_widget_realize(GTK_WIDGET(e->window));
    begin_capture(e);
    g_object_unref(page); /* Owner destruction releases an unfinished capture. */
    g_assert_null(inhibited_surface);
    g_assert_cmpuint(inhibits, ==, restores);
    puts("PASS key editor GTK: controller modifier sequences, modified Escape, cancellation, "
         "inhibit/restore lifecycle and aligned command columns");
    puts("Compositor grant and physical Wayland keyboard delivery were not tested.");
    return 0;
}
