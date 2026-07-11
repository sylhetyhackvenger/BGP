/*
 * bgp.c – Single-file BGP client with JSON output
 * Compile: gcc -o bgp bgp.c -ljansson -lpthread -lm
 */

/* ============================================================
   SYSTEM HEADERS
   ============================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>
#include <time.h>
#include <stdarg.h>
#include <ctype.h>
#include <assert.h>
#include <limits.h>
#include <getopt.h>
#include <signal.h>
#include <jansson.h>

#ifdef __linux__
#include <sys/timerfd.h>
#endif

/* ============================================================
   CONSTANTS & MACROS
   ============================================================ */
#define MAX_BGP_PEERS 256
#define TCP_BGP_PORT 179
#define BGP_HEADER_LEN 19
#define BGP_HEADER_MARKER_LEN 16
#define BGP_OPEN_HEADER_LEN 10
#define BGP_MAX_LEN 4096
#define BGP_MAX_MESSAGE_SIZE BGP_MAX_LEN   /* FIX: define here */
#define MSG_PAD 256
#define MAX_IPV4_ROUTE_STRING 20
#define MAX_IPV6_ROUTE_STRING 48
#define MAX_ATTRIBUTE 255
#define MAX_IPV4_STRING 16
#define MAX_PEER_NAME_LEN 64
#define SDS_LLSTR_SIZE 21
#define SDS_MAX_PREALLOC (1024*1024)

/* ============================================================
   LIST IMPLEMENTATION (kernel-style)
   ============================================================ */
struct list_head {
    struct list_head *next, *prev;
};

static inline void INIT_LIST_HEAD(struct list_head *list) {
    list->next = list;
    list->prev = list;
}

#define list_entry(ptr, type, member) \
    (type *)((char *)(ptr) - (char *) &((type *)0)->member)

#define list_first_entry(ptr, type, member) \
    list_entry((ptr)->next, type, member)

#define list_for_each(pos, head) \
    for (pos = (head)->next; pos != (head); pos = pos->next)

#define list_for_each_safe(pos, n, head) \
    for (pos = (head)->next, n = pos->next; pos != (head); pos = n, n = pos->next)

static inline void list_add_tail(struct list_head *new, struct list_head *head) {
    struct list_head *prev = head->prev;
    prev->next = new;
    new->prev = prev;
    new->next = head;
    head->prev = new;
}

static inline void list_del(struct list_head *entry) {
    entry->prev->next = entry->next;
    entry->next->prev = entry->prev;
}

static inline int list_empty(const struct list_head *head) {
    return head->next == head;
}

/* ============================================================
   SIMPLE DYNAMIC STRINGS (SDS) – minimal version
   ============================================================ */
typedef char *sds;

struct sdshdr {
    size_t len;
    size_t alloc;
    char buf[];
};

#define SDS_HDR(s) ((struct sdshdr *)((s) - sizeof(struct sdshdr)))
#define sdslen(s) (SDS_HDR(s)->len)
#define sdssetlen(s, l) (SDS_HDR(s)->len = (l))
#define sdsavail(s) (SDS_HDR(s)->alloc - SDS_HDR(s)->len)
#define sdssetalloc(s, a) (SDS_HDR(s)->alloc = (a))
#define sdsalloc(s) (SDS_HDR(s)->alloc)
#define sdsinclen(s, inc) (SDS_HDR(s)->len += (inc))

static sds sdsnew(const char *init) {
    size_t len = init ? strlen(init) : 0;
    struct sdshdr *h = malloc(sizeof(struct sdshdr) + len + 1);
    if (!h) return NULL;
    h->len = len;
    h->alloc = len;
    if (len) memcpy(h->buf, init, len);
    h->buf[len] = '\0';
    return h->buf;
}

static sds sdsempty(void) { return sdsnew(""); }

static sds sdsdup(const sds s) {
    return sdsnew(s);
}

static void sdsfree(sds s) {
    if (s) free(SDS_HDR(s));
}

static sds sdscatlen(sds s, const void *t, size_t len) {
    struct sdshdr *h = SDS_HDR(s);
    size_t newlen = h->len + len;
    if (newlen > h->alloc) {
        size_t newalloc = newlen * 2;
        h = realloc(h, sizeof(struct sdshdr) + newalloc + 1);
        if (!h) return NULL;
        h->alloc = newalloc;
    }
    memcpy(h->buf + h->len, t, len);
    h->len = newlen;
    h->buf[newlen] = '\0';
    return h->buf;
}

static sds sdscat(sds s, const char *t) {
    return sdscatlen(s, t, strlen(t));
}

static sds sdscpy(sds s, const char *t) {
    size_t len = strlen(t);
    struct sdshdr *h = SDS_HDR(s);
    if (len > h->alloc) {
        h = realloc(h, sizeof(struct sdshdr) + len + 1);
        if (!h) return NULL;
        h->alloc = len;
    }
    memcpy(h->buf, t, len);
    h->len = len;
    h->buf[len] = '\0';
    return h->buf;
}

static sds sdscatprintf(sds s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[1024];
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return sdscatlen(s, buf, len);
}

static sds sdsnewlen(const void *init, size_t len) {
    struct sdshdr *h = malloc(sizeof(struct sdshdr) + len + 1);
    if (!h) return NULL;
    h->len = len;
    h->alloc = len;
    if (len && init) memcpy(h->buf, init, len);
    h->buf[len] = '\0';
    return h->buf;
}

/* ---------- ADD MISSING SDS SPLIT FUNCTIONS ---------- */
sds *sdssplitlen(const char *s, ssize_t len, const char *sep, int seplen, int *count) {
    int elements = 0, slots = 5;
    long start = 0, j;
    sds *tokens;
    if (seplen < 1 || len <= 0) {
        *count = 0;
        return NULL;
    }
    tokens = malloc(sizeof(sds) * slots);
    if (!tokens) return NULL;
    for (j = 0; j < (len - (seplen - 1)); j++) {
        if (slots < elements + 2) {
            sds *newtokens = realloc(tokens, sizeof(sds) * slots * 2);
            if (!newtokens) goto cleanup;
            slots *= 2;
            tokens = newtokens;
        }
        if ((seplen == 1 && *(s + j) == sep[0]) || (memcmp(s + j, sep, seplen) == 0)) {
            tokens[elements] = sdsnewlen(s + start, j - start);
            if (!tokens[elements]) goto cleanup;
            elements++;
            start = j + seplen;
            j = j + seplen - 1;
        }
    }
    tokens[elements] = sdsnewlen(s + start, len - start);
    if (!tokens[elements]) goto cleanup;
    elements++;
    *count = elements;
    return tokens;
cleanup:
    for (int i = 0; i < elements; i++) sdsfree(tokens[i]);
    free(tokens);
    *count = 0;
    return NULL;
}

void sdsfreesplitres(sds *tokens, int count) {
    if (!tokens) return;
    while (count--) sdsfree(tokens[count]);
    free(tokens);
}

/* ============================================================
   BYTE CONVERSION UTILITIES
   ============================================================ */
