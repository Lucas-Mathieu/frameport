// Guest test library (armv7, C++, static libc++): exceptions thrown and caught inside a guest library unwind
// through the runtime's dl_unwind_find_exidx; static constructors run at load.
#include <stdexcept>
#include <string>
#include <vector>

static std::vector<int> g_init{1, 2, 3}; // a static constructor

static int thrower(int x)
{
    if (x > 2) throw std::runtime_error("too big: " + std::to_string(x));
    return x;
}

extern "C" int cxx_test(int n)
{
    int caught = 0;
    for (int k = 0; k < n; k++) {
        try {
            thrower(k);
        } catch (const std::runtime_error &e) {
            if (std::string(e.what()).find("too big") == 0) caught++;
        }
    }
    return caught * 100 + (int)g_init.size();
}
