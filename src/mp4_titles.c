/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "mp4_titles.h"
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include "windows_file_io.h"
#else
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define MAX_TAGS 64
#define MAX_BOXES 100000
struct box {
    uint64_t start, payload, end;
    unsigned char type[4];
};
struct parser {
    int fd;
    uint64_t size, offsets[MAX_TAGS];
    unsigned count, visited;
    char *error;
    size_t error_size;
};
static const unsigned char title_type[4] = {0xa9, 'n', 'a', 'm'};

static bool fail(struct parser *p, const char *format, ...)
{
    if (p->error_size) {
        va_list args;
        va_start(args, format);
        vsnprintf(p->error, p->error_size, format, args);
        va_end(args);
    }
    return false;
}
static uint32_t be32(const unsigned char *b)
{
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | b[3];
}
static bool read_at(struct parser *p, uint64_t offset, void *data, size_t size)
{
    if (offset > p->size || size > p->size - offset)
        return fail(p, "Metadata extends beyond the file");
    size_t done = 0;
    while (done < size) {
        ssize_t result = pread(p->fd, (char *)data + done, size - done,
                               (off_t)(offset + done));
        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0)
            return fail(p, "Cannot read metadata: %s",
                        result < 0 ? strerror(errno) : "unexpected EOF");
        done += (size_t)result;
    }
    return true;
}
static bool next_box(struct parser *p, uint64_t *position, uint64_t end,
                     struct box *box)
{
    unsigned char header[16];
    uint64_t start = *position, size;
    size_t header_size = 8;
    if (++p->visited > MAX_BOXES)
        return fail(p, "Too many MP4 boxes");
    if (start > end || end - start < 8)
        return fail(p, "Truncated MP4 box header");
    if (!read_at(p, start, header, 8))
        return false;
    size = be32(header);
    if (size == 1) {
        header_size = 16;
        if (end - start < 16 || !read_at(p, start + 8, header + 8, 8))
            return fail(p, "Truncated extended MP4 box");
        size = ((uint64_t)be32(header + 8) << 32) | be32(header + 12);
    } else if (size == 0) {
        size = end - start;
    }
    if (size < header_size || size > end - start)
        return fail(p, "Invalid MP4 box size");
    *box = (struct box){.start = start, .payload = start + header_size,
                        .end = start + size};
    memcpy(box->type, header + 4, 4);
    *position = box->end;
    return true;
}
static bool type_is(const struct box *box, const char *type)
{
    return memcmp(box->type, type, 4) == 0;
}
static bool scan_mdia(struct parser *p, const struct box *mdia, bool *audio)
{
    for (uint64_t pos = mdia->payload; pos < mdia->end;) {
        struct box box;
        if (!next_box(p, &pos, mdia->end, &box))
            return false;
        if (type_is(&box, "hdlr")) {
            unsigned char type[4];
            if (box.end - box.payload < 12)
                return fail(p, "Truncated media handler");
            if (!read_at(p, box.payload + 8, type, 4))
                return false;
            *audio = memcmp(type, "soun", 4) == 0;
        }
    }
    return true;
}
static bool scan_track(struct parser *p, const struct box *track)
{
    bool audio = false, has_title = false, has_name = false;
    uint64_t name_offset = 0;
    for (uint64_t pos = track->payload; pos < track->end;) {
        struct box box;
        if (!next_box(p, &pos, track->end, &box))
            return false;
        if (type_is(&box, "mdia") && !scan_mdia(p, &box, &audio))
            return false;
        if (!type_is(&box, "udta"))
            continue;
        for (uint64_t child_pos = box.payload; child_pos < box.end;) {
            struct box child;
            if (!next_box(p, &child_pos, box.end, &child))
                return false;
            if (memcmp(child.type, title_type, 4) == 0)
                has_title = true;
            if (type_is(&child, "name") && child.end > child.payload) {
                if (has_name)
                    return fail(p, "Multiple name tags in one track");
                has_name = true;
                name_offset = child.start + 4;
            }
        }
    }
    if (!audio || !has_name || has_title)
        return true;
    if (p->count == MAX_TAGS)
        return fail(p, "Too many audio name tags");
    p->offsets[p->count++] = name_offset;
    return true;
}
static bool plan(struct parser *p)
{
    bool has_ftyp = false, has_moov = false;
    for (uint64_t pos = 0; pos < p->size;) {
        struct box box;
        if (!next_box(p, &pos, p->size, &box))
            return false;
        if (type_is(&box, "ftyp") && box.end - box.payload >= 8)
            has_ftyp = true;
        if (!type_is(&box, "moov"))
            continue;
        if (has_moov)
            return fail(p, "Multiple movie headers");
        has_moov = true;
        for (uint64_t child_pos = box.payload; child_pos < box.end;) {
            struct box child;
            if (!next_box(p, &child_pos, box.end, &child))
                return false;
            if (type_is(&child, "trak") && !scan_track(p, &child))
                return false;
        }
    }
    if (!has_ftyp || !has_moov)
        return fail(p, "Not a finalized MP4 file");
    return true;
}
static bool write_type(int fd, uint64_t offset, const unsigned char *type)
{
    size_t done = 0;
    while (done < 4) {
        ssize_t result = pwrite(fd, type + done, 4 - done, (off_t)(offset + done));
        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0) {
            if (!result)
                errno = EIO;
            return false;
        }
        done += (size_t)result;
    }
    return true;
}
static bool same_file_state(const struct stat *a, const struct stat *b)
{
    return a->st_size == b->st_size &&
           a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
           a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
           a->st_ctim.tv_sec == b->st_ctim.tv_sec &&
           a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}