static inline uint8_t uchar_to_uint8(unsigned char *b) { return b[0]; }
static inline uint8_t uchar_to_uint8_inc(unsigned char **b) { uint8_t v = **b; (*b)++; return v; }
static inline uint16_t uchar_be_to_uint16(unsigned char *b) { return (b[0]<<8) | b[1]; }
static inline uint16_t uchar_be_to_uint16_inc(unsigned char **b) { uint16_t v = uchar_be_to_uint16(*b); *b += 2; return v; }
static inline uint32_t uchar_be_to_uint32(unsigned char *b) { return (b[0]<<24)|(b[1]<<16)|(b[2]<<8)|b[3]; }
static inline uint32_t uchar_be_to_uint32_inc(unsigned char **b) { uint32_t v = uchar_be_to_uint32(*b); *b += 4; return v; }
static inline void uint8_to_uchar(unsigned char *b, uint8_t v) { b[0] = v; }
static inline void uint8_to_uchar_inc(unsigned char **b, uint8_t v) { **b = v; (*b)++; }
static inline void uint16_to_uchar_be(unsigned char *b, uint16_t v) { b[0] = v>>8; b[1] = v; }
static inline void uint16_to_uchar_be_inc(unsigned char **b, uint16_t v) { uint16_to_uchar_be(*b, v); *b += 2; }
static inline void uint32_to_uchar_be(unsigned char *b, uint32_t v) { b[0] = v>>24; b[1] = v>>16; b[2] = v>>8; b[3] = v; }
static inline void uint32_to_uchar_be_inc(unsigned char **b, uint32_t v) { uint32_to_uchar_be(*b, v); *b += 4; }

/* ============================================================
   LOGGING
   ============================================================ */
enum LOG_LEVEL { LOG_NONE, LOG_ERROR, LOG_WARN, LOG_INFO, LOG_DEBUG };
static enum LOG_LEVEL current_log_level = LOG_INFO;

static void set_log_level(enum LOG_LEVEL lvl) { if (lvl >= LOG_NONE && lvl <= LOG_DEBUG) current_log_level = lvl; }

static void log_print(enum LOG_LEVEL lvl, const char *fmt, ...) {
    if (lvl > current_log_level) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* ============================================================
   BGP CAPABILITY STRUCTURES (RFC 5492)
   ============================================================ */
enum bgp_capability_code {
    BGP_CAP_MP_EXT = 1,
    BGP_CAP_ROUTE_REFRESH = 2,
    BGP_CAP_FOUR_OCTET_ASN = 65,
    BGP_CAP_ADD_PATH = 69,
};
enum bgp_afi { BGP_AFI_IPV4 = 1, BGP_AFI_IPV6 = 2, BGP_AFI_L2VPN = 25 };
enum bgp_safi { BGP_SAFI_UNICAST = 1, BGP_SAFI_MPLS_VPN = 128, BGP_SAFI_EVPN = 70 };

struct bgp_capability {
    uint8_t code, length;
    uint8_t *value;
    struct list_head list;
};

struct bgp_capabilities {
    int count;
    size_t total_length;
    struct list_head caps;
};

struct bgp_addpath_config {
    uint8_t ipv4_unicast, ipv6_unicast, vpnv4, evpn;
};

/* ============================================================
   BGP TIMERS
   ============================================================ */
enum timer {
    ConnectRetryTimer, HoldTimer, KeepaliveTimer,
    MinASOriginationIntervalTimer, MinRouteAdvertisementIntervalTimer,
    DelayOpenTimer, IdleHoldTimer, N_LOCAL_TIMERS
};

struct bgp_local_timer {
    time_t duration_sec;
    int recurring;
#ifdef __linux__
    struct itimerspec timeout;
    int fd;
#else
    struct timespec expiry;
    int armed;
#endif
};

/* ============================================================
   BGP MESSAGE STRUCTURES (RFC 4271)
   ============================================================ */
enum bgp_msg_type { OPEN = 1, UPDATE, NOTIFICATION, KEEPALIVE, ROUTE_REFRESH };

struct bgp_open {
    uint8_t version;
    uint16_t asn, hold_time;
    uint32_t router_id;
    uint8_t opt_param_len;
    struct bgp_capabilities *capabilities;
};

struct path_segment {
    uint8_t type, n_as;
    uint32_t *as;
    struct list_head list;
};

struct as_path {
    int n_segments, n_total_as;
    struct list_head segments;
};

struct aggregator { uint32_t asn, ip; };
struct community { uint16_t n_communities; uint32_t *communities; };
struct large_community_value { uint32_t global_admin, local_data_1, local_data_2; };
struct large_community { uint16_t n_communities; struct large_community_value *communities; };

struct ipv4_nlri {
    uint32_t path_id;
    uint8_t length, bytes;
    uint8_t prefix[4];
    struct list_head list;
    char string[MAX_IPV4_ROUTE_STRING];
};

struct ipv6_nlri {
    uint32_t path_id;
    uint8_t length, bytes;
    uint8_t prefix[16];
    struct list_head list;
    char string[MAX_IPV6_ROUTE_STRING];
};

enum evpn_route_type {
    EVPN_ETH_AUTO_DISCOVERY = 1,
    EVPN_MAC_IP_ADV = 2,
    EVPN_INCLUSIVE_MCAST = 3,
    EVPN_ETH_SEGMENT = 4,
    EVPN_IP_PREFIX = 5,
};

struct evpn_nlri {
    uint32_t path_id;
    uint8_t route_type, route_length;
    uint16_t rd_type;
    uint8_t rd_value[6];
    uint8_t esi[10];
    uint32_t ethernet_tag;
    uint8_t mac_length, mac[6];
    uint8_t ip_length, ip[16];
    uint8_t gw_ip[16];
    uint8_t prefix_length;
    uint32_t mpls_label1, mpls_label2;
    struct list_head list;
};

struct vpnv4_nlri {
    uint32_t path_id;
    uint32_t mpls_label;
    uint16_t rd_type;
    uint8_t rd_value[6];
    uint8_t prefix_length, prefix[4];
    struct list_head list;
};

struct mp_reach_nlri {
    uint16_t afi;
    uint8_t safi, nh_length;
    uint8_t next_hop[32];
    char nh_string[40], nh_link_local_string[40];
    struct list_head nlri;
};

struct mp_unreach_nlri {
    uint16_t afi;
    uint8_t safi;
    struct list_head withdrawn;
};

enum bgp_update_attrs {
    ORIGIN = 1, AS_PATH, NEXT_HOP, MULTI_EXIT_DISC,
    LOCAL_PREF, ATOMIC_AGGREGATE, AGGREGATOR, COMMUNITY,
    MP_REACH_NLRI = 14, MP_UNREACH_NLRI = 15, LARGE_COMMUNITY = 32
};

struct bgp_path_attribute {
    uint8_t flags, type;
    uint16_t length;
    union {
        uint8_t origin;
        struct as_path *as_path;
        uint32_t next_hop;
        uint32_t multi_exit_disc;
        uint32_t local_pref;
        struct aggregator *aggregator;
        struct community *community;
        struct large_community *large_community;
        struct mp_reach_nlri *mp_reach;
        struct mp_unreach_nlri *mp_unreach;
    };
};

struct bgp_update {
    uint16_t withdrawn_route_length;
    struct list_head withdrawn_routes;
    uint16_t path_attr_length;
    struct bgp_path_attribute *path_attrs[MAX_ATTRIBUTE + 1];
    struct list_head nlri;
};

struct bgp_notification {
    uint8_t code, subcode;
    unsigned char *data;
};

struct bgp_msg {
    time_t recv_time;
    uint64_t id;
    char *peer_name;
    uint16_t length, body_length;
    uint8_t type;
    union {
        struct bgp_open open;
        struct bgp_update *update;
        struct bgp_notification notification;
    };
    int actioned;
    struct list_head ingress, output;
};

/* ============================================================
   BGP PEER & INSTANCE STRUCTURES
   ============================================================ */
enum bgp_fsm_states { IDLE, CONNECT, ACTIVE, OPENSENT, OPENCONFIRM, ESTABLISHED };

struct bgp_socket { int fd; struct sockaddr_in sock_addr; };

struct bgp_peer_timers {
    uint16_t conf_hold_time, recv_hold_time;
    uint16_t *curr_hold_time;
};

struct bgp_stats {
    int total, sent_total;
    int open[2], update[2], notification[2], keepalive[2], route_refresh[2];
};

enum bgp_output { BGP_OUT_JSON, BGP_OUT_JSONL, N_BGP_FORMATS };

struct output_queue;

struct bgp_peer {
    int active;
    sds name;
    unsigned int id;
    uint8_t *version;
    uint32_t *local_asn, peer_asn, *local_rid, peer_rid;
    int four_octet_asn;
    struct bgp_addpath_config addpath;
    sds peer_ip, source_ip;
    pthread_t thread;
    struct bgp_peer_timers peer_timers;
    struct bgp_local_timer local_timers[N_LOCAL_TIMERS];
    enum bgp_fsm_states fsm_state;
    unsigned int connect_retry_counter;
    int reconnect_enabled, reconnect_max_retries;
    uint16_t reconnect_backoff_current, reconnect_backoff_max;
    uint8_t last_notification_code, last_notification_subcode;
    struct bgp_socket socket;
    struct bgp_stats stats;
    pthread_mutex_t stdout_lock;
    int (*print_msg)(struct bgp_peer *, struct bgp_msg *);
    enum bgp_output output_format;
    struct output_queue *output_queue;
    struct list_head ingress_q, output_q;
};

struct bgp_instance {
    uint8_t version;
    uint32_t local_asn, local_rid;
    int n_peers;
    struct bgp_peer *peers[MAX_BGP_PEERS];
    struct output_queue *output_queue;
};

/* ============================================================
   OUTPUT QUEUE (thread-safe)
   ============================================================ */
struct output_item {
    char *json_str;
    struct list_head list;
};

struct output_queue {
    struct list_head items;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int shutdown;
    pthread_t writer_thread;
};

static int output_queue_init(struct output_queue *q) {
    INIT_LIST_HEAD(&q->items);
    q->shutdown = 0;
    if (pthread_mutex_init(&q->lock, NULL)) return -1;
    if (pthread_cond_init(&q->cond, NULL)) { pthread_mutex_destroy(&q->lock); return -1; }
    return 0;
}

static void output_queue_destroy(struct output_queue *q) {
    struct list_head *i, *tmp;
    struct output_item *item;
    list_for_each_safe(i, tmp, &q->items) {
        item = list_entry(i, struct output_item, list);
        list_del(i);
        free(item->json_str);
        free(item);
    }
    pthread_cond_destroy(&q->cond);
    pthread_mutex_destroy(&q->lock);
}

static void output_queue_push(struct output_queue *q, char *json_str) {
    if (!json_str) return;
    struct output_item *item = malloc(sizeof(*item));
    if (!item) { free(json_str); return; }
    item->json_str = json_str;
    pthread_mutex_lock(&q->lock);
    list_add_tail(&item->list, &q->items);
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->lock);
}

