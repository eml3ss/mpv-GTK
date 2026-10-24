#include "key_commands.h"
#include "i18n.h"

#include <math.h>
#include <string.h>

typedef struct {
    const char *name;
    const char *category;
    const char *russian;
    const char *example;
} Command;

/* Behavior checked against the installed mpv manual and upstream sources:
 * DOCS/man/input.rst, player/command.c, input/cmd.c. Runtime signatures always
 * come from command-list rather than this descriptive catalogue. */
static const Command commands[] = {
    {"ignore", "Клавиши", "Ничего не делает; отключает действие назначенной клавиши.", "ignore"},
    {"seek", "Воспроизведение", "Перематывает на указанное время или долю файла; режим задаётся флагами.", "seek 5 relative"},
    {"revert-seek", "Воспроизведение", "Возвращает позицию до последней перемотки; повторный вызов отменяет возврат.", "revert-seek"},
    {"quit", "Приложение", "Завершает проигрыватель с необязательным кодом выхода.", "quit"},
    {"quit-watch-later", "Воспроизведение", "Сохраняет позицию для продолжения и завершает проигрыватель.", "quit-watch-later"},
    {"stop", "Воспроизведение", "Останавливает воспроизведение и очищает плейлист.", "stop"},
    {"frame-step", "Воспроизведение", "Переходит на заданное число кадров вперёд или назад; без аргумента — на один вперёд.", "frame-step"},
    {"frame-back-step", "Воспроизведение", "Возвращается на предыдущий видеокадр.", "frame-back-step"},
    {"playlist-next", "Очередь", "Переходит к следующему файлу в плейлисте.", "playlist-next"},
    {"playlist-prev", "Очередь", "Переходит к предыдущему файлу в плейлисте.", "playlist-prev"},
    {"playlist-next-playlist", "Очередь", "Переходит к следующей записи из другого исходного плейлиста.", "playlist-next-playlist"},
    {"playlist-prev-playlist", "Очередь", "Переходит к началу предыдущего исходного плейлиста.", "playlist-prev-playlist"},
    {"playlist-play-index", "Очередь", "Запускает запись по индексу плейлиста, начиная с нуля.", "playlist-play-index 0"},
    {"playlist-shuffle", "Очередь", "Перемешивает порядок файлов в плейлисте.", "playlist-shuffle"},
    {"playlist-unshuffle", "Очередь", "Пытается восстановить порядок до последнего перемешивания.", "playlist-unshuffle"},
    {"sub-step", "Субтитры", "Сдвигает задержку субтитров к следующей или предыдущей реплике.", "sub-step 1"},
    {"sub-seek", "Субтитры", "Перематывает видео и звук к следующей или предыдущей реплике субтитров.", "sub-seek 1"},
    {"print-text", "Текст и OSD", "Печатает текст с подстановкой свойств в стандартный вывод.", "print-text \"${media-title}\""},
    {"show-text", "Текст и OSD", "Показывает текст с подстановкой свойств поверх видео.", "show-text \"${media-title}\""},
    {"expand-text", "Текст и OSD", "Возвращает строку с подставленными свойствами через API или скрипт.", "expand-text \"${media-title}\""},
    {"expand-path", "Файлы и конфигурация", "Раскрывает сокращения путей mpv вида ~~ и возвращает путь.", "expand-path \"~~home/\""},
    {"normalize-path", "Файлы и конфигурация", "Возвращает нормализованный абсолютный путь; URL остаётся без изменений.", "normalize-path \"./video.mkv\""},
    {"escape-ass", "Текст и OSD", "Экранирует текст для буквального отображения в оверлеях ASS.", "escape-ass \"Text {example}\""},
    {"show-progress", "Текст и OSD", "Показывает полосу прогресса, текущую позицию и длительность на OSD.", "show-progress"},
    {"sub-add", "Субтитры", "Подключает внешний файл или поток субтитров и при необходимости выбирает его.", "sub-add \"subtitles.srt\""},
    {"audio-add", "Звук", "Подключает внешнюю звуковую дорожку из файла или потока.", "audio-add \"audio.flac\""},
    {"video-add", "Видео", "Подключает внешнюю видеодорожку или изображение обложки.", "video-add \"video.mkv\""},
    {"sub-remove", "Субтитры", "Удаляет внешнюю дорожку субтитров по ID или текущую дорожку.", "sub-remove"},
    {"audio-remove", "Звук", "Удаляет внешнюю звуковую дорожку по ID или текущую дорожку.", "audio-remove"},
    {"video-remove", "Видео", "Удаляет внешнюю видеодорожку по ID или текущую дорожку.", "video-remove"},
    {"sub-reload", "Субтитры", "Перечитывает текущие или указанные внешние субтитры с диска.", "sub-reload"},
    {"audio-reload", "Звук", "Перечитывает текущую или указанную внешнюю звуковую дорожку.", "audio-reload"},
    {"video-reload", "Видео", "Перечитывает текущую или указанную внешнюю видеодорожку.", "video-reload"},
    {"rescan-external-files", "Файлы и конфигурация", "Повторно ищет внешние субтитры, звук и обложки по настройкам автозагрузки.", "rescan-external-files"},
    {"screenshot", "Видео", "Сохраняет снимок видео, субтитров или окна согласно выбранному режиму.", "screenshot subtitles"},
    {"screenshot-to-file", "Видео", "Сохраняет снимок в указанный файл; формат определяется расширением.", "screenshot-to-file \"screenshot.png\" video"},
    {"screenshot-raw", "Видео", "Возвращает пиксели снимка в памяти через API или скрипт.", "screenshot-raw video"},
    {"loadfile", "Файлы и конфигурация", "Открывает файл или URL либо добавляет его в плейлист.", "loadfile \"video.mkv\" replace"},
    {"loadlist", "Очередь", "Загружает плейлист из файла или URL.", "loadlist \"playlist.m3u\" replace"},
    {"playlist-clear", "Очередь", "Удаляет записи плейлиста, сохраняя текущий воспроизводимый файл.", "playlist-clear"},
    {"playlist-remove", "Очередь", "Удаляет запись по индексу или текущую запись плейлиста.", "playlist-remove current"},
    {"playlist-move", "Очередь", "Перемещает запись плейлиста к позиции другой записи.", "playlist-move 1 0"},
    {"run", "Скрипты и процессы", "Запускает внешнюю программу с отдельными аргументами без командной оболочки.", "run xdg-open \"${path}\""},
    {"subprocess", "Скрипты и процессы", "Запускает внешний процесс с настройками ввода, вывода и ожидания.", "subprocess /usr/bin/true"},
    {"set", "Свойства", "Устанавливает значение указанного свойства или параметра.", "set speed 1"},
    {"del", "Свойства", "Удаляет свойство, если оно поддерживает удаление.", "del user-data/example"},
    {"change-list", "Свойства", "Добавляет, удаляет или заменяет элементы спискового параметра.", "change-list glsl-shaders append shader.glsl"},
    {"add", "Свойства", "Прибавляет число к свойству, ограничивая результат допустимым диапазоном.", "add volume 5"},
    {"cycle", "Свойства", "Переключает свойство по допустимым значениям вперёд или назад.", "cycle pause"},
    {"multiply", "Свойства", "Умножает числовое свойство на заданный коэффициент.", "multiply speed 1.1"},
    {"cycle-values", "Свойства", "Последовательно переключает свойство между перечисленными значениями.", "cycle-values loop-file inf no"},
    {"enable-section", "Клавиши", "Включает именованную секцию привязок клавиш с заданным приоритетом.", "enable-section custom"},
    {"disable-section", "Клавиши", "Отключает именованную секцию привязок клавиш.", "disable-section custom"},
    {"define-section", "Клавиши", "Создаёт или заменяет именованную секцию привязок в синтаксисе input.conf.", "define-section custom \"p cycle pause\""},
    {"ab-loop", "Воспроизведение", "Последовательно задаёт точки A, B и отключает повтор участка.", "ab-loop"},
    {"drop-buffers", "Воспроизведение", "Сбрасывает буферы звука, видео и демультиплексора и начинает наполнение заново.", "drop-buffers"},
    {"af", "Звук", "Изменяет цепочку звуковых фильтров: добавляет, удаляет или переключает фильтры.", "af add lavfi=[volume=0.5]"},
    {"vf", "Видео", "Изменяет цепочку видеофильтров: добавляет, удаляет или переключает фильтры.", "vf toggle hflip"},
    {"af-command", "Звук", "Передаёт команду поддерживающему её звуковому фильтру по метке.", "af-command all volume 0.5"},
    {"vf-command", "Видео", "Передаёт команду поддерживающему её видеофильтру по метке.", "vf-command all contrast 1.1"},
    {"ao-reload", "Звук", "Переоткрывает устройство звукового вывода.", "ao-reload"},
    {"script-binding", "Скрипты и процессы", "Вызывает именованную привязку, зарегистрированную скриптом.", "script-binding scriptname/bindingname"},
    {"script-message", "Скрипты и процессы", "Посылает сообщение со строковыми аргументами всем клиентам и скриптам.", "script-message mpv-gtk info"},
    {"script-message-to", "Скрипты и процессы", "Посылает сообщение только клиенту или скрипту с указанным именем.", "script-message-to scriptname message"},
    {"overlay-add", "Текст и OSD", "Добавляет растровый OSD-оверлей из сырых пиксельных данных файла.", "overlay-add 1 0 0 overlay.bgra 0 bgra 320 180 1280"},
    {"overlay-remove", "Текст и OSD", "Удаляет растровый оверлей с указанным ID.", "overlay-remove 1"},
    {"osd-overlay", "Текст и OSD", "Создаёт, обновляет или удаляет текстовый оверлей в формате ASS.", "osd-overlay 1 ass-events \"Hello\""},
    {"write-watch-later-config", "Файлы и конфигурация", "Сохраняет текущую позицию для продолжения, не останавливая воспроизведение.", "write-watch-later-config"},
    {"delete-watch-later-config", "Файлы и конфигурация", "Удаляет сохранённые данные продолжения для текущего или указанного файла.", "delete-watch-later-config"},
    {"mouse", "Клавиши", "Передаёт mpv координаты указателя и необязательное событие кнопки мыши.", "mouse 100 100"},
    {"keybind", "Клавиши", "Назначает команду клавише в работающем экземпляре mpv.", "keybind p \"cycle pause\""},
    {"keypress", "Клавиши", "Передаёт нажатие клавиши обработчику привязок mpv.", "keypress SPACE"},
    {"keydown", "Клавиши", "Начинает удержание клавиши и повтор её команды до keyup.", "keydown RIGHT"},
    {"keyup", "Клавиши", "Отпускает указанную клавишу или все удерживаемые клавиши.", "keyup RIGHT"},
    {"apply-profile", "Файлы и конфигурация", "Применяет именованный профиль настроек или восстанавливает предыдущие значения.", "apply-profile example"},
    {"load-config-file", "Файлы и конфигурация", "Читает параметры из дополнительного конфигурационного файла.", "load-config-file mpv.conf"},
    {"load-input-conf", "Клавиши", "Добавляет привязки из файла input.conf к работающему экземпляру mpv.", "load-input-conf input.conf"},
    {"load-script", "Скрипты и процессы", "Загружает и запускает указанный скрипт mpv.", "load-script script.lua"},
    {"dump-cache", "Файлы и конфигурация", "Записывает указанный временной участок демультиплексированного кэша в файл.", "dump-cache 0 30 clip.mkv"},
    {"ab-loop-dump-cache", "Файлы и конфигурация", "Записывает участок кэша между текущими точками A и B в файл.", "ab-loop-dump-cache clip.mkv"},
    {"ab-loop-align-cache", "Воспроизведение", "Подгоняет точки A и B к границам кадров, доступным для выгрузки кэша.", "ab-loop-align-cache"},
    {"begin-vo-dragging", "Приложение", "Запрашивает перетаскивание окна видеовывода при удерживаемой кнопке мыши.", "begin-vo-dragging"},
    {"context-menu", "Приложение", "Запрашивает контекстное меню видеовывода, если оно поддерживается.", "context-menu"},
    {"flush-status-line", "Текст и OSD", "Завершает или очищает строку статуса в терминале.", "flush-status-line yes"},
    {"notify-property", "Свойства", "Уведомляет наблюдающих клиентов о возможном изменении свойства.", "notify-property pause"},
};

