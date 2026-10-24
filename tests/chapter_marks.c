/* Geometry and metadata checks run without a display. The GTK branch checks
 * actual thumb alignment and measurement with the seek scale's negative margins. */
#include "../src/chapter_marks.c"

static mpv_node time_list(mpv_node *entries, mpv_node *values,
                           mpv_node_list *maps, mpv_node_list *list, char **keys)
{
    const double times[] = {20, 0, 10, 10, NAN, -1, INFINITY, 40};
    for (guint i = 0; i < G_N_ELEMENTS(times); i++) {
        values[i] = (mpv_node){.format = MPV_FORMAT_DOUBLE, .u.double_ = times[i]};
        maps[i] = (mpv_node_list){.num = 1, .keys = keys, .values = &values[i]};
        entries[i] = (mpv_node){.format = MPV_FORMAT_NODE_MAP, .u.list = &maps[i]};
    }
    *list = (mpv_node_list){.num = G_N_ELEMENTS(times), .values = entries};
    return (mpv_node){.format = MPV_FORMAT_NODE_ARRAY, .u.list = list};
}

static void check_model(void)
{
    mpv_node entries[8], values[8];
    mpv_node_list maps[8], list;
    char *keys[] = {"time"};
    mpv_node node = time_list(entries, values, maps, &list, keys);
    g_autoptr(GArray) copied = mark_times_copy(&node);
    g_assert_cmpuint(copied->len, ==, 4);
    const double expected[] = {0, 10, 20, 40};
    for (guint i = 0; i < copied->len; i++)
        g_assert_cmpfloat(g_array_index(copied, double, i), ==, expected[i]);
    values[0].u.double_ = 99;
    g_assert_cmpfloat(g_array_index(copied, double, 2), ==, 20);
    g_autoptr(GArray) empty = mark_times_copy(NULL);
    g_assert_cmpuint(empty->len, ==, 0);

    /* A 20px slider with -8px margins has a 4px layout width. The
     * fractional centers also exercise asymmetric/fractional CSS margins. */
    const struct { double value, offset; } positions[] = {
        {10, 0}, {11, 1}, {31, 32}, {130, 187}, {210, 313}
    };
    for (guint reversed = 0; reversed < 2; reversed++) {
        for (guint i = 0; i < G_N_ELEMENTS(positions); i++) {
            double center = 14.25 + (reversed ? 313 - positions[i].offset : positions[i].offset);
            MarkTrack track = {0};
            g_assert_true(track_calibrate(center, 317, 4, 10, 220, 10,
                                           positions[i].value, reversed, &track));
            g_assert_cmpfloat(track.origin, ==, 14.25);
            g_assert_cmpfloat(track.travel, ==, 313);
            g_assert_cmpfloat(mark_position(&track, 0), ==, reversed ? 327.25 : 14.25);
            g_assert_cmpfloat(mark_position(&track, .5), ==, reversed ? 171.25 : 170.25);
            g_assert_cmpfloat(mark_position(&track, 1), ==, reversed ? 14.25 : 327.25);
        }
    }
    MarkTrack track = {0};
    g_assert_false(track_calibrate(0, 0, 4, 0, 100, 0, 0, FALSE, &track));
    g_assert_false(track_calibrate(0, 100, 4, 0, 10, 10, 0, FALSE, &track));
    g_assert_false(track_calibrate(NAN, 100, 4, 0, 100, 0, 0, FALSE, &track));
    puts("PASS chapter mark model: owned finite/sorted/deduplicated times, exact GTK offset rounding, "
         "stable endpoints across playback values, inverse direction and nonzero adjustment/page size");
}

