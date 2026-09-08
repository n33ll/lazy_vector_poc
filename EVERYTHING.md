# lazy_vector — Project Log

A record of a project that started as a tracing exercise on dynamic memory
allocation and ended as a working container with measured results, several
corrected misconceptions, and one measurement we could not make.

Each concept appears in exactly one place. Later sections assume earlier ones.

---

## 0. How the project evolved

It began as `main.cpp`: a program that allocates `std::vector<int>` at two
sizes, pausing on `cin` between each so the process could be inspected from
another terminal. The goal was to see `brk` and `mmap` with our own eyes.

Following that thread led to the page-fault path, then to why anonymous pages
must be zeroed, then to the observation that `std::vector<int>(n, 25)` writes
every page twice — once by the kernel, once by the constructor. Trying to
eliminate that redundancy produced `lazy_vector`.

The final thesis is **not** the one we started with. That change is documented
in §7 and §8, because how it changed is the most useful part.

---

## 1. Phase one — watching allocation happen

### 1.1 Two syscalls, chosen per request

Dynamic allocation bottoms out in one of two syscalls:

- `brk` moves the top of the heap. One argument: the new break address.
Cheap, but it can only grow and shrink at one end, so freed space in the
middle is retained rather than returned.
- `mmap` creates an independent mapping anywhere in the address space, and
can be released independently with `munmap`.

glibc picks per allocation using `M_MMAP_THRESHOLD`, default 128 KB. Requests
below it come from the arena (which `brk` grows); requests above it get their
own `mmap`.

The word *per* matters. The allocator does not reason about your program's
total footprint — only about the size of the request in front of it.

### 1.2 Reading `/proc/<pid>/maps`

```
START-END          perms  offset  dev    inode  pathname
5758e6e20000-5758e6e53000 rw-p 00000000 00:00 0   [heap]
```

Region size is `END - START`, in hex. The pathname column is what makes a line
interpretable:


| pathname    | meaning                                                 |
| ----------- | ------------------------------------------------------- |
| a file path | file-backed (executables, `.so` files)                  |
| `[heap]`    | the main arena, grown by `brk`                          |
| `[stack]`   | main thread stack                                       |
| *blank*     | **anonymous** — this is where `mmap`'d allocations live |


Most of the file is noise. For allocation work only `[heap]` and the blank-path
`rw-p` lines matter, and the technique that actually works is **diffing
snapshots** between pauses, not reading one dump.

### 1.3 Four wrong turns, and what each taught

**Small vectors produced no visible change.** A 4 KB vector went into free
space already inside the ~204 KB arena. No `brk`, no new mapping. This was a
*correct observation of expected behaviour* — the absence of a change is the
result.

**We watched the wrong line.** A 4 KB anonymous region near the loader looked
like a candidate, and it never changed. It was loader bookkeeping. A ~400 KB
allocation can never appear as growth of a 4 KB region — **check that a
candidate line's size is plausible before concluding anything from it.**

**We assumed total size drives** `brk`**.** Four 240 KB vectors were created
expecting the ~204 KB heap to be forced to grow. It didn't: each individual
240 KB request exceeded 128 KB, so each took the `mmap` path. The heap was
never involved. This is where §1.1's *per-request* point was learned the hard
way.

**Making** `[heap]` **grow required going the other way.** Many allocations each
*below* the threshold, enough to exhaust arena free space. Only then does the
`[heap]` end address move.

---

## 2. Phase two — `mmap` and the fault path

### 2.1 The six arguments

```c
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
```


| #   | arg      | role                                          | malloc's value |
| --- | -------- | --------------------------------------------- | -------------- |
| 1   | `addr`   | placement hint; `NULL` lets the kernel choose | `NULL`         |
| 2   | `length` | bytes, rounded up to a page                   | request size   |
| 3   | `prot`   | `PROT_READ`/`WRITE`/`EXEC`/`NONE`             | `READ          |
| 4   | `flags`  | mapping kind                                  | `PRIVATE       |
| 5   | `fd`     | backing file; ignored for anonymous           | `-1`           |
| 6   | `offset` | offset into the file; ignored for anonymous   | `0`            |


Flags that matter here: `MAP_PRIVATE` (writes stay yours, via copy-on-write),
`MAP_SHARED` (writes are visible to others and reach the file),
`MAP_ANONYMOUS` (no file — zero-filled memory), `MAP_FIXED` (place exactly at
`addr`, replacing whatever was there), `MAP_NORESERVE` (don't account commit
charge up front).

### 2.2 Two-phase laziness

`mmap` does almost nothing:

1. The kernel creates a **VMA** — a descriptor recording start, end, flags,
  backing. It goes into the process's VMA tree, which is what `maps` prints.
2. **No physical page is allocated. No page-table entry is created.**
3. A virtual address is returned.

You now hold address space that refers to nothing physical. Everything real
happens later, on first touch.

### 2.3 Anatomy of a page fault

The MMU walks the page table, finds no valid entry, and raises `#PF`. The
kernel handler then:

1. **Looks up the VMA.** "Valid" means the address falls inside some VMA
  *and* that VMA's permissions allow this access kind. A write to a
   read-only-mapped region is as invalid as an address in no VMA at all.
