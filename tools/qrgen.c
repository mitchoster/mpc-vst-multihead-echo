/* qrgen -- print the QR SVG for a string, using the vendored qrcodegen (Nayuki, MIT).
 * Same encoder and output format as the plugin's qr_svg(), so an offline plate and the
 * /qr.svg endpoint cannot disagree. Host tool only: art/make_qr_plate.py compiles it.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "qrcodegen.h"

int main(int argc, char **argv) {
  static uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
  static uint8_t tmp[qrcodegen_BUFFER_LEN_MAX];
  if (argc < 2) return 2;
  const char *text = argv[1];
  if (!qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN,
                            qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true))
    return 3;
  const int n = qrcodegen_getSize(qr);
  int w = printf("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"-2 -2 %d %d\" shape-rendering=\"crispEdges\">"
                 "<rect x=\"-2\" y=\"-2\" width=\"%d\" height=\"%d\" fill=\"#e9e4d2\"/>"
                 "<path fill=\"#0a0c09\" d=\"", n + 4, n + 4, n + 4, n + 4);
  for (int y = 0; y < n; y++)
    for (int x = 0; x < n; x++) {
      if (!qrcodegen_getModule(qr, x, y)) continue;
      int run = 1;
      while (x + run < n && qrcodegen_getModule(qr, x + run, y)) run++;
      w += printf("M%d %dh%dv1h-%dz", x, y, run, run);
      x += run - 1;
    }
  w += printf("\"/></svg>");
  (void)w;
  return 0;
}
