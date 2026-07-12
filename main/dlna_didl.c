#include "dlna_didl.h"
#include "content_dir.h"
#include "sdcard.h"
#include "sdkconfig.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_log.h"

static const char *TAG = "didl";

/* Byte-seek + native-content flags. FLAGS field advertises streaming + byte-based
 * seek + interoperable; this is the widely-used value for direct-play video. */
#define DLNA_FLAGS "DLNA.ORG_OP=01;DLNA.ORG_CI=0;DLNA.ORG_FLAGS=01700000000000000000000000000000"

static const char *DIDL_HEADER =
    "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" "
    "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
    "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\" "
    "xmlns:dlna=\"urn:schemas-dlna-org:metadata-1-0/\">";
static const char *DIDL_FOOTER = "</DIDL-Lite>";

const char *dlna_content_features(const char *mime)
{
    (void)mime;   /* same OP/CI/FLAGS for all direct-play content */
    return DLNA_FLAGS;
}

/* -------- growable string buffer -------- */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    bool err;
} strbuf_t;

static void sb_reserve(strbuf_t *sb, size_t extra)
{
    if (sb->err) {
        return;
    }
    if (sb->len + extra + 1 <= sb->cap) {
        return;
    }
    size_t ncap = sb->cap ? sb->cap : 512;
    while (ncap < sb->len + extra + 1) {
        ncap *= 2;
    }
    char *nb = realloc(sb->buf, ncap);
    if (!nb) {
        sb->err = true;
        return;
    }
    sb->buf = nb;
    sb->cap = ncap;
}

static void sb_puts(strbuf_t *sb, const char *s)
{
    size_t n = strlen(s);
    sb_reserve(sb, n);
    if (sb->err) {
        return;
    }
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

static void sb_printf(strbuf_t *sb, const char *fmt, ...)
{
    char tmp[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) {
        sb->err = true;
        return;
    }
    if ((size_t)n < sizeof(tmp)) {
        sb_puts(sb, tmp);
        return;
    }
    /* Rare: grow and retry. */
    sb_reserve(sb, n);
    if (sb->err) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(sb->buf + sb->len, sb->cap - sb->len, fmt, ap);
    va_end(ap);
    sb->len += n;
}

/* Append s, escaping XML metacharacters. */
static void sb_put_escaped(strbuf_t *sb, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '&':  sb_puts(sb, "&amp;");  break;
        case '<':  sb_puts(sb, "&lt;");   break;
        case '>':  sb_puts(sb, "&gt;");   break;
        case '"':  sb_puts(sb, "&quot;"); break;
        case '\'': sb_puts(sb, "&apos;"); break;
        default: {
            char c[2] = { *s, '\0' };
            sb_puts(sb, c);
        }
        }
    }
}

/* Append s, percent-encoding everything that isn't URL-safe (keeps '/'). */
static void sb_put_urlenc(strbuf_t *sb, const char *s)
{
    static const char *hex = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                    c == '.' || c == '~' || c == '/';
        if (safe) {
            char b[2] = { (char)c, '\0' };
            sb_puts(sb, b);
        } else {
            char b[4] = { '%', hex[c >> 4], hex[c & 0xF], '\0' };
            sb_puts(sb, b);
        }
    }
}

char *dlna_xml_escape(const char *src)
{
    strbuf_t sb = {0};
    sb_put_escaped(&sb, src);
    if (sb.err) {
        free(sb.buf);
        return NULL;
    }
    return sb.buf ? sb.buf : strdup("");
}

/* basename of a relative path: "/a/b.mp4" -> "b.mp4"; "/" -> root title. */
static const char *base_name(const char *rel)
{
    const char *slash = strrchr(rel, '/');
    return (slash && slash[1]) ? slash + 1 : rel;
}

