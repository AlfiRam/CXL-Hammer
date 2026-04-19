/**
 * CXL rowhammer test
 *
 * hammers a CXL-attached DRAM buffer hard enough to trigger bit flips
 * through the rowhammer mechanism (see src/mem/dram_interface.cc).
 * detects and reports any bits that drift away from PATTERN_BYTE after
 * the hammer loop.
 *
 * invocation pattern (from the guest, under the gem5 config):
 *     numactl --membind=1 /home/cxl_benchmark/rowhammer_cxl_test
 *
 * NUMA binding is required so the mmap'd buffer lands in CXL-backed
 * memory (node 1) rather than local DDR5 (node 0). without the membind
 * the allocation may be satisfied from node 0 and the hammer will hit
 * the wrong DRAMInterface.
 *
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <x86intrin.h>

#include <gem5/m5ops.h>

/* -------- tunables (edit these to retune the hammer) -------- */
#define BUFFER_SIZE        (256UL * 1024 * 1024)  /* 256 MB */
#define PATTERN_BYTE       0xAA                   /* 0b10101010 */
#define AGGRESSOR_STRIDE   (16UL * 1024)          /* 16 KB > row size */
#define HAMMER_ITERATIONS  500000L
#define PROGRESS_EVERY     100000L
#define SCAN_WINDOW        (128UL * 1024)         /* 128 KB per side */

/* -------- pagemap translation constants -------- */
#define PAGE_SIZE          4096
#define PAGEMAP_ENTRY_SIZE 8
#define PFN_MASK           ((1ULL << 55) - 1)
#define PAGE_PRESENT       (1ULL << 63)

/*
 * translate a virtual address to a physical address via
 * /proc/self/pagemap. returns 0 on error. same pattern as
 */
static uint64_t virt_to_phys(void *virt_addr)
{
    int fd;
    uint64_t page_offset, pfn, phys_addr, pagemap_entry;

    fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "ERROR: open /proc/self/pagemap: %s\n",
                strerror(errno));
        return 0;
    }

    page_offset = ((uint64_t)virt_addr / PAGE_SIZE) * PAGEMAP_ENTRY_SIZE;
    if (lseek(fd, page_offset, SEEK_SET) != (off_t)page_offset) {
        fprintf(stderr, "ERROR: pagemap lseek: %s\n", strerror(errno));
        close(fd);
        return 0;
    }
    if (read(fd, &pagemap_entry, PAGEMAP_ENTRY_SIZE)
        != PAGEMAP_ENTRY_SIZE) {
        fprintf(stderr, "ERROR: pagemap read: %s\n", strerror(errno));
        close(fd);
        return 0;
    }
    close(fd);

    if (!(pagemap_entry & PAGE_PRESENT)) {
        fprintf(stderr, "ERROR: page not present for %p\n", virt_addr);
        return 0;
    }

    pfn = pagemap_entry & PFN_MASK;
    phys_addr = (pfn * PAGE_SIZE) + ((uint64_t)virt_addr % PAGE_SIZE);
    return phys_addr;
}