static void check_widgets(void)
{
    g_autoptr(GtkCssProvider) css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        "scale.test-seek { padding: 12px; margin: 0; min-height: 10px; }"
        "scale.test-seek > trough { min-height: 4px; padding: 0; border: none; }"
        "scale.test-seek > trough > slider { min-width: 20px; min-height: 20px;"
        "padding: 0; margin: -8px; border: none; }");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_USER + 10);
    GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, .01);
    gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
    gtk_widget_add_css_class(scale, "test-seek");
    int before_minimum, before_natural, after_minimum, after_natural;
    gtk_widget_measure(scale, GTK_ORIENTATION_VERTICAL, -1,
                       &before_minimum, &before_natural, NULL, NULL);
    GtkWidget *overlay = chapter_marks_new(GTK_RANGE(scale));
    g_object_ref_sink(overlay);
    ChapterMarks *p = g_object_get_data(G_OBJECT(overlay), "mpv-chapter-marks");
    g_assert_true(gtk_overlay_get_child(GTK_OVERLAY(overlay)) == scale);
    g_assert_false(gtk_widget_get_can_target(p->canvas));
    g_assert_false(gtk_widget_get_focusable(p->canvas));
    g_assert_false(gtk_overlay_get_measure_overlay(GTK_OVERLAY(overlay), p->canvas));
    mpv_node entries[8], values[8];
    mpv_node_list maps[8], list;
    char *keys[] = {"time"};
    mpv_node node = time_list(entries, values, maps, &list, keys);
    chapter_marks_update(overlay, &node);
    g_assert_false(gtk_widget_get_visible(p->canvas));
    chapter_marks_set_duration(overlay, 100);
    g_assert_true(gtk_widget_get_visible(p->canvas));
    gtk_widget_measure(overlay, GTK_ORIENTATION_VERTICAL, -1,
                       &after_minimum, &after_natural, NULL, NULL);
    g_assert_cmpint(after_minimum, ==, before_minimum);
    g_assert_cmpint(after_natural, ==, before_natural);
    const int widths[] = {257, 701};
    const double playback[] = {0, 13.3, 50, 99.9, 100};
    gtk_range_set_flippable(GTK_RANGE(scale), TRUE);
    for (guint width = 0; width < G_N_ELEMENTS(widths); width++) {
        for (guint rtl = 0; rtl < 2; rtl++) {
            for (guint inverted = 0; inverted < 2; inverted++) {
                gtk_widget_set_direction(scale, rtl ? GTK_TEXT_DIR_RTL : GTK_TEXT_DIR_LTR);
                gtk_range_set_inverted(GTK_RANGE(scale), inverted);
                double fixed_middle = NAN;
                for (guint i = 0; i < G_N_ELEMENTS(playback); i++) {
                    gtk_range_set_value(GTK_RANGE(scale), playback[i]);
                    gtk_widget_allocate(overlay, widths[width], after_natural, -1, NULL);
                    MarkTrack geometry = {0};
                    graphene_rect_t clip, thumb;
                    g_assert_true(get_track(p, &geometry, &clip));
                    GtkWidget *slider = css_child(css_child(scale, "trough"), "slider");
                    g_assert_true(gtk_widget_compute_bounds(slider, p->canvas, &thumb));
                    double center = thumb.origin.x + thumb.size.width / 2.0;
                    g_assert_cmpfloat_with_epsilon(mark_position(&geometry, playback[i] / 100), center, .0001);
                    double middle = mark_position(&geometry, .5);
                    if (i == 0)
                        fixed_middle = middle;
                    else
                        g_assert_cmpfloat_with_epsilon(middle, fixed_middle, .0001);
                    g_assert_cmpfloat(clip.size.height, <=, 4);
                }
            }
        }
    }
    double value = gtk_range_get_value(GTK_RANGE(scale));
    chapter_marks_reset(overlay);
    g_assert_cmpfloat(gtk_range_get_value(GTK_RANGE(scale)), ==, value);
    g_assert_cmpuint(p->times->len, ==, 0);
    g_assert_cmpfloat(p->duration, ==, 0);
    g_assert_false(gtk_widget_get_visible(p->canvas));
    g_object_unref(overlay);
    gtk_style_context_remove_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css));
    puts("PASS chapter mark GTK: unchanged scale height/value, passive overlay, actual thumb centers, "
         "negative margins, resize, RTL/inversion and reset");
}

int main(void)
{
    check_model();
    if (!gtk_init_check()) {
        puts("SKIP chapter mark GTK: no usable display");
        return 77;
    }
    check_widgets();
    return 0;
}
