#include "content_dir.h"
#include "sdcard.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>

struct mime_entry {
    const char *ext;
    const char *mime;
    bool is_video;
};

/* Roku Media Player direct-plays H.264 in mp4/mkv/mov; a few common siblings included. */
static const struct mime_entry MIME_TABLE[] = {
    {".mp4",  "video/mp4",              true},
    {".m4v",  "video/mp4",              true},
    {".mov",  "video/quicktime",        true},
    {".mkv",  "video/x-matroska",       true},
    {".ts",   "video/mp2t",             true},
    {".m2ts", "video/mp2t",             true},
    {".webm", "video/webm",             true},
    {".avi",  "video/x-msvideo",        true},
    {".mp3",  "audio/mpeg",             false},
    {".m4a",  "audio/mp4",              false},
    {".aac",  "audio/aac",              false},
    {".flac", "audio/flac",             false},
    {".wav",  "audio/wav",              false},
    {".jpg",  "image/jpeg",             false},
    {".jpeg", "image/jpeg",             false},
    {".png",  "image/png",              false},
};

static const struct mime_entry *lookup(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(MIME_TABLE) / sizeof(MIME_TABLE[0]); i++) {
        if (strcasecmp(dot, MIME_TABLE[i].ext) == 0) {
            return &MIME_TABLE[i];
        }
    }
    return NULL;
}

const char *content_dir_mime(const char *name)
{
    const struct mime_entry *e = lookup(name);
    return e ? e->mime : "application/octet-stream";
}

bool content_dir_is_video(const char *name)
{
    const struct mime_entry *e = lookup(name);
    return e && e->is_video;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool content_dir_url_decode(const char *src, char *dst, size_t dst_len)
{
    size_t o = 0;
    for (size_t i = 0; src[i] != '\0'; i++) {
        if (o + 1 >= dst_len) {
            return false;
        }
        char c = src[i];
        if (c == '%') {
            int hi = hexval(src[i + 1]);
            int lo = (hi >= 0) ? hexval(src[i + 2]) : -1;
            if (hi < 0 || lo < 0) {
                return false;   /* malformed escape */
            }
            dst[o++] = (char)((hi << 4) | lo);
            i += 2;
        } else if (c == '+') {
            dst[o++] = ' ';
        } else {
            dst[o++] = c;
        }
    }
    dst[o] = '\0';
    return true;
}

bool content_dir_path_is_safe(const char *rel_path)
{
    if (!rel_path || rel_path[0] != '/') {
        return false;
    }
    /* Reject any ".." component (start, middle, or end). */
    const char *p = rel_path;
    while ((p = strstr(p, "..")) != NULL) {
        char before = (p == rel_path) ? '/' : p[-1];
        char after = p[2];
        if ((before == '/' || before == '\0') && (after == '/' || after == '\0')) {
            return false;
        }
        p += 2;
    }
    return true;
}

bool content_dir_full_path(const char *rel_path, char *out, size_t out_len)
{
    /* "/" means the SD root itself. */
    int n;
    if (strcmp(rel_path, "/") == 0) {
        n = snprintf(out, out_len, "%s", SD_MOUNT_POINT);
    } else {
        n = snprintf(out, out_len, "%s%s", SD_MOUNT_POINT, rel_path);
    }
    return n > 0 && (size_t)n < out_len;
}

bool content_dir_objectid_to_relpath(const char *object_id, char *out, size_t out_len)
{
    if (!object_id || object_id[0] == '\0' || strcmp(object_id, "0") == 0) {
        return snprintf(out, out_len, "/") < (int)out_len;
    }
    if (object_id[0] != '/') {
        return false;
    }
    int n = snprintf(out, out_len, "%s", object_id);
    return n > 0 && (size_t)n < out_len;
}