/* Compute parentID for a relative path. "/" -> "-1"; "/a" -> "0"; "/a/b" -> "/a". */
static void parent_id(const char *rel, char *out, size_t out_len)
{
    if (strcmp(rel, "/") == 0) {
        snprintf(out, out_len, "-1");
        return;
    }
    const char *slash = strrchr(rel, '/');
    if (!slash || slash == rel) {
        snprintf(out, out_len, "0");   /* child of root */
        return;
    }
    size_t n = (size_t)(slash - rel);
    if (n >= out_len) {
        n = out_len - 1;
    }
    memcpy(out, rel, n);
    out[n] = '\0';
}

/* Emit one <container> element. object_id is the container's id. */
static void emit_container(strbuf_t *sb, const char *object_id, const char *parent,
                           const char *title, int child_count)
{
    sb_puts(sb, "<container id=\"");
    sb_put_escaped(sb, object_id);
    sb_puts(sb, "\" parentID=\"");
    sb_put_escaped(sb, parent);
    sb_puts(sb, "\" restricted=\"1\"");
    if (child_count >= 0) {
        sb_printf(sb, " childCount=\"%d\"", child_count);
    }
    sb_puts(sb, "><dc:title>");
    sb_put_escaped(sb, title);
    sb_puts(sb, "</dc:title><upnp:class>object.container.storageFolder</upnp:class></container>");
}

/* Emit one <item> element for a media file. */
static void emit_item(strbuf_t *sb, const char *object_id, const char *parent,
                      const char *title, const char *rel_path,
                      const char *server_base, long size)
{
    const char *mime = content_dir_mime(title);
    sb_puts(sb, "<item id=\"");
    sb_put_escaped(sb, object_id);
    sb_puts(sb, "\" parentID=\"");
    sb_put_escaped(sb, parent);
    sb_puts(sb, "\" restricted=\"1\"><dc:title>");
    sb_put_escaped(sb, title);
    sb_puts(sb, "</dc:title><upnp:class>object.item.videoItem</upnp:class>");

    sb_puts(sb, "<res protocolInfo=\"http-get:*:");
    sb_put_escaped(sb, mime);
    sb_puts(sb, ":");
    sb_puts(sb, DLNA_FLAGS);
    sb_puts(sb, "\"");
    if (size >= 0) {
        sb_printf(sb, " size=\"%ld\"", size);
    }
    sb_puts(sb, ">");
    /* res text: absolute URL to our streamer, with the path percent-encoded. */
    sb_puts(sb, server_base);
    sb_puts(sb, "/media?path=");
    sb_put_urlenc(sb, rel_path);
    sb_puts(sb, "</res></item>");
}

/* -------- directory reading -------- */
typedef struct {
    char *name;
    bool is_dir;
    long size;
} entry_t;

static int entry_cmp(const void *a, const void *b)
{
    const entry_t *ea = a, *eb = b;
    if (ea->is_dir != eb->is_dir) {
        return ea->is_dir ? -1 : 1;   /* folders first */
    }
    return strcasecmp(ea->name, eb->name);
}

/* Count children of a directory (folders + playable video), for childCount. */
static int count_children(const char *full_dir)
{
    DIR *d = opendir(full_dir);
    if (!d) {
        return -1;
    }
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') {
            continue;
        }
        if (de->d_type == DT_DIR || content_dir_is_video(de->d_name)) {
            n++;
        }
    }
    closedir(d);
    return n;
}

