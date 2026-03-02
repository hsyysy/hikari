#include <hikari/action_config.h>

#include <string.h>

#include <hikari/memory.h>
#include <hikari/split.h>

void
hikari_action_config_init(struct hikari_action_config *action_config,
    const char *action_name,
    const char *command)
{
  action_config->action_name = strdup(action_name);
  action_config->command = strdup(command);
}

void
hikari_action_config_fini(struct hikari_action_config *action_config)
{
  hikari_free(action_config->action_name);
  hikari_free(action_config->command);
}
