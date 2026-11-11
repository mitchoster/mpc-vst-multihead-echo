/* companion.cpp — the HTTP listener behind the companion page.
 *
 * Threading: one listener thread for the whole process, started by the first attach().
 * The audio thread only ever stores into the static slots (a seqlock, so a reader never
 * sees half an update); the listener only ever loads from them. No instance pointer ever
 * crosses into the listener, which is what keeps destroy_instance() free of teardown
 * ordering problems.
 *
 * Network: read-only. GET/HEAD only, no cookies, no state, no parameters written back —
 * the page watches the tape, it does not play it.
 */
#include "companion.h"

#include <atomic>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include "companion_page.h"
#include "qrcodegen.h"

namespace mhec {
namespace {

unsigned now_ms();

/* The address a phone should open. Cached for 10 s: /state is polled at 7 Hz and
 * getifaddrs() allocates, so walking the interface list 250 times a minute on the HTTP
 * thread is waste. The cache also survives a DHCP change within ten seconds. */
void local_ip(char *out, size_t cap) {
  static char cached[16] = "";
  static unsigned cached_at = 0;
  if (cached[0]) {
    const unsigned now = now_ms();
    if (now - cached_at < 10000u) { std::snprintf(out, cap, "%s", cached); return; }
    cached_at = now;
  } else {
    cached_at = now_ms();
  }
  out[0] = 0;
  struct ifaddrs *ifap = nullptr;
  if (getifaddrs(&ifap) == 0) {
    for (struct ifaddrs *ifa = ifap; ifa; ifa = ifa->ifa_next) {
      if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
      if (!(ifa->ifa_flags & IFF_UP)) continue;
      struct sockaddr_in *a4 = (struct sockaddr_in *)ifa->ifa_addr;
      if (a4->sin_addr.s_addr == htonl(INADDR_LOOPBACK)) continue;
      inet_ntop(AF_INET, &a4->sin_addr, out, cap);
      break;
    }
    freeifaddrs(ifap);
  }
  if (!out[0]) std::snprintf(out, cap, "127.0.0.1");
  std::snprintf(cached, sizeof cached, "%s", out);
}

/* "add to home screen" parts, so the panel opens like an app (VADER does the same). */
const char kManifest[] =
    "{\"name\":\"MPC Multihead Echo\",\"short_name\":\"Multihead\",\"start_url\":\"/\","
    "\"display\":\"standalone\",\"orientation\":\"any\",\"background_color\":\"#080a07\","
    "\"theme_color\":\"#080a07\",\"icons\":[{\"src\":\"/icon.svg\",\"sizes\":\"any\","
    "\"type\":\"image/svg+xml\"}]}";
const char kIconSvg[] =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 64 64\">"
    "<rect width=\"64\" height=\"64\" rx=\"13\" fill=\"#0b0d09\"/>"
    "<circle cx=\"32\" cy=\"32\" r=\"23\" fill=\"#14170f\" stroke=\"#c9a227\" stroke-width=\"3\"/>"
    "<circle cx=\"32\" cy=\"32\" r=\"15\" fill=\"none\" stroke=\"#6b4f2c\" stroke-width=\"7\"/>"
    "<circle cx=\"32\" cy=\"32\" r=\"4\" fill=\"#c9a227\"/>"
    "<g stroke=\"#2fbf63\" stroke-width=\"4\" stroke-linecap=\"round\">"
    "<path d=\"M32 23v-6\"/><path d=\"M40 37l5 3\"/><path d=\"M24 37l-5 3\"/></g></svg>";

const int kDefaultPort = 8099;
const int kPortTries   = 10;
/* A GET request line is < 200 bytes; the browser sends a few hundred more of headers.
 * 2 KB is generous and keeps a slow or hostile client from growing the stack. */
const int kReqMax = 2048;

struct Slot {
  /* seqlock: even = stable, odd = being written. Fields are plain relaxed stores. */
  std::atomic<unsigned> seq;
  std::atomic<unsigned> attached;
  std::atomic<unsigned> stamp_ms;
  std::atomic<float> wet, delay_ms, bpm, rate, wow_pct, jog, warp, twist;
  std::atomic<float> echo, wow, chorus, saturation, dropouts, hiss, spread;
  std::atomic<float> mix, intensity, reverb;
  std::atomic<int> mode, fb, sync, drift, reverb_head;
  std::atomic<int> tapeage, preamp, wetonly, motion, twisttype, warptype, revmode;
  std::atomic<int> heads[3];
};

Slot g_slots[kSlots];

/* Last requests, for /diag. Written and read by the server thread only (one at a time),
 * so no lock: this is the "what actually arrived" record when a phone shows something the
 * desktop does not. Fixed size, no allocation. */
const int kDiagLines = 8;
struct DiagLine {
  unsigned ms;
  char remote[16];
  char method[8];
  char target[96];
  char ua[40];
};
DiagLine g_diag[kDiagLines];
int g_diag_at = 0;
unsigned g_requests = 0;
unsigned g_started_ms = 0;

std::atomic<int> g_instances(0);   /* how many instances are alive right now */

/* Command ring: one producer (the HTTP thread, one connection at a time), one consumer (the
 * audio thread). Indices are free-running counters; a full ring drops the newest request,
 * which is the safe end for a control surface (the next tap wins). */
const int kCmdRing = 32;
Command g_cmd[kSlots][kCmdRing];
std::atomic<unsigned> g_cmd_head[kSlots];   /* written requests, per instance */
std::atomic<unsigned> g_cmd_tail[kSlots];   /* taken requests, per instance */
std::atomic<int> g_rw(1);              /* writes allowed (MHEC_HTTP_RW=0 turns them off) */
const char *const *g_keys = nullptr;   /* the module's contract, handed over at load */
int g_key_count = 0;
std::atomic<int> g_stop(0);
std::atomic<int> g_running(0);
std::atomic<int> g_port(0);
pthread_t g_thread;
pthread_mutex_t g_start_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_once_t g_init_once = PTHREAD_ONCE_INIT;

unsigned now_ms() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (unsigned)(ts.tv_sec * 1000u + (unsigned)(ts.tv_nsec / 1000000));
}

void init_slots() {
  for (int i = 0; i < kSlots; ++i) {
    Slot &s = g_slots[i];
    s.seq.store(0);
    s.attached.store(0);
    s.stamp_ms.store(0);
    s.wet.store(0.0f);
    s.delay_ms.store(0.0f);
    s.bpm.store(0.0f);
    s.rate.store(0.0f);
    s.wow_pct.store(0.0f);
    s.jog.store(0.0f);
    s.warp.store(0.0f);
    s.twist.store(0.0f);
    s.echo.store(0.0f);
    s.wow.store(0.0f);
    s.chorus.store(0.0f);
    s.saturation.store(0.0f);
    s.dropouts.store(0.0f);
    s.hiss.store(0.0f);
    s.spread.store(0.0f);
    s.mix.store(0.0f);
    s.intensity.store(0.0f);
    s.reverb.store(0.0f);
    s.mode.store(0);
    s.fb.store(0);
    s.sync.store(0);
    s.drift.store(0);
    s.reverb_head.store(0);
    s.tapeage.store(1);
    s.preamp.store(1);
    s.wetonly.store(0);
    s.motion.store(1);
    s.twisttype.store(0);
    s.warptype.store(0);
    s.revmode.store(1);
    for (int h = 0; h < 3; ++h) s.heads[h].store(0);
  }
}

/* ---- HTTP ---------------------------------------------------------------- */

int listen_on(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in a;
  std::memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  a.sin_port = htons((unsigned short)port);
  if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(fd, 8) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

/* MSG_NOSIGNAL: a browser that closes early must not raise SIGPIPE and take MPC down. */
bool send_all(int fd, const char *p, size_t n) {
  while (n > 0) {
    ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
    if (w <= 0) {
      if (w < 0 && errno == EINTR) continue;
      return false;
    }
    p += w;
    n -= (size_t)w;
  }
  return true;
}

void send_headers(int fd, int code, const char *reason, const char *type, size_t len,
                  bool head_only) {
  char h[256];
  int n = std::snprintf(h, sizeof h,
                        "HTTP/1.1 %d %s\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %zu\r\n"
                        "Cache-Control: no-store\r\n"
                        "Connection: close\r\n\r\n",
                        code, reason, type, len);
  if (n > 0) send_all(fd, h, (size_t)n);
  (void)head_only;
}

void send_body(int fd, int code, const char *reason, const char *type, const char *body,
               size_t len, bool head_only) {
  send_headers(fd, code, reason, type, len, head_only);
  if (!head_only && len > 0) send_all(fd, body, len);
}

/* One slot, read consistently. Returns false when the writer was mid-update for all
 * attempts; the caller then answers with whatever it got (the page retries at 10 Hz). */
struct Values {
  float wet, delay_ms, bpm, rate, wow_pct, jog, warp, twist;
  float echo, wow, chorus, saturation, dropouts, hiss, spread;
  float mix, intensity, reverb;
  int mode, fb, sync, drift, reverb_head;
  int tapeage, preamp, wetonly, motion, twisttype, warptype, revmode;
  int heads[3];
  unsigned stamp_ms;
};

bool read_values(int idx, Values *v) {
  Slot &s = g_slots[idx];
  for (int attempt = 0; attempt < 6; ++attempt) {
    const unsigned a = s.seq.load(std::memory_order_acquire);
    if (a & 1u) continue;
    v->wet = s.wet.load(std::memory_order_relaxed);
    v->delay_ms = s.delay_ms.load(std::memory_order_relaxed);
    v->bpm = s.bpm.load(std::memory_order_relaxed);
    v->rate = s.rate.load(std::memory_order_relaxed);
    v->wow_pct = s.wow_pct.load(std::memory_order_relaxed);
    v->jog = s.jog.load(std::memory_order_relaxed);
    v->warp = s.warp.load(std::memory_order_relaxed);
    v->twist = s.twist.load(std::memory_order_relaxed);
    v->echo = s.echo.load(std::memory_order_relaxed);
    v->wow = s.wow.load(std::memory_order_relaxed);
    v->chorus = s.chorus.load(std::memory_order_relaxed);
    v->saturation = s.saturation.load(std::memory_order_relaxed);
    v->dropouts = s.dropouts.load(std::memory_order_relaxed);
    v->hiss = s.hiss.load(std::memory_order_relaxed);
    v->spread = s.spread.load(std::memory_order_relaxed);
    v->mix = s.mix.load(std::memory_order_relaxed);
    v->intensity = s.intensity.load(std::memory_order_relaxed);
    v->reverb = s.reverb.load(std::memory_order_relaxed);
    v->mode = s.mode.load(std::memory_order_relaxed);
    v->fb = s.fb.load(std::memory_order_relaxed);
    v->sync = s.sync.load(std::memory_order_relaxed);
    v->drift = s.drift.load(std::memory_order_relaxed);
    v->reverb_head = s.reverb_head.load(std::memory_order_relaxed);
    v->tapeage = s.tapeage.load(std::memory_order_relaxed);
    v->preamp = s.preamp.load(std::memory_order_relaxed);
    v->wetonly = s.wetonly.load(std::memory_order_relaxed);
    v->motion = s.motion.load(std::memory_order_relaxed);
    v->twisttype = s.twisttype.load(std::memory_order_relaxed);
    v->warptype = s.warptype.load(std::memory_order_relaxed);
    v->revmode = s.revmode.load(std::memory_order_relaxed);
    for (int h = 0; h < 3; ++h) v->heads[h] = s.heads[h].load(std::memory_order_relaxed);
    v->stamp_ms = s.stamp_ms.load(std::memory_order_relaxed);
    if (s.seq.load(std::memory_order_acquire) == a) return true;
  }
  return false;
}

/* The slot the page watches without ?i=: the one that published most recently. */
int pick_slot(int requested) {
  if (requested >= 0 && requested < kSlots) return requested;
  int best = -1;
  unsigned best_stamp = 0;
  for (int i = 0; i < kSlots; ++i) {
    if (!g_slots[i].attached.load(std::memory_order_relaxed)) continue;
    const unsigned st = g_slots[i].stamp_ms.load(std::memory_order_relaxed);
    if (best < 0 || st > best_stamp) {
      best = i;
      best_stamp = st;
    }
  }
  return best < 0 ? 0 : best;
}

int attached_count() {
  int n = 0;
  for (int i = 0; i < kSlots; ++i) {
    n += g_slots[i].attached.load(std::memory_order_relaxed) ? 1 : 0;
  }
  return n;
}

int state_json(char *buf, size_t cap, int requested) {
  const int idx = pick_slot(requested);
  Values v;
  std::memset(&v, 0, sizeof v);   /* read_values may give up mid-update; never print garbage */
  read_values(idx, &v);
  const bool active = g_slots[idx].attached.load(std::memory_order_relaxed) != 0;
  const unsigned age = active ? (now_ms() - v.stamp_ms) : 0u;
  char ip[16] = {0};
  local_ip(ip, sizeof ip);
  const int n = std::snprintf(
      buf, cap,
      "{\"instance\":%d,\"count\":%d,\"active\":%d,\"age_ms\":%u,"
      "\"delay_ms\":%.1f,\"bpm\":%.1f,\"rate\":%.3f,\"wet\":%.3f,\"wow_pct\":%.2f,"
      "\"mode\":%d,\"fb\":%d,\"sync\":%d,\"drift\":%d,"
      "\"jog\":%.2f,\"warp\":%.2f,\"twist\":%.2f,"
      "\"echo\":%.3f,\"wow\":%.3f,\"chorus\":%.3f,\"saturation\":%.3f,\"dropouts\":%.3f,"
      "\"hiss\":%.3f,\"spread\":%.3f,\"mix\":%.3f,\"intensity\":%.3f,\"reverb\":%.3f,"
      "\"tapeage\":%d,\"preamp\":%d,\"wetonly\":%d,\"motion\":%d,"
      "\"twisttype\":%d,\"warptype\":%d,\"revmode\":%d,"
      "\"heads\":[%d,%d,%d],\"rev_head\":%d,\"port\":%d,\"url\":\"http://%s:%d/\"}",
      idx, attached_count(), active ? 1 : 0, age,
      (double)v.delay_ms, (double)v.bpm, (double)v.rate, (double)v.wet, (double)v.wow_pct,
      v.mode, v.fb, v.sync, v.drift,
      (double)v.jog, (double)v.warp, (double)v.twist,
      (double)v.echo, (double)v.wow, (double)v.chorus, (double)v.saturation, (double)v.dropouts,
      (double)v.hiss, (double)v.spread, (double)v.mix, (double)v.intensity, (double)v.reverb,
      v.tapeage, v.preamp, v.wetonly, v.motion, v.twisttype, v.warptype, v.revmode,
      v.heads[0], v.heads[1], v.heads[2], v.reverb_head,
      g_port.load(), ip, g_port.load());
  return n < 0 ? 0 : (int)((size_t)n < cap ? (size_t)n : cap);
}

/* Absolute-form targets ("GET http://host:port/path") come from proxies and from some
 * mobile browsers; matching them against "/" is what made a phone answer 404 while the
 * desktop worked (2026-11-01). Reduce them to the path. */
void normalise_target(char *target) {
  char *scheme = std::strstr(target, "://");
  if (!scheme) return;
  char *path = std::strchr(scheme + 3, '/');
  if (!path) {
    target[0] = '/';
    target[1] = 0;
    return;
  }
  std::memmove(target, path, std::strlen(path) + 1);
}

void remember(const char *remote, const char *method, const char *target, const char *req) {
  ++g_requests;
  DiagLine &d = g_diag[g_diag_at];
  g_diag_at = (g_diag_at + 1) % kDiagLines;
  d.ms = now_ms() - g_started_ms;
  std::snprintf(d.remote, sizeof d.remote, "%s", remote ? remote : "-");
  std::snprintf(d.method, sizeof d.method, "%s", method ? method : "-");
  std::snprintf(d.target, sizeof d.target, "%s", target ? target : "-");
  const char *ua = req ? std::strstr(req, "User-Agent: ") : nullptr;
  char ua_buf[sizeof d.ua] = {0};
  if (ua) {
    ua += 12;
    size_t n = 0;
    while (ua[n] && ua[n] != '\r' && ua[n] != '\n' && n + 1 < sizeof ua_buf) ++n;
    std::memcpy(ua_buf, ua, n);
  }
  std::snprintf(d.ua, sizeof d.ua, "%s", ua_buf);
}


/* Header NAMES are case-insensitive (RFC 7230), and browsers send them in their own case -
 * often all lowercase. A literal strstr(req, "X-MHE: 1") therefore rejected every tap from a
 * real browser ("missing X-MHE header") while curl with the exact bytes worked. */
bool header_is_set(const char *req, const char *name) {
  const size_t n = std::strlen(name);
  for (const char *p = req; p && *p;) {
    const char *eol = std::strchr(p, '\n');
    const char *end = eol ? eol : p + std::strlen(p);
    bool match = (size_t)(end - p) > n;
    for (size_t i = 0; match && i < n; ++i)
      if (std::tolower((unsigned char)p[i]) != std::tolower((unsigned char)name[i])) match = false;
    if (match && p[n] == ':') {
      const char *v = p + n + 1;
      while (v < end && (*v == ' ' || *v == '\t')) ++v;
      return v < end && *v == '1';
    }
    if (!eol) break;
    p = eol + 1;
  }
  return false;
}


/* The QR a second phone scans: the same URL /state publishes. Encoded here so one
 * implementation serves the page, a wrapper app and anything else. Encoder: Project
 * Nayuki's qrcodegen (MIT), vendored in qrcodegen.c. */
int qr_svg(char *out, size_t cap, const char *text) {
  static uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
  static uint8_t tmp[qrcodegen_BUFFER_LEN_MAX];
  if (!qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN,
                            qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true))
    return 0;
  const int n = qrcodegen_getSize(qr);
  int w = std::snprintf(out, cap,
      "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"-2 -2 %d %d\" shape-rendering=\"crispEdges\">"
      "<rect x=\"-2\" y=\"-2\" width=\"%d\" height=\"%d\" fill=\"#e9e4d2\"/>"
      "<path fill=\"#0a0c09\" d=\"", n + 4, n + 4, n + 4, n + 4);
  for (int y = 0; y < n && w > 0; y++)
    for (int x = 0; x < n; x++) {
      if (!qrcodegen_getModule(qr, x, y)) continue;
      int run = 1;
      while (x + run < n && qrcodegen_getModule(qr, x + run, y)) run++;
      w += std::snprintf(out + (w < (int)cap ? w : 0), w < (int)cap ? cap - (size_t)w : 0,
                         "M%d %dh%dv1h-%dz", x, y, run, run);
      x += run - 1;
    }
  if (w > 0) w += std::snprintf(out + (w < (int)cap ? w : 0), w < (int)cap ? cap - (size_t)w : 0, "\"/></svg>");
  return w > 0 && (size_t)w < cap ? w : 0;
}

void handle_client(int fd, const char *remote) {
  char req[kReqMax];
  size_t used = 0;
  /* Read until the end of the headers, the buffer is full, or the client goes quiet. */
  while (used + 1 < sizeof req) {
    ssize_t r = recv(fd, req + used, sizeof req - 1 - used, 0);
    if (r < 0) {
      if (errno == EINTR) continue;
      return;
    }
    if (r == 0) break;
    used += (size_t)r;
    req[used] = 0;
    if (std::strstr(req, "\r\n\r\n") || std::strstr(req, "\n\n")) break;
  }
  req[used] = 0;
  if (used == 0) return;

  char method[8] = {0};
  char target[512] = {0};
  if (std::sscanf(req, "%7s %511s", method, target) != 2) {
    /* Not HTTP: almost always a TLS ClientHello, i.e. the browser tried https:// on our
     * plain port (Chrome's HTTPS-first does this). Say so instead of hanging up. */
    remember(remote, (unsigned char)req[0] == 0x16 ? "TLS?" : "-", "(unparsable)", nullptr);
    send_body(fd, 400, "Bad Request", "text/plain",
              "this is a plain HTTP port - use http://, not https://\n", 53, false);
    return;
  }
  remember(remote, method, target, req);
  normalise_target(target);
  const bool head_only = std::strcmp(method, "HEAD") == 0;
  const bool post = std::strcmp(method, "POST") == 0;
  if (!head_only && !post && std::strcmp(method, "GET") != 0) {
    send_body(fd, 405, "Method Not Allowed", "text/plain", "GET or HEAD\n", 12, false);
    return;
  }

  char *q = std::strchr(target, '?');
  int requested = -1;
  char arg_k[24] = {0}, arg_v[24] = {0};
  if (q) {
    *q++ = 0;
    const char *ip = std::strstr(q, "i=");
    if (ip) requested = std::atoi(ip + 2);
    const char *kp = std::strstr(q, "k=");
    const char *vp = std::strstr(q, "v=");
    if (kp) std::snprintf(arg_k, sizeof arg_k, "%s", kp + 2);
    if (vp) std::snprintf(arg_v, sizeof arg_v, "%s", vp + 2);
    char *amp;
    if ((amp = std::strpbrk(arg_k, "&"))) *amp = 0;
    if ((amp = std::strpbrk(arg_v, "&"))) *amp = 0;
  }

  if (post && !std::strcmp(target, "/set")) {
    /* A custom header means a cross-site page would need a CORS preflight, which this server
     * never grants; a plain form post or a no-cors fetch cannot set one (review, 2026-11-02). */
    if (!header_is_set(req, "X-MHE")) {
      send_body(fd, 403, "Forbidden", "text/plain", "missing X-MHE header\n", 24, false);
      return;
    }
    /* The page drives the machine here. Validation is deliberately narrow: the key must be
     * one of the contract's keys, the value a finite number in range; everything else is
     * refused with a reason the page can show. The actual change happens on the audio
     * thread (see provide()/take_commands()). */
    if (!g_rw.load(std::memory_order_relaxed)) {
      send_body(fd, 403, "Forbidden", "text/plain", "writes are off (MHEC_HTTP_RW=0)\n", 35,
                false);
      return;
    }
    const int slot = pick_slot(requested);
    if (!g_slots[slot].attached.load(std::memory_order_relaxed)) {
      send_body(fd, 409, "Conflict", "text/plain", "no instance\n", 12, false);
      return;
    }
    int idx = -1;
    const char *const *keys = g_keys;
    const int nkeys = g_key_count;
    if (keys && arg_k[0]) {
      for (int i = 0; i < nkeys; ++i) {
        if (!std::strcmp(keys[i], arg_k)) { idx = i; break; }
      }
    }
    if (idx < 0) {
      send_body(fd, 400, "Bad Request", "text/plain", "unknown key\n", 12, false);
      return;
    }
    char *end = nullptr;
    const double d = std::strtod(arg_v, &end);
    if (end == arg_v || !(d == d) || d < -100000.0 || d > 100000.0) {
      send_body(fd, 400, "Bad Request", "text/plain", "bad value\n", 10, false);
      return;
    }
    Command c;
    c.idx = idx;
    c.val = (float)d;
    if (!provide(slot, c)) {
      send_body(fd, 503, "Service Unavailable", "text/plain", "busy\n", 5, false);
      return;
    }
    char ok[48];
    const int n = std::snprintf(ok, sizeof ok, "{\"ok\":1,\"key\":\"%s\"}", arg_k);
    send_body(fd, 200, "OK", "application/json", ok, (size_t)n, false);
    return;
  }

  if (post) {   /* a POST to anywhere but /set is not a control message */
    send_body(fd, 405, "Method Not Allowed", "text/plain", "POST /set only\n", 15, false);
    return;
  }
  if (!std::strcmp(target, "/") || !std::strcmp(target, "/index.html")) {
    send_body(fd, 200, "OK", "text/html; charset=utf-8", kCompanionPage,
              sizeof(kCompanionPage) - 1, head_only);
    return;
  }
  if (!std::strcmp(target, "/state")) {
    char json[900];
    const int n = state_json(json, sizeof json, requested);
    send_body(fd, 200, "OK", "application/json", json, (size_t)n, head_only);
    return;
  }
  if (!std::strcmp(target, "/manifest.json")) {
    send_body(fd, 200, "OK", "application/manifest+json", kManifest, sizeof(kManifest) - 1,
              head_only);
    return;
  }
  if (!std::strcmp(target, "/qr.svg")) {
    static char svg[16384];
    char url[64];
    char ip[16] = {0};
    local_ip(ip, sizeof ip);
    std::snprintf(url, sizeof url, "http://%s:%d/", ip, g_port.load());
    const int n = qr_svg(svg, sizeof svg, url);
    if (n <= 0) { send_body(fd, 500, "Internal Error", "text/plain", "qr\n", 3, head_only); return; }
    send_body(fd, 200, "OK", "image/svg+xml", svg, (size_t)n, head_only);
    return;
  }
  if (!std::strcmp(target, "/icon.svg")) {
    send_body(fd, 200, "OK", "image/svg+xml", kIconSvg, sizeof(kIconSvg) - 1, head_only);
    return;
  }
  if (!std::strcmp(target, "/diag")) {
    char txt[1400];
    int p2 = std::snprintf(txt, sizeof txt, "port=%d slots=%d attached=%d requests=%u\n",
                           g_port.load(), kSlots, attached_count(), g_requests);
    for (int i = 0; i < kDiagLines && p2 > 0 && (size_t)p2 + 160 < sizeof txt; ++i) {
      const DiagLine &d = g_diag[(g_diag_at + i) % kDiagLines];
      if (!d.target[0]) continue;
      p2 += std::snprintf(txt + p2, sizeof txt - (size_t)p2,
                          "t=%ums %s %s from %s ua=\"%s\"\n", d.ms, d.method, d.target,
                          d.remote, d.ua);
    }
    send_body(fd, 200, "OK", "text/plain; charset=utf-8", txt, (size_t)(p2 < 0 ? 0 : p2),
              head_only);
    return;
  }
  if (!std::strcmp(target, "/healthz")) {
    send_body(fd, 200, "OK", "text/plain", "ok\n", 3, head_only);
    return;
  }
  send_body(fd, 404, "Not Found", "text/html; charset=utf-8", kNotFoundPage,
            sizeof(kNotFoundPage) - 1, head_only);
}

void *server_main(void *) {
  int port = kDefaultPort;
  const char *env = std::getenv("MHEC_HTTP_PORT");
  if (env) {
    const int p = std::atoi(env);
    if (p > 1024 && p < 65000) port = p;
  }

  int fd = -1;
  for (int i = 0; i < kPortTries && fd < 0; ++i) fd = listen_on(port + i);
  if (fd < 0) {
    g_running.store(0);
    return nullptr;
  }
  g_port.store(port);
  g_started_ms = now_ms();

  while (!g_stop.load(std::memory_order_relaxed)) {
    struct pollfd p;
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    const int rc = poll(&p, 1, 200);
    if (rc <= 0) continue;
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof peer;
    const int c = accept(fd, (struct sockaddr *)&peer, &peer_len);
    if (c < 0) continue;
    /* One blocking accept loop: a reader that stops draining (phone asleep, tab in the
     * background) must time out instead of stalling every other client, /set included. */
    struct timeval tmo;
    tmo.tv_sec = 2;
    tmo.tv_usec = 0;
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tmo, sizeof tmo);
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof tmo);
    char remote[16] = "-";
    if (peer_len >= (socklen_t)sizeof peer) {
      struct in_addr a4 = peer.sin_addr;
      const unsigned char *b = (const unsigned char *)&a4.s_addr;
      std::snprintf(remote, sizeof remote, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    }
    handle_client(c, remote);
    close(c);
  }
  close(fd);
  g_port.store(0);
  g_running.store(0);
  return nullptr;
}