static const KeyCommandPreset presets[] = {
    {"cycle pause", "Пауза / продолжить", "Приостанавливает воспроизведение или продолжает его.", "Воспроизведение"},
    {"seek -5 relative", "Назад на 5 секунд", "Перематывает на 5 секунд назад.", "Воспроизведение"},
    {"seek 5 relative", "Вперёд на 5 секунд", "Перематывает на 5 секунд вперёд.", "Воспроизведение"},
    {"seek -30 relative", "Назад на 30 секунд", "Перематывает на 30 секунд назад.", "Воспроизведение"},
    {"seek 30 relative", "Вперёд на 30 секунд", "Перематывает на 30 секунд вперёд.", "Воспроизведение"},
    {"add volume -5", "Уменьшить громкость", "Уменьшает громкость на 5 пунктов.", "Звук"},
    {"add volume 5", "Увеличить громкость", "Увеличивает громкость на 5 пунктов.", "Звук"},
    {"cycle mute", "Включить / выключить звук", "Переключает отключение звука без изменения громкости.", "Звук"},
    {"multiply speed 1.1", "Ускорить на 10%", "Умножает скорость воспроизведения на 1,1.", "Воспроизведение"},
    {"multiply speed 0.9", "Замедлить на 10%", "Умножает скорость воспроизведения на 0,9.", "Воспроизведение"},
    {"set speed 1", "Обычная скорость", "Возвращает скорость воспроизведения к 1×.", "Воспроизведение"},
    {"cycle aid", "Следующая аудиодорожка", "Переключает активную звуковую дорожку.", "Звук"},
    {"cycle sid", "Следующие субтитры", "Переключает дорожку субтитров.", "Субтитры"},
    {"cycle sub-visibility", "Показать / скрыть субтитры", "Переключает видимость выбранных субтитров.", "Субтитры"},
    {"cycle fullscreen", "Полноэкранный режим", "Переключает полноэкранный и оконный режим.", "Приложение"},
    {"screenshot subtitles", "Снимок с субтитрами", "Сохраняет снимок видео с текущими субтитрами.", "Видео"},
    {"screenshot video", "Снимок без субтитров", "Сохраняет снимок только видеокадра.", "Видео"},
    {"cycle-values loop-file inf no", "Повторять файл", "Включает или выключает бесконечный повтор текущего файла.", "Воспроизведение"},
    {"add sub-delay -0.1", "Субтитры раньше", "Сдвигает субтитры на 0,1 секунды раньше.", "Субтитры"},
    {"add sub-delay 0.1", "Субтитры позже", "Сдвигает субтитры на 0,1 секунды позже.", "Субтитры"},
    {"add audio-delay -0.1", "Звук раньше", "Сдвигает звук на 0,1 секунды раньше видео.", "Звук"},
    {"add audio-delay 0.1", "Звук позже", "Сдвигает звук на 0,1 секунды позже видео.", "Звук"},
    {"frame-step", "Следующий кадр", "Переходит на один видеокадр вперёд.", "Воспроизведение"},
    {"frame-back-step", "Предыдущий кадр", "Возвращается на один видеокадр назад.", "Воспроизведение"},
    {"playlist-prev", "Предыдущий файл", "Запускает предыдущий файл из очереди.", "Очередь"},
    {"playlist-next", "Следующий файл", "Запускает следующий файл из очереди.", "Очередь"},
    {"script-message mpv-gtk chapter-previous", "Предыдущая глава", "Переходит к предыдущей главе.", "Воспроизведение"},
    {"script-message mpv-gtk chapter-next", "Следующая глава", "Переходит к следующей главе.", "Воспроизведение"},
    {"ab-loop", "Повтор участка A–B", "Задаёт начало, конец и затем отключает повтор участка.", "Воспроизведение"},
    {"script-message mpv-gtk open-file", "Открыть файл", "Открывает диалог выбора файла GTK.", "Приложение"},
    {"script-message mpv-gtk open-url", "Открыть ссылку", "Открывает диалог ввода URL.", "Приложение"},
    {"script-message mpv-gtk fullscreen", "Полноэкранный режим GTK", "Переключает полноэкранный режим окна GTK.", "Приложение"},
    {"script-message mpv-gtk info", "Информация поверх видео", "Показывает или скрывает сведения о воспроизведении поверх видео.", "Приложение"},
    {"script-message mpv-gtk settings", "Настройки", "Открывает окно настроек проигрывателя.", "Приложение"},
    {"script-message mpv-gtk queue", "Показать / скрыть очередь", "Показывает или скрывает боковой плейлист.", "Приложение"},
    {"script-message mpv-gtk quit", "Закрыть проигрыватель", "Закрывает окно и завершает проигрыватель GTK.", "Приложение"},
    {"script-message mpv-gtk escape", "Закрыть информацию или полный экран", "Скрывает информацию поверх видео, затем выходит из полноэкранного режима.", "Приложение"},
    {"script-message mpv-gtk focus-next", "Следующий элемент интерфейса", "Переводит фокус на следующий элемент управления GTK.", "Приложение"},
    {"script-message mpv-gtk focus-previous", "Предыдущий элемент интерфейса", "Переводит фокус на предыдущий элемент управления GTK.", "Приложение"},
    {"script-message mpv-gtk activate", "Нажать выбранную кнопку", "Активирует кнопку или элемент управления с фокусом клавиатуры.", "Приложение"},
    {"script-message mpv-gtk context-menu", "Контекстное меню выбранной кнопки", "Открывает контекстное меню элемента управления с фокусом.", "Приложение"},
    {"script-message mpv-gtk focus-video", "Выбрать область видео", "Переводит фокус клавиатуры на область видео.", "Приложение"},
};

