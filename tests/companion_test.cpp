/* companion_test.cpp — offline proof of the companion feed (no device, no MPC).
 *
 * Creates a real instance, feeds it a tone so the meter has something to show, then talks to
 * the embedded HTTP listener over a socket:
 *
 *   1. /state answers 200 + JSON, and the numbers match what the module was told
 *   2. head activity comes from the core's mode table (H3 = head 3 only, H123R = all three)
 *   3. the tempo the host sends reaches the page, and sync turns it into a delay time
 *   4. / serves the page, /healthz answers, anything else is 404, POST is refused
 *   5. the request never blocks the audio thread (published while blocks run)
 */

#include <arpa/inet.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include "../src/host/audio_fx_api_v2.h"

extern "C" audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host);

static int g_fail = 0;
static int g_port = 18099;

static void check(bool ok, const char *name, const std::string &detail) {
  std::printf("%s: %s = %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
  if (!ok) ++g_fail;
}

/* One request, one response. Returns the body, or "" when nothing answered. */
static std::string http(int port, const char *method, const char *path, int *status_out,
                 bool with_header = true) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (status_out) *status_out = 0;
  if (fd < 0) return "";
  struct timeval tv;
  tv.tv_sec = 3;
  tv.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  struct sockaddr_in a;
  std::memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons((unsigned short)port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
    close(fd);
    return "";
  }
  char req[256];
  const int n = std::snprintf(req, sizeof req,
                              "%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\n%s\r\n",
                              method, path, with_header ? "X-MHE: 1\r\n" : "");
  if (send(fd, req, (size_t)n, 0) <= 0) { close(fd); return ""; }
  std::string raw;
  char buf[4096];
  for (;;) {
    const ssize_t r = recv(fd, buf, sizeof buf, 0);
    if (r <= 0) break;
    raw.append(buf, (size_t)r);
  }
  close(fd);
  if (status_out) {
    int code = 0;
    if (std::sscanf(raw.c_str(), "HTTP/1.1 %d", &code) == 1) *status_out = code;
  }
  const size_t hdr = raw.find("\r\n\r\n");
  return hdr == std::string::npos ? std::string() : raw.substr(hdr + 4);
}

/* Minimal JSON field reader: the page only needs flat numbers and the heads array. */
static double num(const std::string &js, const char *key) {
  const std::string pat = std::string("\"") + key + "\":";
  size_t p = js.find(pat);
  if (p == std::string::npos) return -999.0;
  return std::atof(js.c_str() + p + pat.size());
}

static std::string str_field(const std::string &js, const char *key) {
  const std::string pat = std::string("\"") + key + "\":";
  size_t p = js.find(pat);
  if (p == std::string::npos) return "";
  p += pat.size();
  const size_t e = js.find_first_of(",}", p);
  return js.substr(p, e - p);
}

static void feed(void *inst, audio_fx_api_v2_t *api, int blocks, double freq, double amp) {
  static int16_t buf[128 * 2];
  double ph = 0.0;
  for (int b = 0; b < blocks; ++b) {
    for (int i = 0; i < 128; ++i) {
      const double s = std::sin(ph) * amp;
      ph += 2.0 * M_PI * freq / 44100.0;
      const int16_t v = (int16_t)(s * 32767.0);
      buf[i * 2] = v;
      buf[i * 2 + 1] = v;
    }
    api->process_block(inst, buf, 128);
  }
}