static void output_queue_shutdown(struct output_queue *q) {
    pthread_mutex_lock(&q->lock);
    q->shutdown = 1;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->lock);
}

static void *output_writer_thread(void *arg) {
    struct output_queue *q = arg;
    while (1) {
        pthread_mutex_lock(&q->lock);
        while (list_empty(&q->items) && !q->shutdown)
            pthread_cond_wait(&q->cond, &q->lock);
        if (q->shutdown && list_empty(&q->items)) {
            pthread_mutex_unlock(&q->lock);
            break;
        }
        struct list_head *first = q->items.next;
        struct output_item *item = list_entry(first, struct output_item, list);
        list_del(first);
        pthread_mutex_unlock(&q->lock);
        printf("%s\n", item->json_str);
        fflush(stdout);
        free(item->json_str);
        free(item);
    }
    return NULL;
}

static int output_queue_start(struct output_queue *q) {
    return pthread_create(&q->writer_thread, NULL, output_writer_thread, q);
}

static void output_queue_join(struct output_queue *q) {
    pthread_join(q->writer_thread, NULL);
}

/* ============================================================
   TCP CONNECTION HELPER
   ============================================================ */
static int tcp_connect(sds host, const char *port, sds source) {
    int sock = -1, ret;
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if ((ret = getaddrinfo(host, port, &hints, &res)) != 0) {
        log_print(LOG_ERROR, "getaddrinfo: %s\n", gai_strerror(ret));
        return -1;
    }
    for (rp = res; rp; rp = rp->ai_next) {
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock < 0) continue;
        if (sdslen(source) > 0) {
            struct addrinfo shints, *sres;
            memset(&shints, 0, sizeof(shints));
            shints.ai_family = rp->ai_family;
            shints.ai_socktype = rp->ai_socktype;
            shints.ai_protocol = rp->ai_protocol;
            shints.ai_flags = AI_PASSIVE;
            if (getaddrinfo(source, NULL, &shints, &sres) == 0) {
                bind(sock, sres->ai_addr, sres->ai_addrlen);
                freeaddrinfo(sres);
            }
        }
        if (connect(sock, rp->ai_addr, rp->ai_addrlen) == 0)
            break;
        close(sock);
        sock = -1;
    }
    freeaddrinfo(res);
    return sock;
}

/* ============================================================
   BGP CAPABILITY FUNCTIONS
   ============================================================ */
static struct bgp_capabilities *bgp_capabilities_create(void) {
    struct bgp_capabilities *caps = calloc(1, sizeof(*caps));
    if (caps) INIT_LIST_HEAD(&caps->caps);
    return caps;
}

static void bgp_capabilities_free(struct bgp_capabilities *caps) {
    if (!caps) return;
    struct list_head *pos, *tmp;
    struct bgp_capability *cap;
    list_for_each_safe(pos, tmp, &caps->caps) {
        cap = list_entry(pos, struct bgp_capability, list);
        list_del(pos);
        free(cap->value);
        free(cap);
    }
    free(caps);
}

static int bgp_capabilities_add(struct bgp_capabilities *caps, uint8_t code, uint8_t len, const uint8_t *val) {
    if (!caps) return -1;
    struct bgp_capability *cap = calloc(1, sizeof(*cap));
    if (!cap) return -1;
    cap->code = code;
    cap->length = len;
    if (len > 0 && val) {
        cap->value = malloc(len);
        if (!cap->value) { free(cap); return -1; }
        memcpy(cap->value, val, len);
    }
    list_add_tail(&cap->list, &caps->caps);
    caps->count++;
    caps->total_length += 2 + len;
    return 0;
}

static int bgp_capabilities_add_route_refresh(struct bgp_capabilities *caps) {
    return bgp_capabilities_add(caps, BGP_CAP_ROUTE_REFRESH, 0, NULL);
}

static int bgp_capabilities_add_four_octet_asn(struct bgp_capabilities *caps, uint32_t asn) {
    uint8_t val[4];
    uint32_to_uchar_be(val, asn);
    return bgp_capabilities_add(caps, BGP_CAP_FOUR_OCTET_ASN, 4, val);
}

static int bgp_capabilities_add_mp_ext(struct bgp_capabilities *caps, uint16_t afi, uint8_t safi) {
    uint8_t val[4];
    uint16_to_uchar_be(val, afi);
    val[2] = 0;
    val[3] = safi;
    return bgp_capabilities_add(caps, BGP_CAP_MP_EXT, 4, val);
}

static int bgp_capabilities_encode(const struct bgp_capabilities *caps, unsigned char *buf, size_t size) {
    if (!caps || !buf) return -1;
    if (caps->count == 0) return 0;
    if (size < caps->total_length + 2) return -1;
    unsigned char *ptr = buf;
    *ptr++ = 2; /* parameter type = capabilities */
    *ptr++ = (uint8_t)caps->total_length;
    struct list_head *pos;
    struct bgp_capability *cap;
    list_for_each(pos, &caps->caps) {
        cap = list_entry(pos, struct bgp_capability, list);
        *ptr++ = cap->code;
        *ptr++ = cap->length;
        if (cap->length && cap->value) {
            memcpy(ptr, cap->value, cap->length);
            ptr += cap->length;
        }
    }
    return (int)(ptr - buf);
}

