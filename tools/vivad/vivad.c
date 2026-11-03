/*
 * vivad: runs programs on the phone for the viva USB link (bsd/dev/viva_usb.c).
 *
 * A root LaunchDaemon (tools/viva-launchd-add.py). It reads one command per
 * read() from /dev/vivalink, lines "ID TEXT" queued by the link commands
 * "run ARGS.." and "vivad TEXT", and writes everything it has to say back to
 * /dev/vivalink, which the host receives as 'O' frames
 * (tools/viva-link.py run ...). Commands:
 *
 *   run [VAR=VALUE..] PROGRAM [ARGS..]   start a program; stdout and stderr
 *       come back as "[ID] line", then "[ID] exit N" or "[ID] signal N".
 *       A PROGRAM without '/' is looked up in /private/var/root/viva-tests,
 *       which is also the working directory. Double quotes group words.
 *   ps          running programs
 *   kill ID     SIGKILL a program started by "run"
 *   tests       list /private/var/root/viva-tests
 *
 * At start it runs every executable in /private/var/root/viva-tests/autorun
 * (name order, one after the other) the same way, with IDs a1, a2, ...
 * Without the link device (old kernel) the output goes to /dev/console.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define LINK_DEV    "/dev/vivalink"
#define TESTS_DIR   "/private/var/root/viva-tests"
#define AUTORUN_DIR TESTS_DIR "/autorun"
#define MAX_JOBS    16
#define MAX_ARGS    64

extern char **environ;

static int out_fd = -1;
static pthread_mutex_t out_lock = PTHREAD_MUTEX_INITIALIZER;

struct job {
    char id[16];
    pid_t pid;
    int fd;             /* read end of the output pipe */
    char cmd[96];
};
static struct job jobs[MAX_JOBS];
static pthread_mutex_t jobs_lock = PTHREAD_MUTEX_INITIALIZER;

static void
say(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    if (n > (int)sizeof(buf) - 1) {
        n = sizeof(buf) - 1;
    }
    pthread_mutex_lock(&out_lock);
    if (write(out_fd, buf, (size_t)n) < 0 && out_fd != STDERR_FILENO) {
        write(STDERR_FILENO, buf, (size_t)n);
    }
    pthread_mutex_unlock(&out_lock);
}

/* Split a command line into words (double quotes group, no escapes). */
static int
split(char *s, char **argv, int max)
{
    int n = 0;
    while (*s && n < max - 1) {
        while (*s == ' ' || *s == '\t') {
            s++;
        }
        if (!*s) {
            break;
        }
        if (*s == '"') {
            argv[n++] = ++s;
            while (*s && *s != '"') {
                s++;
            }
        } else {
            argv[n++] = s;
            while (*s && *s != ' ' && *s != '\t') {
                s++;
            }
        }
        if (*s) {
            *s++ = 0;
        }
    }
    argv[n] = NULL;
    return n;
}

/* Copy a job's output to the link, one "[ID] " prefixed line at a time; then reap it. */
static void *
pump(void *arg)
{
    struct job *j = arg;
    char buf[4096], line[1100];
    size_t len = 0;
    ssize_t n;
    while ((n = read(j->fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n' || len == sizeof(line) - 2) {
                line[len] = 0;
                say("[%s] %s\n", j->id, line);
                len = 0;
                if (buf[i] == '\n') {
                    continue;
                }
            }
            line[len++] = buf[i];
        }
    }
    if (len) {
        line[len] = 0;
        say("[%s] %s\n", j->id, line);
    }
    close(j->fd);
    int status = 0;
    while (waitpid(j->pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (WIFSIGNALED(status)) {
        say("[%s] signal %d\n", j->id, WTERMSIG(status));
    } else {
        say("[%s] exit %d\n", j->id, WEXITSTATUS(status));
    }
    pthread_mutex_lock(&jobs_lock);
    j->pid = 0;
    pthread_mutex_unlock(&jobs_lock);
    return NULL;
}

/* Start "[VAR=VALUE..] PROGRAM [ARGS..]"; with wait, return only when it is done. */
static void
run(const char *id, char *cmdline, int wait)
{
    char *words[MAX_ARGS], *envp[256], path[1024];
    char copy[96];
    snprintf(copy, sizeof(copy), "%s", cmdline);
    int n = split(cmdline, words, MAX_ARGS);
    int first = 0, envc = 0;
    for (char **e = environ; *e && envc < 200; e++) {
        envp[envc++] = *e;
    }
    while (first < n && strchr(words[first], '=') && words[first][0] != '=' && envc < 254) {
        envp[envc++] = words[first++];
    }
    envp[envc] = NULL;
    if (first == n) {
        say("[%s] nothing to run\n", id);
        return;
    }
    if (strchr(words[first], '/')) {
        snprintf(path, sizeof(path), "%s", words[first]);
    } else {
        snprintf(path, sizeof(path), TESTS_DIR "/%s", words[first]);
    }

    pthread_mutex_lock(&jobs_lock);
    struct job *j = NULL;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].pid == 0) {
            j = &jobs[i];
            j->pid = -1;        /* reserved */
            break;
        }
    }
    pthread_mutex_unlock(&jobs_lock);
    if (j == NULL) {
        say("[%s] too many programs running (%d)\n", id, MAX_JOBS);
        return;
    }
    int fds[2];
    if (pipe(fds) < 0) {
        say("[%s] pipe: %s\n", id, strerror(errno));
        j->pid = 0;
        return;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addclose(&fa, fds[1]);
    if (out_fd > 2) {
        posix_spawn_file_actions_addclose(&fa, out_fd);
    }
    pid_t pid;
    int err = posix_spawn(&pid, path, &fa, NULL, &words[first], envp);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (err) {
        close(fds[0]);
        say("[%s] cannot start %s: %s\n", id, path, strerror(err));
        j->pid = 0;
        return;
    }
    snprintf(j->id, sizeof(j->id), "%s", id);
    snprintf(j->cmd, sizeof(j->cmd), "%s", copy);
    j->fd = fds[0];
    pthread_mutex_lock(&jobs_lock);
    j->pid = pid;
    pthread_mutex_unlock(&jobs_lock);
    say("[%s] started %s (pid %d)\n", id, path, pid);
    if (wait) {
        pump(j);
        return;
    }
    pthread_t t;
    if (pthread_create(&t, NULL, pump, j) == 0) {
        pthread_detach(t);
    } else {
        pump(j);
    }
}