static const Command *find_command(const char *base)
{
    if (base)
        for (guint i = 0; i < G_N_ELEMENTS(commands); i++)
            if (g_str_equal(base, commands[i].name))
                return &commands[i];
    return NULL;
}

gboolean key_command_is_known(const char *base)
{
    return find_command(base) != NULL;
}

const char *key_command_help(const char *base)
{
    const Command *command = find_command(base);
    return command ? tr(command->russian)
                   : tr("Описание этой команды ещё не добавлено в каталог.");
}

const char *key_command_example(const char *base)
{
    const Command *command = find_command(base);
    return command ? command->example : "";
}

const char *key_command_category(const char *base)
{
    const Command *command = find_command(base);
    return tr(command ? command->category : "Другие команды");
}

const KeyCommandPreset *key_command_presets(gsize *count)
{
    if (count)
        *count = G_N_ELEMENTS(presets);
    return presets;
}

static gboolean command_prefix(const char *word)
{
    static const char *const prefixes[] = {
        "no-osd", "osd-bar", "osd-msg", "osd-msg-bar", "osd-auto", "expand-properties",
        "raw", "repeatable", "nonrepeatable", "nonscalable", "async", "sync"
    };
    for (guint i = 0; i < G_N_ELEMENTS(prefixes); i++)
        if (g_str_equal(word, prefixes[i]))
            return TRUE;
    return FALSE;
}