static struct bgp_capabilities *bgp_capabilities_parse(const unsigned char *opt, uint8_t len) {
    struct bgp_capabilities *caps = bgp_capabilities_create();
    if (!caps) return NULL;
    const unsigned char *end = opt + len;
    while (opt < end) {
        if (opt + 2 > end) break;
        uint8_t type = *opt++;
        uint8_t plen = *opt++;
        if (opt + plen > end) break;
        if (type == 2) { /* capabilities */
            const unsigned char *cend = opt + plen;
            while (opt < cend) {
                if (opt + 2 > cend) break;
                uint8_t code = *opt++;
                uint8_t clen = *opt++;
                if (opt + clen > cend) break;
                bgp_capabilities_add(caps, code, clen, clen ? opt : NULL);
                opt += clen;
            }
        } else {
            opt += plen;
        }
    }
    return caps;
}

static const char *bgp_capability_name(uint8_t code) {
    switch (code) {
        case BGP_CAP_MP_EXT: return "Multiprotocol Extensions";
        case BGP_CAP_ROUTE_REFRESH: return "Route Refresh";
        case BGP_CAP_FOUR_OCTET_ASN: return "4-octet ASN";
        case BGP_CAP_ADD_PATH: return "ADD-PATH";
        default: return "Unknown";
    }
}

static int bgp_capabilities_has_four_octet_asn(const struct bgp_capabilities *caps, uint32_t *asn) {
    if (!caps) return 0;
    struct list_head *pos;
    struct bgp_capability *cap;
    list_for_each(pos, &caps->caps) {
        cap = list_entry(pos, struct bgp_capability, list);
        if (cap->code == BGP_CAP_FOUR_OCTET_ASN && cap->length == 4 && cap->value) {
            if (asn) *asn = uchar_be_to_uint32(cap->value);
            return 1;
        }
    }
    return 0;
}

/* ============================================================
   BGP MESSAGE PARSING & SENDING
   ============================================================ */
static void create_header(uint16_t length, uint8_t type, unsigned char *buf) {
    const unsigned char marker[16] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
                                       0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    memcpy(buf, marker, 16);
    unsigned char *p = buf + 16;
    uint16_to_uchar_be_inc(&p, length);
    uint8_to_uchar(p, type);
}

static int validate_header(unsigned char *header, struct bgp_msg *msg) {
    const unsigned char marker[16] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
                                       0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    if (memcmp(header, marker, 16)) return -1;
    unsigned char *p = header + 16;
    uint16_t len = uchar_be_to_uint16_inc(&p);
    if (len < BGP_HEADER_LEN) return -1;
    uint8_t type = *p++;
    if (type == 0 || type > ROUTE_REFRESH) return -1;
    msg->type = type;
    msg->length = len;
    msg->body_length = len - BGP_HEADER_LEN;
    return 0;
}

static struct bgp_msg *alloc_sent_msg(void) {
    struct bgp_msg *m = calloc(1, sizeof(*m));
    if (m) {
        INIT_LIST_HEAD(&m->ingress);
        INIT_LIST_HEAD(&m->output);
        m->actioned = 1;
        m->recv_time = time(NULL);
    }
    return m;
}

static int free_msg(struct bgp_msg *msg) {
    if (!msg) return 0;
    if (msg->type == OPEN) bgp_capabilities_free(msg->open.capabilities);
    if (msg->type == UPDATE && msg->update) {
        // simplified free – in practice we'd free all NLRI and attributes
        free(msg->update);
    }
    free(msg);
    return 0;
}

static struct bgp_msg *recv_msg(struct bgp_peer *peer) {
    struct bgp_msg *msg = calloc(1, sizeof(*msg));
    if (!msg) return NULL;
    INIT_LIST_HEAD(&msg->ingress);
    INIT_LIST_HEAD(&msg->output);
    msg->actioned = 0;
    msg->recv_time = time(NULL);

    unsigned char header[BGP_HEADER_LEN];
    ssize_t ret = recv(peer->socket.fd, header, sizeof(header), MSG_WAITALL);
    if (ret <= 0) { free(msg); return NULL; }
    if (validate_header(header, msg) < 0) { free(msg); return NULL; }

    unsigned char *body = NULL;
    if (msg->body_length > 0) {
        body = malloc(msg->body_length);
        if (!body) { free(msg); return NULL; }
        ret = recv(peer->socket.fd, body, msg->body_length, MSG_WAITALL);
        if (ret <= 0) { free(body); free(msg); return NULL; }
    }

    int r = 0;
    switch (msg->type) {
        case OPEN: {
            unsigned char *p = body;
            msg->open.version = uchar_to_uint8_inc(&p);
            msg->open.asn = uchar_be_to_uint16_inc(&p);
            msg->open.hold_time = uchar_be_to_uint16_inc(&p);
            msg->open.router_id = uchar_be_to_uint32_inc(&p);
            msg->open.opt_param_len = uchar_to_uint8_inc(&p);
            msg->open.capabilities = bgp_capabilities_parse(p, msg->open.opt_param_len);
            break;
        }
        case NOTIFICATION: {
            unsigned char *p = body;
            msg->notification.code = uchar_to_uint8_inc(&p);
            msg->notification.subcode = uchar_to_uint8_inc(&p);
            break;
        }
        case KEEPALIVE:
            break;
        case UPDATE:
            // simplified: just store raw body; actual parsing would be more complex
            msg->update = calloc(1, sizeof(*msg->update));
            if (msg->update) {
                INIT_LIST_HEAD(&msg->update->withdrawn_routes);
                INIT_LIST_HEAD(&msg->update->nlri);
                // not fully parsing, just storing for demo
            }
            break;
        default:
            r = -1;
    }
    if (body) free(body);
    if (r < 0) { free_msg(msg); return NULL; }
    return msg;
}

static ssize_t send_open(int fd, uint8_t version, uint16_t asn, uint16_t hold_time,
                         uint32_t router_id, const struct bgp_capabilities *caps) {
    unsigned char buf[BGP_MAX_MESSAGE_SIZE];
    unsigned char *p = buf + BGP_HEADER_LEN;
    uint8_to_uchar_inc(&p, version);
    uint16_to_uchar_be_inc(&p, asn);
    uint16_to_uchar_be_inc(&p, hold_time);
    uint32_to_uchar_be_inc(&p, router_id);
    unsigned char *opt_len_pos = p;
    p++;
    int enc_len = 0;
    if (caps && caps->count > 0) {
        enc_len = bgp_capabilities_encode(caps, p, BGP_MAX_MESSAGE_SIZE - (p - buf));
        if (enc_len > 0) { p += enc_len; }
    }
    uint8_to_uchar(opt_len_pos, (uint8_t)enc_len);
    uint16_t total = (uint16_t)(p - buf);
    create_header(total, OPEN, buf);
    return send(fd, buf, total, 0);
}

static ssize_t send_keepalive(int fd) {
    unsigned char buf[BGP_HEADER_LEN];
    create_header(BGP_HEADER_LEN, KEEPALIVE, buf);
    return send(fd, buf, BGP_HEADER_LEN, 0);
}

static ssize_t send_notification(int fd, uint8_t code, uint8_t subcode) {
    unsigned char buf[BGP_HEADER_LEN + 2];
    create_header(BGP_HEADER_LEN + 2, NOTIFICATION, buf);
    unsigned char *p = buf + BGP_HEADER_LEN;
    uint8_to_uchar_inc(&p, code);
    uint8_to_uchar_inc(&p, subcode);
    return send(fd, buf, BGP_HEADER_LEN + 2, 0);
}