/* Does something accept on this port? Used by companion.sh to prove MHEC_HTTP=0 works. */
static bool listening(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;
  struct sockaddr_in a;
  std::memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons((unsigned short)port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  const bool ok = connect(fd, (struct sockaddr *)&a, sizeof a) == 0;
  close(fd);
  return ok;
}

int main(int argc, char **argv) {
  const char *env = std::getenv("MHEC_HTTP_PORT");
  if (env) g_port = std::atoi(env);

  if (argc > 2 && std::strcmp(argv[1], "--probe") == 0) {
    const bool open = listening(std::atoi(argv[2]));
    std::printf("%s\n", open ? "LISTENING" : "CLOSED");
    return open ? 3 : 0;   /* 0 = the switch closed the socket, as it must */
  }

  audio_fx_api_v2_t *api = move_audio_fx_init_v2(nullptr);
  if (!api) { std::printf("FAIL: no api\n"); return 1; }

  const char *dir = "/tmp/opencode/companion-fake-plugin";
  if (std::system("rm -rf /tmp/opencode/companion-fake-plugin && "
                  "mkdir -p '/tmp/opencode/companion-fake-plugin/Plugin Skins/roots'") != 0) {
    std::printf("FAIL: cannot prepare the fake plugin folder\n");
    return 1;
  }

  void *inst = api->create_instance(dir, nullptr);
  if (!inst) { std::printf("FAIL: no instance\n"); return 1; }

  if (argc > 1 && std::strcmp(argv[1], "--instance-only") == 0) {
    /* only there to be probed: an instance exists, the listener must not */
    feed(inst, api, 40, 220.0, 0.2);
    usleep(1500 * 1000);
    api->destroy_instance(inst);
    return 0;
  }

  /* Nothing rendered yet: the listener must already answer. */
  int status = 0;
  std::string js = http(g_port, "GET", "/state", &status);
  check(status == 200 && js.find("\"active\":1") != std::string::npos,
        "the listener answers before the first block", js.substr(0, 120));

  feed(inst, api, 400, 220.0, 0.25);   /* ~1.2 s: 24 publishes */

  js = http(g_port, "GET", "/state", &status);
  check(status == 200 && js.find("\"active\":1") != std::string::npos, "/state is live", str_field(js, "active"));
  check(js.find("\"count\":1") != std::string::npos, "one instance is reported", str_field(js, "count"));

  const double wet = num(js, "wet");
  check(wet > 0.05 && wet <= 1.0, "the wet level reaches the page", std::to_string(wet));
  const double ms = num(js, "delay_ms");
  check(ms > 60.0 && ms < 200.0, "the echo time is the motor's range", std::to_string(ms));
  check(std::fabs(num(js, "bpm") - 120.0) < 0.01, "default tempo is 120", std::to_string(num(js, "bpm")));
  check(num(js, "mode") == 2.0, "default mode is H3", std::to_string(num(js, "mode")));
  check(js.find("\"heads\":[0,0,1]") != std::string::npos, "H3 reads head 3 only", str_field(js, "heads"));

  /* the mode table drives the picture, not a second copy of it */
  api->set_param(inst, "mode", "H123R");
  feed(inst, api, 80, 220.0, 0.25);
  js = http(g_port, "GET", "/state", &status);
  check(js.find("\"heads\":[1,1,1]") != std::string::npos, "H123R reads all three heads", str_field(js, "heads"));
  check(num(js, "rev_head") == 1.0, "H123R feeds the spring", std::to_string(num(js, "rev_head")));

  api->set_param(inst, "mode", "H1");
  feed(inst, api, 80, 220.0, 0.25);
  js = http(g_port, "GET", "/state", &status);
  check(js.find("\"heads\":[1,1,0]") == std::string::npos && js.find("\"heads\":[1,0,0]") != std::string::npos,
        "H1 reads head 1 only", str_field(js, "heads"));

  /* host tempo -> page, and sync -> the delay time the page draws */
  api->set_param(inst, "lfo_bpm", "96");
  api->set_param(inst, "sync", "1/16");
  feed(inst, api, 80, 220.0, 0.25);
  js = http(g_port, "GET", "/state", &status);
  check(std::fabs(num(js, "bpm") - 96.0) < 0.01, "the host tempo arrives", std::to_string(num(js, "bpm")));
  const double sync_ms = num(js, "delay_ms");
  check(std::fabs(sync_ms - 156.25) < 3.0, "1/16 at 96 BPM is 156 ms",
        std::to_string(sync_ms));

  /* ---- writing: the page drives the machine ---------------------------------- */
  std::string set = http(g_port, "POST", "/set?k=rate&v=0.80", &status);
  check(status == 200 && set.find("\"ok\":1") != std::string::npos, "POST /set takes a value",
        set.substr(0, 40));
  /* `rate` in /state is the motor's effective rate, and the SYNC test above left it synced;
   * release sync first, then the knob is what the motor runs at. */
  http(g_port, "POST", "/set?k=sync&v=0", &status);
  feed(inst, api, 20, 220.0, 0.25);            /* the audio thread applies both */
  js = http(g_port, "GET", "/state", &status);
  check(std::fabs(num(js, "rate") - 0.80) < 0.01, "the change reached the engine",
        std::to_string(num(js, "rate")));
  check(std::fabs(num(js, "intensity") - 0.40) < 0.01, "and nothing else moved",
        std::to_string(num(js, "intensity")));

  /* an enum by index, the way the mode grid sends it */
  http(g_port, "POST", "/set?k=mode&v=10", &status);
  feed(inst, api, 40, 220.0, 0.25);
  js = http(g_port, "GET", "/state", &status);
  check(num(js, "mode") == 10.0, "an enum index works", std::to_string(num(js, "mode")));
  check(js.find("\"heads\":[1,1,1]") != std::string::npos, "H123R follows the grid", str_field(js, "heads"));

  http(g_port, "POST", "/set?k=nonsense&v=1", &status);
  check(status == 400, "an unknown key is refused", std::to_string(status));
  http(g_port, "POST", "/set?k=rate&v=abc", &status);
  check(status == 400, "a non-number is refused", std::to_string(status));
  /* the skin folder is not a remote control: `ui` may be known but must never rewrite files */
  http(g_port, "POST", "/set?k=ui&v=0", &status);
  check(status == 200, "ui is accepted and ignored", std::to_string(status));
  /* a page on another origin cannot set a custom header without a preflight we refuse */
  http(g_port, "POST", "/set?k=rate&v=0.1", &status, false);
  check(status == 403, "a write without X-MHE is refused", std::to_string(status));

  /* the page itself */
  std::string page = http(g_port, "GET", "/", &status);
  check(status == 200 && page.find("<canvas") != std::string::npos,
        "/ serves the page", std::to_string(page.size()) + " bytes");
  check(page.find("/state") != std::string::npos, "the page polls /state", "found");
  check(page.find("http://") == std::string::npos || page.find("http://127.0.0.1") != std::string::npos,
        "the page needs no network of its own", "no external reference");

  std::string health = http(g_port, "GET", "/healthz", &status);
  check(status == 200 && health == "ok\n", "/healthz", health.substr(0, 8));

  std::string nf = http(g_port, "GET", "/nope", &status);
  check(status == 404, "an unknown path is 404", std::to_string(status));
  check(nf.find("<!doctype html>") != std::string::npos && nf.find("/state") != std::string::npos,
        "the 404 is a page, not naked text a JSON viewer can hijack", nf.substr(0, 40));

  /* A proxy, or a phone browser behind one, asks for the absolute URI. That used to fall
   * through to the 404 while the same URL worked on the desktop (2026-11-01). */
  std::string abs = http(g_port, "GET", "http://127.0.0.1:18099/", &status);
  check(status == 200 && abs.find("<canvas") != std::string::npos,
        "an absolute-form target still serves the page", std::to_string(abs.size()) + " bytes");

  std::string diag = http(g_port, "GET", "/diag", &status);
  check(status == 200 && diag.find("requests=") != std::string::npos,
        "/diag counts requests", diag.substr(0, 40));
  check(diag.find("127.0.0.1") != std::string::npos, "/diag shows the peer address", "found");
  check(diag.find("/set") != std::string::npos || diag.find("/state") != std::string::npos,
        "/diag lists the paths that arrived", "found");

  http(g_port, "POST", "/state", &status);
  check(status == 405, "the feed is read-only (POST refused)", std::to_string(status));

  /* still alive after all of that, and the second instance gets its own slot */
  void *inst2 = api->create_instance(dir, nullptr);
  feed(inst2, api, 80, 220.0, 0.25);
  js = http(g_port, "GET", "/state", &status);
  check(js.find("\"count\":2") != std::string::npos, "a second instance takes its own slot", str_field(js, "count"));
  js = http(g_port, "GET", "/state?i=1", &status);
  check(status == 200 && js.find("\"instance\":1") != std::string::npos, "?i= selects the instance",
        str_field(js, "instance"));

  /* each instance has its own command ring: a write to i=1 must not touch instance 0 */
  const double before0 = num(http(g_port, "GET", "/state?i=0", &status), "intensity");
  http(g_port, "POST", "/set?i=1&k=intensity&v=0.9", &status);
  feed(inst2, api, 20, 220.0, 0.25);
  feed(inst, api, 20, 220.0, 0.25);
  const double after0 = num(http(g_port, "GET", "/state?i=0", &status), "intensity");
  const double after1 = num(http(g_port, "GET", "/state?i=1", &status), "intensity");
  check(std::fabs(after0 - before0) < 0.001 && std::fabs(after1 - 0.9) < 0.01,
        "a write reaches only the addressed instance",
        "i0 " + std::to_string(after0) + ", i1 " + std::to_string(after1));

  api->destroy_instance(inst2);
  feed(inst, api, 40, 220.0, 0.25);
  js = http(g_port, "GET", "/state", &status);
  check(js.find("\"count\":1") != std::string::npos, "destroy releases the slot", str_field(js, "count"));

  api->destroy_instance(inst);

  /* The listener must NOT outlive the last instance. MPC dlclose()s the .so when the plugin
   * is removed; a thread still parked in poll() then runs in unmapped memory and takes the
   * whole app down — on the device that was "OOPS! Your MPC recovered from an internal
   * error", twice, on 2026-11-01. */
  check(!listening(g_port), "the listener stops with the last instance", "port closed");

  /* ... and a fresh insert brings it back on the same port (SO_REUSEADDR). */
  void *inst3 = api->create_instance(dir, nullptr);
  feed(inst3, api, 40, 220.0, 0.2);
  js = http(g_port, "GET", "/state", &status);
  check(status == 200 && js.find("\"active\":1") != std::string::npos,
        "a new instance restarts the listener", std::to_string(status));
  api->destroy_instance(inst3);
  check(!listening(g_port), "and stops it again", "port closed");

  std::printf("%s\n", g_fail == 0 ? "PASSED" : "FAILED");
  return g_fail == 0 ? 0 : 1;
}
