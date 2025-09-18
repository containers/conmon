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
#include <pthread.h>
#include <json-c/json.h>

/* Healthcheck validation constants */
#define HEALTHCHECK_INTERVAL_MIN 1
#define HEALTHCHECK_INTERVAL_MAX 3600
#define HEALTHCHECK_TIMEOUT_MIN 1
#define HEALTHCHECK_TIMEOUT_MAX 300
#define HEALTHCHECK_START_PERIOD_MIN 0
#define HEALTHCHECK_START_PERIOD_MAX 3600
#define HEALTHCHECK_RETRIES_MIN 0
#define HEALTHCHECK_RETRIES_MAX 100

/* Simple hash table implementation */
struct hash_entry {
	char *key;
	void *value;
	struct hash_entry *next;
};

struct hash_table {
	struct hash_entry **buckets;
	size_t size;
	size_t count;
};

static unsigned int hash_string(const char *str)
{
	unsigned int hash = 5381;
	int c;
	while ((c = *str++)) {
		hash = ((hash << 5) + hash) + c;
	}
	return hash;
}

struct hash_table *hash_table_new(size_t size)
{
	struct hash_table *ht = calloc(1, sizeof(struct hash_table));
	if (!ht) {
		return NULL;
	}

	ht->buckets = calloc(size, sizeof(struct hash_entry *));
	if (!ht->buckets) {
		free(ht);
		return NULL;
	}

	ht->size = size;
	return ht;
}

void hash_table_free(struct hash_table *ht)
{
	if (!ht)
		return;

	for (size_t i = 0; i < ht->size; i++) {
		struct hash_entry *entry = ht->buckets[i];
		while (entry) {
			struct hash_entry *next = entry->next;
			free(entry->key);
			free(entry);
			entry = next;
		}
	}

	free(ht->buckets);
	free(ht);
}

void *hash_table_get(struct hash_table *ht, const char *key)
{
	if (!ht || !key)
		return NULL;

	unsigned int hash = hash_string(key) % ht->size;
	struct hash_entry *entry = ht->buckets[hash];

	while (entry) {
		if (strcmp(entry->key, key) == 0) {
			return entry->value;
		}
		entry = entry->next;
	}

	return NULL;
}

bool hash_table_put(struct hash_table *ht, const char *key, void *value)
{
	if (!ht || !key)
		return false;

	unsigned int hash = hash_string(key) % ht->size;
	struct hash_entry *entry = ht->buckets[hash];

	/* Check if key already exists */
	while (entry) {
		if (strcmp(entry->key, key) == 0) {
			entry->value = value;
			return true;
		}
		entry = entry->next;
	}

	/* Create new entry */
	entry = malloc(sizeof(struct hash_entry));
	if (!entry)
		return false;

	entry->key = strdup(key);
	if (!entry->key) {
		free(entry);
		return false;
	}

	entry->value = value;
	entry->next = ht->buckets[hash];
	ht->buckets[hash] = entry;
	ht->count++;

	return true;
}

/* Global healthcheck timers hash table */
struct hash_table *active_healthcheck_timers = NULL;


/* Initialize healthcheck subsystem */
bool healthcheck_init(void)
{
	if (active_healthcheck_timers != NULL) {
		return true;
	}

	active_healthcheck_timers = hash_table_new(16);
	if (active_healthcheck_timers == NULL) {
		return false;
	}

	return true;
}

