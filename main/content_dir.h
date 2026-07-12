#pragma once

#include <stdbool.h>
#include <stddef.h>

/*
 * Helpers shared by the HTTP streamer and the DLNA ContentDirectory. Files live
 * under SD_MOUNT_POINT (see sdcard.h). Paths handled here are "relative" paths
 * rooted at the SD card, always starting with '/', e.g. "/Movies/foo.mp4".
 *
 * DLNA object IDs: the root container is "0"; every other object ID is simply its
 * relative path. This keeps browsing completely stateless — whatever ID we hand
 * Roku in a Browse result comes straight back to us in the next Browse request.
 */

/* Look up a MIME type from a filename's extension. Returns a static string;
 * falls back to "application/octet-stream" for unknown types. */
const char *content_dir_mime(const char *name);

/* True if the filename has a video extension Roku can direct-play (mp4/mkv/mov/...). */
bool content_dir_is_video(const char *name);

/*
 * Percent-decode src into dst (dst_len includes the NUL). Decodes %XX and '+'.
 * Returns true on success, false if the output would overflow.
 */
bool content_dir_url_decode(const char *src, char *dst, size_t dst_len);

/*
 * Reject a relative path that tries to escape the SD root. Returns true if the
 * path is safe: it must start with '/' and contain no ".." component.
 */
bool content_dir_path_is_safe(const char *rel_path);

/*
 * Build the absolute VFS path (SD_MOUNT_POINT + rel_path) into out. The caller
 * must have already validated rel_path with content_dir_path_is_safe().
 * Returns true on success, false on overflow.
 */
bool content_dir_full_path(const char *rel_path, char *out, size_t out_len);

/*
 * Translate a DLNA object ID into a relative path. "0" (or "") maps to "/".
 * Any other ID is treated as the relative path itself. Returns true on success.
 */
bool content_dir_objectid_to_relpath(const char *object_id, char *out, size_t out_len);
