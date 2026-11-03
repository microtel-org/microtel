// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// LD_PRELOAD allocation call-stack recorder for the parity study
// (bench/parity/README.md), the call-site companion to malloc_count.c.
//
// Every call to malloc, calloc, realloc, posix_memalign, aligned_alloc,
// memalign, valloc and pvalloc records glibc backtrace() (DWARF unwinding, so
// libraries built without frame pointers are walked too) and counts it under
// (thread id, call stack). On SIGUSR2 a dedicated thread writes
// $MALLOC_STACKS_OUT.<seq> with:
//
//     map <line of /proc/self/maps>
//     task <tid> <comm>                  (threads alive at the time)
//     stack <tid> <count> <pc> <pc> ...  (return addresses, innermost first)
//
// The constructor blocks SIGUSR2 in the initial thread before the program
// starts, so every thread the program creates inherits the block and only the
// dumper thread (sigwait) receives it. Overhead is large (one unwind per
// allocation): use it for call sites and counts, never for timing.
//
// Build:  cc -O2 -fPIC -shared -o malloc_stacks.so malloc_stacks.c -lpthread

#define _GNU_SOURCE
#include <dirent.h>
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

extern void* __libc_malloc(size_t);
extern void* __libc_calloc(size_t, size_t);
extern void* __libc_realloc(void*, size_t);
extern void* __libc_memalign(size_t, size_t);
extern void* __libc_valloc(size_t);
extern void* __libc_pvalloc(size_t);

#define MAX_DEPTH 32
#define TABLE_BITS 16
#define TABLE_SIZE (1u << TABLE_BITS)

struct entry
{
    long tid;
    int depth;
    unsigned long count;
    void* pcs[MAX_DEPTH];
};

static struct entry g_table[TABLE_SIZE];
static unsigned long g_overflow;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_ready;
static __thread int t_busy __attribute__((tls_model("initial-exec")));
static __thread long t_tid __attribute__((tls_model("initial-exec")));

static uint64_t hash(long tid, void* const* pcs, int depth)
{
    uint64_t h = 1469598103934665603ull ^ (uint64_t)tid;
    for (int i = 0; i < depth; ++i)
    {
        h = (h ^ (uint64_t)(uintptr_t)pcs[i]) * 1099511628211ull;
    }
    return h;
}

static void record(void)
{
    if (!g_ready || t_busy)
    {
        return;
    }
    t_busy = 1;
    if (t_tid == 0)
    {
        t_tid = (long)syscall(SYS_gettid);
    }
    void* raw[MAX_DEPTH + 2];
    int n = backtrace(raw, MAX_DEPTH + 2);
    // Frame 0 is record(), frame 1 the interposed allocator.
    void* const* pcs = raw + 2;
    const int depth = n > 2 ? n - 2 : 0;
    const uint64_t h = hash(t_tid, pcs, depth);
    pthread_mutex_lock(&g_mu);
    int stored = 0;
    for (unsigned probe = 0; probe < TABLE_SIZE && !stored; ++probe)
    {
        struct entry* e = &g_table[(h + probe) & (TABLE_SIZE - 1)];
        if (e->count == 0)
        {
            e->tid = t_tid;
            e->depth = depth;
            memcpy(e->pcs, pcs, sizeof(void*) * (size_t)depth);
            e->count = 1;
            stored = 1;
        }
        else if (e->tid == t_tid && e->depth == depth && memcmp(e->pcs, pcs, sizeof(void*) * (size_t)depth) == 0)
        {
            ++e->count;
            stored = 1;
        }
    }
    if (!stored)
    {
        ++g_overflow;
    }
    pthread_mutex_unlock(&g_mu);
    t_busy = 0;
}

static void dump(unsigned seq)
{
    const char* base = getenv("MALLOC_STACKS_OUT");
    if (base == NULL)
    {
        return;
    }
    char path[512];
    snprintf(path, sizeof path, "%s.%u", base, seq);
    FILE* f = fopen(path, "w");
    if (f == NULL)
    {
        return;
    }
    FILE* maps = fopen("/proc/self/maps", "r");
    char line[1024];
    while (maps != NULL && fgets(line, sizeof line, maps) != NULL)
    {
        fprintf(f, "map %s", line);
    }
    if (maps != NULL)
    {
        fclose(maps);
    }
    DIR* d = opendir("/proc/self/task");
    struct dirent* de;
    while (d != NULL && (de = readdir(d)) != NULL)
    {
        if (de->d_name[0] == '.')
        {
            continue;
        }
        char cpath[300];
        snprintf(cpath, sizeof cpath, "/proc/self/task/%s/comm", de->d_name);
        FILE* c = fopen(cpath, "r");
        char comm[64] = "?";
        if (c != NULL)
        {
            if (fgets(comm, sizeof comm, c) != NULL)
            {
                comm[strcspn(comm, "\n")] = '\0';
            }
            fclose(c);
        }
        fprintf(f, "task %s %s\n", de->d_name, comm);
    }
    if (d != NULL)
    {
        closedir(d);
    }
    pthread_mutex_lock(&g_mu);
    for (unsigned i = 0; i < TABLE_SIZE; ++i)
    {
        const struct entry* e = &g_table[i];
        if (e->count == 0)
        {
            continue;
        }
        fprintf(f, "stack %ld %lu", e->tid, e->count);
        for (int k = 0; k < e->depth; ++k)
        {
            fprintf(f, " %p", e->pcs[k]);
        }
        fputc('\n', f);
    }
    fprintf(f, "overflow %lu\n", g_overflow);
    pthread_mutex_unlock(&g_mu);
    fclose(f);
    char done[512];
    snprintf(done, sizeof done, "%s.%u.done", base, seq);
    FILE* m = fopen(done, "w");
    if (m != NULL)
    {
        fclose(m);
    }
}

static void* dumper(void* arg)
{
    (void)arg;
    t_busy = 1;  // never record the dumper's own allocations
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR2);
    for (unsigned seq = 0;; ++seq)
    {
        int sig = 0;
        if (sigwait(&set, &sig) != 0)
        {
            return NULL;
        }
        dump(seq);
    }
}

__attribute__((constructor)) static void init(void)
{
    t_busy = 1;
    void* warm[4];
    backtrace(warm, 4);  // loads libgcc_s before any recorded call
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR2);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
    pthread_t t;
    pthread_create(&t, NULL, dumper, NULL);
    pthread_detach(t);
    t_busy = 0;
    g_ready = 1;
}

void* malloc(size_t n)
{
    record();
    return __libc_malloc(n);
}

void* calloc(size_t a, size_t b)
{
    record();
    return __libc_calloc(a, b);
}

void* realloc(void* p, size_t n)
{
    record();
    return __libc_realloc(p, n);
}

void* memalign(size_t al, size_t n)
{
    record();
    return __libc_memalign(al, n);
}

void* aligned_alloc(size_t al, size_t n)
{
    record();
    return __libc_memalign(al, n);
}

int posix_memalign(void** out, size_t al, size_t n)
{
    record();
    void* p = __libc_memalign(al, n);
    if (p == NULL)
    {
        return 12;  // ENOMEM
    }
    *out = p;
    return 0;
}

void* valloc(size_t n)
{
    record();
    return __libc_valloc(n);
}

void* pvalloc(size_t n)
{
    record();
    return __libc_pvalloc(n);
}
