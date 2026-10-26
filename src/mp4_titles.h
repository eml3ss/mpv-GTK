/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MP4_TITLES_H
#define MP4_TITLES_H
#include <stddef.h>
/* Return changed audio tag count, zero for a no-op/non-MP4, or -1 with an
 * error. Only call on a private, completed FFmpeg export, never the source. */
int mp4_fix_audio_titles(const char *path, char *error, size_t error_size);
#endif
