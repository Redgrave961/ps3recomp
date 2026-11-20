#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
#endif
#include "../ps3_stdio.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int g_pass, g_fail;
#define CHECK(cond) do { if (cond) g_pass++; else { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

enum { END_EXIT, END_ABORT, END_CRASH, END_WAIT };

static volatile uintptr_t g_bad_addr = 8;

typedef struct {
    pid_t pid;
    int   out, err;
    int   ctl_in;
    int   ctl_out;
} child_t;

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void child_main(int ctl_w, int ctl_r, const char* flush_ms, int unbuffered, int end)
{
    if (flush_ms) setenv("PS3_STDIO_FLUSH_MS", flush_ms, 1); else unsetenv("PS3_STDIO_FLUSH_MS");
    if (unbuffered) setenv("PS3_STDIO_UNBUFFERED", "1", 1); else unsetenv("PS3_STDIO_UNBUFFERED");
    ps3_stdio_init();
    fflush(stderr);
    if (write(ctl_w, "i", 1) != 1) _exit(90);
    { char c; if (read(ctl_r, &c, 1) != 1) _exit(91); }

    printf("out-line\n");
    fprintf(stderr, "err-line\n");
    if (write(ctl_w, "p", 1) != 1) _exit(92);

    switch (end) {
    case END_EXIT:  exit(0);
    case END_ABORT: abort();
    case END_CRASH: *(volatile int*)g_bad_addr = 1; _exit(93);
    case END_WAIT:  { char c; if (read(ctl_r, &c, 1) != 1) _exit(94); exit(0); }
    }
    _exit(95);
}

static child_t spawn(const char* flush_ms, int unbuffered, int end)
{
    int o[2], e[2], up[2], down[2];
    if (pipe(o) || pipe(e) || pipe(up) || pipe(down)) { perror("pipe"); exit(100); }
    fflush(stdout); fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(100); }
    if (pid == 0) {
        dup2(o[1], 1); dup2(e[1], 2);
        close(o[0]); close(o[1]); close(e[0]); close(e[1]);
        close(up[0]); close(down[1]);
        child_main(up[1], down[0], flush_ms, unbuffered, end);
    }
    close(o[1]); close(e[1]); close(up[1]); close(down[0]);
    fcntl(o[0], F_SETFL, O_NONBLOCK);
    fcntl(e[0], F_SETFL, O_NONBLOCK);
    child_t c = { pid, o[0], e[0], up[0], down[1] };
    return c;
}

static void wait_byte(int fd)
{
    char c;
    CHECK(read(fd, &c, 1) == 1);
}

static int drain(int fd, char* buf, size_t cap)
{
    size_t len = strlen(buf);
    for (;;) {
        if (len + 1 >= cap) return 0;
        ssize_t n = read(fd, buf + len, cap - 1 - len);
        if (n > 0) { len += (size_t)n; buf[len] = 0; continue; }
        if (n == 0) return 1;
        return 0;
    }
}

static int wait_for(int fd, char* buf, size_t cap, const char* needle, double ms)
{
    double end = now_ms() + ms;
    for (;;) {
        int eof = drain(fd, buf, cap);
        if (strstr(buf, needle)) return 1;
        if (eof) return 0;
        double left = end - now_ms();
        if (left <= 0) return 0;
        struct pollfd p = { fd, POLLIN, 0 };
        poll(&p, 1, (int)(left < 20 ? left + 1 : 20));
    }
}

static int finish(child_t* c)
{
    int st = 0;
    if (write(c->ctl_out, "x", 1) != 1) { }
    waitpid(c->pid, &st, 0);
    close(c->out); close(c->err); close(c->ctl_in); close(c->ctl_out);
    return st;
}

static child_t start(const char* flush_ms, int unbuffered, int end)
{
    child_t c = spawn(flush_ms, unbuffered, end);
    wait_byte(c.ctl_in);
    { char junk[512] = ""; drain(c.err, junk, sizeof junk); }
    CHECK(write(c.ctl_out, "g", 1) == 1);
    wait_byte(c.ctl_in);
    return c;
}

static void test_buffered_by_default(void)
{
    child_t c = start("5000", 0, END_WAIT);
    char out[256] = "", err[256] = "";
    drain(c.out, out, sizeof out);
    drain(c.err, err, sizeof err);
    CHECK(strstr(out, "out-line") == NULL);
    CHECK(strstr(err, "err-line") == NULL);
    int st = finish(&c);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

static void test_flushed_within_interval(void)
{
    child_t c = start("50", 0, END_WAIT);
    double t0 = now_ms();
    char out[256] = "", err[256] = "";
    CHECK(wait_for(c.out, out, sizeof out, "out-line\n", 2000));
    CHECK(wait_for(c.err, err, sizeof err, "err-line\n", 2000));
    double took = now_ms() - t0;
    CHECK(kill(c.pid, 0) == 0);
    int st = finish(&c);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    printf("  timed flush: both lines arrived %.0f ms after printing (interval 50 ms)\n", took);
}

static void test_end(int end, const char* what, int want_sig)
{
    child_t c = start("5000", 0, end);
    char out[256] = "", err[512] = "";
    double t0 = now_ms();
    int st = 0;
    waitpid(c.pid, &st, 0);
    double took = now_ms() - t0;
    drain(c.out, out, sizeof out);
    drain(c.err, err, sizeof err);
    int ok = strstr(out, "out-line\n") && strstr(err, "err-line\n");
    if (!ok) fprintf(stderr, "  %s: stdout=\"%s\" stderr=\"%s\"\n", what, out, err);
    CHECK(strstr(out, "out-line\n") != NULL);
    CHECK(strstr(err, "err-line\n") != NULL);
    CHECK(took < 4000);
    if (want_sig == SIGSEGV)
        CHECK(WIFSIGNALED(st) && (WTERMSIG(st) == SIGSEGV || WTERMSIG(st) == SIGBUS));
    else if (want_sig) CHECK(WIFSIGNALED(st) && WTERMSIG(st) == want_sig);
    else          CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    close(c.out); close(c.err); close(c.ctl_in); close(c.ctl_out);
    if (ok) printf("  %s: both lines delivered, child ended in %.0f ms\n", what, took);
}

static void test_unbuffered_env(void)
{
    child_t c = start("5000", 1, END_WAIT);
    char out[256] = "", err[256] = "";
    drain(c.out, out, sizeof out);
    drain(c.err, err, sizeof err);
    CHECK(strstr(out, "out-line\n") != NULL);
    CHECK(strstr(err, "err-line\n") != NULL);
    int st = finish(&c);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    test_buffered_by_default();
    test_flushed_within_interval();
    test_end(END_EXIT,  "exit()",  0);
    test_end(END_ABORT, "abort()", SIGABRT);
    test_end(END_CRASH, "SIGSEGV", SIGSEGV);
    test_unbuffered_env();
    printf("test_stdio_flush: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
