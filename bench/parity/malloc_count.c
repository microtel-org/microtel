// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// LD_PRELOAD allocation counter for the parity study (bench/parity/README.md).
//
// Counts every call to malloc, calloc, realloc, posix_memalign, aligned_alloc,
// memalign, valloc and pvalloc, per thread. operator new reaches malloc through
// the PLT in libstdc++, so C++ allocations are counted too. On SIGUSR2 it
// writes one line per thread seen so far to stderr:
//
//     malloc_count tid=<tid> comm=<name> allocs=<n>
//
// bracketed by "malloc_count begin <seq>" / "malloc_count end <seq>". Threads
// that have exited keep their line (comm is then "?"). Only async-signal-safe
// calls are made in the handler.
//
// Build:  cc -O2 -fPIC -shared -o malloc_count.so malloc_count.c

#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <sys/syscall.h>
#include <unistd.h>

extern void* __libc_malloc(size_t);
extern void* __libc_calloc(size_t, size_t);
extern void* __libc_realloc(void*, size_t);
extern void* __libc_memalign(size_t, size_t);
extern void* __libc_valloc(size_t);
extern void* __libc_pvalloc(size_t);

#define MAX_THREADS 4096

struct slot
{
    _Atomic long tid;
    _Atomic unsigned long allocs;
};

static struct slot g_slots[MAX_THREADS];
static _Atomic int g_used;
static _Atomic unsigned long g_overflow;
static _Atomic unsigned long g_seq;
static __thread struct slot* t_slot __attribute__((tls_model("initial-exec")));

static void count(void)
{
    struct slot* s = t_slot;
    if (s == NULL)
    {
        const int i = atomic_fetch_add_explicit(&g_used, 1, memory_order_relaxed);
        if (i >= MAX_THREADS)
        {
            atomic_fetch_add_explicit(&g_overflow, 1, memory_order_relaxed);
            return;
        }
        s = &g_slots[i];
        atomic_store_explicit(&s->tid, (long)syscall(SYS_gettid), memory_order_relaxed);
        t_slot = s;
    }
    atomic_fetch_add_explicit(&s->allocs, 1, memory_order_relaxed);
}

void* malloc(size_t n)
{
    count();
    return __libc_malloc(n);
}

void* calloc(size_t a, size_t b)
{
    count();
    return __libc_calloc(a, b);
}

void* realloc(void* p, size_t n)
{
    count();
    return __libc_realloc(p, n);
}

void* memalign(size_t al, size_t n)
{
    count();
    return __libc_memalign(al, n);
}

void* aligned_alloc(size_t al, size_t n)
{
    count();
    return __libc_memalign(al, n);
}

int posix_memalign(void** out, size_t al, size_t n)
{
    count();
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
    count();
    return __libc_valloc(n);
}

void* pvalloc(size_t n)
{
    count();
    return __libc_pvalloc(n);
}

static char* put_str(char* p, const char* s)
{
    while (*s != '\0')
    {
        *p++ = *s++;
    }
    return p;
}

static char* put_ulong(char* p, unsigned long v)
{
    char tmp[24];
    int n = 0;
    do
    {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v != 0);
    while (n > 0)
    {
        *p++ = tmp[--n];
    }
    return p;
}

// Reads /proc/self/task/<tid>/comm into p (no newline); "?" if gone.
static char* put_comm(char* p, long tid)
{
    char path[64];
    char* q = put_str(path, "/proc/self/task/");
    q = put_ulong(q, (unsigned long)tid);
    q = put_str(q, "/comm");
    *q = '\0';
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        return put_str(p, "?");
    }
    char buf[32];
    const ssize_t r = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (r <= 0)
    {
        return put_str(p, "?");
    }
    for (ssize_t i = 0; i < r && buf[i] != '\n'; ++i)
    {
        *p++ = (buf[i] == ' ') ? '_' : buf[i];
    }
    return p;
}

static void dump(int sig)
{
    (void)sig;
    char line[160];
    const unsigned long seq = atomic_fetch_add(&g_seq, 1);
    char* p = put_ulong(put_str(line, "malloc_count begin "), seq);
    *p++ = '\n';
    write(2, line, (size_t)(p - line));
    int used = atomic_load(&g_used);
    if (used > MAX_THREADS)
    {
        used = MAX_THREADS;
    }
    for (int i = 0; i < used; ++i)
    {
        const long tid = atomic_load(&g_slots[i].tid);
        if (tid == 0)
        {
            continue;
        }
        p = put_ulong(put_str(line, "malloc_count tid="), (unsigned long)tid);
        p = put_comm(put_str(p, " comm="), tid);
        p = put_ulong(put_str(p, " allocs="), atomic_load(&g_slots[i].allocs));
        *p++ = '\n';
        write(2, line, (size_t)(p - line));
    }
    p = put_ulong(put_str(line, "malloc_count end "), seq);
    p = put_ulong(put_str(p, " overflow="), atomic_load(&g_overflow));
    *p++ = '\n';
    write(2, line, (size_t)(p - line));
}

__attribute__((constructor)) static void install(void)
{
    struct sigaction sa = {0};
    sa.sa_handler = dump;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR2, &sa, NULL);
}