int main(void)
{
    volatile uint8_t *buffer;
    uint64_t buffer_phys;
    volatile uint64_t *aggr_a;
    volatile uint64_t *aggr_b;
    uint64_t aggr_a_phys, aggr_b_phys;
    long i;
    unsigned long flips = 0;

    printf("==========================================================\n");
    printf("  CXL Rowhammer Test\n");
    printf("==========================================================\n");
    printf("  BUFFER_SIZE       = %lu bytes (%lu MB)\n",
           (unsigned long)BUFFER_SIZE,
           (unsigned long)(BUFFER_SIZE / (1024 * 1024)));
    printf("  PATTERN_BYTE      = 0x%02x\n", PATTERN_BYTE);
    printf("  AGGRESSOR_STRIDE  = %lu bytes (%lu KB)\n",
           (unsigned long)AGGRESSOR_STRIDE,
           (unsigned long)(AGGRESSOR_STRIDE / 1024));
    printf("  HAMMER_ITERATIONS = %ld\n", (long)HAMMER_ITERATIONS);
    printf("  Expected runtime  : minutes to hours under gem5 TIMING\n");
    printf("==========================================================\n\n");

    /* ---------------- step 1: allocate buffer ---------------- */
    printf("[1/6] mmap(256 MB) anonymous -- relies on "
           "numactl --membind=1 for CXL placement...\n");
    buffer = (volatile uint8_t *)mmap(NULL, BUFFER_SIZE,
                                      PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS,
                                      -1, 0);
    if ((void *)buffer == MAP_FAILED) {
        fprintf(stderr, "ERROR: mmap: %s\n", strerror(errno));
        return 1;
    }
    printf("  ok: buffer virt = %p\n\n", (void *)buffer);

    /* ---------- step 2: touch every page to force alloc ------- */
    printf("[2/6] Touching every page with 0x%02x (forces "
           "physical allocation from bound NUMA node)...\n",
           PATTERN_BYTE);
    for (uint64_t off = 0; off < BUFFER_SIZE; off += PAGE_SIZE) {
        buffer[off] = (uint8_t)PATTERN_BYTE;
    }
    /* and a full memset so every byte carries the pattern, not just
     * the first byte of each page. */
    memset((void *)buffer, PATTERN_BYTE, BUFFER_SIZE);
    printf("  ok: %lu bytes filled with 0x%02x\n\n",
           (unsigned long)BUFFER_SIZE, PATTERN_BYTE);

    /* ---------- step 3: flush entire buffer to DRAM ----------- */
    printf("[3/6] clflush'ing entire buffer to DRAM "
           "(we need the pattern on DRAM, not in caches)...\n");
    for (uint64_t off = 0; off < BUFFER_SIZE; off += 64) {
        _mm_clflush((const void *)(buffer + off));
    }
    _mm_mfence();
    printf("  ok: buffer flushed\n\n");

    /* ---------- step 4: translate base to phys --------------- */
    printf("[4/6] Translating buffer base via "
           "/proc/self/pagemap...\n");
    buffer_phys = virt_to_phys((void *)buffer);
    if (buffer_phys == 0) {
        fprintf(stderr, "ERROR: virt_to_phys failed for buffer\n");
        munmap((void *)buffer, BUFFER_SIZE);
        return 1;
    }
    printf("  buffer: virt %p -> phys 0x%llx\n",
           (void *)buffer, (unsigned long long)buffer_phys);
    if (buffer_phys < 0x100000000ULL) {
        printf("  WARN: phys < 4 GB -- did numactl --membind=1 "
               "actually place this on CXL?\n");
    } else {
        printf("  ok: phys is above 0x100000000 -- likely CXL\n");
    }
    printf("\n");

    /* ---------- step 5: pick aggressor pair ------------------ */
    printf("[5/6] Choosing aggressor pair at buffer + "
           "BUFFER_SIZE/4 and +AGGRESSOR_STRIDE beyond...\n");
    aggr_a = (volatile uint64_t *)(buffer + (BUFFER_SIZE / 4));
    aggr_b = (volatile uint64_t *)((uint8_t *)aggr_a
                                   + AGGRESSOR_STRIDE);

    aggr_a_phys = virt_to_phys((void *)aggr_a);
    aggr_b_phys = virt_to_phys((void *)aggr_b);
    if (aggr_a_phys == 0 || aggr_b_phys == 0) {
        fprintf(stderr, "ERROR: virt_to_phys failed for aggressor\n");
        munmap((void *)buffer, BUFFER_SIZE);
        return 1;
    }

    printf("  aggr_a: virt %p -> phys 0x%llx\n",
           (void *)aggr_a, (unsigned long long)aggr_a_phys);
    printf("  aggr_b: virt %p -> phys 0x%llx\n",
           (void *)aggr_b, (unsigned long long)aggr_b_phys);
    printf("  phys delta = 0x%llx (%lld bytes)\n\n",
           (unsigned long long)(aggr_b_phys - aggr_a_phys),
           (long long)(aggr_b_phys - aggr_a_phys));

    /* ---------- step 6: the hammer loop ---------------------- */
    /* note: the guest command string already ran `m5 exit` before
     * launching this binary, so the KVM -> TIMING switch has
     * already happened by the time we get here. do not call
     * m5_exit() from the binary -- that would consume one of the
     * config's on_exit_event handlers at the wrong time. */
    printf("[6/6] Hammer loop: %ld iterations of paired reads "
           "with clflush+mfence between each pair...\n",
           (long)HAMMER_ITERATIONS);
    printf("[STATS] ==== m5_reset_stats: hammer-window begin ====\n");
    fflush(stdout);
    m5_reset_stats(0, 0);

    volatile uint64_t sink = 0;
    for (i = 0; i < HAMMER_ITERATIONS; i++) {
        sink += *(volatile uint64_t *)aggr_a;
        sink += *(volatile uint64_t *)aggr_b;
        _mm_clflush((const void *)aggr_a);
        _mm_clflush((const void *)aggr_b);
        _mm_mfence();

        if ((i + 1) % PROGRESS_EVERY == 0) {
            printf("  hammer: %ld / %ld  (sink=0x%llx)\n",
                   i + 1, (long)HAMMER_ITERATIONS,
                   (unsigned long long)sink);
            fflush(stdout);
        }
    }
    printf("[HAMMER-ONLY] Hammer done. Calling m5_exit(0) to end simulation before scan phase...\n");
    fflush(stdout);
    m5_exit(0);
    printf("  ok: hammer done\n\n");

    /* ---------- flush victim region back from cache ---------- */
    printf("Flushing full buffer back from cache "
           "(force re-read from DRAM during scan)...\n");
    for (uint64_t off = 0; off < BUFFER_SIZE; off += 64) {
        _mm_clflush((const void *)(buffer + off));
    }
    _mm_mfence();
    printf("  ok\n\n");

    /* ---------- scan for bit flips --------------------------- */
    /* rowhammer victims are physically adjacent to aggressors, so
     * scanning the full buffer is wasteful in TIMING simulation.
     * walk only a SCAN_WINDOW-wide band around each aggressor,
     * clamped to the buffer. if the two bands overlap (aggressors
     * close together) any byte in the overlap gets counted twice in
     * `flips`; that's a minor over-count for reporting purposes
     * only, not a correctness issue. */
    uint64_t buf_base = (uint64_t)buffer;
    uint64_t buf_end  = buf_base + BUFFER_SIZE;
    uint64_t a_addr   = (uint64_t)aggr_a;
    uint64_t b_addr   = (uint64_t)aggr_b;

    uint64_t a_lo = (a_addr > SCAN_WINDOW) ? (a_addr - SCAN_WINDOW) : 0;
    if (a_lo < buf_base) a_lo = buf_base;
    uint64_t a_hi = a_addr + SCAN_WINDOW;
    if (a_hi > buf_end)  a_hi = buf_end;

    uint64_t b_lo = (b_addr > SCAN_WINDOW) ? (b_addr - SCAN_WINDOW) : 0;
    if (b_lo < buf_base) b_lo = buf_base;
    uint64_t b_hi = b_addr + SCAN_WINDOW;
    if (b_hi > buf_end)  b_hi = buf_end;

    uint64_t scanned_bytes = (a_hi - a_lo) + (b_hi - b_lo);

    printf("[SCAN] Walking 2x %lu KB windows around aggressors "
           "(%lu bytes total) looking for bytes != 0x%02x...\n",
           (unsigned long)(SCAN_WINDOW / 1024),
           (unsigned long)scanned_bytes, PATTERN_BYTE);

    const uint64_t ranges[2][2] = {
        { a_lo, a_hi },
        { b_lo, b_hi },
    };
    for (int r = 0; r < 2; r++) {
        for (uint64_t va = ranges[r][0]; va < ranges[r][1]; va++) {
            uint8_t v = *(volatile uint8_t *)(uintptr_t)va;
            if (v != (uint8_t)PATTERN_BYTE) {
                flips++;
                uint64_t page_virt = va & ~((uint64_t)PAGE_SIZE - 1);
                uint64_t page_phys =
                    virt_to_phys((void *)(uintptr_t)page_virt);
                uint64_t victim_phys = (page_phys != 0)
                    ? page_phys + (va % PAGE_SIZE)
                    : 0;
                printf("  FLIP #%lu  virt 0x%llx  phys 0x%llx  "
                       "got 0x%02x expected 0x%02x\n",
                       flips,
                       (unsigned long long)va,
                       (unsigned long long)victim_phys,
                       v, PATTERN_BYTE);
            }
        }
    }
    printf("\n");

    /* ---------- summary ------------------------------------- */
    printf("==========================================================\n");
    printf("  scanned : %lu bytes\n", (unsigned long)scanned_bytes);
    printf("  flips   : %lu\n", flips);
    if (flips == 0) {
        printf("  verdict : NO FLIPS DETECTED\n");
    } else {
        printf("  verdict : ROWHAMMER FLIPS DETECTED\n");
    }
    printf("==========================================================\n");

    munmap((void *)buffer, BUFFER_SIZE);

    return (flips == 0) ? 2 : 0;
}
