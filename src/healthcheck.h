#ifndef HEALTHCHECK_H
#define HEALTHCHECK_H

#include <stdbool.h>
#include <time.h>
#include <pthread.h>
#include <json-c/json.h>

/* Healthcheck status enumeration */
typedef enum { HEALTHCHECK_NONE = 0, HEALTHCHECK_STARTING, HEALTHCHECK_HEALTHY, HEALTHCHECK_UNHEALTHY } healthcheck_status_t;

/* Healthcheck configuration structure */
typedef struct {
	char **test;	  /* Healthcheck command array */
	int interval;	  /* Interval between checks (seconds) */
	int timeout;	  /* Timeout for each check (seconds) */
	int start_period; /* Grace period before first failure counts (seconds) */
	int retries;	  /* Number of consecutive failures before marking unhealthy */
	bool enabled;	  /* Whether healthcheck is enabled */
} healthcheck_config_t;

/* Healthcheck timer structure */
typedef struct {
	char *container_id;	     /* Container ID */
	healthcheck_config_t config; /* Healthcheck configuration */
	healthcheck_status_t status; /* Current healthcheck status */
	int consecutive_failures;    /* Number of consecutive failures */
	int start_period_remaining;  /* Remaining start period (seconds) */
	bool timer_active;	     /* Whether timer is currently active */
	pthread_t timer_thread;	     /* Timer thread */
	time_t last_check_time;	     /* Time of last healthcheck */
} healthcheck_timer_t;

/* Healthcheck message types for communication with Podman */
#define HEALTHCHECK_MSG_STATUS_UPDATE -100

/* Global healthcheck timers hash table */
extern struct hash_table *active_healthcheck_timers;

/* Hash table functions */
struct hash_table *hash_table_new(size_t size);
void hash_table_free(struct hash_table *ht);
void *hash_table_get(struct hash_table *ht, const char *key);
bool hash_table_put(struct hash_table *ht, const char *key, void *value);

/* Healthcheck subsystem management */
bool healthcheck_init(void);
void healthcheck_cleanup(void);

/* Healthcheck configuration management */
void healthcheck_config_free(healthcheck_config_t *config);

/* Healthcheck timer management */
healthcheck_timer_t *healthcheck_timer_new(const char *container_id, const healthcheck_config_t *config);
void healthcheck_timer_free(healthcheck_timer_t *timer);
bool healthcheck_timer_start(healthcheck_timer_t *timer);
void healthcheck_timer_stop(healthcheck_timer_t *timer);

/* Healthcheck command execution */
bool healthcheck_execute_command(const healthcheck_config_t *config, const char *container_id, const char *runtime_path, int *exit_code);

/* Healthcheck status utilities */
char *healthcheck_status_to_string(healthcheck_status_t status);

/* Healthcheck status reporting */
bool healthcheck_send_status_update(const char *container_id, healthcheck_status_t status, int exit_code);

/* Automatic healthcheck discovery from OCI config */
bool healthcheck_discover_from_oci_config(const char *bundle_path, healthcheck_config_t *config);
bool healthcheck_parse_oci_annotations(const char *annotations_json, healthcheck_config_t *config);

/* Timer thread function */
void *healthcheck_timer_thread(void *user_data);

#endif /* HEALTHCHECK_H */