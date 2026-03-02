#include <hikari/command.h>

#include <errno.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

void
hikari_command_execute(const char *cmd)
{
  pid_t child;
  int status;

  child = fork();
  if (child == 0) {
    child = fork();
    if (child == 0) {
      setsid();
      execl("/bin/sh", "/bin/sh", "-c", cmd, NULL);
      _exit(127);
    }
    _exit(child == -1);
  }

  for (;;) {
    if (waitpid(child, &status, 0) == -1) {
      if (errno == EINTR) {
        continue;
      }
    }
    return;
  }
}
