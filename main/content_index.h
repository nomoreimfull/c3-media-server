#pragma once

#include <stdbool.h>
#include <stddef.h>

/*
 * Lazy, SD-backed directory index. For each folder browsed, a small per-folder
 * index file is written under /sdcard/.info/<folder>/index.xml (the tree is
 * mirrored under .info/). The first listing of a folder scans it once (the only
 * time we open subdirectories to count children); later listings read the cached
 * XML. Nothing is held in RAM between requests.
 *
 * Both DLNA Browse (dlna_didl.c) and WebDAV PROPFIND (webdav.c) list through this.
 * Relative dirs are SD-rooted, always starting '/', e.g. "/" or "/Movies".
 */

typedef struct {
    const char *name;   /* child basename (valid only during the callback) */
    bool is_dir;
    long size;          /* file size in bytes, or -1 for directories */
    int child_count;    /* directory child count, or -1 for files */
} content_index_entry_t;

/* Per-child callback. Return value is ignored (reserved for early-stop). */
typedef bool (*content_index_cb)(const content_index_entry_t *e, void *ctx);

/*
 * List the children of rel_dir with paging [start, start+count) (count <= 0 means
 * "all from start"). Ensures the index exists (scans + caches on first use),
 * invokes cb for each child in the page (cb may be NULL to just count), and fills
 * *number_returned (emitted in page) and *total_matches (all children).
 * Returns false only if rel_dir is not a readable directory.
 */
bool content_index_iterate(const char *rel_dir, int start, int count,
                           content_index_cb cb, void *ctx,
                           int *number_returned, int *total_matches);

/* Total child count of rel_dir (builds/reads the index). -1 if not a directory. */
int content_index_count(const char *rel_dir);

/* Drop the cached index for rel_dir and its parent (whose childCount changed).
 * Call after a WebDAV mutation so the next listing rebuilds. */
void content_index_invalidate(const char *rel_dir);