2. If invalid → `SIGSEGV` (signal 11, "segmentation violation"). Default
  disposition is terminate and dump core. Catchable, but it always means a
   bug: null dereference, overflow, use-after-free.
3. If valid, resolve the page — see below.
4. Install a PTE and return. The faulting instruction **re-executes** and
  succeeds.

**Minor vs major** is about whether resolution needs disk I/O:

- **Minor** — satisfiable from memory. A fresh anonymous page (allocate a
frame, zero it) or a file page already in the page cache.
- **Major** — requires disk. Two distinct causes that are easy to conflate:
a *file-backed* mapping whose content isn't cached yet (this is how
executables and `.so` files load), or a page previously **swapped out**
under memory pressure. Only the second is about RAM being full.

### 2.4 Why anonymous pages must be zeroed

A recycled physical frame may hold another process's data — keys, passwords,
buffers. The kernel cannot hand it over as-is. Since a fault is typically
triggered by a store to *one* address, not a declaration that you will
overwrite all 4096 bytes, the kernel must zero the **whole page** and then let
your store proceed.

The contrast makes the rule clear:


| mapping         | what fills a faulting page                         |
| --------------- | -------------------------------------------------- |
| `MAP_ANONYMOUS` | zeros — mandatory, nothing to leak                 |
| file-backed     | the file's bytes, read from the page cache or disk |
| after `fork`    | copy-on-write of the parent's page                 |


There is one optimisation worth knowing: for a **read** fault on untouched
anonymous memory, the kernel maps a single global all-zeros frame
(`ZERO_PAGE`) read-only rather than allocating anything. A later write
upgrades it to a private frame. This is the seed of the entire idea in §4.6.

### 2.5 Page tables versus the TLB

A frequent confusion, resolved:


| structure                              | written by          | when                                       |
| -------------------------------------- | ------------------- | ------------------------------------------ |
| page table (in RAM, 4-level on x86-64) | **kernel software** | during fault handling                      |
| TLB (on-CPU cache)                     | **hardware MMU**    | automatically, on the next page-table walk |


The kernel never writes TLB entries on x86. It writes a PTE; when the
instruction retries, the MMU walks the table, finds it, and caches the
translation itself. The kernel *does* **invalidate** TLB entries (`invlpg`)
when unmapping or changing a mapping, otherwise stale translations would be
served.

---

## 3. Phase three — the double-zero, and what it actually costs

### 3.1 What the constructors really do

`std::vector<int>(n)` and `std::vector<int>(n, 0)` are the same story: allocate
raw storage, then initialize every element in user space. `MAP_ANONYMOUS` only
guarantees zeros *at first residency*; it does not know or care what value you
asked for, and the container does not treat "freshly mapped" as "already
initialized."

So per page, first touch looks like:

```
constructor stores to &buf[i]
  → #PF
  → kernel: allocate frame, zero all 4 KB      (§2.4's requirement)
  → install PTE, retry
  → the store lands                            (C++ semantics)
```

For value `0` the second write is redundant *as data*. For value `25` the
kernel's zeroing is still mandatory and then gets overwritten. Either way the
page is written twice.

### 3.2 Why that is not a 2× cost

This was the project's central early error.

The kernel zeroes page P, then the constructor writes page P — **the same cache
lines, touched twice**. The second pass hits L1/L2 because the kernel just
touched it. So:

- **Cache footprint** is `N`, not `2N`. You evict exactly as much either way.
- **DRAM traffic** is one pass, not two.
- What you genuinely waste is *store instructions*.

And crucially, the copy-on-write alternative doesn't fix it: `copy_page` reads
4 KB and writes 4 KB, dirtying the same lines. **For a page you write, both
approaches cost about the same.** The redundant zero was never the prize.

### 3.3 A corrected misconception: `rep stosb` vs non-temporal stores

The project notes originally claimed `rep stosq` "bypasses cache and writes
directly to memory." That is wrong, and the distinction changes the cost model:

