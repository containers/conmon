#define _GNU_SOURCE

#include "healthcheck.h"
#include "utils.h"
#include "ctr_logging.h"
#include "parent_pipe_fd.h"
#include "globals.h"
#include "cli.h"
#include "ctr_exit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <glib.h>

/* Healthcheck validation constants */
#define HEALTHCHECK_INTERVAL_MIN 1
#define HEALTHCHECK_INTERVAL_MAX 3600
#define HEALTHCHECK_TIMEOUT_MIN 1
#define HEALTHCHECK_TIMEOUT_MAX 300
#define HEALTHCHECK_START_PERIOD_MIN 0
#define HEALTHCHECK_START_PERIOD_MAX 3600
#define HEALTHCHECK_RETRIES_MIN 0
#define HEALTHCHECK_RETRIES_MAX 100

/* Static string constants for healthcheck statuses */
const char *healthcheck_status_strings[] = {"none", "starting", "healthy", "unhealthy"};

/* Global healthcheck timer (one per conmon instance) */
healthcheck_timer_t *active_healthcheck_timer = NULL;


/* Cleanup healthcheck subsystem */
void healthcheck_cleanup(void)
{
	if (active_healthcheck_timer != NULL) {
		healthcheck_timer_stop(active_healthcheck_timer);
		healthcheck_timer_free(active_healthcheck_timer);
		active_healthcheck_timer = NULL;
	}
}

/* Free healthcheck configuration */
void healthcheck_config_free(healthcheck_config_t *config)
{
	if (config == NULL) {
		return;
	}

	if (config->test != NULL) {
		for (int i = 0; config->test[i] != NULL; i++) {
			free(config->test[i]);
		}
		free(config->test);
		config->test = NULL;
	}
	// Don't free config itself - it's a local variable on the stack
}

/* Create a new healthcheck timer */
healthcheck_timer_t *healthcheck_timer_new(const char *container_id, const healthcheck_config_t *config)
{
	if (container_id == NULL || config == NULL) {
		return NULL;
	}

	healthcheck_timer_t *timer = calloc(1, sizeof(healthcheck_timer_t));
	if (timer == NULL) {
		nwarn("Failed to allocate memory for healthcheck timer");
		return NULL;
	}

	timer->container_id = strdup(container_id);
	if (timer->container_id == NULL) {
		free(timer);
		return NULL;
	}

	timer->config = *config;
	timer->status = HEALTHCHECK_NONE;
	timer->consecutive_failures = 0;
	timer->start_period_remaining = config->start_period;
	timer->timer_active = false;
	timer->last_check_time = 0;

	/* Copy the test command array */
	if (config->test != NULL) {
		int argc = 0;
		while (config->test[argc] != NULL) {
			argc++;
		}

		timer->config.test = calloc(argc + 1, sizeof(char *));
		if (timer->config.test == NULL) {
			free(timer->container_id);
			free(timer);
			return NULL;
		}

		for (int i = 0; i < argc; i++) {
			timer->config.test[i] = strdup(config->test[i]);
			if (timer->config.test[i] == NULL) {
				/* Clean up on error */
				for (int j = 0; j < i; j++) {
					free(timer->config.test[j]);
				}
				free(timer->config.test);
				free(timer->container_id);
				free(timer);
				return NULL;
			}
		}
		timer->config.test[argc] = NULL;
	}

	return timer;
}

/* Free healthcheck timer */
void healthcheck_timer_free(healthcheck_timer_t *timer)
{
	if (timer == NULL) {
		return;
	}

	/* Stop the timer if it's still active */
	if (timer->timer_active) {
		healthcheck_timer_stop(timer);
	}

	/* Free container ID */
	if (timer->container_id != NULL) {
		free(timer->container_id);
	}

	/* Free test command array */
	if (timer->config.test != NULL) {
		for (int i = 0; timer->config.test[i] != NULL; i++) {
			free(timer->config.test[i]);
		}
		free(timer->config.test);
	}

	/* Clear the timer structure to prevent double-free */
	memset(timer, 0, sizeof(healthcheck_timer_t));
	free(timer);
}

/* Start healthcheck timer */
bool healthcheck_timer_start(healthcheck_timer_t *timer)
{
	if (timer == NULL || timer->timer_active) {
		return false;
	}

	if (!timer->config.enabled || timer->config.test == NULL) {
		return false;
	}

	/* Create GLib timeout source */
	timer->timer_id = g_timeout_add_seconds(timer->config.interval, healthcheck_timer_callback, timer);
	if (timer->timer_id == 0) {
		nwarn("Failed to create healthcheck timer");
		return false;
	}

	timer->timer_active = true;
	timer->status = HEALTHCHECK_STARTING;
	timer->last_check_time = time(NULL);
	return true;
}

