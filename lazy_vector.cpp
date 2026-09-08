#pragma once

#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <type_traits>

// I am aware of leaks when a runtime error is thrown, 
// THIS IS A POC.
template <class T>
class lazy_vector {
    static_assert(std::is_trivially_copyable_v<T>,
                 "lazy_vector materializes elements by copying raw bytes");
    
    size_t n_ = 0; // number of elements
    size_t t_size_ = 0; // size of template T
    size_t page_size_ = 0;
    size_t tile_ = 0; // size of a tile
    size_t num_tiles_ = 0; // number of tiles required to store n entries of datatype T, with each tile being of size tile_
    size_t l2_size_ = 0; // detected L2 size, tile_ is sized off this
    T* base_ = nullptr; // start location of the contigeous array
    int fd_ = -1; // file descriptor for the pattern tile.

    // L2 size in bytes. on x86 glibc answers this straight from CPUID, so it
    // works even under WSL2 where sysfs can be incomplete. if it cannot tell
    // us we fall back to 256kB, which is a typical per-core L2.
    static size_t l2_cache_size(){
        long sz = sysconf(_SC_LEVEL2_CACHE_SIZE);
        if(sz > 0){
            return (size_t)sz;
        }
        return 256 * 1024;
    }

    void map_tiles() {
        for (size_t off = 0; off < tile_ * num_tiles_; off += tile_) {
            size_t len = std::min(tile_, tile_ * num_tiles_ - off);
            if(mmap((char*)base_ + off, len, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_FIXED, fd_, 0) == MAP_FAILED){
                throw std::runtime_error("mmap tile chunk");
            }   
        }
    }

public:
    
    // tile_fraction is how much of L2 we are willing to spend on the tile.
    // pass something bigger than 1.0 to deliberately overflow L2, that is how
    // you sweep the latency-vs-cache tradeoff.
    lazy_vector(size_t n, T value, double tile_fraction = 0.5): n_(n){
        if(n==0) return;

        // determine value of tile_ and corresponding values
        t_size_ = sizeof(T);
        page_size_ = sysconf(_SC_PAGESIZE);
        l2_size_ = l2_cache_size();

            // smallest tile the pattern can legally repeat over. it has to be
            // a multiple of the element size (so no element straddles a tile
            // boundary) and of the page size (so mmap can place it).
        size_t unit = std::lcm(page_size_, t_size_);
        size_t total = n * t_size_;

            // tile_ is our cache budget. it is the only memory this container
            // ever pulls into cache, both when we fill it once below and when
            // a read-only consumer walks the whole vector. so we size it off
            // L2 and let the syscall count fall out of that, rather than
            // picking a syscall count and hoping the cache survives.
        size_t want = (size_t)(l2_size_ * tile_fraction);
        if(want < unit){
            want = unit;
        }
        if(want > total){
            want = total;
        }
            // round up to a legal tile
        tile_ = ((want + unit - 1) / unit) * unit;

            // number of tiles required to contain the data
            // classic ceil
        num_tiles_ = (total + tile_ - 1) / tile_;
        
        // create the page tile
        // should use the MFD_CLOEXEC flag, need to look into this.
        fd_ = memfd_create("lazy_tile", 0);
        if(fd_ < 0){
            throw std::runtime_error("memfd_create failed (fd_==0)");
        }
        if(ftruncate(fd_, (off_t)tile_)){
            close(fd_);
            throw std::runtime_error("ftruncate failed");
        }

        // fill the pattern inside the virtual file created above.
            //create a new virual address space for the pattern tile.
        void* tile = mmap(nullptr, tile_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (tile == MAP_FAILED){
            throw std::runtime_error("mmap tile failed");
        }
            // fill the virtual address space with the value,
            // writing into the virtual file, using tile. 

            // here fill_n needs 
        std::fill_n(static_cast<T*>(tile), tile_ / sizeof(T), value);
        // remove the virtual address as the virtual file is populated
        // take void* unlike fill_n
        munmap(tile, tile_);

        // reserve virtual address space for the full vector
        void* v = mmap(nullptr, tile_ * num_tiles_, PROT_NONE, 
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (v == MAP_FAILED){
            throw std::runtime_error("mmap vector failed");
        }
        // point to the base
        base_ = static_cast<T*>(v);

        //map the tiles to the virtual file
        map_tiles();
        
    }

    ~lazy_vector(){
        if(base_){
            munmap(base_, num_tiles_ * tile_);
        }
        if(fd_ >= 0){
            close(fd_);
        }
    }

    /* Removing copy assignment and copy initialization so that compiler
    doesnt auto generate and it is out of scope - refer to project.txt*/
    lazy_vector(const lazy_vector&) = delete;
    lazy_vector& operator=(const lazy_vector&) = delete;

    // so the benchmark can report what tiling we actually picked
    size_t tile_bytes() const { return tile_; }
    size_t num_tiles()  const { return num_tiles_; }
    size_t l2_size()    const { return l2_size_; }

    //helper vector stuff:
    T*       data()       { return base_; }
    const T* data() const { return base_; }
    size_t   size() const { return n_; }
    T&       operator[](size_t i)       { return base_[i]; }
    const T& operator[](size_t i) const { return base_[i]; }
    T*       begin() { return base_; }
    T*       end()   { return base_ + n_; }
};