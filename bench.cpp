// barebones benchmark. two questions only:
//   1. how long does it take to initialize
//   2. how much cache did that initialization burn
//
//   g++ -O2 -std=c++20 bench.cpp -o bench && ./bench
//
// THIS IS A POC.

#include "lazy_vector.cpp"

#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

using clk = std::chrono::steady_clock;
using elem_t = int;

static const size_t N = 16 * 1024 * 1024;   // 64MB of ints

// keeps the optimizer from deleting our loops
static volatile long sink = 0;

static double ms(clk::duration d){
    return std::chrono::duration<double, std::milli>(d).count();
}

// physical memory we are actually holding, statm field 2 is resident pages
static long rss_kb(){
    long total = 0;
    long resident = 0;
    FILE* f = fopen("/proc/self/statm", "r");
    if(f == nullptr){
        return 0;
    }
    if(fscanf(f, "%ld %ld", &total, &resident) != 2){
        resident = 0;
    }
    fclose(f);
    return resident * (sysconf(_SC_PAGESIZE) / 1024);
}

// cache probe, 256kB so it sits in L2. this is a pointer chase, not a linear
// scan: every load depends on the previous one, so the prefetcher cannot hide
// a miss. hot that is ~15 cycles a hop, from DRAM it is 200+, so eviction
// shows up as a 5-15x slowdown instead of the 20% a linear scan gives us.
static const size_t HOPS = (256 * 1024) / sizeof(size_t);
static std::vector<size_t> probe(HOPS);

static void build_probe(){
    // one random cycle through every slot, so the walk is unpredictable
    std::vector<size_t> order(HOPS);
    for(size_t i = 0; i < HOPS; ++i){
        order[i] = i;
    }
    std::mt19937_64 rng(12345);
    std::shuffle(order.begin(), order.end(), rng);
    for(size_t i = 0; i < HOPS; ++i){
        probe[order[i]] = order[(i + 1) % HOPS];
    }
}

static double walk_probe(){
    auto t0 = clk::now();
    size_t p = 0;
    for(size_t i = 0; i < HOPS; ++i){
        p = probe[p];
    }
    auto t1 = clk::now();
    sink = (long)p;
    return ms(t1 - t0);
}

// get the probe hot, return the best time we saw
static double warm_probe(){
    double best = 1e18;
    for(int i = 0; i < 20; ++i){
        double t = walk_probe();
        if(t < best){
            best = t;
        }
    }
    return best;
}

int main(){
    build_probe();
    printf("%zu MB of %zu-byte elements, probe = %zu kB pointer chase\n\n",
           N * sizeof(elem_t) / (1024 * 1024), sizeof(elem_t),
           HOPS * sizeof(size_t) / 1024);

    // 64MB is well over the mmap threshold, so glibc mmaps this and munmaps it
    // on free. the lazy run below cannot inherit already-faulted pages from it.
    {
        double hot = warm_probe();
        long rss0 = rss_kb();
        auto t0 = clk::now();
        std::vector<elem_t> v(N, 25);
        auto t1 = clk::now();
        long rss = rss_kb() - rss0;
        double cold = walk_probe();   // single shot, walking twice re-warms it
        sink = v[0];
        printf("std   init = %9.3f ms   rss = %8ld kB   probe %7.3f -> %7.3f ms  (%5.1fx)\n",
               ms(t1 - t0), rss, hot, cold, cold / hot);
    }

    {
        double hot = warm_probe();
        long rss0 = rss_kb();
        auto t0 = clk::now();
        lazy_vector<elem_t> v(N, 25);
        auto t1 = clk::now();
        long rss = rss_kb() - rss0;
        double cold = walk_probe();
        sink = v[0];
        printf("lazy  init = %9.3f ms   rss = %8ld kB   probe %7.3f -> %7.3f ms  (%5.1fx)\n",
               ms(t1 - t0), rss, hot, cold, cold / hot);
        printf("      L2 = %zu kB, tile = %zu kB, tiles = %zu (so %zu mmap calls)\n",
               v.l2_size() / 1024, v.tile_bytes() / 1024,
               v.num_tiles(), v.num_tiles());
    }

    return 0;
}