/* Stop healthcheck timer */
void healthcheck_timer_stop(healthcheck_timer_t *timer)
{
	if (timer == NULL || !timer->timer_active) {
		return;
	}

	timer->timer_active = false;
	timer->status = HEALTHCHECK_NONE;

	/* Remove the GLib timeout source */
	if (timer->timer_id != 0) {
		g_source_remove(timer->timer_id);
		timer->timer_id = 0;
	}
}

/* Execute healthcheck command inside container using runtime */
bool healthcheck_execute_command(const healthcheck_config_t *config, const char *container_id, const char *runtime_path, int *exit_code)
{
	if (config == NULL || config->test == NULL || container_id == NULL || runtime_path == NULL || exit_code == NULL) {
		return false;
	}

	/* Initialize exit code to failure */
	*exit_code = -1;

	/* Create stderr pipe to capture error output */
	int stderr_pipe[2];
	if (pipe(stderr_pipe) == -1) {
		nwarnf("Failed to create pipe for healthcheck stderr: %s", strerror(errno));
		return false;
	}

	/* Fork a child process to execute the healthcheck command inside container */
	pid_t pid = fork();
	if (pid == -1) {
		nwarnf("Failed to fork process for healthcheck command: %s", strerror(errno));
		close(stderr_pipe[0]);
		close(stderr_pipe[1]);
		return false;
	}

	if (pid == 0) {
		/* Child process - execute the healthcheck command inside container */
		close(stderr_pipe[0]); /* Close read end of stderr pipe */

		/* Redirect stdout to /dev/null and stderr to pipe */
		int devnull = open("/dev/null", O_WRONLY);
		if (devnull != -1) {
			dup2(devnull, STDOUT_FILENO);
			close(devnull);
		}
		dup2(stderr_pipe[1], STDERR_FILENO); /* Redirect stderr to pipe */
		close(stderr_pipe[1]);

		/* Build runtime command for direct execution */
		/* Format: runtime exec container_id command args... */
		char **runtime_argv;

		/* Count arguments needed */
		int argc = 0;
		while (config->test[argc] != NULL) {
			argc++;
		}

		/* Allocate runtime command array: runtime + exec + container_id + command + args + NULL */
		runtime_argv = calloc(3 + argc + 1, sizeof(char *));
		if (runtime_argv == NULL) {
			nwarn("Failed to allocate memory for runtime command");
			_exit(127);
		}

		runtime_argv[0] = (char *)runtime_path; /* Runtime executable */
		runtime_argv[1] = "exec";		/* Runtime subcommand */
		runtime_argv[2] = (char *)container_id; /* Container ID */

		/* Copy healthcheck command and arguments */
		for (int i = 0; i < argc; i++) {
			runtime_argv[3 + i] = config->test[i];
		}
		runtime_argv[3 + argc] = NULL; /* NULL terminator */

		/* Execute the runtime command */
		if (execvp(runtime_path, runtime_argv) == -1) {
			/* If execvp fails, exit with error code */
			_exit(127); /* Command not found */
		}
	} else {
		/* Parent process - wait for child to complete */
		close(stderr_pipe[1]); /* Close write end of stderr pipe */
		int status;
		pid_t wait_result = waitpid(pid, &status, 0);

		if (wait_result == -1) {
			nwarnf("Failed to wait for healthcheck command: %s", strerror(errno));
			close(stderr_pipe[0]);
			return false;
		}

		/* Read stderr output */
		char stderr_buffer[4096];
		ssize_t stderr_len = read(stderr_pipe[0], stderr_buffer, sizeof(stderr_buffer) - 1);
		close(stderr_pipe[0]);

		if (stderr_len > 0) {
			stderr_buffer[stderr_len] = '\0';
			/* Trim trailing newlines */
			while (stderr_len > 0 && (stderr_buffer[stderr_len - 1] == '\n' || stderr_buffer[stderr_len - 1] == '\r')) {
				stderr_buffer[--stderr_len] = '\0';
			}
		} else {
			stderr_buffer[0] = '\0';
		}

		if (WIFEXITED(status)) {
			*exit_code = WEXITSTATUS(status);
			if (*exit_code != 0) {
				nwarnf("Healthcheck command failed (exit code %d): %s", *exit_code, config->test[0]);
				if (stderr_len > 0) {
					nwarnf("Healthcheck command stderr: %s", stderr_buffer);
				}
			}
			return true;
		} else if (WIFSIGNALED(status)) {
			nwarnf("Healthcheck command terminated by signal %d: %s", WTERMSIG(status), config->test[0]);
			if (stderr_len > 0) {
				nwarnf("Healthcheck command stderr: %s", stderr_buffer);
			}
			*exit_code = 128 + WTERMSIG(status); /* Standard convention for signal termination */
			return true;
		} else {
			nwarnf("Healthcheck command did not terminate normally: %s", config->test[0]);
			if (stderr_len > 0) {
				nwarnf("Healthcheck command stderr: %s", stderr_buffer);
			}
			*exit_code = -1;
			return false;
		}
	}

	/* This should never be reached */
	return false;
}

