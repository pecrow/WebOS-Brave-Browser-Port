#define _GNU_SOURCE
#include <errno.h>
#include <grp.h>
#include <linux/capability.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static void fail(const char *operation)
{
    perror(operation);
    exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: browser-exec PROGRAM [ARGUMENTS...]\n");
        return EXIT_FAILURE;
    }
    struct passwd *account = getpwnam("nobody");
    if (!account || account->pw_uid == 0) {
        fprintf(stderr, "No unprivileged browser account is available\n");
        return EXIT_FAILURE;
    }
    const uid_t uid = account->pw_uid;
    const gid_t gid = account->pw_gid;
    struct group *audio = getgrnam("pulse-access");
    if (!audio) {
        fprintf(stderr, "TV audio access group is missing\n");
        return EXIT_FAILURE;
    }
    const gid_t audio_gid = audio->gr_gid;
    umask(0077);
    const struct rlimit core_limit = {0, 0};
    if (setrlimit(RLIMIT_CORE, &core_limit))
        fail("Disable browser core dumps");
    FILE *priority = fopen("/proc/self/oom_score_adj", "w");
    if (!priority || fputs("600\n", priority) == EOF || fclose(priority))
        fail("Set browser-only low-memory priority");
    if (geteuid() == 0) {
        if (prctl(PR_SET_KEEPCAPS, 0, 0, 0, 0) ||
            setgroups(1, &audio_gid) || setresgid(gid, gid, gid) ||
            setresuid(uid, uid, uid))
            fail("Drop browser privileges");
    }
    if (getuid() != uid || geteuid() != uid || getgid() != gid || getegid() != gid) {
        fprintf(stderr, "Browser identity is not unprivileged\n");
        return EXIT_FAILURE;
    }
    struct __user_cap_header_struct header = {_LINUX_CAPABILITY_VERSION_3, 0};
    struct __user_cap_data_struct capabilities[2] = {{0}, {0}};
    if (syscall(SYS_capset, &header, capabilities) ||
        prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) ||
        prctl(PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0))
        fail("Lock browser privileges");
    if (getppid() == 1) {
        fprintf(stderr, "Native display owner exited before browser startup\n");
        return EXIT_FAILURE;
    }
    const char *runtime = getenv("BRAVE_WEBOS_BROWSER_RUNTIME_DIR");
    if (runtime && setenv("XDG_RUNTIME_DIR", runtime, 1))
        fail("Set private browser runtime directory");
    if (unsetenv("LD_PRELOAD") || unsetenv("LD_AUDIT") ||
        unsetenv("BRAVE_WEBOS_BROWSER_RUNTIME_DIR") ||
        unsetenv("LD_LIBRARY_PATH") || setenv("USER", "nobody", 1) ||
        setenv("LOGNAME", "nobody", 1))
        fail("Prepare browser environment");
    execv(argv[1], &argv[1]);
    fail("Execute unprivileged browser");
    return EXIT_FAILURE;
}