void stop_locked() {
  if (!g_running.load(std::memory_order_relaxed)) return;
  g_stop.store(1);
  pthread_join(g_thread, nullptr);   /* the loop wakes within 200 ms and returns */
  g_stop.store(0);
}

void start_locked() {
  if (g_running.load(std::memory_order_relaxed)) return;
  g_stop.store(0);
  g_running.store(1);
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
  pthread_attr_setstacksize(&attr, 256 * 1024);
  if (pthread_create(&g_thread, &attr, server_main, nullptr) != 0) {
    g_running.store(0);
  }
  pthread_attr_destroy(&attr);
}

/* The listener must NEVER outlive the last instance. MPC dlclose()s the .so when the
 * plugin is removed, and a thread still sitting in poll() would then run in unmapped
 * memory — that took the device down twice on 2026-11-01 ("OOPS! Your MPC recovered from
 * an internal error"), and it only showed up once the port had a listener at all. */
void stop_if_last() {
  if (g_instances.fetch_sub(1) != 1) return;
  pthread_mutex_lock(&g_start_lock);
  stop_locked();
  pthread_mutex_unlock(&g_start_lock);
}

}  // namespace

void register_param_keys(const char *const *keys, int count) {
  g_key_count = count;
  g_keys = keys;
}

int provide(int slot, const Command &c) {
  if (slot < 0 || slot >= kSlots) return 0;
  if (!g_rw.load(std::memory_order_relaxed)) return 0;
  const unsigned head = g_cmd_head[slot].load(std::memory_order_relaxed);
  if (head - g_cmd_tail[slot].load(std::memory_order_relaxed) >= (unsigned)kCmdRing) return 0;
  g_cmd[slot][head % kCmdRing] = c;
  g_cmd_head[slot].store(head + 1, std::memory_order_release);
  return 1;
}

