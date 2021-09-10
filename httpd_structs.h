#include "driver.h"

#if HTTP_ENABLE

#ifndef LWIP_HTTPD_STRUCTS_H
#define LWIP_HTTPD_STRUCTS_H

#include "lwip/init.h"
#include "lwip/apps/httpd.h"
#include "lwip/altcp.h"
#include "lwip/altcp_tcp.h"
#include "lwip/apps/fs.h"
#if HTTPD_ENABLE_HTTPS
#include "lwip/altcp_tls.h"
#endif

#if LWIP_HTTPD_DYNAMIC_HEADERS

/* The number of individual strings that comprise the headers sent before each requested file. */
#define NUM_FILE_HDR_STRINGS 5
#define HDR_STRINGS_IDX_HTTP_STATUS           0 /* e.g. "HTTP/1.0 200 OK\r\n" */
#define HDR_STRINGS_IDX_SERVER_NAME           1 /* e.g. "Server: "HTTPD_SERVER_AGENT"\r\n" */
#define HDR_STRINGS_IDX_CONTENT_LEN_KEEPALIVE 2 /* e.g. "Content-Length: xy\r\n" and/or "Connection: keep-alive\r\n" */
#define HDR_STRINGS_IDX_CONTENT_LEN_NR        3 /* the byte count, when content-length is used */
#define HDR_STRINGS_IDX_CONTENT_TYPE          4 /* the content type (or default answer content type including default document) */

/* The dynamically generated Content-Length buffer needs space for CRLF + NULL */
#define LWIP_HTTPD_MAX_CONTENT_LEN_OFFSET 3
#ifndef LWIP_HTTPD_MAX_CONTENT_LEN_SIZE
/* The dynamically generated Content-Length buffer shall be able to work with ~953 MB (9 digits) */
#define LWIP_HTTPD_MAX_CONTENT_LEN_SIZE   (9 + LWIP_HTTPD_MAX_CONTENT_LEN_OFFSET)
#endif

/** This struct is used for a list of HTTP header strings for various filename extensions. */
typedef struct {
  const char *extension;
  const char *content_type;
} tHTTPHeader;


#define HTTP_CONTENT_TYPE(contenttype) "Content-Type: "contenttype"\r\n\r\n"
#define HTTP_CONTENT_TYPE_ENCODING(contenttype, encoding) "Content-Type: "contenttype"\r\nContent-Encoding: "encoding"\r\n\r\n"

#define HTTP_HDR_HTML           HTTP_CONTENT_TYPE("text/html")
#define HTTP_HDR_SSI            HTTP_CONTENT_TYPE("text/html\r\nExpires: Fri, 10 Apr 2008 14:00:00 GMT\r\nPragma: no-cache")
#define HTTP_HDR_GIF            HTTP_CONTENT_TYPE("image/gif")
#define HTTP_HDR_PNG            HTTP_CONTENT_TYPE("image/png")
#define HTTP_HDR_JPG            HTTP_CONTENT_TYPE("image/jpeg")
#define HTTP_HDR_BMP            HTTP_CONTENT_TYPE("image/bmp")
#define HTTP_HDR_ICO            HTTP_CONTENT_TYPE("image/x-icon")
#define HTTP_HDR_APP            HTTP_CONTENT_TYPE("application/octet-stream")
#define HTTP_HDR_JS             HTTP_CONTENT_TYPE("application/javascript")
#define HTTP_HDR_RA             HTTP_CONTENT_TYPE("application/javascript")
#define HTTP_HDR_CSS            HTTP_CONTENT_TYPE("text/css")
#define HTTP_HDR_SWF            HTTP_CONTENT_TYPE("application/x-shockwave-flash")
#define HTTP_HDR_XML            HTTP_CONTENT_TYPE("text/xml")
#define HTTP_HDR_PDF            HTTP_CONTENT_TYPE("application/pdf")
#define HTTP_HDR_JSON           HTTP_CONTENT_TYPE("application/json")
#define HTTP_HDR_CSV            HTTP_CONTENT_TYPE("text/csv")
#define HTTP_HDR_TSV            HTTP_CONTENT_TYPE("text/tsv")
#define HTTP_HDR_SVG            HTTP_CONTENT_TYPE("image/svg+xml")
#define HTTP_HDR_GZIP           HTTP_CONTENT_TYPE("application/gzip")
#define HTTP_HDR_SVGZ           HTTP_CONTENT_TYPE_ENCODING("image/svg+xml", "gzip")

#define HTTP_HDR_DEFAULT_TYPE   HTTP_CONTENT_TYPE("text/plain")

/** A list of extension-to-HTTP header strings (see outdated RFC 1700 MEDIA TYPES
 * and http://www.iana.org/assignments/media-types for registered content types
 * and subtypes) */
