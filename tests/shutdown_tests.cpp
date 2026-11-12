#define NOMINMAX
#include <core/threads/cache.hxx>
#include <chrono>
#include <cstdio>

int main() {
    threads::c_cache cache;
    if(!cache.init()) return 1;
    cache.start();
    Sleep(100); // Closing during the initial connection scan used to wait for the whole scan.
    const auto start=std::chrono::steady_clock::now();
    cache.stop();
    cache.stop(); // explicit cleanup followed by destruction must be safe
    const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
    std::printf("Cache shutdown during attachment: %lld ms\n",static_cast<long long>(ms));
    return ms<1500?0:1;
}