int mp4_fix_audio_titles(const char *path, char *error, size_t error_size)
{
    struct parser p = {.fd = -1, .error = error, .error_size = error_size};
    struct stat before, after;
    int result = -1;
    if (error_size)
        error[0] = '\0';
    if (!path || !*path) {
        fail(&p, "No file path");
        return -1;
    }
    p.fd = open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (p.fd < 0) {
        fail(&p, "Cannot open file: %s", strerror(errno));
        return -1;
    }
    if (fstat(p.fd, &before) < 0 || !S_ISREG(before.st_mode) || before.st_size < 8) {
        fail(&p, "Not a regular MP4 file");
        goto done;
    }
    /* FFmpeg writes ftyp first for MP4-family exports. Other containers do
     * not need this normalization and must retain their original metadata. */
    unsigned char header[8];
    p.size = (uint64_t)before.st_size;
    if (!read_at(&p, 0, header, sizeof(header)))
        goto done;
    if (memcmp(header + 4, "ftyp", 4) != 0) {
        result = 0;
        goto done;
    }
    if (flock(p.fd, LOCK_EX | LOCK_NB) < 0) {
        fail(&p, "File is locked: %s", strerror(errno));
        goto done;
    }
    if (!plan(&p))
        goto done;
    if (!p.count) {
        result = 0;
        goto done;
    }
    if (fstat(p.fd, &after) < 0 || !same_file_state(&before, &after)) {
        fail(&p, "File changed during metadata inspection");
        goto done;
    }
    /* Validate the entire plan before changing any bytes. */
    for (unsigned i = 0; i < p.count; ++i) {
        unsigned char current[4];
        if (!read_at(&p, p.offsets[i], current, 4))
            goto done;
        if (memcmp(current, "name", 4) != 0) {
            fail(&p, "Name tag changed during metadata inspection");
            goto done;
        }
    }
    unsigned attempted = 0;
    bool success = true;
    for (unsigned i = 0; i < p.count; ++i) {
        attempted = i + 1;
        if (!write_type(p.fd, p.offsets[i], title_type)) {
            success = false;
            break;
        }
    }
    if (success && fsync(p.fd) < 0)
        success = false;
    if (!success) {
        int saved_errno = errno;
        bool restored = true;
        for (unsigned i = 0; i < attempted; ++i) {
            if (!write_type(p.fd, p.offsets[i], (const unsigned char *)"name"))
                restored = false;
        }
        if (fsync(p.fd) < 0)
            restored = false;
        fail(&p, restored ? "Metadata write failed; original tags restored: %s"
                          : "Metadata write failed; tag rollback incomplete: %s",
             strerror(saved_errno));
        goto done;
    }
    result = (int)p.count;
done:
    close(p.fd);
    return result;
}
