/* Parser tests always run. GTK interaction tests need an available display. */
#include "../src/chapters.c"

typedef struct {
    mpv_node root, entries[4], values[4][2];
    mpv_node_list list, maps[4];
    char *keys[2];
    char title[32];
} Fixture;

static void fixture_init(Fixture *f)
{
    *f = (Fixture){0};
    strcpy(f->title, "Opening");
    f->keys[0] = "title";
    f->keys[1] = "time";
    for (guint i = 0; i < G_N_ELEMENTS(f->entries); i++) {
        f->values[i][0] = (mpv_node){.format = MPV_FORMAT_STRING, .u.string = f->title};
        f->values[i][1] = (mpv_node){.format = MPV_FORMAT_DOUBLE, .u.double_ = (double)i};
        f->maps[i] = (mpv_node_list){.num = 2, .keys = f->keys, .values = f->values[i]};
        f->entries[i] = (mpv_node){.format = MPV_FORMAT_NODE_MAP, .u.list = &f->maps[i]};
    }
    f->values[1][1].u.double_ = NAN;
    f->values[2][0].format = MPV_FORMAT_NONE;
    f->values[2][1] = (mpv_node){.format = MPV_FORMAT_INT64, .u.int64 = 3661};
    f->values[3][1].u.double_ = -1;
    f->list = (mpv_node_list){.num = 4, .values = f->entries};
    f->root = (mpv_node){.format = MPV_FORMAT_NODE_ARRAY, .u.list = &f->list};
}

static void check_model(void)
{
    Fixture f;
    fixture_init(&f);
    g_autoptr(GPtrArray) items = chapter_items_from_node(&f.root);
    g_assert_cmpuint(items->len, ==, 2);
    ChapterItem *opening = g_ptr_array_index(items, 0);
    ChapterItem *unnamed = g_ptr_array_index(items, 1);
    g_assert_cmpint(opening->index, ==, 0);
    g_assert_cmpint(unnamed->index, ==, 2);
    g_assert_cmpfloat(unnamed->time, ==, 3661);
    g_autofree char *fallback = chapter_title(unnamed);
    g_assert_cmpstr(fallback, ==, "Глава 3");
    strcpy(f.title, "Changed after mpv event");
    f.values[0][1].u.double_ = 99;
    g_assert_cmpstr(opening->title, ==, "Opening");
    g_assert_cmpfloat(opening->time, ==, 0);
    const struct { double seconds; const char *label; } times[] = {
        {0, "00:00:00"}, {3599.9, "00:59:59"},
        {3661, "01:01:01"}, {90061, "25:01:01"}
    };
    for (guint i = 0; i < G_N_ELEMENTS(times); i++) {
        g_autofree char *label = chapter_time(times[i].seconds);
        g_assert_cmpstr(label, ==, times[i].label);
    }
    g_autoptr(GPtrArray) empty = chapter_items_from_node(NULL);
    g_assert_cmpuint(empty->len, ==, 0);
    f.values[0][0].u.string = "Bad\xff title";
    g_autoptr(GPtrArray) repaired = chapter_items_from_node(&f.root);
    g_assert_true(g_utf8_validate(((ChapterItem *)g_ptr_array_index(repaired, 0))->title, -1, NULL));
    puts("PASS chapter model: copied event metadata, original indices, missing titles, invalid times, UTF-8 and long-duration timestamps");
}

typedef struct { guint selections, steps; int64_t selected; int direction; } Actions;

static void selected(int64_t index, gpointer data)
{
    Actions *actions = data;
    actions->selections++;
    actions->selected = index;
}

static void stepped(int direction, gpointer data)
{
    Actions *actions = data;
    actions->steps++;
    actions->direction = direction;
}

static void check_widgets(void)
{
    Fixture f;
    fixture_init(&f);
    Actions actions = {0};
    GtkWidget *button = chapters_button_new(selected, stepped, &actions);
    g_object_ref_sink(button);
    Chapters *p = g_object_get_data(G_OBJECT(button), "mpv-gtk-chapters");
    g_assert_false(gtk_widget_get_visible(button));
    g_assert_cmpint(gtk_menu_button_get_direction(p->button), ==, GTK_ARROW_UP);
    g_assert_cmpint(gtk_popover_get_position(p->popover), ==, GTK_POS_TOP);
    g_assert_null(gtk_popover_get_child(p->popover));
    chapters_button_update(button, &f.root, 0);
    g_assert_true(gtk_widget_get_visible(button));
    g_assert_null(p->list);
    chapters_button_set_current(button, 2);
    g_assert_null(p->list);
    g_assert_cmpuint(actions.selections, ==, 0);
    g_assert_cmpuint(actions.steps, ==, 0);
    build_content(p); /* The same construction used when the popover opens. */
    g_assert_nonnull(p->list);
    GtkListBoxRow *row = gtk_list_box_get_selected_row(p->list);
    g_assert_cmpint(gtk_list_box_row_get_index(row), ==, 1);
    g_assert_false(gtk_widget_get_sensitive(p->next));
    chapters_button_set_current(button, -1);
    g_assert_null(gtk_list_box_get_selected_row(p->list));
    g_assert_false(gtk_widget_get_sensitive(p->previous));
    g_assert_true(gtk_widget_get_sensitive(p->next));
    chapters_button_set_current(button, 2);
    row = gtk_list_box_get_row_at_index(p->list, 0);
    g_signal_emit_by_name(p->list, "row-activated", row);
    g_assert_cmpuint(actions.selections, ==, 1);
    g_assert_cmpint(actions.selected, ==, 0);
    g_assert_cmpint(p->current, ==, 2); /* Only an engine observation changes it. */
    chapters_button_set_current(button, 0);
    g_signal_emit_by_name(p->next, "clicked");
    g_assert_cmpint(actions.direction, ==, 1);
    g_signal_emit_by_name(p->previous, "clicked");
    g_assert_cmpint(actions.direction, ==, -1);
    g_assert_cmpuint(actions.steps, ==, 2);
    chapters_button_reset(button);
    g_assert_false(gtk_widget_get_visible(button));
    g_assert_null(p->list);
    g_assert_null(gtk_popover_get_child(p->popover));
    g_assert_cmpuint(p->items->len, ==, 0);
    chapters_button_update(button, &f.root, 0);
    g_assert_null(p->list);
    g_assert_cmpuint(actions.selections, ==, 1);
    g_assert_cmpuint(actions.steps, ==, 2);
    g_object_unref(button);
    puts("PASS chapter GTK: hidden empty state, lazy list, observed selection, user-only navigation, release and reset");
}

int main(void)
{
    check_model();
    if (!gtk_init_check()) {
        puts("SKIP chapter GTK: no usable display");
        return 77;
    }
    check_widgets();
    return 0;
}