/* ============================================================
   TIMER IMPLEMENTATION (Linux timerfd)
   ============================================================ */
#ifdef __linux__
static int initialise_local_timers(struct bgp_local_timer *timers) {
    for (int i = 0; i < N_LOCAL_TIMERS; i++) {
        timers[i].duration_sec = (i == ConnectRetryTimer) ? 120 :
                                 (i == HoldTimer) ? 30 :
                                 (i == KeepaliveTimer) ? 15 : 0;
        timers[i].recurring = 0;
        timers[i].fd = timerfd_create(CLOCK_BOOTTIME, 0);
        if (timers[i].fd < 0) return -1;
        timers[i].timeout.it_value.tv_sec = timers[i].duration_sec;
        timers[i].timeout.it_value.tv_nsec = 0;
        timers[i].timeout.it_interval.tv_sec = 0;
        timers[i].timeout.it_interval.tv_nsec = 0;
    }
    return 0;
}
static int start_timer(struct bgp_local_timer *timers, enum timer id) {
    if (id < 0 || id >= N_LOCAL_TIMERS) return -1;
    struct bgp_local_timer *t = &timers[id];
    t->recurring = 0;
    t->timeout.it_interval.tv_sec = 0;
    t->timeout.it_interval.tv_nsec = 0;
    return timerfd_settime(t->fd, 0, &t->timeout, NULL);
}
static int start_timer_recurring(struct bgp_local_timer *timers, enum timer id) {
    if (id < 0 || id >= N_LOCAL_TIMERS) return -1;
    struct bgp_local_timer *t = &timers[id];
    t->recurring = 1;
    t->timeout.it_interval.tv_sec = t->timeout.it_value.tv_sec;
    return timerfd_settime(t->fd, 0, &t->timeout, NULL);
}
static int disarm_timer(struct bgp_local_timer *timers, enum timer id) {
    if (id < 0 || id >= N_LOCAL_TIMERS) return -1;
    struct itimerspec dis = {0};
    return timerfd_settime(timers[id].fd, 0, &dis, NULL);
}
static void set_timer_value(struct bgp_local_timer *timers, enum timer id, time_t sec) {
    if (id < 0 || id >= N_LOCAL_TIMERS) return;
    timers[id].duration_sec = sec;
    timers[id].timeout.it_value.tv_sec = sec;
    timers[id].timeout.it_value.tv_nsec = 0;
    timers[id].timeout.it_interval.tv_sec = 0;
    timers[id].timeout.it_interval.tv_nsec = 0;
}
static uint64_t timer_has_fired(struct bgp_local_timer *timers, enum timer id, fd_set *set) {
    if (id < 0 || id >= N_LOCAL_TIMERS) return 0;
    int fd = timers[id].fd;
    if (FD_ISSET(fd, set)) {
        uint64_t n;
        if (read(fd, &n, sizeof(n)) > 0) return n;
    }
    return 0;
}
#else
/* Portable fallback – simplified, not used in production */
static int initialise_local_timers(struct bgp_local_timer *timers) {
    for (int i = 0; i < N_LOCAL_TIMERS; i++) {
        timers[i].duration_sec = 0;
        timers[i].armed = 0;
        timers[i].expiry.tv_sec = 0;
        timers[i].expiry.tv_nsec = 0;
    }
    return 0;
}
static int start_timer(struct bgp_local_timer *timers, enum timer id) { return 0; }
static int start_timer_recurring(struct bgp_local_timer *timers, enum timer id) { return 0; }
static int disarm_timer(struct bgp_local_timer *timers, enum timer id) { return 0; }
static void set_timer_value(struct bgp_local_timer *timers, enum timer id, time_t sec) {}
static uint64_t timer_has_fired(struct bgp_local_timer *timers, enum timer id, fd_set *set) { return 0; }
#endif

/* ============================================================
   BGP PEER FSM FUNCTIONS
   ============================================================ */
static void bgp_close_socket(struct bgp_peer *peer) {
    if (peer->socket.fd >= 0) close(peer->socket.fd);
    peer->socket.fd = -1;
    peer->fsm_state = IDLE;
}

static int get_read_fd_set(struct bgp_peer *peer, fd_set *set) {
    FD_ZERO(set);
    int max = 0;
#ifdef __linux__
    for (int i = 0; i < N_LOCAL_TIMERS; i++) {
        int fd = peer->local_timers[i].fd;
        FD_SET(fd, set);
        if (fd > max) max = fd;
    }
#endif
    if (peer->socket.fd > 0) {
        FD_SET(peer->socket.fd, set);
        if (peer->socket.fd > max) max = peer->socket.fd;
    }
    return max;
}

static int check_hold_timer_expired(struct bgp_peer *peer, fd_set *set, const char *state) {
    if (!timer_has_fired(peer->local_timers, HoldTimer, set)) return 0;
    log_print(LOG_WARN, "HoldTimer expired in %s for %s\n", state, peer->name);
    send_notification(peer->socket.fd, 4, 0); // Hold Timer Expired
    bgp_close_socket(peer);
    peer->fsm_state = IDLE;
    peer->last_notification_code = 4;
    peer->last_notification_subcode = 0;
    return 1;
}

static struct bgp_msg *pop_ingress_queue(struct bgp_peer *peer) {
    if (list_empty(&peer->ingress_q)) return NULL;
    struct list_head *first = peer->ingress_q.next;
    struct bgp_msg *msg = list_entry(first, struct bgp_msg, ingress);
    list_del(first);
    msg->actioned = 1;
    return msg;
}

static int classify_failure(uint8_t code, uint8_t subcode) {
    if (code == 0 || code == 4 || code == 5) return 1;
    if (code == 6 && (subcode == 2 || subcode == 4 || subcode == 5 || subcode == 6 || subcode == 7 || subcode == 8))
        return 1;
    return 0;
}

static void calculate_next_backoff(struct bgp_peer *peer) {
    uint16_t next = peer->reconnect_backoff_current * 2;
    if (next > peer->reconnect_backoff_max) next = peer->reconnect_backoff_max;
    peer->reconnect_backoff_current = next;
}

static void reset_backoff(struct bgp_peer *peer) {
    peer->reconnect_backoff_current = 5;
}

static int setup_reconnect_timer(struct bgp_peer *peer) {
    set_timer_value(peer->local_timers, IdleHoldTimer, peer->reconnect_backoff_current);
    return start_timer(peer->local_timers, IdleHoldTimer);
}

static ssize_t queue_and_send_open(struct bgp_peer *peer, uint8_t ver, uint16_t asn,
                                   uint16_t hold, uint32_t rid, struct bgp_capabilities *caps) {
    struct bgp_msg *msg = alloc_sent_msg();
    if (msg) {
        msg->id = --peer->stats.sent_total;
        msg->peer_name = peer->name;
        msg->type = OPEN;
        msg->open.version = ver;
        msg->open.asn = asn;
        msg->open.hold_time = hold;
        msg->open.router_id = rid;
        msg->open.capabilities = caps;
        msg->open.opt_param_len = caps ? (uint8_t)(caps->total_length + 2) : 0;
        list_add_tail(&msg->output, &peer->output_q);
    }
    return send_open(peer->socket.fd, ver, asn, hold, rid, caps);
}

static ssize_t queue_and_send_keepalive(struct bgp_peer *peer) {
    struct bgp_msg *msg = alloc_sent_msg();
    if (msg) {
        msg->id = --peer->stats.sent_total;
        msg->peer_name = peer->name;
        msg->type = KEEPALIVE;
        list_add_tail(&msg->output, &peer->output_q);
    }
    return send_keepalive(peer->socket.fd);
}