- `rep stosb`**/**`stosq` **with ERMS** gets one real benefit — it avoids **RFO**
(read-for-ownership), so it does not fetch the old cache line before
overwriting it. That eliminates the read-before-write penalty. But it still
writes *into* the cache hierarchy.
- **Non-temporal stores** (`movntdq`, `_mm256_stream_si256`, plus `sfence`)
are the actual cache-bypass instructions. They write through
write-combining buffers and leave the cache alone.

glibc's `memset` switches to non-temporal stores above a size threshold tied to
cache size. This has a direct consequence measured later (§7.3): a 64 MB
`std::vector` fill is *already* cache-friendly, which makes it a tougher
baseline than expected.

---

## 4. Phase four — the design space

Five approaches were considered. Four were rejected, and the reasons are worth
more than the one that was chosen.

### 4.1 eBPF — structurally impossible

The original plan was to use eBPF to make the kernel fill faulting pages with a
caller-supplied value. It cannot work, for three independent reasons:

1. **No hook exists.** The zeroing happens in `do_anonymous_page()`
  (`mm/memory.c`) via `clear_page`/`folio_zero_user`. There is no BPF
   attachment point there and no `struct_ops` for fill decisions. Upstream work
   on BPF hooks for memory management (e.g. THP order selection) chooses
   between existing kernel behaviours; it does not supply data.
2. **The verifier forbids it.** A BPF program cannot write arbitrary kernel
  memory, cannot loop over a 4 KB folio it does not own, and cannot issue SIMD
   or non-temporal stores.
3. `bpf_override_return` **is not general control flow.** It only forces an
  error return, only on functions annotated `ALLOW_ERROR_INJECTION`, only with
   `CONFIG_BPF_KPROBE_OVERRIDE`. It is a fault-injection tool.
   `bpf_probe_write_user` writes *user* memory and is too late to help.

eBPF is the right tool for *observing* this problem, not changing it.

### 4.2 A kernel patch — what it would actually take

For contrast: attach a kernel-held pattern page to a VMA, settable through a
new syscall or `prctl` (plain `madvise` cannot carry a data payload, and
`MAP_*` flag space is nearly exhausted and cannot carry a pointer). Then branch
in `do_anonymous_page()`: if the VMA has a pattern, `copy_page` from it instead
of `clear_page`.

Cost is identical to zeroing — one page-sized write either way — and the user
fill disappears entirely. Security is fine: the page is wholly overwritten with
known bytes. Open design questions would be `fork`/CoW inheritance, THP (a
2 MB fault needs the tile replicated 512×), and what `MADV_DONTNEED`
re-faulting should produce — zeros or the pattern.

### 4.3 `MAP_UNINITIALIZED` — a dead end

It exists in the ABI, but is only honoured on nommu embedded kernels with
`CONFIG_MMAP_ALLOW_UNINITIALIZED`, and is silently ignored elsewhere. Skipping
the zero on an MMU kernel is an information leak; it will never be accepted.

### 4.4 `calloc`-aware allocation — only solves zero

`calloc` for large sizes gets fresh anonymous `mmap` memory, knows it is
already zero, and skips the `memset`. That is exactly the optimisation we
wanted — but only for the value `0`.

### 4.5 A custom `std::vector` allocator — a useful negative result

Tempting and worthless. `vector(n, val)` is *specified* to initialize every
element, and an allocator has no channel to say "the memory already holds
`val`." Give `std::vector` a pattern-filled buffer and it will dutifully
`memset` over all of it: every cost retained, every benefit lost.

This is why the deliverable had to be a **new container**, not an allocator.

### 4.6 memfd + `MAP_PRIVATE` pattern tiling — chosen

Build one page-tiled copy of the pattern in a `memfd_create()` file (anonymous,
RAM-backed via tmpfs, no disk, no filesystem path), then map that same tile
`MAP_PRIVATE` repeatedly across the whole range.

Untouched pages copy-on-write-share the tile: reads cost nothing, and writes
get a private frame whose copy source is already your pattern. It is the
user-space generalization of §2.4's `ZERO_PAGE`, aimed at an arbitrary value.

A regular file would also work and would break the design — first touch would
become a **major** fault reading from disk (§2.3), worse than the problem being
solved. The memfd is load-bearing.

---

## 5. Copy-on-write

### 5.1 The mechanism

Two virtual pages that logically need independent copies are pointed at one
physical frame, with **both PTEs marked read-only** even though the mapping is
logically writable. Reads just work.

A write traps on the read-only PTE. The handler consults the VMA, sees the
mapping *is* writable and that the read-only bit was a CoW marker rather than a
permission (so: not `SIGSEGV`, per §2.3), then allocates a frame, copies the
4 KB, repoints that PTE read-write, and drops the shared frame's refcount.

The refcount detail matters: once only one user remains, the kernel can mark
that PTE writable again and skip the copy entirely. **The last holder never
pays.**

### 5.2 Where it already lives

- `fork()` — a 1 GB parent does not copy 1 GB. This is why
`fork()`+`exec()` is cheap: `exec` discards the address space before much
copying happens.
- **Shared libraries** — `libc.so` is `MAP_PRIVATE` in every process on the
machine, all sharing one set of physical text pages; the writable data
segments CoW per process.
- `ZERO_PAGE` — §2.4's read-fault optimisation is CoW against a global
zero frame.

### 5.3 What the pattern trick contributes

Nothing mechanically new. CoW's copy source is normally "whatever was there";
here it is deliberately *your value*. The new private page arrives correct, so
the constructor's fill becomes unnecessary. The mechanism was already in the
kernel — the contribution is aiming it at more useful data.

---

## 6. Implementation notes

### 6.1 Tiling legality

The pattern must repeat exactly at every tile boundary, so a tile must be a
multiple of both `sizeof(T)` (no element straddles a boundary) and the page
size (so `mmap` can place it):

```
unit = lcm(sizeof(T), page_size)
```

`int` → 4096. A 12-byte struct → `lcm(12, 4096)` = 12288, a three-page tile.
`lcm` gives the **minimum legal** tile; any multiple of it is equally legal.
Choosing the actual size needs a separate rounding step, which is where the
`up(v, a) = ceil(v/a)*a` helper comes in — used both for the tile and for
rounding `n * sizeof(T)` up to a whole page for the mapping length.

### 6.2 The tile is a cache budget

The framing that resolved the sizing question: `tile_` is *the total memory the
container will ever pull into cache* — both when filling it once and when a
read-only consumer scans the entire vector. Every other page is untouched or a
CoW copy of it.

So `std::vector`'s cache damage is `O(N)` while `lazy_vector`'s is
`O(tile_)`, **independent of** `N`. You do not merely reduce the damage; you
*choose* it. The corollary is that `tile_ = N` throws the property away, which
rules out "just use a huge tile."

Detection is dynamic: `sysconf(_SC_LEVEL2_CACHE_SIZE)`, which glibc answers
from CPUID on x86 and therefore works under WSL2 where sysfs can be
incomplete, with a 256 KB fallback. `tile_fraction` (default 0.5) sets how much
of L2 to spend, and passing a value > 1.0 deliberately overflows L2 — which is
how §7.4's sweep is driven.

### 6.3 Reserve, then overwrite

`mmap` the whole range `PROT_NONE | MAP_ANONYMOUS | MAP_NORESERVE` first, then
`MAP_FIXED` each tile over it. The reservation claims the address space so
nothing else can land mid-range while the tiles are being placed; `MAP_FIXED`
replaces the placeholder atomically.

### 6.4 File descriptor lifetime

Each mapping holds its own reference to the memfd, so the fd can be closed once
the tiles are mapped. Unmapping the *fill window* (§4.6) is likewise safe
immediately: `munmap` drops a **view**, while the memfd still owns the pattern
bytes. This was a genuine point of confusion — the pattern lives in the file,
not in the temporary address range used to write it.

### 6.5 `reset()` — designed, not built

Re-`mmap` the tiles over the range and every CoW frame is freed, every page
reverts to the shared tile. Cost is `bytes/tile_` syscalls instead of a `memset`
of the whole buffer — and unlike `memset`, it **returns the physical memory**.
For a 1 GB buffer that is microseconds against hundreds of milliseconds.

### 6.6 RAII and the special members

The class owns a mapping and a file descriptor, so the compiler-generated copy
is actively dangerous. A memberwise copy duplicates `base_` and `fd_` as
*values*, giving two objects that each believe they own the same resources.
Then:

- `munmap` runs twice on the same range. The second call may tear down a
mapping that a later `mmap` placed in the freed hole — a segfault with no
visible connection to the cause.
- `close` runs twice. The second closes whatever that fd number has since
become, breaking unrelated I/O.
- Before that, the shorter-lived object's destructor leaves the other with a
dangling `base_`.

Deleting copy converts all of it into a compile error at the mistake site.
Writing a *correct* copy was rejected on design grounds: without soft-dirty
tracking the object cannot know which pages were written, so a deep copy must
materialize everything — destroying the laziness that is the point. It also
protects the benchmark, since a silent hidden copy would materialize the whole
vector and quietly corrupt every measurement.

This is the **rule of three/five**: a class with a resource-managing destructor
has wrong default copy operations.

One consequence caught the project out: **declaring a copy operation, even as**
`= delete`**, suppresses the implicit move constructor and move assignment.** The
type is currently neither copyable nor movable. Prvalue initialization still
works via C++17 guaranteed elision, so `auto v = lazy_vector<int>(n, 25);`
compiles, but `std::move` and container storage do not. A correct move must
leave the source with `base_ = nullptr` and `fd_ = -1` so its destructor is a
no-op; move *assignment* additionally has to release what the target already
holds. Related: `int fd_ = 0` was a real bug — combined with the `n == 0` early
return, the destructor's `fd_ >= 0` guard would `close(0)`, i.e. **stdin**.

### 6.7 Iterators for free

Raw pointers already satisfy `random_access_iterator` and
`contiguous_iterator`, and the standard specializes
`std::iterator_traits<T*>`. Since storage is one flat mapping, returning `T*`
from `begin()`/`end()` gives working range-`for`, `std::sort`, and all of
`std::ranges` with no adaptor. What is genuinely needed is the const overloads
(without them, `const lazy_vector&` iteration will not compile) and the member
typedefs generic code looks for.

`base_ + n_` is pointer arithmetic: the compiler scales by `sizeof(T)`, so it
means "advance `n_` elements," landing one past the last element. Forming that
address is legal; dereferencing it is not.

### 6.8 Constraints and non-goals

- `T` must be **trivially copyable** — elements are materialized by copying
raw bytes.
- Data must be **position-independent**. Self-referential pointers break,
because each mapping sits at a different virtual address; offset-based
relative pointers would be required.
- No cheap `resize`/`push_back` — growth invalidates the mapping. (A stretch
design: over-reserve `PROT_NONE` space and grow by mapping more tiles, making
`resize` a few syscalls with no data movement.)
- **VMA pressure.** `vm.max_map_count` caps the tile count. On the test box it
is 1048576, so with a 4 KB tile the hard wall is at 4 GiB; the stock default
of 65530 would fail at ~268 MB.
- Linux only, and `memfd_create` needs WSL2 rather than WSL1.
- Constructor leaks on its throw paths. A constructor that throws does not run
its destructor, so the fd and any successful mapping leak. Acceptable for a
POC; the fix is per-resource RAII wrappers.

---

## 7. Benchmarking

### 7.1 What to measure

Three quantities, chosen because they answer different questions:

- **Init time** — how long construction takes.
- **RSS** (`/proc/self/statm` field 2) — physical memory actually held.
- **Cache damage** — whether the initialization evicted a hot working set,
measured by timing a probe array before and after.

Minor faults (`getrusage`'s `ru_minflt`) were added as a page-materialization
counter.

### 7.2 Six measurement bugs found

This turned out to be the most instructive part of the project.

**Allocator reuse across iterations.** Early runs showed `std::vector` doing
`minflt=0, rss=0` for a fresh 1 MB allocation. Impossible — unless the pages
were already faulted. They were: the previous iteration's `free` left the pages
in glibc's arena, and the next allocation got them back warm. Only the *first*
`std::vector` in a process paid honest costs. Fixed by running one measurement
per process, or by working above the mmap threshold where `free` genuinely
`munmap`s.

**Measuring syscalls instead of initialization.** With `tile_` at the `lcm`
minimum of 4096, a 64 MB vector needed 16384 `mmap` calls. Measured init was
104.755 ms; 104.755 / 16384 = **6.4 µs per call**, which is simply the WSL2
`mmap` cost. The entire init column was syscall throughput.

**A bandwidth probe cannot see a latency effect.** The first probe read 512 KB
sequentially. Hot-vs-cold sequential bandwidth differs by perhaps 2×, and
prefetching hides most of that, yielding a useless ±20%. Replacing it with a
**pointer chase** over a random permutation — every load dependent on the
previous, so prefetch cannot help — moved the theoretical dynamic range to
5–15× (L2 latency ~15 cycles vs DRAM 200+).

**RSS counts PTEs, not unique frames.** The most important one. After a
*read-only* pass over `lazy_vector`, RSS rose by 65536 kB. No CoW occurred, so
no memory was allocated — all 16384 virtual pages point at the same 128 tile
pages. RSS counts *resident page-table entries*, so one physical page mapped at
128 addresses counts 128 times. `rss = 0` at construction is correct, but it
means "no PTEs installed," not "no memory committed," and RSS misreports the
moment anything is touched. `Pss` from `/proc/<pid>/smaps_rollup` is the honest
metric.

**Phase order changes what you measure.** In `materialize.cpp`, `write all`
ran after `read all`, so every page already had a read-only PTE and the write
phase took the **two-fault** read-then-write path (§7.5) — the worst case, not
the write-first best case.

**The platform's noise floor exceeded the signal.** Within a single sweep run,
the probe's *hot* baseline — the stable half of the comparison — varied from
0.29 ms to 0.78 ms, a 2.7× spread. `std::vector` itself measured 1.8× in one
run and 2.9× in the next. Timing sub-millisecond intervals inside a Hyper-V VM
on a busy desktop does not work, and no probe design fixes it.

### 7.3 Results that hold

64 MB of `int` (16384 pages), L2 = 256 KB, L3 = 6 MB.


|                           | `std::vector`          | `lazy_vector` (512 KB tile) |
| ------------------------- | ---------------------- | --------------------------- |
| construct                 | 85–272 ms              | **2.3–4.4 ms**              |
| faults during construct   | 16389                  | 128                         |
| RSS after construct       | 65604 kB               | **0 kB**                    |
| read all                  | 12.9–26.9 ms, 0 faults | 18.7–36.7 ms, 1024 faults   |
| write all (after reading) | 10.3–18.3 ms, 0 faults | 104–180 ms, 16384 faults    |


**Initialization no longer commits physical memory.** That is the headline, and
its real strength is not the ratio but its **determinism** — it reproduced
identically at every tile size across every run, because it does not depend on
timing. Stated honestly the figure is `tile_` against `N`, i.e. 512 KB against
64 MB (~128×), not infinity: the tile is real memory, it just does not appear in
RSS because the fill window is unmapped (§6.4) and the pages are charged to
shmem.

**Dense writes are a wash or worse.** Construct-plus-write-everything is
~91 ms for `lazy_vector` against ~85 ms for `std::vector`. Two contributing
facts: a CoW fault measured ~5.4 µs against ~3.4 µs for an anonymous fault
(`copy_page` reads *and* writes 4 KB where `clear_page` only writes), and
§3.3's non-temporal `memset` means the baseline's fill was already efficient.

### 7.4 The tile-size sweep

Sweeping `tile_fraction` produced a clean, reproducible U-curve:


| tile       | tiles (= `mmap` calls) | init run 1 | init run 2 |
| ---------- | ---------------------- | ---------- | ---------- |
| 32 KB      | 2048                   | 46.1 ms    | 25.0 ms    |
| 128 KB     | 512                    | 12.6 ms    | 10.0 ms    |
| **512 KB** | **128**                | **3.5 ms** | **3.1 ms** |
| 2 MB       | 32                     | 3.6 ms     | 6.6 ms     |
| 8 MB       | 8                      | 11.8 ms    | 30.9 ms    |
| 32 MB      | 2                      | 63.1 ms    | 116.6 ms   |


Syscall-bound on the left, fill-bound on the right. Modelling total init as

```
f(t) = a·t + b·(N/t)     a ≈ 0.24 ns/byte (fill), b ≈ 6.4 µs (mmap)
```

gives `t* = sqrt(bN/a) ≈ 1.3 MB` and `f(t*) = 2·sqrt(abN) ≈ 0.64 ms`. Measured
optimum was 512 KB–2 MB. Agreement within 2× on an estimated syscall cost is a
good match, and it makes the constant *derived* rather than tuned.

The shape matters more than the constant: **the optimal tile grows as**
`sqrt(N)`**, so the eager fill is** `O(sqrt(N))` **while the baseline is** `O(N)`**.**
Eagerly filling a tile can never dominate, because the tile you need grows only
as the square root of the data. At 1 GB the optimum is ~5 MB — 0.5% of the
array.

One unresolved tension: the latency optimum (512 KB) is **2× L2**, so it
conflicts with §6.2's cache-budget cap of 128 KB. Honouring the cap costs ~4×
init time. Because the cache probe could not resolve whether that matters
(§7.2), the choice is currently made on theory, not evidence.

### 7.5 Fault-around — an unexpected finding

Reading all 16384 pages produced only **1024** faults, not 16384. That is
Linux's **fault-around**: on a read fault against a file mapping, the kernel
opportunistically maps ~16 surrounding pages in one trap
(`fault_around_bytes`, default 64 KB). 16384/16 = 1024 exactly.

This makes read materialization far cheaper than predicted and is specific to
*file-backed* mappings — an advantage of the memfd approach that was not part
of the original design reasoning. Anonymous mappings do not get it.

It also sets up the workload where the container should be strongest, which
this benchmark never measured: **repeated** read passes. The first pass pays
1024 faults but touches only `tile_` bytes; subsequent passes pay nothing and
still touch only `tile_`, while `std::vector` re-streams 64 MB from DRAM every
time.

---

## 8. Honest conclusions

**What it is.** `lazy_vector` trades deterministic up-front allocation for
pay-per-use allocation. Cost becomes proportional to pages *written*, and
independent of pages merely *read*.

**Where it wins.** Large `n`, a repeating value, and sparse or read-mostly
access. Initialization is ~30× faster and commits ~128× less memory. A
read-mostly array has a cache footprint of one tile regardless of `n`.

**Where it loses.** Dense writes are a wash or slightly worse. Read-then-write
costs two faults per page where `std::vector` costs one, because
`std::vector`'s single fault is *prepaid by the constructor's write* — a page
whose first touch is a write takes one fault; read-first then written takes two.
Small allocations are pure loss: below glibc's 128 KB threshold there is no
per-allocation faulting to save, and the syscall overhead is a net cost.

**The failure mode changes, and not for the better.** `std::vector` acquires
everything during construction, so exhaustion produces a catchable
`std::bad_alloc` at one well-defined point. `lazy_vector` acquires memory
during ordinary writes, and a page fault has nowhere to return an error — so
running out at page 180000 means the **OOM killer**, with no exception, no
unwinding, no cleanup. This is the standard overcommit trade, and it is why
systems needing predictability deliberately pre-fault with `MAP_POPULATE` or
`mlock`.

---

## 9. Prior art

The value-zero case is thoroughly solved and shipped:

- **glibc** `calloc` — skips the `memset` for large fresh `mmap`s (§4.4).
- **Rust** — `vec![0u8; n]` is special-cased to `alloc_zeroed`; `vec![25u8; n]`
is not and does the full fill. Our idea is the generalization of exactly that
special case.
- **NumPy** — `np.zeros(10**9)` returns near-instantly with near-zero RSS;
`np.full(10**9, 25)` writes the whole gigabyte. A two-line demonstration of
the thesis in a language most people already know.
- **The kernel's** `ZERO_PAGE` (§2.4) — read-fault sharing, natively.

Mapping one memfd at multiple offsets is also established, for other ends: the
"magic ring buffer" maps a buffer twice at adjacent addresses so a wrapping
read is one `memcpy`; `fork`-based CoW snapshotting (Redis `BGSAVE`);
`MADV_DONTNEED` bulk reset inside JVMs and allocators.

**Not found shipping anywhere: the arbitrary-repeated-value case as a
container.** No `std::` facility, no mainstream C++ library. Plausibly because
the payoff needs large `n`, a repeating value, *and* sparse access
simultaneously, while the constraints (§6.8) are real. Sparse containers such
as `boost::numeric::ublas` are data-structure sparse, not page-level lazy.

Honest positioning: *Linux and libc already do this for zero; this is the
general case for any repeated value, with measurements of where it pays.*

---

## 10. Skills used and learned

**Tools.** `/proc/<pid>/maps` and `smaps` (region classification, diffing
snapshots); `getrusage` fault counters; `/proc/self/statm` and the discovery of
what RSS actually counts; `lscpu --caches`; `sysctl` for `vm.max_map_count` and
`vm.overcommit_memory`; `strace` for syscall attribution; `sysconf` for runtime
page and cache geometry. `perf` was wanted throughout and was unavailable —
WSL2 does not expose a virtual PMU.

**Kernel and systems.** The `brk`/`mmap` split and per-request threshold
decisions; VMA lifecycle and lazy population; the page-fault handler's decision
tree; minor vs major fault causes; anonymous zero-fill as a security invariant;
copy-on-write end to end including refcount behaviour; `memfd`/tmpfs;
page-table vs TLB responsibility; fault-around; overcommit and OOM semantics.

**Microarchitecture.** Cache hierarchy sizes and why they bound design choices;
RFO and how ERMS avoids it; non-temporal stores as the real cache-bypass
mechanism; why a dependent-load pointer chase measures latency while a
sequential scan measures bandwidth.

**C++.** Rule of three/five; that declaring copy suppresses implicit move;
C++17 guaranteed elision and what it does *not* cover; why raw pointers are
conforming random-access iterators; `std::lcm` for alignment periodicity;
`is_trivially_copyable_v` as a real precondition rather than decoration; and the
negative result that a custom allocator cannot capture this optimisation.

**Measurement discipline.** The single largest lesson. Allocator state leaking
between iterations; one dominant constant hiding the effect under study; probe
design that cannot resolve the quantity of interest; a metric that does not mean
what its name suggests; phase ordering silently changing the experiment;
and knowing when the platform's noise floor exceeds the signal and the honest
move is to report that rather than pick a favourable run.

---

## 11. Open work

1. **Switch RSS to** `Pss` from `/proc/<pid>/smaps_rollup`, so the memory
  column stays truthful once pages are touched (§7.2).
2. **Measure cache damage credibly** — `taskset` pinning plus the median of
  many full warm→init→cold trials, or move to native Linux, or get `perf`
   working. Until then §6.2's cap is theory.
3. **Split write-first from read-then-write** into separate phases on fresh
  vectors, to measure the one-fault and two-fault paths independently.
4. **Benchmark repeated read passes** (§7.5) — the workload most favourable to
  the design, currently unmeasured.
5. **Implement** `reset()` (§6.5). Likely the most striking single number
  available.
6. **Add move constructor and move assignment**, and const `begin`/`end`.
7. **Try** `MADV_HUGEPAGE` — 512× fewer faults and much less TLB pressure; may
  change the dense-write verdict, since at 2 MB the baseline's second pass no
   longer fits in cache.
8. **Test a non-page-tiling** `T` (e.g. 12 bytes) to exercise the `lcm` path.
9. Fix constructor leak paths; add `MFD_CLOEXEC`; rename `lazy_vector.cpp` to
  `.hpp`.

---

---

# LinkedIn drafts

Three posts, intended to be published a few days apart so the series reads as a
series rather than a burst.

---

## Post 1 — the hook

`np.zeros(10**9)` **returns instantly and uses almost no memory.**
`np.full(10**9, 25)` **takes seconds and eats 8 GB.**

Same shape. Same dtype. Same amount of "data." Wildly different cost.

The reason is that zero is special to your operating system, and 25 is not.

When you allocate a large array, the kernel hands back virtual address space
and nothing else — no physical memory at all. Pages get created one at a time,
on first touch. And because a recycled page of RAM might still contain another
process's data, the kernel is obliged to zero every page before you can see it.

So an array of zeros can be *free*: the memory is already zero by the time it
reaches you, and `calloc` knows it and skips the fill. Rust special-cases
`vec![0; n]` for exactly this reason. The Linux kernel goes further and keeps a
single shared all-zeros page that every untouched read-only page can point at.

An array of 25s gets none of that. The kernel still zeroes every page — it has
to — and then your constructor overwrites all of it with 25.

I got curious about how far that "zero is free" trick could be generalized. Can
you make `vector<int>(n, 25)` as lazy as `vector<int>(n, 0)`?

It turns out you can, using copy-on-write and a trick with `memfd`. It also
turns out the win is not where I expected, the thing I set out to eliminate was
never the real cost, and one of my measurements was quietly lying to me.

Writing that up next.

`#systems` `#cpp` `#linux` `#performance`

---

## Post 2 — what I built and what it measured

**I built a** `std::vector` **that doesn't initialize anything, and measured where
that's a good idea.**

The trick, in three steps:

1. Create a `memfd` — an anonymous, RAM-backed file — and fill **one** small
  tile with your repeating value.
2. Reserve the full range of address space you need.
3. Map that same tile `MAP_PRIVATE` over the whole range, repeatedly.

Now every page of a 64 MB "array" points at the same tile of physical memory.
Reads cost nothing. And when you *write* to a page, copy-on-write gives you a
private copy — whose source is already full of 25, so nobody ever has to fill
it.

On the C++ side: the type owns a mapping and a file descriptor, so copy and
move are deleted. A compiler-generated copy would alias both handles, and the
second destructor would `munmap`/`close` somebody else's resources. Rule of
five, not a style choice.

Results on 64 MB of `int`, with the tile the constructor actually picked
(128 KB — half of this machine's 256 KB L2, 512 mappings):

- **Construction: ~90 ms → ~3 ms.** About 30× faster.
- **Physical memory at construction: 64 MB → 128 KB.** ~512× less.
- **Page faults during construction: ~16384 → ~32.** The tile is 32 pages;
those are the only pages that exist yet.

Then the honest half.

**Dense writes are a wash.** If you write every page, you materialize all 64 MB
anyway, and a copy-on-write fault is *more* expensive than a plain one
(`copy_page` reads and writes 4 KB; `clear_page` only writes). Total time comes
out about even.

**Read-then-write costs two faults per page instead of one.** `std::vector`
gets away with one because its constructor's write *is* the first touch. Mine
doesn't control the order — the caller does.

**And the failure mode gets worse.** `std::vector` fails at construction with a
catchable `bad_alloc`. Mine fails on an ordinary write, deep in your loop,
where a page fault has nowhere to return an error. That means the OOM killer,
not an exception.

The numbers above are from a clean construct-and-stop. How I measured them —
and the places the measurement lied — is the next post. The short version: I
timed construction, counted faults, and read RSS. Cache damage is the claim I
could not prove.

One thing that *was* clean: tile size has a real optimum. Too small and you
drown in `mmap` syscalls; too large and the eager fill you were avoiding comes
back. Sweeping it produced a U-curve. Modelling it as `a·t + b·N/t` predicts
`t* = sqrt(bN/a) ≈ 1.3 MB`; the latency minimum sat around 512 KB–2 MB. The
useful part is that `t*` grows as `sqrt(N)`, so the eager fill is `O(sqrt(N))`
while the baseline is `O(N)` — it can never dominate.

That's why the tile is sized at runtime, not hardcoded. The constructor reads
L2 via `sysconf` and takes a fraction of it (default half), then rounds up to a
legal multiple of `lcm(sizeof(T), page)`. On this box that is 128 KB. Syscall
count falls out of the cache budget, not the other way around. Pass a fraction

> 1.0 and you deliberately overflow L2 — which is how the U-curve got drawn.
> The latency-optimal tile is larger than half of L2; I kept the cache cap
> anyway, because the tile *is* the cache budget.

Not a `std::vector` replacement. A demonstration that "allocate" and
"initialize" don't have to be the same operation.

`#cpp` `#linux` `#systemsprogramming` `#performance`

---

## Post 3 — how I measured it, and the probe that wouldn't sit still

**Two questions. One of them I could time. The other I could not.**

I wanted to know, for a 64 MB `vector<int>(n, 25)` versus `lazy_vector`:

1. How long does construction take?
2. How much of the CPU's cache does that construction destroy?

Same process, same size, construct-and-stop. No consumer loop. The vector is
still alive when I take the second measurement, so teardown is not in the
number.

**Construction is a stopwatch.** Warm nothing. Start the clock, call the
constructor, stop the clock. That is the whole init column. On this machine
that is ~90 ms for `std::vector` and ~3 ms for `lazy_vector` with a 128 KB
tile. That number is loud enough to survive WSL2 jitter. I trust it.

**Cache is the one I actually care about**, and it has to be inferred. I
cannot read "was line X in L2" from userspace. So I planted a witness.

The witness is a 256 KB array — sized to sit in this CPU's 256 KB L2. Before
the constructor runs, I walk it until it is as hot as I can get it, and I
keep the fastest walk. That is the *hot* time: the cost of the working set
when it is definitely cache-resident.

Then I construct the vector under test.

Then I walk the witness **once**. Walking it twice would just pull it back
in and hide the eviction. If construction flushed L2, that single walk
misses to L3 or DRAM and comes back slower. The ratio cold/hot is the
damage.

It is a pointer chase, not a linear scan. I shuffle the 32,768 slots into
one random cycle; each load's address is the value of the previous load.
The prefetcher cannot hide a miss, because it does not know the next
address until the current load returns. Sequential bandwidth would have
shown maybe 20%. Latency should have shown 5–15× if the probe fell out of
L2 entirely.

That is what I was trying to *say* with the probe: `std::vector` writes 64
MB and should evict the witness; `lazy_vector` writes 128 KB and should
leave it almost untouched.

**What actually happened.** Init time is a real win. Construction is about
30× faster, and the only bytes `lazy_vector` eagerly writes are the tile.
Dense writes still lose or break even — if you touch every page you pay
copy-on-write faults and you materialize the whole 64 MB anyway. I already
said that in the last post.

The probe did not say the rest. Hot time, which is supposed to be the
stable half, jumped around inside a single run. Cold/hot for `std::vector`
came back 1.5× in one run and 3.6× in the next. `lazy_vector` sometimes
looked *worse* than the baseline, sometimes identical. The ratio does not
track tile size, does not track init time, and does not repeat.

I can list suspects. I am on WSL2, so the clock is in a VM. The interval
is a fraction of a millisecond. glibc's `memset` for a 64 MB fill may
already be using non-temporal stores, so the baseline pollutes less than
the textbook story. A 256 KB probe on a 256 KB L2 has no margin — anything
the kernel touches during `mmap` can push it out. I cannot tell which of
those is in charge, because I cannot see the cache.

**So here is the actual question.**

How do you debug what the cache is doing from userspace when the only
instrument you have is "this array got slower"? What would you read —
perf events, `perf mem`, PMCs, something in `/sys` — to find out whether
the probe is still in L2, whether it was the fill that evicted it, or
whether the VM just scheduled you off the core for 200 µs? And if a timed
pointer chase is the wrong proxy, what is the right one?

I am not looking for "buy a bare metal box," even if that is the answer. I
want to know what the cache was actually doing while the probe lied.

`#performance` `#linux` `#benchmarking` `#systemsprogramming`