static int
by_name(const struct dirent **a, const struct dirent **b)
{
    return strcmp((*a)->d_name, (*b)->d_name);
}

static void
list_dir(const char *id, const char *dir)
{
    struct dirent **names;
    int n = scandir(dir, &names, NULL, by_name);
    if (n < 0) {
        say("[%s] %s: %s\n", id, dir, strerror(errno));
        return;
    }
    for (int i = 0; i < n; i++) {
        char p[1024];
        struct stat st;
        snprintf(p, sizeof(p), "%s/%s", dir, names[i]->d_name);
        if (names[i]->d_name[0] != '.' && stat(p, &st) == 0) {
            say("[%s] %c%s %lld %s\n", id, S_ISDIR(st.st_mode) ? 'd' : '-', (st.st_mode & 0111) ? "x" : " ",
                (long long)st.st_size, names[i]->d_name);
        }
        free(names[i]);
    }
    free(names);
    say("[%s] exit 0\n", id);
}

/* Every executable regular file in the autorun directory, in name order, one after the other. */
static void *
autorun(void *unused)
{
    (void)unused;
    struct dirent **names;
    int n = scandir(AUTORUN_DIR, &names, NULL, by_name);
    for (int i = 0, k = 0; i < n; i++) {
        char p[1024], id[16];
        struct stat st;
        snprintf(p, sizeof(p), AUTORUN_DIR "/%s", names[i]->d_name);
        if (names[i]->d_name[0] != '.' && stat(p, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0111)) {
            snprintf(id, sizeof(id), "a%d", ++k);
            run(id, p, 1);
        }
        free(names[i]);
    }
    if (n > 0) {
        free(names);
    }
    return NULL;
}

static void
command(char *line)
{
    char *id = line, *text = strchr(line, ' ');
    if (text == NULL) {
        return;
    }
    *text++ = 0;
    if (strncmp(text, "run ", 4) == 0) {
        run(id, text + 4, 0);
    } else if (strcmp(text, "ps") == 0) {
        pthread_mutex_lock(&jobs_lock);
        for (int i = 0; i < MAX_JOBS; i++) {
            if (jobs[i].pid > 0) {
                say("[%s] %s pid %d: %s\n", id, jobs[i].id, jobs[i].pid, jobs[i].cmd);
            }
        }
        pthread_mutex_unlock(&jobs_lock);
        say("[%s] exit 0\n", id);
    } else if (strncmp(text, "kill ", 5) == 0) {
        int found = 0;
        pthread_mutex_lock(&jobs_lock);
        for (int i = 0; i < MAX_JOBS; i++) {
            if (jobs[i].pid > 0 && strcmp(jobs[i].id, text + 5) == 0) {
                kill(jobs[i].pid, SIGKILL);
                found = 1;
            }
        }
        pthread_mutex_unlock(&jobs_lock);
        say("[%s] %s %s\n[%s] exit %d\n", id, text + 5, found ? "killed" : "not running", id, !found);
    } else if (strcmp(text, "tests") == 0) {
        list_dir(id, TESTS_DIR);
    } else {
        say("[%s] unknown vivad command: %s (run, ps, kill ID, tests)\n[%s] exit 1\n", id, text, id);
    }
}

int
main(void)
{
    signal(SIGPIPE, SIG_IGN);
    mkdir(TESTS_DIR, 0755);
    mkdir(AUTORUN_DIR, 0755);
    chdir(TESTS_DIR);
    int fd;
    for (int tries = 0; (fd = open(LINK_DEV, O_RDWR)) < 0; tries++) {
        if (tries == 30) {      /* old kernel without the device: autorun to the console, then keep waiting */
            out_fd = open("/dev/console", O_WRONLY);
            say("vivad: no %s, autorun output on the console\n", LINK_DEV);
            autorun(NULL);
        }
        sleep(2);
    }
    if (out_fd >= 0) {
        close(out_fd);
    }
    out_fd = fd;
    say("vivad: ready (pid %d)\n", getpid());
    pthread_t t;
    if (pthread_create(&t, NULL, autorun, NULL) == 0) {
        pthread_detach(t);
    }
    char line[600];
    for (;;) {
        ssize_t n = read(fd, line, sizeof(line) - 1);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            sleep(1);
            continue;
        }
        line[n] = 0;
        if (n > 0 && line[n - 1] == '\n') {
            line[n - 1] = 0;
        }
        command(line);
    }
}