static ssize_t queue_and_send_notification(struct bgp_peer *peer, uint8_t code, uint8_t subcode) {
    struct bgp_msg *msg = alloc_sent_msg();
    if (msg) {
        msg->id = --peer->stats.sent_total;
        msg->peer_name = peer->name;
        msg->type = NOTIFICATION;
        msg->notification.code = code;
        msg->notification.subcode = subcode;
        list_add_tail(&msg->output, &peer->output_q);
    }
    return send_notification(peer->socket.fd, code, subcode);
}

/* FSM state handlers */
static int fsm_state_idle(struct bgp_peer *peer, fd_set *set) {
    start_timer(peer->local_timers, ConnectRetryTimer);
    log_print(LOG_INFO, "Opening connection to %s (AS %u)\n", peer->peer_ip, peer->peer_asn);
    peer->socket.fd = tcp_connect(peer->peer_ip, "179", peer->source_ip);
    if (peer->socket.fd < 0) {
        bgp_close_socket(peer);
        peer->last_notification_code = 0;
        peer->last_notification_subcode = 0;
        return -1;
    }
    disarm_timer(peer->local_timers, ConnectRetryTimer);
    peer->fsm_state = CONNECT;
    return 0;
}

static int fsm_state_connect(struct bgp_peer *peer) {
    struct bgp_capabilities *caps = bgp_capabilities_create();
    if (caps) {
        bgp_capabilities_add_route_refresh(caps);
        bgp_capabilities_add_mp_ext(caps, BGP_AFI_IPV4, BGP_SAFI_UNICAST);
        bgp_capabilities_add_mp_ext(caps, BGP_AFI_IPV6, BGP_SAFI_UNICAST);
        bgp_capabilities_add_four_octet_asn(caps, *peer->local_asn);
    }
    uint16_t open_asn = (*peer->local_asn > 65535) ? 23456 : (uint16_t)*peer->local_asn;
    queue_and_send_open(peer, *peer->version, open_asn,
                        peer->peer_timers.conf_hold_time, *peer->local_rid, caps);
    start_timer(peer->local_timers, HoldTimer);
    peer->fsm_state = OPENSENT;
    return 0;
}

static int fsm_state_active(struct bgp_peer *peer) { return 0; }

static int fsm_state_opensent(struct bgp_peer *peer, struct bgp_msg *msg, fd_set *set) {
    struct bgp_msg *m = pop_ingress_queue(peer);
    if (m) {
        if (m->type == OPEN) {
            uint32_t real_asn = m->open.asn;
            uint32_t cap_asn = 0;
            int has4 = bgp_capabilities_has_four_octet_asn(m->open.capabilities, &cap_asn);
            if (has4) { real_asn = cap_asn; peer->four_octet_asn = 1; }
            if (real_asn != peer->peer_asn) {
                queue_and_send_notification(peer, 2, 2); // Bad Peer AS
                bgp_close_socket(peer);
                peer->last_notification_code = 2; peer->last_notification_subcode = 2;
                return -1;
            }
            if (m->open.version != *peer->version) {
                queue_and_send_notification(peer, 2, 1);
                bgp_close_socket(peer);
                peer->last_notification_code = 2; peer->last_notification_subcode = 1;
                return -1;
            }
            if (m->open.hold_time != 0 && m->open.hold_time < 3) {
                queue_and_send_notification(peer, 2, 6);
                bgp_close_socket(peer);
                peer->last_notification_code = 2; peer->last_notification_subcode = 6;
                return -1;
            }
            peer->peer_rid = m->open.router_id;
            uint16_t neg_hold = 0;
            if (peer->peer_timers.conf_hold_time && m->open.hold_time) {
                neg_hold = (m->open.hold_time < peer->peer_timers.conf_hold_time) ?
                           m->open.hold_time : peer->peer_timers.conf_hold_time;
            }
            if (neg_hold > 0) {
                set_timer_value(peer->local_timers, HoldTimer, neg_hold);
                set_timer_value(peer->local_timers, KeepaliveTimer, neg_hold / 3);
            }
            queue_and_send_keepalive(peer);
            if (neg_hold > 0) start_timer_recurring(peer->local_timers, KeepaliveTimer);
            peer->fsm_state = OPENCONFIRM;
            return 0;
        } else if (m->type == NOTIFICATION) {
            peer->last_notification_code = m->notification.code;
            peer->last_notification_subcode = m->notification.subcode;
            bgp_close_socket(peer);
            return -1;
        }
    }
    if (check_hold_timer_expired(peer, set, "OPENSENT")) return -1;
    return 0;
}

static int fsm_state_openconfirm(struct bgp_peer *peer, struct bgp_msg *msg, fd_set *set) {
    if (timer_has_fired(peer->local_timers, KeepaliveTimer, set))
        queue_and_send_keepalive(peer);
    struct bgp_msg *m = pop_ingress_queue(peer);
    if (m) {
        if (m->type == KEEPALIVE) {
            start_timer(peer->local_timers, HoldTimer);
            peer->fsm_state = ESTABLISHED;
            log_print(LOG_INFO, "Session established with %s\n", peer->name);
            reset_backoff(peer);
        } else if (m->type == NOTIFICATION) {
            peer->last_notification_code = m->notification.code;
            peer->last_notification_subcode = m->notification.subcode;
            bgp_close_socket(peer);
            return -1;
        }
    }
    if (check_hold_timer_expired(peer, set, "OPENCONFIRM")) return -1;
    return 0;
}

static int fsm_state_established(struct bgp_peer *peer, struct bgp_msg *msg, fd_set *set) {
    if (timer_has_fired(peer->local_timers, KeepaliveTimer, set))
        queue_and_send_keepalive(peer);
    struct bgp_msg *m = pop_ingress_queue(peer);
    if (m) {
        switch (m->type) {
            case KEEPALIVE:
                start_timer(peer->local_timers, HoldTimer);
                break;
            case NOTIFICATION:
                peer->last_notification_code = m->notification.code;
                peer->last_notification_subcode = m->notification.subcode;
                bgp_close_socket(peer);
                return -1;
            case UPDATE:
                start_timer(peer->local_timers, HoldTimer);
                break;
        }
    }
    if (check_hold_timer_expired(peer, set, "ESTABLISHED")) return -1;
    return 0;
}

/* ============================================================
   BGP PEER THREAD
   ============================================================ */
static void msg_queue_gc(struct list_head *q) {
    struct list_head *pos, *tmp;
    struct bgp_msg *m;
    list_for_each_safe(pos, tmp, q) {
        m = list_entry(pos, struct bgp_msg, output);
        list_del(pos);
        free_msg(m);
    }
}

static void print_bgp_msg_and_gc(struct bgp_peer *peer) {
    struct list_head *pos, *tmp;
    struct bgp_msg *m;
    list_for_each_safe(pos, tmp, &peer->output_q) {
        m = list_entry(pos, struct bgp_msg, output);
        if (!m->actioned) break;
        char *json_str = NULL;
        // simplified JSON printing – just dump ID and type
        json_str = malloc(128);
        if (json_str) {
            snprintf(json_str, 128, "{\"id\":%ld,\"type\":%d}", (long)m->id, m->type);
            output_queue_push(peer->output_queue, json_str);
        }
        list_del(pos);
        free_msg(m);
    }
}

