// C++: exceptions (ARM EHABI unwinding), RTTI, virtual calls, STL containers, std::thread.
#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

struct Shape { virtual ~Shape() {} virtual double area() const = 0; };
struct Sq : Shape { double s; Sq(double v) : s(v) {} double area() const override { return s * s; } };
struct Circ : Shape { double r; Circ(double v) : r(v) {} double area() const override { return 3.14159 * r * r; } };

static int depth(int n) {
    if (n == 0) throw std::runtime_error("bottom");
    std::string guard(n, 'x');
    return depth(n - 1) + (int)guard.size();
}

int main() {
    std::vector<std::unique_ptr<Shape>> v;
    for (int i = 0; i < 10; i++) v.emplace_back(i & 1 ? (Shape *)new Sq(i) : (Shape *)new Circ(i));
    double a = 0;
    for (auto &s : v) a += s->area();
    printf("area %.3f sq %d\n", a, (int)std::count_if(v.begin(), v.end(), [](auto &s) { return dynamic_cast<Sq *>(s.get()) != nullptr; }));
    try {
        depth(50);
    } catch (const std::exception &e) {
        printf("caught %s\n", e.what());
    }
    try {
        throw 42;
    } catch (int x) {
        printf("int %d\n", x);
    }
    std::map<std::string, int> m;
    for (int i = 0; i < 1000; i++) m["k" + std::to_string(i * 7919 % 1000)] += i;
    long s = 0;
    for (auto &kv : m) s += kv.second * (long)kv.first.size();
    printf("map %zu %ld\n", m.size(), s);
    std::vector<std::thread> ts;
    std::vector<long> res(4);
    for (int t = 0; t < 4; t++) ts.emplace_back([t, &res] { long x = 0; for (int i = 0; i < 100000; i++) x += i % (t + 2); res[t] = x; });
    for (auto &t : ts) t.join();
    printf("threads %ld %ld %ld %ld\n", res[0], res[1], res[2], res[3]);
    return 0;
}