typedef enum { NUMERIC, BOOLEAN, TRACK, DELAY, SPEED, CHAPTER } PropertyKind;
typedef struct {
    const char *name;
    const char *label;
    PropertyKind kind;
} Property;

static const Property properties[] = {
    {"volume", "громкость", NUMERIC}, {"speed", "скорость воспроизведения", SPEED},
    {"mute", "отключение звука", BOOLEAN}, {"pause", "паузу", BOOLEAN},
    {"fullscreen", "полноэкранный режим", BOOLEAN},
    {"aid", "звуковую дорожку", TRACK}, {"sid", "дорожку субтитров", TRACK},
    {"secondary-sid", "дополнительную дорожку субтитров", TRACK},
    {"vid", "видеодорожку", TRACK}, {"audio-delay", "задержку звука", DELAY},
    {"sub-delay", "задержку субтитров", DELAY},
    {"deinterlace", "устранение чересстрочности", BOOLEAN},
    {"deband", "сглаживание цветовых полос", BOOLEAN},
    {"interpolation", "интерполяцию кадров", BOOLEAN},
    {"keepaspect", "сохранение пропорций видео", BOOLEAN},
    {"contrast", "контраст", NUMERIC}, {"brightness", "яркость", NUMERIC},
    {"gamma", "гамму", NUMERIC}, {"saturation", "насыщенность", NUMERIC},
    {"video-zoom", "масштаб видео", NUMERIC},
    {"video-pan-x", "горизонтальное смещение видео", NUMERIC},
    {"video-pan-y", "вертикальное смещение видео", NUMERIC},
    {"video-align-x", "горизонтальное выравнивание видео", NUMERIC},
    {"video-align-y", "вертикальное выравнивание видео", NUMERIC},
    {"video-scale-x", "горизонтальное растяжение видео", NUMERIC},
    {"video-scale-y", "вертикальное растяжение видео", NUMERIC},
    {"video-rotate", "поворот видео в градусах", NUMERIC},
    {"panscan", "обрезку видео для заполнения окна", NUMERIC},
    {"video-aspect-override", "соотношение сторон видео", NUMERIC},
    {"window-scale", "масштаб окна", NUMERIC},
    {"chapter", "номер главы", CHAPTER}, {"edition", "редакцию файла", NUMERIC},
    {"sub-scale", "масштаб субтитров", NUMERIC},
    {"sub-pos", "вертикальное положение субтитров", NUMERIC},
    {"sub-visibility", "показ субтитров", BOOLEAN},
    {"secondary-sub-visibility", "показ дополнительных субтитров", BOOLEAN},
    {"sub-ass-override", "переопределение оформления ASS", NUMERIC},
    {"sub-ass-use-video-data", "использование параметров видео в ASS", NUMERIC},
    {"hwdec", "режим аппаратного декодирования", NUMERIC},
    {"ontop", "размещение окна поверх остальных", BOOLEAN},
    {"osd-level", "уровень экранных сообщений mpv", NUMERIC},
    {"loop-file", "повтор текущего файла", NUMERIC},
    {"playlist-pos", "позицию файла в очереди", NUMERIC},
    {"playlist-pos-1", "позицию файла в очереди, начиная с 1", NUMERIC},
};

