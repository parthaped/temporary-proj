#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#define NCHILD 4

static int sig8[] = {
    SIGINT, SIGABRT, SIGILL, SIGCHLD,
    SIGSEGV, SIGFPE, SIGHUP, SIGTSTP
};
#define NSIG8 ((int)(sizeof sig8 / sizeof sig8[0]))

static const char *sig_name(int s) {
    switch (s) {
        case SIGINT:  return "SIGINT";
        case SIGABRT: return "SIGABRT";
        case SIGILL:  return "SIGILL";
        case SIGCHLD: return "SIGCHLD";
        case SIGSEGV: return "SIGSEGV";
        case SIGFPE:  return "SIGFPE";
        case SIGHUP:  return "SIGHUP";
        case SIGTSTP: return "SIGTSTP";
        case SIGQUIT: return "SIGQUIT";
        case SIGTERM: return "SIGTERM";
        default:      return "UNKNOWN";
    }
}

static void common_handler(int sig, siginfo_t *info, void *ctx) {
    (void)ctx;
    char buf[180];
    int n = snprintf(buf, sizeof buf,
        "[handler] PID %d got %s(#%d) from sender PID %d\n",
        getpid(), sig_name(sig), sig, info ? info->si_pid : -1);
    {
        ssize_t w = write(STDERR_FILENO, buf, (size_t)n);
        (void)w;
    }
}

static void install_handlers_for_all9(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sa.sa_sigaction = common_handler;
    sigemptyset(&sa.sa_mask);
    for (int i = 0; i < NSIG8; i++) {
        if (sig8[i] == SIGCHLD) continue;     /* keep default for waitpid */
        sigaction(sig8[i], &sa, NULL);
    }
    /* SIGQUIT is added in Q3 */
    sigaction(SIGQUIT, &sa, NULL);
}

static void print_pending(const char *label) {
    sigset_t pend;
    if (sigpending(&pend) < 0) { perror("sigpending"); return; }
    char line[256];
    int off = snprintf(line, sizeof line, "[pending] %s PID %d:",
                       label, (int)getpid());
    int any = 0;
    int look[] = { SIGINT, SIGABRT, SIGILL, SIGCHLD,
                   SIGSEGV, SIGFPE, SIGHUP, SIGTSTP, SIGQUIT };
    for (int i = 0; i < (int)(sizeof look / sizeof look[0]); i++) {
        if (sigismember(&pend, look[i])) {
            off += snprintf(line + off, sizeof line - off, " %s",
                            sig_name(look[i]));
            any = 1;
        }
    }
    if (!any) off += snprintf(line + off, sizeof line - off, " (empty)");
    off += snprintf(line + off, sizeof line - off, "\n");
    {
        ssize_t w = write(STDERR_FILENO, line, (size_t)off);
        (void)w;
    }
}

static void child_main(int idx) {
    sigset_t blk;
    sigemptyset(&blk);

    if (idx < NCHILD / 2) {
        /* first half blocks { INT, QUIT, TSTP } */
        sigaddset(&blk, SIGINT);
        sigaddset(&blk, SIGQUIT);
        sigaddset(&blk, SIGTSTP);
    } else {
        /* second half blocks the other 6 (everything in sig8 except INT/TSTP) */
        sigaddset(&blk, SIGABRT);
        sigaddset(&blk, SIGILL);
        sigaddset(&blk, SIGCHLD);
        sigaddset(&blk, SIGSEGV);
        sigaddset(&blk, SIGFPE);
        sigaddset(&blk, SIGHUP);
    }
    sigprocmask(SIG_SETMASK, &blk, NULL);
    install_handlers_for_all9();

    printf("[child idx=%d PID=%d] mask installed (first-half=%d).\n",
           idx, getpid(), idx < NCHILD / 2);

    /* let the parent send signals our way */
    sleep(3);

    /* peek at the pending queue */
    print_pending("after-first-burst");

    if (idx == 0) {
        /* drain pending non-blocking via sigtimedwait */
        struct timespec ts = { 0, 0 };
        siginfo_t info;
        int got;
        while ((got = sigtimedwait(&blk, &info, &ts)) > 0) {
            char b[180];
            int n = snprintf(b, sizeof b,
                "[child %d sigtimedwait] pulled %s from PID %d\n",
                idx, sig_name(got), info.si_pid);
            {
                ssize_t w = write(STDERR_FILENO, b, (size_t)n);
                (void)w;
            }
        }
        if (got < 0 && errno != EAGAIN) perror("sigtimedwait");
    } else if (idx == NCHILD / 2) {
        /* one of the second-half children does a single blocking sigwait */
        int got = 0;
        if (sigwait(&blk, &got) == 0) {
            char b[160];
            int n = snprintf(b, sizeof b,
                "[child %d sigwait] received %s\n", idx, sig_name(got));
            {
                ssize_t w = write(STDERR_FILENO, b, (size_t)n);
                (void)w;
            }
        }
    }

    sleep(2);
    print_pending("after-drain");
    printf("[child idx=%d PID=%d] exiting.\n", idx, getpid());
    exit(0);
}

int main(void) {
    setbuf(stdout, NULL);

    /* parent blocks INT/QUIT/TSTP BEFORE forking */
    sigset_t pblk;
    sigemptyset(&pblk);
    sigaddset(&pblk, SIGINT);
    sigaddset(&pblk, SIGQUIT);
    sigaddset(&pblk, SIGTSTP);
    sigprocmask(SIG_BLOCK, &pblk, NULL);

    install_handlers_for_all9();

    printf("[parent PID %d] blocked SIGINT/SIGQUIT/SIGTSTP. About to send each "
           "of the 8 listed signals 3x to myself BEFORE forking.\n", getpid());

    for (int i = 0; i < NSIG8; i++) {
        for (int rep = 0; rep < 3; rep++) {
            kill(getpid(), sig8[i]);
        }
    }

    print_pending("parent before fork");

    pid_t kids[NCHILD];
    for (int i = 0; i < NCHILD; i++) {
        pid_t p = fork();
        if (p < 0) { perror("fork"); exit(1); }
        if (p == 0) { child_main(i); exit(0); }
        kids[i] = p;
    }

    sleep(1);

    /* now send each of the 8 to self 3x AGAIN, and to each child 3x */
    printf("[parent] sending each of 8 signals 3x to self and to each child.\n");
    for (int i = 0; i < NSIG8; i++) {
        for (int rep = 0; rep < 3; rep++) {
            kill(getpid(), sig8[i]);
            for (int c = 0; c < NCHILD; c++) kill(kids[c], sig8[i]);
        }
    }

    /* SIGQUIT 3x to each child since Q3's blocked set includes it */
    for (int rep = 0; rep < 3; rep++)
        for (int c = 0; c < NCHILD; c++) kill(kids[c], SIGQUIT);

    print_pending("parent after burst");

    for (int i = 0; i < NCHILD; i++) {
        int st;
        waitpid(kids[i], &st, 0);
        if (WIFEXITED(st))
            printf("[parent] child PID %d exit %d\n", kids[i], WEXITSTATUS(st));
        else if (WIFSIGNALED(st))
            printf("[parent] child PID %d killed by signal %d (%s)\n",
                   kids[i], WTERMSIG(st), sig_name(WTERMSIG(st)));
    }

    print_pending("parent end");
    printf("[parent] done.\n");
    return 0;
}
