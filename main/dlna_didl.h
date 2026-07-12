#pragma once

/*
 * DIDL-Lite generation for the UPnP ContentDirectory Browse action, plus the
 * DLNA.ORG content-features string shared with the HTTP streamer.
 *
 * Object IDs are relative SD paths ("0" == root); see content_dir.h.
 * server_base is the absolute origin for <res> URLs, e.g. "http://192.168.4.1:80".
 *
 * The returned strings are heap-allocated DIDL-Lite XML that the caller must
 * free(). They are NOT yet XML-escaped for SOAP embedding — use dlna_xml_escape()
 * on the whole document before placing it in the BrowseResponse <Result>.
 */

/* BrowseDirectChildren: list the children of a directory with paging.
 * requested_count == 0 means "all remaining". Fills *number_returned and
 * *total_matches. Returns NULL on error (e.g. not a directory). */
char *dlna_didl_children(const char *dir_rel_path, const char *server_base,
                         int starting_index, int requested_count,
                         int *number_returned, int *total_matches);

/* BrowseMetadata: describe a single object (container or item). */
char *dlna_didl_metadata(const char *rel_path, const char *server_base);

/* XML-escape src (& < > " ') into a newly-allocated string. Caller frees. */
char *dlna_xml_escape(const char *src);

/* The "DLNA.ORG_OP=01;DLNA.ORG_CI=0;DLNA.ORG_FLAGS=..." feature string for a MIME
 * type. Advertises byte-range seek (OP=01) and direct/native content (CI=0).
 * Returns a static string. */
const char *dlna_content_features(const char *mime);