static const Property *find_property(const char *name)
{
    if (g_str_equal(name, "audio")) name = "aid";
    else if (g_str_equal(name, "sub")) name = "sid";
    else if (g_str_equal(name, "secondary-sub")) name = "secondary-sid";
    else if (g_str_equal(name, "video")) name = "vid";
    for (guint i = 0; i < G_N_ELEMENTS(properties); i++)
        if (g_str_equal(name, properties[i].name))
            return &properties[i];
    return NULL;
}

static gboolean number_value(const char *text, double *value)
{
    char *end;
    double numerator = g_ascii_strtod(text, &end);
    if (end == text)
        return FALSE;
    if (*end == '/') {
        const char *denominator_text = end + 1;
        double denominator = g_ascii_strtod(denominator_text, &end);
        if (end == denominator_text || denominator == 0)
            return FALSE;
        numerator /= denominator;
    }
    if (*end || !isfinite(numerator))
        return FALSE;
    *value = numerator;
    return TRUE;
}

static char *property_description(char **args, int count)
{
    if (count < 2)
        return NULL;
    const char *operation = args[0];
    int property_index = g_str_equal(operation, "cycle-values") &&
                         g_str_equal(args[1], "!reverse") ? 2 : 1;
    if (property_index >= count)
        return NULL;
    const Property *property = find_property(args[property_index]);
    if (!property)
        return NULL;
    const char *label = tr(property->label);
    const char *value = property_index + 1 < count ? args[property_index + 1] : "1";
    double number = 0;
    gboolean numeric = number_value(value, &number);
    char formatted[G_ASCII_DTOSTR_BUF_SIZE];
    if (numeric)
        g_ascii_formatd(formatted, sizeof(formatted), "%.6g", fabs(number));
    if (g_str_equal(operation, "add")) {
        if (property->kind == CHAPTER && numeric && fabs(number) == 1)
            return g_strdup(tr(number > 0 ? "Переходит к следующей главе." : "Переходит к предыдущей главе."));
        if (property->kind == DELAY && numeric) {
            const char *format = g_str_equal(property->name, "audio-delay")
                ? (number < 0 ? "Сдвигает звук на %s с раньше видео." : "Сдвигает звук на %s с позже видео.")
                : (number < 0 ? "Сдвигает субтитры на %s с раньше." : "Сдвигает субтитры на %s с позже.");
            return g_strdup_printf(tr(format), formatted);
        }
        if (numeric)
            return g_strdup_printf(tr(number < 0 ? "Уменьшает %s на %s." : "Увеличивает %s на %s."), label, formatted);
        return g_strdup_printf(tr("Изменяет %s на %s."), label, value);
    }
    if (g_str_equal(operation, "multiply")) {
        if (property->kind == SPEED && numeric && number > 0 && number != 1)
            return g_strdup_printf(tr(number < 1 ? "Замедляет воспроизведение: множитель %s." : "Ускоряет воспроизведение: множитель %s."), formatted);
        return g_strdup_printf(tr("Умножает %s на %s."), label, value);
    }
    if (g_str_equal(operation, "cycle")) {
        if (property->kind == TRACK)
            return g_strdup_printf(tr(g_str_equal(value, "down") ? "Выбирает предыдущую %s." : "Выбирает следующую %s."), label);
        return g_strdup_printf(tr("Переключает %s."), label);
    }
    if (g_str_equal(operation, "cycle-values") && property_index + 1 < count) {
        g_autofree char *values = g_strjoinv(", ", args + property_index + 1);
        return g_strdup_printf(tr(property_index == 2 ? "Переключает %s в обратном порядке: %s." : "Переключает %s между значениями: %s."), label, values);
    }
    if (g_str_equal(operation, "set") && count > 2) {
        if (property->kind == SPEED && numeric && number == 1)
            return g_strdup(tr("Возвращает скорость воспроизведения к 1×."));
        if (property->kind == BOOLEAN && (g_str_equal(value, "yes") || g_str_equal(value, "no"))) {
            gboolean enabled = g_str_equal(value, "yes");
            if (g_str_equal(property->name, "pause"))
                return g_strdup(tr(enabled ? "Приостанавливает воспроизведение." : "Продолжает воспроизведение."));
            if (g_str_equal(property->name, "mute"))
                return g_strdup(tr(enabled ? "Выключает звук." : "Включает звук."));
            if (g_str_equal(property->name, "fullscreen"))
                return g_strdup(tr(enabled ? "Включает полноэкранный режим." : "Возвращает оконный режим."));
            return g_strdup_printf(tr(enabled ? "Включает %s." : "Выключает %s."), label);
        }
        if (property->kind == TRACK) {
            if (g_str_equal(value, "no"))
                return g_strdup_printf(tr("Отключает %s."), label);
            if (g_str_equal(value, "auto"))
                return g_strdup_printf(tr("Выбирает %s автоматически."), label);
            return g_strdup_printf(tr("Выбирает %s с ID %s."), label, value);
        }
        return g_strdup_printf(tr("Устанавливает %s: %s."), label, value);
    }
    return NULL;
}

