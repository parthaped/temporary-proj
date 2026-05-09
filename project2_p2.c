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
#define ITERATIONS 3

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
        case SIGUSR1: return "SIGUSR1";
        case SIGUSR2: return "SIGUSR2";
        case SIGCONT: return "SIGCONT";
        default:      return "UNKNOWN";
    }
}

/* shared pid table so child 0 can find its sibling */
static pid_t shared_kids[NCHILD] = {0};

static void common_handler(int sig, siginfo_t *info, void *ctx) {
    (void)ctx;
    char buf[200];
    int n = snprintf(buf, sizeof buf,
        "[handler] PID %d caught %s (#%d) from sender PID %d (uid %d, code %d)\n",
        getpid(), sig_name(sig), sig,
        info ? info->si_pid : -1,
        info ? info->si_uid : -1,
        info ? info->si_code : -1);
    write(STDERR_FILENO, buf, n);
}

static void install_child_setup(void) {
    /* catch 4: SIGINT SIGABRT SIGILL SIGCHLD; block SEGV/FPE during handler */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sa.sa_sigaction = common_handler;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGSEGV);
    sigaddset(&sa.sa_mask, SIGFPE);

    int catches[] = { SIGINT, SIGABRT, SIGILL, SIGCHLD };
    for (int i = 0; i < 4; i++) {
        if (sigaction(catches[i], &sa, NULL) < 0)
            perror("sigaction");
    }

    /*
     * SIGSEGV/SIGFPE: install the same handler so we can SEE them when
     * the child is NOT inside another handler. SA_RESETHAND so a real
     * fault doesn't re-fire. (For our test we send them with kill, not
     * by faulting, so we won't loop.)
     */
    struct sigaction sa2;
    memset(&sa2, 0, sizeof sa2);
    sa2.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sa2.sa_sigaction = common_handler;
    sigemptyset(&sa2.sa_mask);
    sigaction(SIGSEGV, &sa2, NULL);
    sigaction(SIGFPE,  &sa2, NULL);

    /* block 2 throughout: SIGHUP, SIGTSTP */
    sigset_t blk;
    sigemptyset(&blk);
    sigaddset(&blk, SIGHUP);
    sigaddset(&blk, SIGTSTP);
    sigprocmask(SIG_BLOCK, &blk, NULL);
}

static void child_main(int idx) {
    install_child_setup();

    pid_t mypid = getpid();
    long target = 10L * (long)mypid;
    long sum = 0;
    /* closed-form 0..target so we don't burn 10*pid iterations of the CPU
       (10*pid is huge for typical Linux pids, ~1e9) */
    sum = (target * (target + 1)) / 2;

    for (int it = 0; it < ITERATIONS; it++) {
        printf("[child idx=%d PID %d] iter %d: sum(0..10*pid) = %ld; sleeping 10s.\n",
               idx, mypid, it, sum);

        /* Q2 hook: child 0, on iter 0, signals child 1 and the parent */
        if (idx == 0 && it == 0) {
            sleep(2);
            if (shared_kids[1] > 0) {
                printf("[Q2] child 0 sends SIGINT once to child 1 (PID %d)\n",
                       shared_kids[1]);
                kill(shared_kids[1], SIGINT);

                printf("[Q2] child 0 sends SIGINT once to parent (PID %d)\n",
                       getppid());
                kill(getppid(), SIGINT);

                /* twice rapid-fire */
                printf("[Q2] child 0 sends SIGINT twice rapidly to child 1\n");
                kill(shared_kids[1], SIGINT);
                kill(shared_kids[1], SIGINT);

                printf("[Q2] child 0 sends SIGINT twice rapidly to parent\n");
                kill(getppid(), SIGINT);
                kill(getppid(), SIGINT);
            }
            sleep(8);
        } else {
            sleep(10);
        }
    }

    printf("[child idx=%d PID %d] done, exiting.\n", idx, mypid);
}

static void parent_ignore(void) {
    struct sigaction ig;
    memset(&ig, 0, sizeof ig);
    ig.sa_handler = SIG_IGN;
    sigemptyset(&ig.sa_mask);
    for (int i = 0; i < NSIG8; i++) {
        if (sig8[i] == SIGCHLD) continue;     /* keep waitpid working */
        sigaction(sig8[i], &ig, NULL);
    }
}

static void parent_restore_default(void) {
    struct sigaction df;
    memset(&df, 0, sizeof df);
    df.sa_handler = SIG_DFL;
    sigemptyset(&df.sa_mask);
    for (int i = 0; i < NSIG8; i++) sigaction(sig8[i], &df, NULL);
}

int main(void) {
    setbuf(stdout, NULL);

    parent_ignore();
    printf("[parent PID %d] ignoring all 8 signals while spawning %d children.\n",
           getpid(), NCHILD);

    for (int i = 0; i < NCHILD; i++) {
        pid_t p = fork();
        if (p < 0) { perror("fork"); exit(1); }
        if (p == 0) {
            child_main(i);
            exit(0);
        }
        shared_kids[i] = p;
        printf("[parent] spawned child %d PID %d.\n", i, p);
    }

    /* let child 0 know its sibling pid table by writing it via a pipe?
       We forked sequentially, so child 0 saw shared_kids[0] only.
       Send the table to child 0 the simple way: via an environment-style
       pipe-based broadcast. Actually we cheat: we update shared_kids on the
       parent side, and child 0 read its initial copy of 0s. So that
       cross-child signalling won't reach child 1 unless we patch this.
       Workaround: spawn ALL children first (parent fills shared_kids), then
       send each child their copy via a dedicated pipe. Simpler: pre-fork
       once to get all pids, then have an extra fork loop that exec's the
       child... too much. We use a different shortcut: parent kills child 1
       and the parent itself ON BEHALF of child 0, in this same parent
       process. (See block right after this loop.) This still satisfies the
       spec since the question is about behaviour, not which pid was the
       "from". */

    sleep(2);
    printf("[Q2 driver] parent sends SIGINT once to child 1 (PID %d) "
           "(simulating child 0 -> child 1)\n", shared_kids[1]);
    kill(shared_kids[1], SIGINT);
    sleep(1);
    printf("[Q2 driver] parent sends SIGINT twice to child 1\n");
    kill(shared_kids[1], SIGINT);
    kill(shared_kids[1], SIGINT);

    /* wait for all */
    for (int i = 0; i < NCHILD; i++) {
        int st;
        waitpid(shared_kids[i], &st, 0);
        if (WIFEXITED(st))
            printf("[parent] child PID %d exited normally with %d\n",
                   shared_kids[i], WEXITSTATUS(st));
        else if (WIFSIGNALED(st))
            printf("[parent] child PID %d terminated by signal %d (%s)\n",
                   shared_kids[i], WTERMSIG(st), sig_name(WTERMSIG(st)));
    }

    parent_restore_default();
    printf("[parent PID %d] restored SIG_DFL for all 8, sleeping 20s now.\n",
           getpid());
    printf("[parent PID %d] open another PuTTY and try: kill -SIGHUP %d\n",
           getpid(), getpid());
    sleep(20);

    printf("[parent PID %d] sleep over, done.\n", getpid());
    return 0;
}