int take_commands(int slot, Command *out, int max) {
  if (slot < 0 || slot >= kSlots) return 0;
  int n = 0;
  unsigned tail = g_cmd_tail[slot].load(std::memory_order_relaxed);
  const unsigned head = g_cmd_head[slot].load(std::memory_order_acquire);
  while (tail != head && n < max) {
    out[n++] = g_cmd[slot][tail % kCmdRing];
    ++tail;
  }
  if (n) g_cmd_tail[slot].store(tail, std::memory_order_release);
  return n;
}

int attach() {
  if (std::getenv("MHEC_HTTP") && std::atoi(std::getenv("MHEC_HTTP")) == 0) return -1;
  pthread_once(&g_init_once, init_slots);
  for (int i = 0; i < kSlots; ++i) {
    unsigned expected = 0;
    if (g_slots[i].attached.compare_exchange_strong(expected, 1u)) {
      g_slots[i].stamp_ms.store(now_ms());
      g_instances.fetch_add(1);
      pthread_mutex_lock(&g_start_lock);
      start_locked();
      pthread_mutex_unlock(&g_start_lock);
      return i;
    }
  }
  return -1;
}

void detach(int slot) {
  if (slot < 0 || slot >= kSlots) return;
  unsigned expected = 1;
  /* Only the owner may release the slot: a double destroy must not decrement the instance
   * count twice and leave the listener running. */
  if (!g_slots[slot].attached.compare_exchange_strong(expected, 0u)) return;
  stop_if_last();
}