static const char *script_description(const char *name)
{
    static const struct { const char *name; const char *help; } scripts[] = {
        {"stats/display-stats", "Показывает статистику воспроизведения mpv."},
        {"stats/display-stats-toggle", "Включает или выключает постоянный показ статистики mpv."},
        {"stats/display-page-4-toggle", "Показывает или скрывает список привязок клавиш mpv."},
        {"commands/open", "Открывает консоль команд mpv."},
        {"osc/visibility", "Переключает видимость экранных элементов управления mpv."},
        {"select/select-playlist", "Открывает выбор файла из очереди mpv."},
        {"select/select-sid", "Открывает выбор дорожки субтитров."},
        {"select/select-secondary-sid", "Открывает выбор дополнительной дорожки субтитров."},
        {"select/select-aid", "Открывает выбор звуковой дорожки."},
        {"select/select-vid", "Открывает выбор видеодорожки."},
        {"select/select-track", "Открывает выбор звуковой, видео- или субтитровой дорожки."},
        {"select/select-chapter", "Открывает выбор главы."},
        {"select/select-edition", "Открывает выбор редакции файла."},
        {"select/select-subtitle-line", "Открывает переход к реплике субтитров."},
        {"select/select-audio-device", "Открывает выбор устройства звукового вывода."},
        {"select/select-watch-history", "Открывает историю просмотренных файлов."},
        {"select/select-watch-later", "Открывает список файлов с сохранённой позицией."},
        {"select/select-binding", "Открывает выбор привязки клавиши."},
        {"select/show-properties", "Открывает просмотр свойств mpv."},
        {"select/menu", "Открывает меню выбора действий mpv."},
    };
    for (guint i = 0; i < G_N_ELEMENTS(scripts); i++)
        if (g_str_equal(name, scripts[i].name))
            return tr(scripts[i].help);
    return NULL;
}

