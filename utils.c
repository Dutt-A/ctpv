#include <sys/wait.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "error.h"
#include "utils.h"

char *program;

/*
 * Call command
 *
 * If cpid is NULL, wait for the command to finish executing;
 * otherwise store pid in cpid
 *
 * fd is a NULL-terminated array of pairs of file descriptors
 * to pass to dup2()
 */
int spawn(char *args[], pid_t *cpid, int *exitcode, int *fds[2])
{
    if (exitcode)
        *exitcode = -1;

    pid_t pid = fork();
    ERRCHK_RET(pid == -1, FUNCFAILED("fork"), ERRNOS);

    /* Child process */
    if (pid == 0) {
        if (fds) {
            while (*fds) {
                if (dup2((*fds)[0], (*fds)[1]) == -1) {
                    print_errorf("dup2() failed: %s", strerror(errno));
                    exit(EXIT_FAILURE);
                }
                fds = &fds[1];
            }
        }

        execvp(args[0], args);
        PRINTINTERR(FUNCFAILED("exec"), ERRNOS);
        exit(EXIT_FAILURE);
    }

    if (cpid) {
        *cpid = pid;
    } else {
        int stat;
        ERRCHK_RET(waitpid(pid, &stat, 0) == -1, FUNCFAILED("waitpid"),
                   ERRNOS);

        if (exitcode && WIFEXITED(stat))
            *exitcode = WEXITSTATUS(stat);
    }

    return OK;
}

int strcmpnull(char const *s1, char const *s2)
{
    if (!s1 && !s2)
        return 0;
    else if (s1 && !s2)
        return 1;
    else if (!s1 && s2)
        return -1;

    return strcmp(s1, s2);
}

CharVec char_v_new(size_t cap)
{
    CharVec v;
    v.buf = NULL;
    v.len = 0;
    v.cap = cap;

    return v;
}

void char_v_free(CharVec *v)
{
    if (v->buf) {
        free(v->buf);
        v->buf = NULL;
        v->len = 0;
    }
}

void char_v_append(CharVec *v, char c)
{
    if (!v->buf) {
        v->buf = malloc(v->cap * sizeof(v->buf[0]));
        if (!v->buf) {
            PRINTINTERR(FUNCFAILED("malloc"), ERRNOS);
            abort();
        }
        v->buf[0] = '\0';
        v->len++;
    }

    if (v->len + 1 >= v->cap) {
        v->cap *= 2;
        v->buf = realloc(v->buf, v->cap * sizeof(v->buf[0]));
        if (!v->buf) {
            PRINTINTERR(FUNCFAILED("realloc"), ERRNOS);
            abort();
        }
    }

    v->buf[v->len - 1] = c;
    v->buf[v->len] = '\0';
    v->len++;
}
