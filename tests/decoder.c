/* Regression coverage for unknown decoder observations and truthful status
 * text. No GTK widgets, display connection, or hardware decoder are needed. */
#define main mpv_gtk_program_main
#include "../src/main.c"
#undef main

#include <string.h>

static void decoder_observation(Player *p, const char *decoder)
{
    const char *value = decoder;
    mpv_event_property property = {
        .name = "hwdec-current",
        .format = decoder ? MPV_FORMAT_STRING : MPV_FORMAT_NONE,
        .data = decoder ? (void *)&value : NULL
    };
    property_changed(p, &property);
}

static void assert_unknown(Player *p)
{
    g_assert_false(p->preference_values.hwdec_known);
    /* Retaining a stale hardware name is acceptable while marked unknown;
     * converting an absent value into an explicit "no" is the regression. */
    g_assert_cmpstr(p->preference_values.hwdec_current, !=, "no");
    g_autofree char *status = preferences_decoder_status(&p->preference_values);
    g_assert_null(strstr(status, "software"));
    g_assert_null(strstr(status, "программ"));
}

static void check_observations(void)
{
    Player p = {
        .preference_values = {.hwdec = "nvdec", .media_loaded = TRUE, .has_video = TRUE}
    };
    decoder_observation(&p, NULL);
    assert_unknown(&p);
    decoder_observation(&p, "no");
    g_assert_true(p.preference_values.hwdec_known);
    g_assert_cmpstr(p.hwdec_current, ==, "no");
    g_assert_cmpstr(p.preference_values.hwdec_current, ==, "no");
    decoder_observation(&p, "nvdec");
    g_assert_true(p.preference_values.hwdec_known);
    g_assert_cmpstr(p.hwdec_current, ==, "nvdec");
    g_assert_cmpstr(p.preference_values.hwdec_current, ==, "nvdec");
    decoder_observation(&p, NULL);
    assert_unknown(&p);
    decoder_observation(&p, "nvdec-copy");
    g_assert_true(p.preference_values.hwdec_known);
    g_assert_cmpstr(p.hwdec_current, ==, "nvdec-copy");
    g_assert_cmpstr(p.preference_values.hwdec_current, ==, "nvdec-copy");
    decoder_observation(&p, "no");
    g_assert_true(p.preference_values.hwdec_known);
    g_assert_cmpstr(p.hwdec_current, ==, "no");
    g_assert_cmpstr(p.preference_values.hwdec_current, ==, "no");
    g_free(p.hwdec_current);
}

static void check_statuses(void)
{
    const struct {
        PreferenceValues values;
        const char *russian;
        const char *english;
    } cases[] = {
        {
            {.hwdec = "nvdec", .hwdec_current = "nvdec", .hwdec_known = TRUE},
            "Откройте видео, чтобы увидеть активный декодер.",
            "Open a video to see the active decoder."
        }, {
            {.hwdec = "nvdec", .hwdec_current = "nvdec", .media_loaded = TRUE, .hwdec_known = TRUE},
            "В текущем файле нет видеодорожки.",
            "The current file has no video track."
        }, {
            {.hwdec = "nvdec", .hwdec_current = "no", .media_loaded = TRUE, .has_video = TRUE,
             .hwdec_known = TRUE, .hwdec_pending = TRUE},
            "Применяется выбранный режим декодирования…",
            "Applying the selected decoding mode…"
        }, {
            {.hwdec = "nvdec", .hwdec_current = "no", .media_loaded = TRUE, .has_video = TRUE},
            "Активный декодер ещё не определён.",
            "The active decoder is not yet known."
        }, {
            {.hwdec = "no", .hwdec_current = "no", .media_loaded = TRUE, .has_video = TRUE,
             .hwdec_known = TRUE},
            "Аппаратное ускорение отключено; используется программный декодер.",
            "Hardware acceleration is disabled; using a software decoder."
        }, {
            {.hwdec = "nvdec", .hwdec_current = "no", .media_loaded = TRUE, .has_video = TRUE,
             .hwdec_known = TRUE},
            "Аппаратный режим недоступен для этого видео; используется программный декодер.",
            "Hardware decoding is unavailable for this video; using a software decoder."
        }, {
            {.hwdec = "nvdec", .hwdec_current = "nvdec", .media_loaded = TRUE, .has_video = TRUE,
             .hwdec_known = TRUE},
            "nvdec",
            "nvdec"
        }, {
            {.hwdec = "vaapi", .hwdec_current = "nvdec", .media_loaded = TRUE, .has_video = TRUE,
             .hwdec_known = TRUE, .hwdec_pending = TRUE, .hwdec_restart_required = TRUE},
            "nvdec",
            "nvdec"
        }, {
            {.hwdec = "vaapi", .hwdec_current = "no", .media_loaded = TRUE, .has_video = TRUE,
             .hwdec_known = TRUE, .hwdec_restart_required = TRUE},
            "Программное декодирование",
            "Software decoding"
        }, {
            {.hwdec = "vdpau", .hwdec_current = NULL, .media_loaded = TRUE, .has_video = TRUE,
             .hwdec_restart_required = TRUE},
            "Активный декодер ещё не определён.",
            "The active decoder is not yet known."
        }
    };
    const char *languages[] = {"ru", "en"};
    for (unsigned language = 0; language < G_N_ELEMENTS(languages); ++language) {
        i18n_set_language(languages[language]);
        for (unsigned i = 0; i < G_N_ELEMENTS(cases); ++i) {
            g_autofree char *status = preferences_decoder_status(&cases[i].values);
            g_assert_cmpstr(status, ==, language ? cases[i].english : cases[i].russian);
            if (i == 3) {
                g_assert_null(strstr(status, "software"));
                g_assert_null(strstr(status, "программ"));
            }
        }
        check_observations();
    }
    i18n_set_language("ru");
}

int main(void)
{
    check_statuses();
    puts("PASS decoder: absent observations remain unknown; truthful active and restart-pending statuses in Russian and English");
    return 0;
}
