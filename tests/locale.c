/* Include the actual initialization boundary: GTK may have changed the
 * process locale by the time init_mpv runs. No display is needed here. */
#define main mpv_gtk_program_main
#include "../src/main.c"
#undef main

#include <assert.h>
#include <string.h>

int main(void)
{
    const char *numeric = setlocale(LC_NUMERIC, "en_US.utf8");
    assert(numeric && strcmp(numeric, "C") != 0);
    const char *ctype = setlocale(LC_CTYPE, "C.utf8");
    assert(ctype);
    g_autofree char *saved_ctype = g_strdup(ctype);

    /* Do not load the user's private mpv-gtk configuration into the test. */
    g_autofree char *config_dir = g_dir_make_tmp("mpv-gtk-locale-XXXXXX", NULL);
    assert(config_dir);
    g_autofree char *config_file = g_build_filename(config_dir, "mpv.conf", NULL);
    g_autofree char *input_file = g_build_filename(config_dir, "input.conf", NULL);
    assert(g_setenv("MPV_GTK_CONFIG_FILE", config_file, TRUE));

    Player p = {.wake_fd = -1};
    assert(init_mpv(&p));
    assert(p.mpv);
    assert(strcmp(setlocale(LC_NUMERIC, NULL), "C") == 0);
    assert(strcmp(setlocale(LC_CTYPE, NULL), saved_ctype) == 0);

    p.closing = TRUE;
    if (p.wake_source)
        g_source_remove(p.wake_source);
    mpv_set_wakeup_callback(p.mpv, NULL, NULL);
    mpv_terminate_destroy(p.mpv);
    if (p.wake_fd >= 0)
        close(p.wake_fd);
    if (p.gl_library)
        dlclose(p.gl_library);
    config_clear(&p.config);
    assert(unlink(config_file) == 0);
    assert(unlink(input_file) == 0);
    assert(rmdir(config_dir) == 0);

    puts("PASS locale: actual init_mpv restores C numeric locale and preserves LC_CTYPE");
    return 0;
}