static const tHTTPHeader g_psHTTPHeaders[] = {
  { "html", HTTP_HDR_HTML},
  { "htm",  HTTP_HDR_HTML},
  { "shtml", HTTP_HDR_SSI},
  { "shtm", HTTP_HDR_SSI},
  { "ssi",  HTTP_HDR_SSI},
  { "gif",  HTTP_HDR_GIF},
  { "png",  HTTP_HDR_PNG},
  { "jpg",  HTTP_HDR_JPG},
  { "bmp",  HTTP_HDR_BMP},
  { "ico",  HTTP_HDR_ICO},
  { "class", HTTP_HDR_APP},
  { "cls",  HTTP_HDR_APP},
  { "js",   HTTP_HDR_JS},
  { "ram",  HTTP_HDR_RA},
  { "css",  HTTP_HDR_CSS},
  { "swf",  HTTP_HDR_SWF},
  { "xml",  HTTP_HDR_XML},
  { "xsl",  HTTP_HDR_XML},
  { "pdf",  HTTP_HDR_PDF},
  { "json", HTTP_HDR_JSON}
#ifdef HTTPD_ADDITIONAL_CONTENT_TYPES
  /* If you need to add content types not listed here:
   * #define HTTPD_ADDITIONAL_CONTENT_TYPES {"ct1", HTTP_CONTENT_TYPE("text/ct1")}, {"exe", HTTP_CONTENT_TYPE("application/exe")}
   */
  , HTTPD_ADDITIONAL_CONTENT_TYPES
#endif
};

#define NUM_HTTP_HEADERS LWIP_ARRAYSIZE(g_psHTTPHeaders)

#endif /* LWIP_HTTPD_DYNAMIC_HEADERS */

typedef enum {
    HTTP_Get = 0,
    HTTP_Post,
    HTTP_Delete,
    HTTP_Options
} http_method_t;

#define HTTP_METHODS "GET,POST,DELETE,OPTIONS"

typedef struct http_state {
#if LWIP_HTTPD_KILL_OLD_ON_CONNECTIONS_EXCEEDED
    struct http_state *next;
#endif /* LWIP_HTTPD_KILL_OLD_ON_CONNECTIONS_EXCEEDED */
    struct fs_file file_handle;
    struct fs_file *handle;
    const char *file;       /* Pointer to first unsent byte in buf. */
    const char *uri;       /* Pointer to uri. */
    http_method_t method;
    struct altcp_pcb *pcb;
    u32_t left;       /* Number of unsent bytes in buf. */
    u8_t retries;
    uint_fast8_t param_count;
    char *params[LWIP_HTTPD_MAX_CGI_PARAMETERS]; /* Params extracted from the request URI */
    char *param_vals[LWIP_HTTPD_MAX_CGI_PARAMETERS]; /* Values for each extracted param */
#if LWIP_HTTPD_SUPPORT_REQUESTLIST
    struct pbuf *req;
#endif /* LWIP_HTTPD_SUPPORT_REQUESTLIST */
#if LWIP_HTTPD_DYNAMIC_FILE_READ
    char *buf;        /* File read buffer. */
    int buf_len;      /* Size of file read buffer, buf. */
#endif /* LWIP_HTTPD_DYNAMIC_FILE_READ */
#if LWIP_HTTPD_SUPPORT_11_KEEPALIVE
    u8_t keepalive;
#endif /* LWIP_HTTPD_SUPPORT_11_KEEPALIVE */
#if LWIP_HTTPD_DYNAMIC_HEADERS
    const char *hdrs[NUM_FILE_HDR_STRINGS]; /* HTTP headers to be sent. */
    char hdr_content_len[LWIP_HTTPD_MAX_CONTENT_LEN_SIZE];
    u16_t hdr_pos;     /* The position of the first unsent header byte in the current string */
    u16_t hdr_index;   /* The index of the hdr string currently being sent. */
#endif /* LWIP_HTTPD_DYNAMIC_HEADERS */
#if LWIP_HTTPD_TIMING
    u32_t time_started;
#endif /* LWIP_HTTPD_TIMING */
#if LWIP_HTTPD_SUPPORT_POST
    u32_t post_content_len_left;
  #if LWIP_HTTPD_POST_MANUAL_WND
    u32_t unrecved_bytes;
    u8_t no_auto_wnd;
    u8_t post_finished;
  #endif /* LWIP_HTTPD_POST_MANUAL_WND */
#endif /* LWIP_HTTPD_SUPPORT_POST*/
} http_state_t;

typedef http_state_t http_request_t;

typedef const char *(*uri_handler_fn)(http_request_t *request);

typedef struct {
    const char *uri;
    http_method_t method;
    uri_handler_fn handler;
    void *private_data;
} httpd_uri_handler_t;

char *http_get_key_value (http_request_t *request, char *key, char *value, uint32_t size);

void httpd_register_uri_handlers(const httpd_uri_handler_t *httpd_uri_handlers, uint_fast8_t httpd_num_uri_handlers);

#endif /* LWIP_HTTPD_STRUCTS_H */

#endif // HTTP_ENABLE