char *key_command_description(const char *command)
{
    if (!command || !*command)
        return g_strdup(tr("Команда не назначена."));
    int count;
    g_auto(GStrv) arguments = NULL;
    if (!g_shell_parse_argv(command, &count, &arguments, NULL)) {
        g_auto(GStrv) words = g_strsplit_set(command, " \t\r\n", -1);
        for (guint i = 0; words[i]; i++)
            if (*words[i] && !command_prefix(words[i]))
                return g_strdup(key_command_help(words[i]));
        return g_strdup(tr("Команда не назначена."));
    }
    int first = 0;
    while (first < count && command_prefix(arguments[first]))
        first++;
    if (first == count)
        return g_strdup(tr("Команда не назначена."));
    g_strdelimit(arguments[first], "_", '-');
    g_autofree char *normalized = NULL;
    if (count - first == 2 && g_str_equal(arguments[first], "seek")) {
        char *end = NULL;
        double seconds = g_ascii_strtod(arguments[first + 1], &end);
        if (end && !*end && (seconds == -5 || seconds == 5 || seconds == -30 || seconds == 30)) {
            char value[G_ASCII_DTOSTR_BUF_SIZE];
            g_ascii_dtostr(value, sizeof(value), seconds);
            normalized = g_strdup_printf("seek %s relative", value);
        }
    }
    if (!normalized)
        normalized = g_strjoinv(" ", arguments + first);
    for (guint i = 0; i < G_N_ELEMENTS(presets); i++)
        if (g_str_equal(normalized, presets[i].command))
            return g_strdup(tr(presets[i].help));
    if (g_str_equal(arguments[first], "script-binding") && count > first + 1) {
        const char *help = script_description(arguments[first + 1]);
        if (help)
            return g_strdup(help);
    }
    /* Composite bindings retain the generic command description rather than
     * describing only the first operation as if it were the whole binding. */
    if (!strchr(normalized, ';')) {
        char *description = property_description(arguments + first, count - first);
        if (description)
            return description;
    }
    return g_strdup(key_command_help(arguments[first]));
}

