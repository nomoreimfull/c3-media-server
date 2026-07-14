#include "dlna_upnp.h"
#include "dlna_didl.h"
#include "content_dir.h"
#include "wifi.h"
#include "templates.h"
#include "sdkconfig.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

#include "esp_log.h"

static const char *TAG = "upnp";

#define SOAP_MAX_BODY 4096

static char s_server_base[48];   /* "http://192.168.4.1:80" */
static char s_location[64];      /* "http://192.168.4.1:80/desc.xml" */

const char *dlna_upnp_location(void)
{
    return s_location;
}

/* ---- small XML helpers for parsing the inbound SOAP body ---- */

/* Extract the text of <tag>...</tag> (namespace-unqualified) into out.
 * Returns true if found. */
static bool xml_tag_value(const char *body, const char *tag, char *out, size_t out_len)
{
    char open[64];
    snprintf(open, sizeof(open), "<%s", tag);
    const char *p = strstr(body, open);
    if (!p) {
        return false;
    }
    p = strchr(p, '>');
    if (!p) {
        return false;
    }
    p++;
    char close[64];
    snprintf(close, sizeof(close), "</%s>", tag);
    const char *e = strstr(p, close);
    if (!e) {
        return false;
    }
    size_t n = (size_t)(e - p);
    if (n >= out_len) {
        n = out_len - 1;
    }
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

/* Unescape XML entities in-place (&amp; &lt; &gt; &quot; &apos;). */
static void xml_unescape(char *s)
{
    char *w = s;
    for (char *r = s; *r;) {
        if (*r == '&') {
            if (strncmp(r, "&amp;", 5) == 0)  { *w++ = '&';  r += 5; continue; }
            if (strncmp(r, "&lt;", 4) == 0)   { *w++ = '<';  r += 4; continue; }
            if (strncmp(r, "&gt;", 4) == 0)   { *w++ = '>';  r += 4; continue; }
            if (strncmp(r, "&quot;", 6) == 0) { *w++ = '"';  r += 6; continue; }
            if (strncmp(r, "&apos;", 6) == 0) { *w++ = '\''; r += 6; continue; }
        }
        *w++ = *r++;
    }
    *w = '\0';
}

/* Return the action name from a SOAPAction header value like
 * "urn:...:ContentDirectory:1#Browse" -> "Browse". */
static void action_from_soapaction(const char *hdr, char *out, size_t out_len)
{
    const char *hash = strrchr(hdr, '#');
    const char *start = hash ? hash + 1 : hdr;
    size_t n = 0;
    while (start[n] && start[n] != '"' && n < out_len - 1) {
        out[n] = start[n];
        n++;
    }
    out[n] = '\0';
}

/* ---- static XML endpoints ---- */

static esp_err_t desc_handler(httpd_req_t *req)
{
    int need = snprintf(NULL, 0, TMPL_DEVICE_DESC,
                        CONFIG_DLNA_FRIENDLY_NAME, CONFIG_DLNA_UUID);
    char *xml = malloc(need + 1);
    if (!xml) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }
    snprintf(xml, need + 1, TMPL_DEVICE_DESC, CONFIG_DLNA_FRIENDLY_NAME, CONFIG_DLNA_UUID);
    httpd_resp_set_type(req, "text/xml; charset=\"utf-8\"");
    esp_err_t r = httpd_resp_send(req, xml, need);
    free(xml);
    return r;
}

static esp_err_t cd_scpd_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/xml; charset=\"utf-8\"");
    return httpd_resp_send(req, TMPL_CD_SCPD, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t cm_scpd_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/xml; charset=\"utf-8\"");
    return httpd_resp_send(req, TMPL_CM_SCPD, HTTPD_RESP_USE_STRLEN);
}

/* ---- SOAP responses ---- */

static esp_err_t send_soap(httpd_req_t *req, const char *body)
{
    static const char PRE[] =
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>";
    static const char POST[] = "</s:Body></s:Envelope>";

    size_t total = sizeof(PRE) - 1 + strlen(body) + sizeof(POST) - 1;
    char *out = malloc(total + 1);
    if (!out) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }
    char *w = out;
    memcpy(w, PRE, sizeof(PRE) - 1);        w += sizeof(PRE) - 1;
    memcpy(w, body, strlen(body));          w += strlen(body);
    memcpy(w, POST, sizeof(POST) - 1);      w += sizeof(POST) - 1;
    *w = '\0';

    httpd_resp_set_type(req, "text/xml; charset=\"utf-8\"");
    esp_err_t r = httpd_resp_send(req, out, total);
    free(out);
    return r;
}

