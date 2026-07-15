#include "content_index.h"
#include "content_dir.h"
#include "sdcard.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include "esp_log.h"

static const char *TAG = "cidx";

#define INFO_DIR ".info"

/* Hide dotfiles, the .info mirror, and common filesystem junk from listings. */
static bool skip_name(const char *n)
{
    if (n[0] == '.') {
        return true;
    }
    if (!strcasecmp(n, "System Volume Information") ||
        !strcasecmp(n, "$RECYCLE.BIN") ||
        !strcasecmp(n, "found.000")) {
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ paths */

/* /sdcard/.info[/<rel_dir>] — the mirror directory holding rel_dir's index. */
static bool info_dir_path(const char *rel_dir, char *out, size_t out_len)
{
    int n;
    if (strcmp(rel_dir, "/") == 0) {
        n = snprintf(out, out_len, "%s/%s", SD_MOUNT_POINT, INFO_DIR);
    } else {
        n = snprintf(out, out_len, "%s/%s%s", SD_MOUNT_POINT, INFO_DIR, rel_dir);
    }
    return n > 0 && (size_t)n < out_len;
}

static bool index_file_path(const char *rel_dir, char *out, size_t out_len)
{
    char dir[512];
    if (!info_dir_path(rel_dir, dir, sizeof(dir))) {
        return false;
    }
    int n = snprintf(out, out_len, "%s/index.xml", dir);
    return n > 0 && (size_t)n < out_len;
}

/* Parent of rel_dir: "/Movies/Action" -> "/Movies", "/Movies" -> "/". False at root. */
static bool parent_rel(const char *rel_dir, char *out, size_t out_len)
{
    if (strcmp(rel_dir, "/") == 0) {
        return false;
    }
    const char *slash = strrchr(rel_dir, '/');
    if (!slash || slash == rel_dir) {
        snprintf(out, out_len, "/");
        return true;
    }
    size_t k = (size_t)(slash - rel_dir);
    if (k >= out_len) {
        k = out_len - 1;
    }
    memcpy(out, rel_dir, k);
    out[k] = '\0';
    return true;
}

/* mkdir -p on a VFS directory path. */
static void mkdir_p(const char *path)
{
    char tmp[600];
    int n = snprintf(tmp, sizeof(tmp), "%s", path);
    if (n <= 0 || (size_t)n >= sizeof(tmp)) {
        return;
    }
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    mkdir(tmp, 0777);
}

/* ------------------------------------------------------- scanning a folder */

typedef struct {
    char *name;
    bool is_dir;
    long size;
    int child_count;
} scan_entry_t;

static int scan_cmp(const void *a, const void *b)
{
    const scan_entry_t *ea = a, *eb = b;
    if (ea->is_dir != eb->is_dir) {
        return ea->is_dir ? -1 : 1;   /* folders first */
    }
    return strcasecmp(ea->name, eb->name);
}

/* Count listable children (folders + playable video) of a directory. */
static int count_children(const char *full_dir)
{
    DIR *d = opendir(full_dir);
    if (!d) {
        return -1;
    }
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (skip_name(de->d_name)) {
            continue;
        }
        bool is_dir = (de->d_type == DT_DIR);
        if (de->d_type == DT_UNKNOWN) {
            char fp[600];
            snprintf(fp, sizeof(fp), "%s/%s", full_dir, de->d_name);
            struct stat st;
            if (stat(fp, &st) == 0) {
                is_dir = S_ISDIR(st.st_mode);
            }
        }
        if (is_dir || content_dir_is_video(de->d_name)) {
            n++;
        }
    }
    closedir(d);
    return n;
}

/* Scan rel_dir into a sorted array (folders first). Returns false if not a dir. */
static bool scan_dir(const char *rel_dir, scan_entry_t **out_arr, int *out_n)
{
    char full[512];
    if (!content_dir_full_path(rel_dir, full, sizeof(full))) {
        return false;
    }
    DIR *d = opendir(full);
    if (!d) {
        return false;
    }
    size_t cap = 16;
    int n = 0;
    scan_entry_t *arr = malloc(cap * sizeof(scan_entry_t));
    if (!arr) {
        closedir(d);
        return false;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (skip_name(de->d_name)) {
            continue;   /* dotfiles, .info mirror, and FS junk */
        }
        char child_full[600];
        snprintf(child_full, sizeof(child_full), "%s/%s", full, de->d_name);
        bool is_dir = (de->d_type == DT_DIR);
        if (de->d_type == DT_UNKNOWN) {
            struct stat st;
            if (stat(child_full, &st) == 0) {
                is_dir = S_ISDIR(st.st_mode);
            }
        }
        if (!is_dir && !content_dir_is_video(de->d_name)) {
            continue;
        }
        if ((size_t)n == cap) {
            cap *= 2;
            scan_entry_t *ne = realloc(arr, cap * sizeof(scan_entry_t));
            if (!ne) {
                break;
            }
            arr = ne;
        }
        arr[n].name = strdup(de->d_name);
        arr[n].is_dir = is_dir;
        if (is_dir) {
            arr[n].size = -1;
            arr[n].child_count = count_children(child_full);
        } else {
            struct stat st;
            arr[n].size = (stat(child_full, &st) == 0) ? (long)st.st_size : -1;
            arr[n].child_count = -1;
        }
        n++;
    }
    closedir(d);
    qsort(arr, n, sizeof(scan_entry_t), scan_cmp);
    *out_arr = arr;
    *out_n = n;
    return true;
}

static void free_scan(scan_entry_t *arr, int n)
{
    for (int i = 0; i < n; i++) {
        free(arr[i].name);
    }
    free(arr);
}

/* ------------------------------------------------------ index file I/O */

static void fput_escaped(FILE *f, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '&':  fputs("&amp;", f);  break;
        case '<':  fputs("&lt;", f);   break;
        case '>':  fputs("&gt;", f);   break;
        case '"':  fputs("&quot;", f); break;
        case '\'': fputs("&apos;", f); break;
        default:   fputc(*s, f);
        }
    }
}