/* Cleanup healthcheck subsystem */
void healthcheck_cleanup(void)
{
	if (active_healthcheck_timers != NULL) {
		/* Free all timers first */
		for (size_t i = 0; i < active_healthcheck_timers->size; i++) {
			struct hash_entry *entry = active_healthcheck_timers->buckets[i];
			while (entry) {
				if (entry->value != NULL) {
					healthcheck_timer_free((healthcheck_timer_t *)entry->value);
					entry->value = NULL; /* Prevent double-free */
				}
				entry = entry->next;
			}
		}

		/* Now free the hash table structure */
		hash_table_free(active_healthcheck_timers);
		active_healthcheck_timers = NULL;
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
		while (config->test[argc] != NULL)
			argc++;

		timer->config.test = calloc(argc + 1, sizeof(char *));
		if (timer->config.test == NULL) {
			free(timer->container_id);
			free(timer);
			return NULL;
		}

		for (int i = 0; i < argc; i++) {
			timer->config.test[i] = strdup(config->test[i]);
			if (timer->config.test[i] == NULL) {
				for (int j = 0; j < i; j++) {
					free(timer->config.test[j]);
				}
				free(timer->config.test);
				free(timer->container_id);
				free(timer);
				return NULL;
			}
		}
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
		timer->container_id = NULL;
	}

	/* Free test command array */
	if (timer->config.test != NULL) {
		for (int i = 0; timer->config.test[i] != NULL; i++) {
			free(timer->config.test[i]);
		}
		free(timer->config.test);
		timer->config.test = NULL;
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
	/* Create a timer thread */
	int result = pthread_create(&timer->timer_thread, NULL, healthcheck_timer_thread, timer);
	if (result != 0) {
		nwarnf("Failed to create healthcheck timer thread: %s", strerror(result));
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

	/* Wait for the timer thread to finish with a timeout */
	void *thread_result;
	int join_result = pthread_join(timer->timer_thread, &thread_result);
	if (join_result != 0) {
		nwarnf("Failed to join healthcheck timer thread: %s", strerror(join_result));
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

		/* Build runtime command using standard format */
		/* Count arguments needed */
		int argc = 0;
		while (config->test[argc] != NULL) {
			argc++;
		}

		/* Build runtime command using standard format: <runtime> exec <container> <command> */
		char **runtime_argv;
		int runtime_argc;
		/* Standard format: runtime exec container_id command */
		runtime_argc = 3 + argc; /* runtime + exec + container_id + command + NULL */
		runtime_argv = calloc(runtime_argc, sizeof(char *));
		if (runtime_argv == NULL) {
			nwarn("Failed to allocate memory for runtime command");
			_exit(127);
		}

		runtime_argv[0] = (char *)runtime_path; /* Runtime executable */
		runtime_argv[1] = "exec";		/* Runtime subcommand */
		runtime_argv[2] = (char *)container_id; /* Container ID */

		/* Copy healthcheck command arguments */
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
char *healthcheck_status_to_string(healthcheck_status_t status)
{
	switch (status) {
	case HEALTHCHECK_NONE:
		return strdup("none");
	case HEALTHCHECK_STARTING:
		return strdup("starting");
	case HEALTHCHECK_HEALTHY:
		return strdup("healthy");
	case HEALTHCHECK_UNHEALTHY:
		return strdup("unhealthy");
	default:
		return strdup("unknown");
	}
}

/* Send healthcheck status update to Podman */
bool healthcheck_send_status_update(const char *container_id, healthcheck_status_t status, int exit_code)
{
	if (container_id == NULL) {
		return false;
	}

	char *status_str = healthcheck_status_to_string(status);
	if (status_str == NULL) {
		nwarn("Failed to convert healthcheck status to string");
		return false;
	}

	/* Create simple JSON message for status update */
	char json_msg[1024];
	snprintf(json_msg, sizeof(json_msg),
		 "{\"type\":\"healthcheck_status\",\"container_id\":\"%s\",\"status\":\"%s\",\"exit_code\":%d,\"timestamp\":%ld}",
		 container_id, status_str, exit_code, time(NULL));

	free(status_str);

	/* Send via sync pipe to Podman */
	write_or_close_sync_fd(&sync_pipe_fd, HEALTHCHECK_MSG_STATUS_UPDATE, json_msg);

	return true;
}

/* Discover healthcheck configuration from OCI config.json */
bool healthcheck_discover_from_oci_config(const char *bundle_path, healthcheck_config_t *config)
{
	if (bundle_path == NULL || config == NULL) {
		return false;
	}

	char config_path[PATH_MAX];
	snprintf(config_path, sizeof(config_path), "%s/config.json", bundle_path);

	struct stat st;
	if (stat(config_path, &st) != 0) {
		return false;
	}

	/* Read the config file */
	FILE *fp = fopen(config_path, "r");
	if (!fp) {
		nwarnf("Failed to open OCI config: %s", strerror(errno));
		return false;
	}

	/* Read entire file */
	fseek(fp, 0, SEEK_END);
	long file_size = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	char *file_content = malloc(file_size + 1);
	if (!file_content) {
		fclose(fp);
		return false;
	}

	size_t bytes_read = fread(file_content, 1, file_size, fp);
	if (bytes_read != (size_t)file_size) {
		nwarn("Failed to read entire config file");
		free(file_content);
		fclose(fp);
		return false;
	}
	file_content[file_size] = '\0';
	fclose(fp);

	/* Parse JSON using json-c */
	json_object *json = json_tokener_parse(file_content);
	if (json == NULL) {
		nwarn("Failed to parse OCI config JSON");
		free(file_content);
		return false;
	}

	/* Look for annotations */
	json_object *annotations;
	if (json_object_object_get_ex(json, "annotations", &annotations) && json_object_is_type(annotations, json_type_object)) {
		json_object *healthcheck;
		if (json_object_object_get_ex(annotations, "io.podman.healthcheck", &healthcheck)
		    && json_object_is_type(healthcheck, json_type_string)) {
			/* Parse the healthcheck JSON */
			bool result = healthcheck_parse_oci_annotations(json_object_get_string(healthcheck), config);
			json_object_put(json);
			free(file_content);
			return result;
		}
	}

	json_object_put(json);
	free(file_content);
	return false;
}

/* Parse healthcheck configuration from OCI annotations */
bool healthcheck_parse_oci_annotations(const char *annotations_json, healthcheck_config_t *config)
{
	if (annotations_json == NULL || config == NULL) {
		return false;
	}

	/* Parse the JSON using json-c */
	json_object *json = json_tokener_parse(annotations_json);
	if (json == NULL) {
		nwarn("Failed to parse healthcheck JSON");
		return false;
	}

	/* Initialize config - no defaults, all values must be provided by Podman */
	config->enabled = true;
	config->interval = 0;
	config->timeout = 0;
	config->start_period = 0;
	config->retries = 0;
	config->test = NULL;

	/* Parse Test command - REQUIRED */
	json_object *test_array;
	if (!json_object_object_get_ex(json, "test", &test_array) || !json_object_is_type(test_array, json_type_array)
	    || json_object_array_length(test_array) < 2) {
		nwarn("Healthcheck configuration missing required 'test' command");
		json_object_put(json);
		return false;
	}

	json_object *cmd_type = json_object_array_get_idx(test_array, 0);

	if (!json_object_is_type(cmd_type, json_type_string)) {
		nwarn("Healthcheck command type must be a string");
		json_object_put(json);
		return false;
	}

	if (strcmp(json_object_get_string(cmd_type), "CMD") == 0) {
		/* CMD (Exec Form) - Array of command and arguments */
		int array_size = json_object_array_length(test_array);
		if (array_size < 2) {
			nwarn("CMD healthcheck requires at least one command argument");
			json_object_put(json);
			return false;
		}

		/* Allocate memory for command array (excluding the "CMD" type) */
		config->test = calloc(array_size, sizeof(char *));
		if (config->test == NULL) {
			nwarn("Failed to allocate memory for healthcheck test command");
			json_object_put(json);
			return false;
		}

		/* Copy all arguments except the first one (which is "CMD") */
		for (int i = 1; i < array_size; i++) {
			json_object *arg = json_object_array_get_idx(test_array, i);
			if (!json_object_is_type(arg, json_type_string)) {
				nwarnf("CMD healthcheck argument %d must be a string", i);
				for (int j = 0; j < i - 1; j++) {
					free(config->test[j]);
				}
				free(config->test);
				json_object_put(json);
				return false;
			}

			config->test[i - 1] = strdup(json_object_get_string(arg));
			if (config->test[i - 1] == NULL) {
				nwarn("Failed to duplicate healthcheck command argument");
				for (int j = 0; j < i - 1; j++) {
					free(config->test[j]);
				}
				free(config->test);
				json_object_put(json);
				return false;
			}
		}
		config->test[array_size - 1] = NULL;

	} else if (strcmp(json_object_get_string(cmd_type), "CMD-SHELL") == 0) {
		/* CMD-SHELL (Shell Form) - Single string executed via /bin/sh -c */
		if (json_object_array_length(test_array) != 2) {
			nwarn("CMD-SHELL healthcheck requires exactly one command string");
			json_object_put(json);
			return false;
		}

		json_object *cmd_value = json_object_array_get_idx(test_array, 1);
		if (!json_object_is_type(cmd_value, json_type_string)) {
			nwarn("CMD-SHELL healthcheck command must be a string");
			json_object_put(json);
			return false;
		}

		size_t cmd_len = strlen(json_object_get_string(cmd_value));
		const size_t MAX_HEALTHCHECK_CMD_LEN = 4096;

		if (cmd_len == 0) {
			nwarn("Healthcheck command cannot be empty");
			json_object_put(json);
			return false;
		}

		if (cmd_len > MAX_HEALTHCHECK_CMD_LEN) {
			nwarnf("Healthcheck command too long (%zu chars, max %zu)", cmd_len, MAX_HEALTHCHECK_CMD_LEN);
			json_object_put(json);
			return false;
		}

		/* Create test command array for shell execution */
		config->test = calloc(3, sizeof(char *));
		if (config->test == NULL) {
			nwarn("Failed to allocate memory for healthcheck test command");
			json_object_put(json);
			return false;
		}

		config->test[0] = strdup("/bin/sh");
		config->test[1] = strdup("-c");
		config->test[2] = strdup(json_object_get_string(cmd_value));

		if (config->test[0] == NULL || config->test[1] == NULL || config->test[2] == NULL) {
			nwarn("Failed to duplicate healthcheck test command strings");
			for (int i = 0; i < 3; i++) {
				if (config->test[i] != NULL) {
					free(config->test[i]);
				}
			}
			free(config->test);
			json_object_put(json);
			return false;
		}
		config->test[3] = NULL;

	} else {
		nwarnf("Unsupported healthcheck command type: %s (only CMD and CMD-SHELL supported)", json_object_get_string(cmd_type));
		json_object_put(json);
		return false;
	}

	/* Parse Interval (now in seconds) - REQUIRED */
	json_object *interval;
	if (!json_object_object_get_ex(json, "interval", &interval) || !json_object_is_type(interval, json_type_int)) {
		nwarn("Healthcheck interval must be a number");
		json_object_put(json);
		return false;
	}
	int interval_val = json_object_get_int(interval);
	if (interval_val < HEALTHCHECK_INTERVAL_MIN || interval_val > HEALTHCHECK_INTERVAL_MAX) {
		nwarnf("Healthcheck interval must be between %d and %d seconds, got: %d", HEALTHCHECK_INTERVAL_MIN,
		       HEALTHCHECK_INTERVAL_MAX, interval_val);
		json_object_put(json);
		return false;
	}
	config->interval = interval_val;

	/* Parse Timeout (now in seconds) - REQUIRED */
	json_object *timeout;
	if (!json_object_object_get_ex(json, "timeout", &timeout) || !json_object_is_type(timeout, json_type_int)) {
		nwarn("Healthcheck timeout must be a number");
		json_object_put(json);
		return false;
	}
	int timeout_val = json_object_get_int(timeout);
	if (timeout_val < HEALTHCHECK_TIMEOUT_MIN || timeout_val > HEALTHCHECK_TIMEOUT_MAX) {
		nwarnf("Healthcheck timeout must be between %d and %d seconds, got: %d", HEALTHCHECK_TIMEOUT_MIN, HEALTHCHECK_TIMEOUT_MAX,
		       timeout_val);
		json_object_put(json);
		return false;
	}
	config->timeout = timeout_val;

	/* Parse StartPeriod (now in seconds) - REQUIRED (can be 0) */
	json_object *start_period;
	if (!json_object_object_get_ex(json, "start_period", &start_period) || !json_object_is_type(start_period, json_type_int)) {
		nwarn("Healthcheck start_period must be a number");
		json_object_put(json);
		return false;
	}
	int start_period_val = json_object_get_int(start_period);
	if (start_period_val < HEALTHCHECK_START_PERIOD_MIN || start_period_val > HEALTHCHECK_START_PERIOD_MAX) {
		nwarnf("Healthcheck start_period must be between %d and %d seconds, got: %d", HEALTHCHECK_START_PERIOD_MIN,
		       HEALTHCHECK_START_PERIOD_MAX, start_period_val);
		json_object_put(json);
		return false;
	}
	config->start_period = start_period_val;

	/* Parse Retries - REQUIRED */
	json_object *retries;
	if (!json_object_object_get_ex(json, "retries", &retries) || !json_object_is_type(retries, json_type_int)) {
		nwarn("Healthcheck retries must be a number");
		json_object_put(json);
		return false;
	}
	int retries_val = json_object_get_int(retries);
	if (retries_val < HEALTHCHECK_RETRIES_MIN || retries_val > HEALTHCHECK_RETRIES_MAX) {
		nwarnf("Healthcheck retries must be between %d and %d, got: %d", HEALTHCHECK_RETRIES_MIN, HEALTHCHECK_RETRIES_MAX,
		       retries_val);
		json_object_put(json);
		return false;
	}
	config->retries = retries_val;

	/* Clean up */
	json_object_put(json);

	return true;
}

/* Timer thread function */
void *healthcheck_timer_thread(void *user_data)
{
	healthcheck_timer_t *timer = (healthcheck_timer_t *)user_data;
	if (timer == NULL) {
		return NULL;
	}

	/* Make a local copy of the timer pointer to avoid race conditions */
	healthcheck_timer_t *local_timer = timer;


	while (local_timer->timer_active) {
		/* Sleep for the interval, but check timer_active periodically */
		int sleep_time = local_timer->config.interval;
		while (sleep_time > 0 && local_timer->timer_active) {
			if (sleep_time > 1) {
				sleep(1); /* Sleep 1 second at a time */
				sleep_time -= 1;
			} else {
				sleep(sleep_time);
				sleep_time = 0;
			}
		}

		if (!local_timer->timer_active) {
			break;
		}

		/* Check if we're still in start period */
		if (local_timer->start_period_remaining > 0) {
			local_timer->start_period_remaining -= local_timer->config.interval;
			if (local_timer->start_period_remaining > 0) {
				/* Still in startup period - send "starting" status */
				if (local_timer->status != HEALTHCHECK_STARTING) {
					local_timer->status = HEALTHCHECK_STARTING;
					healthcheck_send_status_update(local_timer->container_id, local_timer->status, 0);
				}
				continue;
			} else {
				/* Startup period just ended - transition to regular healthchecks */
			}
		}

		/* Execute healthcheck command */
		int exit_code;
		bool success = healthcheck_execute_command(&local_timer->config, local_timer->container_id, opt_runtime_path, &exit_code);

		if (!success) {
			nwarnf("Failed to execute healthcheck command for container %s", local_timer->container_id);
			local_timer->consecutive_failures++;
			local_timer->status = HEALTHCHECK_UNHEALTHY;
			healthcheck_send_status_update(local_timer->container_id, local_timer->status, exit_code);
			continue;
		}

		/* Check if healthcheck passed */
		if (exit_code == 0) {
			/* Healthcheck passed */
			local_timer->consecutive_failures = 0;
			if (local_timer->status != HEALTHCHECK_HEALTHY) {
				local_timer->status = HEALTHCHECK_HEALTHY;
			}
			/* Always send status update to keep Podman informed */
			healthcheck_send_status_update(local_timer->container_id, local_timer->status, exit_code);
		} else {
			/* Healthcheck failed */
			local_timer->consecutive_failures++;

			/* During startup period, failures don't count against retry limit */
			if (local_timer->start_period_remaining > 0) {
				/* Still send status update to show we're trying */
				healthcheck_send_status_update(local_timer->container_id, local_timer->status, exit_code);
			} else {
				/* Regular healthcheck failure - count against retry limit */
				if (local_timer->consecutive_failures >= local_timer->config.retries) {
					local_timer->status = HEALTHCHECK_UNHEALTHY;
					healthcheck_send_status_update(local_timer->container_id, local_timer->status, exit_code);
				} else {
				}
			}
		}

		local_timer->last_check_time = time(NULL);
	}

	return NULL;
}