static const mpv_node *field(const mpv_node *node, const char *name)
{
    if (!node || node->format != MPV_FORMAT_NODE_MAP || !node->u.list)
        return NULL;
    for (int i = 0; i < node->u.list->num; i++)
        if (g_str_equal(node->u.list->keys[i], name))
            return &node->u.list->values[i];
    return NULL;
}

static const char *string_field(const mpv_node *node, const char *name)
{
    const mpv_node *value = field(node, name);
    return value && value->format == MPV_FORMAT_STRING ? value->u.string : NULL;
}

static gboolean flag_field(const mpv_node *node, const char *name)
{
    const mpv_node *value = field(node, name);
    return value && value->format == MPV_FORMAT_FLAG && value->u.flag;
}

static void free_argument(gpointer data)
{
    KeyCommandArgument *argument = data;
    g_free(argument->name);
    g_free(argument->type);
    g_free(argument);
}

static void free_spec(gpointer data)
{
    KeyCommandSpec *spec = data;
    g_free(spec->name);
    g_free(spec->signature);
    g_free(spec->description);
    g_free(spec->category);
    g_free(spec->example);
    g_ptr_array_unref(spec->arguments);
    g_free(spec);
}

GPtrArray *key_command_list_from_node(const mpv_node *node)
{
    GPtrArray *result = g_ptr_array_new_with_free_func(free_spec);
    if (!node || node->format != MPV_FORMAT_NODE_ARRAY || !node->u.list)
        return result;
    for (int i = 0; i < node->u.list->num; i++) {
        const mpv_node *command = &node->u.list->values[i];
        const char *name = string_field(command, "name");
        if (!name || !*name)
            continue;
        KeyCommandSpec *spec = g_new0(KeyCommandSpec, 1);
        spec->name = g_strdup(name);
        spec->description = g_strdup(key_command_help(name));
        spec->category = g_strdup(key_command_category(name));
        spec->example = g_strdup(key_command_example(name));
        spec->vararg = flag_field(command, "vararg");
        spec->arguments = g_ptr_array_new_with_free_func(free_argument);
        GString *signature = g_string_new(name);
        const mpv_node *arguments = field(command, "args");
        if (arguments && arguments->format == MPV_FORMAT_NODE_ARRAY && arguments->u.list) {
            for (int j = 0; j < arguments->u.list->num; j++) {
                const mpv_node *argument = &arguments->u.list->values[j];
                const char *arg_name = string_field(argument, "name");
                const char *arg_type = string_field(argument, "type");
                if (!arg_name || !arg_type)
                    continue;
                KeyCommandArgument *copy = g_new0(KeyCommandArgument, 1);
                copy->name = g_strdup(arg_name);
                copy->type = g_strdup(arg_type);
                copy->optional = flag_field(argument, "optional");
                g_ptr_array_add(spec->arguments, copy);
                g_string_append_printf(signature, " %s%s=%s%s%s", copy->optional ? "[" : "",
                    arg_name, arg_type,
                    spec->vararg && j + 1 == arguments->u.list->num ? "..." : "",
                    copy->optional ? "]" : "");
            }
        }
        spec->signature = g_string_free(signature, FALSE);
        g_ptr_array_add(result, spec);
    }
    return result;
}