static void *bgp_peer_thread(void *param) {
    struct bgp_peer *peer = param;
    fd_set *set = calloc(1, sizeof(*set));
    if (!set) return NULL;
    int waiting_for_reconnect = 0;
    int was_established = 0;

    while (peer->active) {
        struct timeval tv = {1, 0};
        int maxfd = get_read_fd_set(peer, set);
        int n = select(maxfd + 1, set, NULL, NULL, &tv);
        if (waiting_for_reconnect) {
            if (timer_has_fired(peer->local_timers, IdleHoldTimer, set)) {
                waiting_for_reconnect = 0;
                peer->fsm_state = IDLE;
                calculate_next_backoff(peer);
            }
            continue;
        }
        if (n > 0 && peer->socket.fd >= 0 && FD_ISSET(peer->socket.fd, set)) {
            struct bgp_msg *m = recv_msg(peer);
            if (!m) {
                bgp_close_socket(peer);
                peer->last_notification_code = 0;
                peer->last_notification_subcode = 0;
                goto handle_failure;
            }
            m->peer_name = peer->name;
            m->id = peer->stats.total++;
            list_add_tail(&m->ingress, &peer->ingress_q);
            list_add_tail(&m->output, &peer->output_q);
        }
        int ret = 0;
        switch (peer->fsm_state) {
            case IDLE: ret = fsm_state_idle(peer, set); break;
            case CONNECT: ret = fsm_state_connect(peer); break;
            case ACTIVE: ret = fsm_state_active(peer); break;
            case OPENSENT: ret = fsm_state_opensent(peer, NULL, set); break;
            case OPENCONFIRM: ret = fsm_state_openconfirm(peer, NULL, set); break;
            case ESTABLISHED: ret = fsm_state_established(peer, NULL, set); break;
        }
        if (ret < 0) goto handle_failure;
        if (peer->fsm_state == ESTABLISHED && !was_established) {
            was_established = 1;
            if (peer->connect_retry_counter > 0) {
                reset_backoff(peer);
                peer->connect_retry_counter = 0;
            }
        } else if (peer->fsm_state != ESTABLISHED) {
            was_established = 0;
        }
        print_bgp_msg_and_gc(peer);
        continue;

    handle_failure:
        print_bgp_msg_and_gc(peer);
        if (peer->reconnect_enabled && classify_failure(peer->last_notification_code, peer->last_notification_subcode)) {
            if (peer->reconnect_max_retries == 0 || peer->connect_retry_counter < (unsigned)peer->reconnect_max_retries) {
                peer->connect_retry_counter++;
                log_print(LOG_INFO, "Reconnecting %s in %ds (attempt %d)\n",
                          peer->name, peer->reconnect_backoff_current, peer->connect_retry_counter);
                setup_reconnect_timer(peer);
                waiting_for_reconnect = 1;
                was_established = 0;
                continue;
            }
        }
        goto exit_thread;
    }

    if (peer->socket.fd >= 0) {
        queue_and_send_notification(peer, 6, 2); // Admin Shutdown
        print_bgp_msg_and_gc(peer);
        bgp_close_socket(peer);
    }

exit_thread:
    if (waiting_for_reconnect) disarm_timer(peer->local_timers, IdleHoldTimer);
    free(set);
    msg_queue_gc(&peer->output_q);
    log_print(LOG_INFO, "Peer %s thread exiting\n", peer->name);
    return NULL;
}

/* ============================================================
   BGP INSTANCE MANAGEMENT
   ============================================================ */
static struct bgp_instance *create_bgp_instance(uint32_t asn, uint32_t rid, uint8_t ver) {
    struct bgp_instance *i = calloc(1, sizeof(*i));
    if (!i) return NULL;
    i->version = ver;
    i->local_asn = asn;
    i->local_rid = rid;
    i->n_peers = 0;
    i->output_queue = malloc(sizeof(*i->output_queue));
    if (!i->output_queue) { free(i); return NULL; }
    if (output_queue_init(i->output_queue) < 0) { free(i->output_queue); free(i); return NULL; }
    if (output_queue_start(i->output_queue) < 0) { output_queue_destroy(i->output_queue); free(i->output_queue); free(i); return NULL; }
    return i;
}

static void free_bgp_instance(struct bgp_instance *i) {
    if (!i) return;
    if (i->output_queue) {
        output_queue_shutdown(i->output_queue);
        output_queue_join(i->output_queue);
        output_queue_destroy(i->output_queue);
        free(i->output_queue);
    }
    free(i);
}

static int max_peer_id(struct bgp_instance *i) {
    return (int)(sizeof(i->peers)/sizeof(i->peers[0])) - 1;
}

static struct bgp_peer *get_peer_from_instance(struct bgp_instance *i, unsigned int id) {
    if (id > (unsigned)max_peer_id(i)) return NULL;
    return i->peers[id];
}

static int create_bgp_peer(struct bgp_instance *i, const char *ip, uint32_t asn, const char *name) {
    unsigned int id;
    for (id = 0; id <= (unsigned)max_peer_id(i); id++) {
        if (i->peers[id] == NULL) break;
    }
    if (id > (unsigned)max_peer_id(i)) return -1;
    struct bgp_peer *p = calloc(1, sizeof(*p));
    if (!p) return -1;
    p->id = id;
    i->peers[id] = p;
    i->n_peers++;
    p->fsm_state = IDLE;
    p->peer_ip = sdsnew(ip);
    p->source_ip = sdsempty();
    p->name = sdsnew(name);
    p->version = &i->version;
    p->local_asn = &i->local_asn;
    p->local_rid = &i->local_rid;
    p->peer_timers.conf_hold_time = 30;
    p->peer_timers.recv_hold_time = 0;
    p->peer_timers.curr_hold_time = &p->peer_timers.conf_hold_time;
    p->connect_retry_counter = 0;
    p->reconnect_enabled = 0;
    p->reconnect_max_retries = 0;
    p->reconnect_backoff_current = 5;
    p->reconnect_backoff_max = 120;
    p->last_notification_code = 0;
    p->last_notification_subcode = 0;
    memset(&p->stats, 0, sizeof(p->stats));
    if (initialise_local_timers(p->local_timers) < 0) { free(p); return -1; }
    p->socket.fd = -1;
    p->peer_asn = asn;
    p->four_octet_asn = 0;
    pthread_mutex_init(&p->stdout_lock, NULL);
    p->output_format = BGP_OUT_JSON;
    p->output_queue = i->output_queue;
    INIT_LIST_HEAD(&p->ingress_q);
    INIT_LIST_HEAD(&p->output_q);
    return (int)id;
}

static void free_bgp_peer(struct bgp_instance *i, unsigned int id) {
    struct bgp_peer *p = get_peer_from_instance(i, id);
    if (!p) return;
    sdsfree(p->peer_ip);
    sdsfree(p->source_ip);
    sdsfree(p->name);
    pthread_mutex_destroy(&p->stdout_lock);
    free(p);
    i->peers[id] = NULL;
    i->n_peers--;
}

static void free_all_bgp_peers(struct bgp_instance *i) {
    for (int id = 0; id <= max_peer_id(i); id++)
        free_bgp_peer(i, id);
}

static int activate_bgp_peer(struct bgp_instance *i, unsigned int id) {
    struct bgp_peer *p = get_peer_from_instance(i, id);
    if (!p) return -1;
    p->active = 1;
    if (pthread_create(&p->thread, NULL, bgp_peer_thread, p) != 0) return -1;
    return 0;
}

static int deactivate_bgp_peer(struct bgp_instance *i, unsigned int id) {
    struct bgp_peer *p = get_peer_from_instance(i, id);
    if (!p) return -1;
    p->active = 0;
    pthread_join(p->thread, NULL);
    return 0;
}

static int deactivate_all_bgp_peers(struct bgp_instance *i) {
    int ret = 0;
    for (int id = 0; id <= max_peer_id(i); id++) {
        if (i->peers[id]) ret |= deactivate_bgp_peer(i, id);
    }
    return ret;
}

static int set_bgp_output(struct bgp_instance *i, unsigned int id, enum bgp_output fmt) {
    struct bgp_peer *p = get_peer_from_instance(i, id);
    if (!p) return -1;
    p->output_format = fmt;
    return 0;
}