char *dlna_didl_children(const char *dir_rel_path, const char *server_base,
                         int starting_index, int requested_count,
                         int *number_returned, int *total_matches)
{
    *number_returned = 0;
    *total_matches = 0;

    char full_dir[512];
    if (!content_dir_full_path(dir_rel_path, full_dir, sizeof(full_dir))) {
        return NULL;
    }
    DIR *d = opendir(full_dir);
    if (!d) {
        ESP_LOGW(TAG, "opendir(%s) failed", full_dir);
        return NULL;
    }

    /* Collect folders + playable files. */
    size_t cap = 32, n = 0;
    entry_t *entries = malloc(cap * sizeof(entry_t));
    if (!entries) {
        closedir(d);
        return NULL;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') {
            continue;
        }
        bool is_dir = (de->d_type == DT_DIR);
        if (!is_dir && de->d_type == DT_UNKNOWN) {
            /* Some FAT setups report UNKNOWN; stat to disambiguate. */
            char fp[512];
            snprintf(fp, sizeof(fp), "%s/%s", full_dir, de->d_name);
            struct stat st;
            if (stat(fp, &st) == 0) {
                is_dir = S_ISDIR(st.st_mode);
            }
        }
        if (!is_dir && !content_dir_is_video(de->d_name)) {
            continue;
        }
        if (n == cap) {
            cap *= 2;
            entry_t *ne = realloc(entries, cap * sizeof(entry_t));
            if (!ne) {
                break;
            }
            entries = ne;
        }
        entries[n].name = strdup(de->d_name);
        entries[n].is_dir = is_dir;
        entries[n].size = -1;
        n++;
    }
    closedir(d);

    qsort(entries, n, sizeof(entry_t), entry_cmp);
    *total_matches = (int)n;

    if (requested_count <= 0) {
        requested_count = (int)n;   /* 0 == all remaining */
    }

    strbuf_t sb = {0};
    sb_puts(&sb, DIDL_HEADER);

    char base_dir[384];
    /* Normalise the container prefix so child IDs are "/dir/child" (root -> "/child"). */
    if (strcmp(dir_rel_path, "/") == 0) {
        base_dir[0] = '\0';
    } else {
        snprintf(base_dir, sizeof(base_dir), "%s", dir_rel_path);
    }
    const char *parent_of_children = (dir_rel_path[0] == '\0' || strcmp(dir_rel_path, "/") == 0)
                                         ? "0" : dir_rel_path;

    int emitted = 0;
    for (int i = starting_index; i < (int)n && emitted < requested_count; i++) {
        char child_id[512];
        snprintf(child_id, sizeof(child_id), "%s/%s", base_dir, entries[i].name);

        if (entries[i].is_dir) {
            char child_full[512];
            content_dir_full_path(child_id, child_full, sizeof(child_full));
            emit_container(&sb, child_id, parent_of_children, entries[i].name,
                           count_children(child_full));
        } else {
            char child_full[512];
            long size = -1;
            if (content_dir_full_path(child_id, child_full, sizeof(child_full))) {
                struct stat st;
                if (stat(child_full, &st) == 0) {
                    size = (long)st.st_size;
                }
            }
            emit_item(&sb, child_id, parent_of_children, entries[i].name,
                      child_id, server_base, size);
        }
        emitted++;
    }

    sb_puts(&sb, DIDL_FOOTER);
    *number_returned = emitted;

    for (size_t i = 0; i < n; i++) {
        free(entries[i].name);
    }
    free(entries);

    if (sb.err) {
        free(sb.buf);
        return NULL;
    }
    return sb.buf;
}

char *dlna_didl_metadata(const char *rel_path, const char *server_base)
{
    char parent[384];
    parent_id(rel_path, parent, sizeof(parent));

    strbuf_t sb = {0};
    sb_puts(&sb, DIDL_HEADER);

    if (strcmp(rel_path, "/") == 0) {
        char full[512];
        content_dir_full_path(rel_path, full, sizeof(full));
        emit_container(&sb, "0", "-1", CONFIG_DLNA_FRIENDLY_NAME, count_children(full));
    } else {
        char full[512];
        if (!content_dir_full_path(rel_path, full, sizeof(full))) {
            free(sb.buf);
            return NULL;
        }
        struct stat st;
        if (stat(full, &st) != 0) {
            free(sb.buf);
            return NULL;
        }
        const char *title = base_name(rel_path);
        if (S_ISDIR(st.st_mode)) {
            emit_container(&sb, rel_path, parent, title, count_children(full));
        } else {
            emit_item(&sb, rel_path, parent, title, rel_path, server_base,
                      (long)st.st_size);
        }
    }

    sb_puts(&sb, DIDL_FOOTER);
    if (sb.err) {
        free(sb.buf);
        return NULL;
    }
    return sb.buf;
}