/* Convert healthcheck status to string */
const char *healthcheck_status_to_string(int status)
{
	if (status >= 0 && status < 4) {
		return healthcheck_status_strings[status];
	}
	return "unknown";
}

/* Send healthcheck status update to Podman */
bool healthcheck_send_status_update(const char *container_id, int status, int exit_code)
{
	if (container_id == NULL) {
		return false;
	}

	const char *status_str = healthcheck_status_to_string(status);

	/* Create simple JSON message for status update */
	char json_msg[1024];
	snprintf(json_msg, sizeof(json_msg),
		 "{\"type\":\"healthcheck_status\",\"container_id\":\"%s\",\"status\":\"%s\",\"exit_code\":%d,\"timestamp\":%ld}",
		 container_id, status_str, exit_code, time(NULL));

	/* Send via sync pipe to Podman */
	write_or_close_sync_fd(&sync_pipe_fd, HEALTHCHECK_MSG_STATUS_UPDATE, json_msg);

	return true;
}


/* GLib timer callback function */
gboolean healthcheck_timer_callback(gpointer user_data)
{
	healthcheck_timer_t *timer = (healthcheck_timer_t *)user_data;
	if (timer == NULL || !timer->timer_active) {
		return G_SOURCE_REMOVE; /* Stop the timer */
	}

	/* Check if we're still in start period */
	if (timer->start_period_remaining > 0) {
		timer->start_period_remaining -= timer->config.interval;
		if (timer->start_period_remaining > 0) {
			/* Still in startup period - send "starting" status */
			if (timer->status != HEALTHCHECK_STARTING) {
				timer->status = HEALTHCHECK_STARTING;
				healthcheck_send_status_update(timer->container_id, timer->status, 0);
			}
			return G_SOURCE_CONTINUE; /* Continue the timer */
		} else {
			/* Startup period just ended - transition to regular healthchecks */
		}
	}

	/* Execute healthcheck command */
	int exit_code;
	bool success = healthcheck_execute_command(&timer->config, timer->container_id, opt_runtime_path, &exit_code);

	if (!success) {
		nwarnf("Failed to execute healthcheck command for container %s", timer->container_id);
		timer->consecutive_failures++;
		timer->status = HEALTHCHECK_UNHEALTHY;
		healthcheck_send_status_update(timer->container_id, timer->status, exit_code);
		return G_SOURCE_CONTINUE; /* Continue the timer */
	}

	/* Check if healthcheck passed */
	if (exit_code == 0) {
		/* Healthcheck passed */
		timer->consecutive_failures = 0;
		if (timer->status != HEALTHCHECK_HEALTHY) {
			timer->status = HEALTHCHECK_HEALTHY;
		}
		/* Always send status update to keep Podman informed */
		healthcheck_send_status_update(timer->container_id, timer->status, exit_code);
	} else {
		/* Healthcheck failed */
		timer->consecutive_failures++;

		/* During startup period, failures don't count against retry limit */
		if (timer->start_period_remaining > 0) {
			/* Still send status update to show we're trying */
			healthcheck_send_status_update(timer->container_id, timer->status, exit_code);
		} else {
			/* Regular healthcheck failure - count against retry limit */
			if (timer->consecutive_failures >= timer->config.retries) {
				timer->status = HEALTHCHECK_UNHEALTHY;
				healthcheck_send_status_update(timer->container_id, timer->status, exit_code);
			}
		}
	}

	timer->last_check_time = time(NULL);

	/* Continue the timer */
	return G_SOURCE_CONTINUE;
}