static esp_err_t send_soap_fault(httpd_req_t *req, int code, const char *desc)
{
    char body[256];
    snprintf(body, sizeof(body),
             "<s:Fault><faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring>"
             "<detail><UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\">"
             "<errorCode>%d</errorCode><errorDescription>%s</errorDescription>"
             "</UPnPError></detail></s:Fault>", code, desc);
    httpd_resp_set_status(req, "500 Internal Server Error");
    return send_soap(req, body);
}

/* Read the SOAP request body into a heap buffer (caller frees). */
static char *read_body(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 0 || len > SOAP_MAX_BODY) {
        return NULL;
    }
    char *buf = malloc(len + 1);
    if (!buf) {
        return NULL;
    }
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, buf + got, len - got);
        if (r <= 0) {
            free(buf);
            return NULL;
        }
        got += r;
    }
    buf[got] = '\0';
    return buf;
}

/* ---- ContentDirectory Browse ---- */

static esp_err_t handle_browse(httpd_req_t *req, const char *body)
{
    char object_id[384] = "0";
    char browse_flag[32] = "BrowseDirectChildren";
    char idx_s[16] = "0";
    char cnt_s[16] = "0";

    xml_tag_value(body, "ObjectID", object_id, sizeof(object_id));
    xml_tag_value(body, "BrowseFlag", browse_flag, sizeof(browse_flag));
    xml_tag_value(body, "StartingIndex", idx_s, sizeof(idx_s));
    xml_tag_value(body, "RequestedCount", cnt_s, sizeof(cnt_s));
    xml_unescape(object_id);

    int starting_index = atoi(idx_s);
    int requested_count = atoi(cnt_s);

    char rel_path[384];
    if (!content_dir_objectid_to_relpath(object_id, rel_path, sizeof(rel_path)) ||
        !content_dir_path_is_safe(rel_path)) {
        return send_soap_fault(req, 701, "No such object");
    }

    char *didl;
    int number_returned = 0, total_matches = 0;
    if (strcmp(browse_flag, "BrowseMetadata") == 0) {
        didl = dlna_didl_metadata(rel_path, s_server_base);
        number_returned = didl ? 1 : 0;
        total_matches = didl ? 1 : 0;
    } else {
        didl = dlna_didl_children(rel_path, s_server_base, starting_index,
                                  requested_count, &number_returned, &total_matches);
    }
    if (!didl) {
        return send_soap_fault(req, 701, "No such object");
    }

    char *escaped = dlna_xml_escape(didl);
    free(didl);
    if (!escaped) {
        return send_soap_fault(req, 501, "Action failed");
    }

    size_t body_len = strlen(escaped) + 512;
    char *resp = malloc(body_len);
    if (!resp) {
        free(escaped);
        return send_soap_fault(req, 501, "Action failed");
    }
    snprintf(resp, body_len,
             "<u:BrowseResponse xmlns:u=\"urn:schemas-upnp-org:service:ContentDirectory:1\">"
             "<Result>%s</Result>"
             "<NumberReturned>%d</NumberReturned>"
             "<TotalMatches>%d</TotalMatches>"
             "<UpdateID>1</UpdateID>"
             "</u:BrowseResponse>",
             escaped, number_returned, total_matches);
    free(escaped);

    ESP_LOGI(TAG, "Browse %s '%s' -> %d/%d", browse_flag, rel_path,
             number_returned, total_matches);
    esp_err_t r = send_soap(req, resp);
    free(resp);
    return r;
}