static void xml_unescape(char *s)
{
    char *w = s;
    for (char *r = s; *r;) {
        if (*r == '&') {
            if (!strncmp(r, "&amp;", 5))  { *w++ = '&';  r += 5; continue; }
            if (!strncmp(r, "&lt;", 4))   { *w++ = '<';  r += 4; continue; }
            if (!strncmp(r, "&gt;", 4))   { *w++ = '>';  r += 4; continue; }
            if (!strncmp(r, "&quot;", 6)) { *w++ = '"';  r += 6; continue; }
            if (!strncmp(r, "&apos;", 6)) { *w++ = '\''; r += 6; continue; }
        }
        *w++ = *r++;
    }
    *w = '\0';
}

/* Best-effort write of the index file for rel_dir. */
static bool write_index(const char *rel_dir, scan_entry_t *arr, int n)
{
    char dir[512];
    if (!info_dir_path(rel_dir, dir, sizeof(dir))) {
        return false;
    }
    mkdir_p(dir);

    char path[600];
    if (!index_file_path(rel_dir, path, sizeof(path))) {
        return false;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGW(TAG, "cannot write %s (SD read-only?) — serving live", path);
        return false;
    }
    fputs("<idx v=\"1\">\n", f);
    for (int i = 0; i < n; i++) {
        if (arr[i].is_dir) {
            fputs("<d name=\"", f);
            fput_escaped(f, arr[i].name);
            fprintf(f, "\" n=\"%d\"/>\n", arr[i].child_count);
        } else {
            fputs("<f name=\"", f);
            fput_escaped(f, arr[i].name);
            fprintf(f, "\" s=\"%ld\"/>\n", arr[i].size);
        }
    }
    fputs("</idx>\n", f);
    fclose(f);
    return true;
}

/* Extract key="value" from a line into out. Values never contain a raw '"'
 * (they were XML-escaped on write), so reading to the next '"' is safe. */
static bool get_attr(const char *line, const char *key, char *out, size_t out_len)
{
    char pat[16];
    snprintf(pat, sizeof(pat), "%s=\"", key);
    const char *p = strstr(line, pat);
    if (!p) {
        return false;
    }
    p += strlen(pat);
    const char *e = strchr(p, '"');
    if (!e) {
        return false;
    }
    size_t k = (size_t)(e - p);
    if (k >= out_len) {
        k = out_len - 1;
    }
    memcpy(out, p, k);
    out[k] = '\0';
    return true;
}

static bool iterate_from_file(FILE *f, int start, int count,
                              content_index_cb cb, void *ctx,
                              int *number_returned, int *total_matches)
{
    char *line = malloc(1600);
    char *name = malloc(300);
    if (!line || !name) {
        free(line);
        free(name);
        return false;
    }
    int idx = 0, emitted = 0;
    while (fgets(line, 1600, f)) {
        if (line[0] != '<' || (line[1] != 'd' && line[1] != 'f')) {
            continue;   /* header/footer or junk */
        }
        bool is_dir = (line[1] == 'd');
        if (!get_attr(line, "name", name, 300)) {
            continue;
        }
        xml_unescape(name);

        content_index_entry_t e = {
            .name = name, .is_dir = is_dir, .size = -1, .child_count = -1,
        };
        char val[24];
        if (is_dir) {
            if (get_attr(line, "n", val, sizeof(val))) {
                e.child_count = atoi(val);
            }
        } else {
            if (get_attr(line, "s", val, sizeof(val))) {
                e.size = atol(val);
            }
        }

        if (idx >= start && (count <= 0 || idx < start + count)) {
            if (cb) {
                cb(&e, ctx);
            }
            emitted++;
        }
        idx++;
    }
    free(line);
    free(name);
    *total_matches = idx;
    *number_returned = emitted;
    return true;
}

