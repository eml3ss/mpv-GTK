#ifndef MPV_GTK_CHAPTER_MARKS_H
#define MPV_GTK_CHAPTER_MARKS_H

#include <gtk/gtk.h>
#include <mpv/client.h>

/* Wrap an unparented horizontal seek scale. The original range remains the
 * interactive child; decorative marks add no requested width or height. */
GtkWidget *chapter_marks_new(GtkRange *scale);

/* Copy chapter start times. Duration is independent so either event order is
 * supported. None of these functions changes the range value or its input. */
void chapter_marks_update(GtkWidget *overlay, const mpv_node *chapter_list);
void chapter_marks_set_duration(GtkWidget *overlay, double duration);
void chapter_marks_reset(GtkWidget *overlay);

#endif