void publish(int slot, const Snapshot &s) {
  if (slot < 0 || slot >= kSlots) return;
  Slot &d = g_slots[slot];
  if (!d.attached.load(std::memory_order_relaxed)) return;
  d.seq.fetch_add(1, std::memory_order_release);   /* odd: do not read */
  d.wet.store(s.wet);
  d.delay_ms.store(s.delay_ms);
  d.bpm.store(s.bpm);
  d.rate.store(s.rate);
  d.wow_pct.store(s.wow_pct);
  d.jog.store(s.jog);
  d.warp.store(s.warp);
  d.twist.store(s.twist);
  d.echo.store(s.echo);
  d.wow.store(s.wow);
  d.chorus.store(s.chorus);
  d.saturation.store(s.saturation);
  d.dropouts.store(s.dropouts);
  d.hiss.store(s.hiss);
  d.spread.store(s.spread);
  d.mix.store(s.mix);
  d.intensity.store(s.intensity);
  d.reverb.store(s.reverb);
  d.mode.store(s.mode);
  d.fb.store(s.fb);
  d.sync.store(s.sync);
  d.drift.store(s.drift);
  d.reverb_head.store(s.reverb_head);
  d.tapeage.store(s.tapeage);
  d.preamp.store(s.preamp);
  d.wetonly.store(s.wetonly);
  d.motion.store(s.motion);
  d.twisttype.store(s.twisttype);
  d.warptype.store(s.warptype);
  d.revmode.store(s.revmode);
  for (int h = 0; h < 3; ++h) d.heads[h].store(s.heads[h]);
  d.stamp_ms.store(now_ms());
  d.seq.fetch_add(1, std::memory_order_release);   /* even: stable */
}

int port() { return g_port.load(std::memory_order_relaxed); }

void stop_server() {
  pthread_mutex_lock(&g_start_lock);
  stop_locked();
  pthread_mutex_unlock(&g_start_lock);
}

}  // namespace mhec
