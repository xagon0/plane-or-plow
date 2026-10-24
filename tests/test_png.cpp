#include <assert.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>
#include <pngle.h>

static bool done;
static unsigned pixels;
static void on_done(pngle_t *) { done = true; }
static void on_pixel(pngle_t *, uint32_t, uint32_t, uint32_t, uint32_t,
                     const uint8_t[4]) { pixels++; }

int main(int argc, char **argv) {
    assert(argc > 1);
    for (int i = 1; i < argc; i++) {
        std::ifstream input(argv[i], std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
        assert(bytes.size() >= 24);
        pngle_t *png = pngle_new();
        pngle_set_done_callback(png, on_done);
        pngle_set_draw_callback(png, on_pixel);
        done = false; pixels = 0;
        int consumed = pngle_feed(png, bytes.data(), bytes.size());
        assert(consumed == (int)bytes.size() && done);
        assert(pngle_get_width(png) == 64 && pngle_get_height(png) == 64);
        assert(pixels == 64 * 64);
        pngle_reset(png);
        done = false;
        consumed = pngle_feed(png, bytes.data(), bytes.size() - 12);
        // An otherwise valid PNG without IEND was accepted by the old code.
        assert(consumed >= 0 && !done);
        pngle_destroy(png);
    }
    std::cout << "PASS: buffered PNG decode and truncated IEND rejection\n";
}