static esp_err_t cd_control_handler(httpd_req_t *req)
{
    char action[48] = {0};
    char soapaction[128];
    if (httpd_req_get_hdr_value_str(req, "SOAPAction", soapaction, sizeof(soapaction)) == ESP_OK) {
        action_from_soapaction(soapaction, action, sizeof(action));
    }

    char *body = read_body(req);
    if (!body) {
        return send_soap_fault(req, 402, "Invalid Args");
    }

    /* Fall back to sniffing the body if the SOAPAction header was absent. */
    if (action[0] == '\0') {
        if (strstr(body, "Browse")) {
            strcpy(action, "Browse");
        }
    }

    esp_err_t r;
    if (strcmp(action, "Browse") == 0) {
        r = handle_browse(req, body);
    } else if (strcmp(action, "GetSearchCapabilities") == 0) {
        r = send_soap(req,
            "<u:GetSearchCapabilitiesResponse xmlns:u=\"urn:schemas-upnp-org:service:ContentDirectory:1\">"
            "<SearchCaps></SearchCaps></u:GetSearchCapabilitiesResponse>");
    } else if (strcmp(action, "GetSortCapabilities") == 0) {
        r = send_soap(req,
            "<u:GetSortCapabilitiesResponse xmlns:u=\"urn:schemas-upnp-org:service:ContentDirectory:1\">"
            "<SortCaps></SortCaps></u:GetSortCapabilitiesResponse>");
    } else if (strcmp(action, "GetSystemUpdateID") == 0) {
        r = send_soap(req,
            "<u:GetSystemUpdateIDResponse xmlns:u=\"urn:schemas-upnp-org:service:ContentDirectory:1\">"
            "<Id>1</Id></u:GetSystemUpdateIDResponse>");
    } else {
        r = send_soap_fault(req, 401, "Invalid Action");
    }
    free(body);
    return r;
}

static esp_err_t cm_control_handler(httpd_req_t *req)
{
    char action[48] = {0};
    char soapaction[128];
    if (httpd_req_get_hdr_value_str(req, "SOAPAction", soapaction, sizeof(soapaction)) == ESP_OK) {
        action_from_soapaction(soapaction, action, sizeof(action));
    }
    /* Body isn't needed for the actions we support, but drain it. */
    char *body = read_body(req);

    esp_err_t r;
    if (strcmp(action, "GetProtocolInfo") == 0) {
        char resp[512];
        snprintf(resp, sizeof(resp),
            "<u:GetProtocolInfoResponse xmlns:u=\"urn:schemas-upnp-org:service:ConnectionManager:1\">"
            "<Source>%s</Source><Sink></Sink></u:GetProtocolInfoResponse>",
            CM_SOURCE_PROTOCOLINFO);
        r = send_soap(req, resp);
    } else if (strcmp(action, "GetCurrentConnectionIDs") == 0) {
        r = send_soap(req,
            "<u:GetCurrentConnectionIDsResponse xmlns:u=\"urn:schemas-upnp-org:service:ConnectionManager:1\">"
            "<ConnectionIDs>0</ConnectionIDs></u:GetCurrentConnectionIDsResponse>");
    } else if (strcmp(action, "GetCurrentConnectionInfo") == 0) {
        r = send_soap(req,
            "<u:GetCurrentConnectionInfoResponse xmlns:u=\"urn:schemas-upnp-org:service:ConnectionManager:1\">"
            "<RcsID>-1</RcsID><AVTransportID>-1</AVTransportID><ProtocolInfo></ProtocolInfo>"
            "<PeerConnectionManager></PeerConnectionManager><PeerConnectionID>-1</PeerConnectionID>"
            "<Direction>Output</Direction><Status>OK</Status></u:GetCurrentConnectionInfoResponse>");
    } else {
        r = send_soap_fault(req, 401, "Invalid Action");
    }
    free(body);
    return r;
}

esp_err_t dlna_upnp_register(httpd_handle_t server)
{
    char ip[16] = "192.168.4.1";
    wifi_get_ip(ip, sizeof(ip));
    snprintf(s_server_base, sizeof(s_server_base), "http://%s:%d", ip, CONFIG_HTTP_PORT);
    snprintf(s_location, sizeof(s_location), "%s/desc.xml", s_server_base);
    ESP_LOGI(TAG, "server base = %s", s_server_base);

    const httpd_uri_t routes[] = {
        { .uri = "/desc.xml",    .method = HTTP_GET,  .handler = desc_handler },
        { .uri = "/cd_scpd.xml", .method = HTTP_GET,  .handler = cd_scpd_handler },
        { .uri = "/cm_scpd.xml", .method = HTTP_GET,  .handler = cm_scpd_handler },
        { .uri = "/cd_control",  .method = HTTP_POST, .handler = cd_control_handler },
        { .uri = "/cm_control",  .method = HTTP_POST, .handler = cm_control_handler },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %s failed: %s", routes[i].uri, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}