static int set_bgp_reconnect(struct bgp_instance *i, unsigned int id, int en, int maxr) {
    struct bgp_peer *p = get_peer_from_instance(i, id);
    if (!p) return -1;
    p->reconnect_enabled = en;
    p->reconnect_max_retries = maxr;
    return 0;
}

static int set_bgp_hold_time(struct bgp_instance *i, unsigned int id, uint16_t ht) {
    struct bgp_peer *p = get_peer_from_instance(i, id);
    if (!p) return -1;
    p->peer_timers.conf_hold_time = ht;
    return 0;
}

static int bgp_peer_source(struct bgp_instance *i, unsigned int id, const char *src) {
    struct bgp_peer *p = get_peer_from_instance(i, id);
    if (!p) return -1;
    if (sdslen(p->source_ip)) log_print(LOG_WARN, "Source IP already set for %s\n", p->name);
    sdsfree(p->source_ip);
    p->source_ip = sdsnew(src);
    return 0;
}

/* ============================================================
   BANNER & HELP
   ============================================================ */
static void print_banner(void) {
    const char *banner =
    "\033[1;32m"  // light green
    "______ / /\n"
    " / /##\\\n"
    " / /####\\\n"
    " / /######\\\n"
    " / /########\\\n"
    " / /##########\\\n"
    " / /#####/\\#####\\\n"
    " / /#####/++\\#####\\\n"
    " / /#####/++++\\#####\\\n"
    " / /#####/\\+++++\\#####\\\n"
    " / /#####/ \\+++++\\#####\\\n"
    " / /#####/ \\+++++\\#####\\\n"
    " / /#####/ \\+++++\\#####\\\n"
    " / /#####/ \\+++++\\#####\\\n"
    " / /#####/__________\\+++++\\#####\\\n"
    " / \\+++++\\#####\\\n"
    " /__________________________\\+++++\\####/\n"
    " \\+++++++++++++++++++++++++++++++++\\##/\n"
    " \\+++++++++++++++++++++++++++++++++\\/\n"
    " ``````````````````````````````````\n"
    "\033[0m";
    printf("%s", banner);
    printf("\033[1;32mBGP v2.0 - BGP Peer CLI Tool\033[0m\n");
    printf("Author: SYLHETYHACKVENGER (THE-ERROR808)\n\n");
}

static void print_help(void) {
    printf(
    "Usage: bgp [options...] <peer> [<peer> ...]\n"
    "-s, --source <ip>        IP to source BGP connection from\n"
    "-a, --asn <asn>          Local ASN (supports 4-byte ASNs). Default: 65000\n"
    "-r, --rid <ip>           Local router ID. Default: 1.1.1.1\n"
    "-l, --logging <level>    Logging level (0-4). Default: 3 (Info)\n"
    "-f, --format <fmt>       Output format: 'json' or 'jsonl'. Default: json\n"
    "-R, --reconnect          Enable automatic reconnection with backoff\n"
    "-m, --max-retries <n>    Max reconnection attempts (0 = infinite)\n"
    "-t, --hold-time <sec>    Hold time in seconds. Default: 600\n"
    "-h, --help               Show this help\n"
    "\n<peer> formats: <ip>,<asn> or <ip>,<asn>,<name>\n"
    );
}

/* ============================================================
   MAIN
   ============================================================ */
struct cmdline_opts {
    int log_level;
    enum bgp_output format;
    sds source_ip;
    uint32_t local_asn, local_rid;
    int reconnect_enabled, reconnect_max_retries;
    uint16_t hold_time;
};

static struct cmdline_opts parse_cmdline(int argc, char **argv) {
    struct cmdline_opts opts = {
        .log_level = LOG_INFO,
        .format = BGP_OUT_JSON,
        .source_ip = sdsempty(),
        .local_asn = 65000,
        .local_rid = 0x01010101,
        .reconnect_enabled = 0,
        .reconnect_max_retries = 0,
        .hold_time = 600
    };
    int c, idx = 0;
    static struct option longopts[] = {
        {"source", required_argument, 0, 's'},
        {"asn", required_argument, 0, 'a'},
        {"rid", required_argument, 0, 'r'},
        {"logging", required_argument, 0, 'l'},
        {"format", required_argument, 0, 'f'},
        {"reconnect", no_argument, 0, 'R'},
        {"max-retries", required_argument, 0, 'm'},
        {"hold-time", required_argument, 0, 't'},
        {"help", no_argument, 0, 'h'},
        {0,0,0,0}
    };
    while ((c = getopt_long(argc, argv, "s:a:r:l:f:Rm:t:h", longopts, &idx)) != -1) {
        switch (c) {
            case 's': opts.source_ip = sdscpy(opts.source_ip, optarg); break;
            case 'a': opts.local_asn = (uint32_t)strtoul(optarg, NULL, 10); break;
            case 'r': { struct in_addr addr; if (inet_pton(AF_INET, optarg, &addr)==1) opts.local_rid = ntohl(addr.s_addr); } break;
            case 'l': opts.log_level = atoi(optarg); break;
            case 'f': if (strcmp(optarg, "jsonl")==0) opts.format = BGP_OUT_JSONL; else opts.format = BGP_OUT_JSON; break;
            case 'R': opts.reconnect_enabled = 1; break;
            case 'm': opts.reconnect_max_retries = atoi(optarg); break;
            case 't': opts.hold_time = (uint16_t)strtoul(optarg, NULL, 10); break;
            case 'h': print_help(); exit(0);
        }
    }
    return opts;
}

int main(int argc, char **argv) {
    print_banner();
    struct cmdline_opts opts = parse_cmdline(argc, argv);
    set_log_level(opts.log_level);

    if (optind >= argc) {
        log_print(LOG_ERROR, "No peers specified.\n");
        print_help();
        return 1;
    }

    struct bgp_instance *bgp = create_bgp_instance(opts.local_asn, opts.local_rid, 4);
    if (!bgp) { log_print(LOG_ERROR, "Failed to create BGP instance\n"); return 1; }

    int peer_ids[MAX_BGP_PEERS] = {0};
    for (int i = optind; i < argc; i++) {
        sds peer_arg = sdsnew(argv[i]);
        int n;
        sds *tokens = sdssplitlen(peer_arg, sdslen(peer_arg), ",", 1, &n);
        if (n < 2 || n > 3) {
            log_print(LOG_ERROR, "Invalid peer format: %s\n", argv[i]);
            continue;
        }
        uint32_t asn = (uint32_t)strtoul(tokens[1], NULL, 10);
        sds name = (n == 3) ? sdsdup(tokens[2]) : sdsnew("BGP_Peer");
        int id = create_bgp_peer(bgp, tokens[0], asn, name);
        if (id >= 0) {
            bgp_peer_source(bgp, id, opts.source_ip);
            set_bgp_output(bgp, id, opts.format);
            set_bgp_reconnect(bgp, id, opts.reconnect_enabled, opts.reconnect_max_retries);
            set_bgp_hold_time(bgp, id, opts.hold_time);
            peer_ids[id] = 1;
        }
        sdsfree(peer_arg);
        sdsfree(name);
        sdsfreesplitres(tokens, n);
    }
    sdsfree(opts.source_ip);

    for (int id = 0; id < MAX_BGP_PEERS; id++) {
        if (peer_ids[id]) activate_bgp_peer(bgp, id);
    }

    log_print(LOG_INFO, "Press Ctrl+D to exit\n");
    char buf[32];
    while (read(0, buf, sizeof(buf)) > 0);

    deactivate_all_bgp_peers(bgp);
    free_all_bgp_peers(bgp);
    free_bgp_instance(bgp);
    return 0;
}
