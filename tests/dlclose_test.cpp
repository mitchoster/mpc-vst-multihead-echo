/* dlclose_test.cpp — the device crash of 2026-11-01, reproduced offline.
 *
 * MPC unloads a plugin's .so as soon as the last instance is removed. If the listener thread is
 * still parked in poll() at that moment, it keeps executing inside unmapped memory and takes the
 * whole app down. This host program does exactly that sequence, in-process:
 *
 *   dlopen  -> attach (listener starts) -> one HTTP request -> detach -> dlclose -> idle
 *
 * The idle second after dlclose is the crash window: with the bug it dies (SIGSEGV), with the fix
 * (detach joins the thread) it exits 0. Companion flags: --expect-dead to require the crash, so the
 * regression can be shown both ways.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <dlfcn.h>

typedef int (*attach_fn)();
typedef void (*publish_fn)(int);
typedef void (*detach_fn)(int);

static int probe_once(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return 0;
  struct sockaddr_in a;
  std::memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons((unsigned short)port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  const int ok = connect(fd, (struct sockaddr *)&a, sizeof a) == 0;
  close(fd);
  return ok;
}

/* attach() starts the thread, the bind inside it is a moment later — give it a second. */
static int wait_listen(int port, bool want) {
  for (int i = 0; i < 100; ++i) {
    if (probe_once(port) == (want ? 1 : 0)) return 1;
    usleep(10 * 1000);
  }
  return 0;
}

int main(int argc, char **argv) {
  const char *lib = argc > 1 ? argv[1] : "/tmp/libcp_under_test.so";
  const int port = argc > 2 ? std::atoi(argv[2]) : 18199;
  const bool expect_dead = argc > 3 && std::strcmp(argv[3], "--expect-dead") == 0;
  /* --no-detach is the "before" case: the host unloads while the plugin still owns a running
   * thread (what MPC did when destroy_instance() did not stop it). It must die here, or the
   * whole experiment proves nothing. */
  const bool no_detach = argc > 3 && std::strcmp(argv[3], "--no-detach") == 0;

  void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    std::printf("FAIL: dlopen: %s\n", dlerror());
    return 2;
  }
  attach_fn attach = (attach_fn)dlsym(h, "tc_attach");
  publish_fn publish = (publish_fn)dlsym(h, "tc_publish");
  detach_fn detach = (detach_fn)dlsym(h, "tc_detach");
  if (!attach || !publish || !detach) {
    std::printf("FAIL: missing symbols\n");
    return 2;
  }

  const int slot = attach();
  if (slot < 0) {
    std::printf("FAIL: no slot (MHEC_HTTP=0?)\n");
    return 2;
  }
  publish(slot);
  if (!wait_listen(port, true)) {
    std::printf("FAIL: listener not up\n");
    return 2;
  }
  std::printf("PASS: listener answering on %d\n", port);

  if (!no_detach) {
    detach(slot);
    if (!wait_listen(port, false)) {
      std::printf("FAIL: the listener survived detach\n");
      return 2;
    }
    std::printf("PASS: listener closed when the instance was destroyed\n");
  } else {
    std::printf("PASS: listener running, unloading it anyway (the old behaviour)\n");
  }
  std::fflush(stdout);
  dlclose(h);            /* this is the moment MPC removed the plugin on the device */

  /* The crash window: a surviving thread would now run in unmapped memory. */
  for (int i = 0; i < 20; ++i) usleep(50 * 1000);

  std::printf("%s: process still alive after dlclose\n", (expect_dead || no_detach) ? "UNEXPECTED" : "PASS");
  return (expect_dead || no_detach) ? 3 : 0;
}