/* --------------------------------------------------------------- public */

bool content_index_iterate(const char *rel_dir, int start, int count,
                           content_index_cb cb, void *ctx,
                           int *number_returned, int *total_matches)
{
    *number_returned = 0;
    *total_matches = 0;

    char path[600];
    if (!index_file_path(rel_dir, path, sizeof(path))) {
        return false;
    }

    FILE *f = fopen(path, "r");
    if (f) {
        bool ok = iterate_from_file(f, start, count, cb, ctx,
                                    number_returned, total_matches);
        fclose(f);
        return ok;
    }

    /* No cached index — scan once, write the cache, serve from the scan. */
    scan_entry_t *arr = NULL;
    int n = 0;
    if (!scan_dir(rel_dir, &arr, &n)) {
        return false;
    }
    write_index(rel_dir, arr, n);   /* best-effort */
    *total_matches = n;

    int emitted = 0;
    for (int i = (start > 0 ? start : 0);
         i < n && (count <= 0 || i < start + count); i++) {
        content_index_entry_t e = {
            .name = arr[i].name, .is_dir = arr[i].is_dir,
            .size = arr[i].size, .child_count = arr[i].child_count,
        };
        if (cb) {
            cb(&e, ctx);
        }
        emitted++;
    }
    *number_returned = emitted;
    free_scan(arr, n);
    ESP_LOGI(TAG, "scanned %s: %d entries", rel_dir, n);
    return true;
}

int content_index_count(const char *rel_dir)
{
    int num = 0, total = 0;
    if (!content_index_iterate(rel_dir, 0, 0, NULL, NULL, &num, &total)) {
        return -1;
    }
    return total;
}

bool content_index_list_all(const char *rel_dir, content_index_cb cb, void *ctx,
                            int *number_returned, int *total_matches)
{
    *number_returned = 0;
    *total_matches = 0;

    char full[512];
    if (!content_dir_full_path(rel_dir, full, sizeof(full))) {
        return false;
    }
    DIR *d = opendir(full);
    if (!d) {
        return false;
    }
    size_t cap = 16;
    int n = 0;
    scan_entry_t *arr = malloc(cap * sizeof(scan_entry_t));
    if (!arr) {
        closedir(d);
        return false;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (skip_name(de->d_name)) {
            continue;   /* dotfiles (incl. .part temps), .info, FS junk */
        }
        char child_full[600];
        snprintf(child_full, sizeof(child_full), "%s/%s", full, de->d_name);
        struct stat st;
        if (stat(child_full, &st) != 0) {
            continue;
        }
        if ((size_t)n == cap) {
            cap *= 2;
            scan_entry_t *ne = realloc(arr, cap * sizeof(scan_entry_t));
            if (!ne) {
                break;
            }
            arr = ne;
        }
        bool is_dir = S_ISDIR(st.st_mode);
        arr[n].name = strdup(de->d_name);
        arr[n].is_dir = is_dir;
        arr[n].size = is_dir ? -1 : (long)st.st_size;
        arr[n].child_count = -1;   /* not needed by file-manager listings */
        n++;
    }
    closedir(d);
    qsort(arr, n, sizeof(scan_entry_t), scan_cmp);

    for (int i = 0; i < n; i++) {
        content_index_entry_t e = {
            .name = arr[i].name, .is_dir = arr[i].is_dir,
            .size = arr[i].size, .child_count = arr[i].child_count,
        };
        if (cb) {
            cb(&e, ctx);
        }
    }
    *number_returned = n;
    *total_matches = n;
    free_scan(arr, n);
    return true;
}

static void rm_tree(const char *full)
{
    DIR *d = opendir(full);
    if (!d) {
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
            continue;
        }
        char child[600];
        snprintf(child, sizeof(child), "%s/%s", full, de->d_name);
        struct stat st;
        if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
            rm_tree(child);
        } else {
            unlink(child);
        }
    }
    closedir(d);
    rmdir(full);
}

void content_index_reset(void)
{
    char info[512];
    snprintf(info, sizeof(info), "%s/%s", SD_MOUNT_POINT, INFO_DIR);
    rm_tree(info);
    ESP_LOGI(TAG, "cleared %s — index rebuilds on demand", info);
}

void content_index_invalidate(const char *rel_dir)
{
    char path[600];
    if (index_file_path(rel_dir, path, sizeof(path))) {
        unlink(path);
    }
    char par[384];
    if (parent_rel(rel_dir, par, sizeof(par)) &&
        index_file_path(par, path, sizeof(path))) {
        unlink(path);
    }
}
