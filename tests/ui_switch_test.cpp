/* ui_switch_test.cpp — proves the skin handover and its single-interface repair (2026-11-01).
 *
 * Builds the module's real entry points against a temporary plugin folder that looks like the
 * deployed one (Plugin Skins/roots/ with TUI.json + Q-Links.json, a root TUI.json, ui.txt):
 *
 *   1. insert adopts ui.txt instead of overwriting the skin
 *   2. a stale record ("rack", whose interface was removed) heals the root skin once
 *   3. a value the UI no longer offers cannot select a folder that does not exist
 *   4. re-applying the adopted value (what the host does when restoring a project) does not
 *      touch the files
 *   5. re-insert adopts the file state, not a default
 *   6. a neighbouring control keeps its real range (the 0..1 clamp regression)
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../src/host/audio_fx_api_v2.h"

extern "C" audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host);

static int g_fail = 0;

static std::string slurp(const std::string &p) {
  FILE *f = std::fopen(p.c_str(), "rb");
  if (!f) return "";
  std::string s;
  char buf[512];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
  std::fclose(f);
  return s;
}

static void write_file(const std::string &p, const std::string &v) {
  FILE *f = std::fopen(p.c_str(), "wb");
  if (!f) return;
  std::fwrite(v.data(), 1, v.size(), f);
  std::fclose(f);
}

static void check(bool ok, const char *name, const std::string &detail) {
  std::printf("%s: %s = %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
  if (!ok) ++g_fail;
}

int main() {
  const std::string dir = "/tmp/opencode/ui-switch-fake-plugin";
  const std::string skins = dir + "/Plugin Skins";
  const std::string roots = skins + "/roots";

  std::string cmd = "rm -rf '" + dir + "' && mkdir -p '" + roots + "'";
  if (std::system(cmd.c_str()) != 0) return 2;
  write_file(roots + "/TUI.json", "tui-roots");
  write_file(roots + "/Q-Links.json", "qlinks-roots");

  const std::string root_tui = skins + "/TUI.json";
  const std::string root_ql = skins + "/Q-Links.json";
  const std::string ui_txt = skins + "/ui.txt";
  write_file(root_tui, "tui-roots");
  write_file(root_ql, "qlinks-roots");
  write_file(ui_txt, "roots\n");

  audio_fx_api_v2_t *api = move_audio_fx_init_v2(nullptr);
  if (!api) { std::printf("FAIL: no api\n"); return 1; }

  void *inst = api->create_instance(dir.c_str(), nullptr);
  if (!inst) { std::printf("FAIL: no instance\n"); return 1; }

  char buf[64];
  api->get_param(inst, "ui", buf, sizeof buf);
  check(std::string(buf) == "Roots", "insert adopts ui.txt", std::string("get ui = ") + buf);
  check(slurp(root_tui) == "tui-roots", "insert leaves the skin alone", slurp(root_tui));

  api->set_param(inst, "ui", "Roots");
  check(slurp(root_tui) == "tui-roots", "same value does not rewrite", slurp(root_tui));

  /* a preset from before 2026-11-01 still carries ui=Rack or ui=2 */
  api->set_param(inst, "ui", "Rack");
  api->get_param(inst, "ui", buf, sizeof buf);
  check(std::string(buf) == "Roots", "a removed interface cannot be selected", std::string("get ui = ") + buf);
  check(slurp(root_tui) == "tui-roots", "and the skin is untouched", slurp(root_tui));

  api->set_param(inst, "ui", "1");
  api->get_param(inst, "ui", buf, sizeof buf);
  check(std::string(buf) == "Roots", "an out-of-range index clamps to Roots", std::string("get ui = ") + buf);

  api->set_param(inst, "spread", "5");
  api->get_param(inst, "spread", buf, sizeof buf);
  check(std::string(buf) == "5.0000", "spread keeps 5 ms", std::string("get spread = ") + buf);

  api->destroy_instance(inst);

  /* Re-insert adopts the files. */
  inst = api->create_instance(dir.c_str(), nullptr);
  api->get_param(inst, "ui", buf, sizeof buf);
  check(std::string(buf) == "Roots", "re-insert keeps the last choice", std::string("get ui = ") + buf);
  api->destroy_instance(inst);

  /* The device state from before the removal: ui.txt says "rack", the root TUI.json is the
   * rack one, and the rack folder is gone. Insert must repair, not adopt a ghost. */
  write_file(ui_txt, "rack\n");
  write_file(root_tui, "tui-rack");
  write_file(root_ql, "qlinks-rack");
  inst = api->create_instance(dir.c_str(), nullptr);
  api->get_param(inst, "ui", buf, sizeof buf);
  check(std::string(buf) == "Roots", "a stale ui.txt falls back to Roots", std::string("get ui = ") + buf);
  check(slurp(root_tui) == "tui-roots", "and the root TUI.json is repaired", slurp(root_tui));
  check(slurp(root_ql) == "qlinks-roots", "and the Q-Link map too", slurp(root_ql));
  check(slurp(ui_txt) == "roots\n", "and ui.txt is healed", slurp(ui_txt));
  api->destroy_instance(inst);

  std::printf("%s\n", g_fail == 0 ? "PASSED" : "FAILED");
  return g_fail == 0 ? 0 : 1;
}
