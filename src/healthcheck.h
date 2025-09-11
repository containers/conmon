#ifndef HEALTHCHECK_H
#define HEALTHCHECK_H

#include <stdbool.h>
#include <time.h>
#include <glib.h>

/* Healthcheck status constants */
#define HEALTHCHECK_NONE 0
#define HEALTHCHECK_STARTING 1
#define HEALTHCHECK_HEALTHY 2
#define HEALTHCHECK_UNHEALTHY 3

/* Static string constants for healthcheck statuses */
extern const char *healthcheck_status_strings[];

/* Healthcheck configuration structure */
typedef struct {
	char **test;	      /* Healthcheck command array */
	int interval;	      /* Interval between checks (seconds) */
	int timeout;	      /* Timeout for each check (seconds) */
	int start_period;     /* Grace period before first failure counts (seconds) */
	unsigned int retries; /* Number of consecutive failures before marking unhealthy */
	bool enabled;	      /* Whether healthcheck is enabled */
} healthcheck_config_t;

/* Healthcheck timer structure */
typedef struct {
	char *container_id;		   /* Container ID */
	healthcheck_config_t config;	   /* Healthcheck configuration */
	int status;			   /* Current healthcheck status */
	unsigned int consecutive_failures; /* Number of consecutive failures */
	int start_period_remaining;	   /* Remaining start period (seconds) - DEPRECATED, use start_time */
	bool timer_active;		   /* Whether timer is currently active */
	guint timer_id;			   /* GLib timer ID */
	time_t last_check_time;		   /* Time of last healthcheck */
	time_t start_time;		   /* Time when timer started (for elapsed time calculation) */
} healthcheck_timer_t;

/* Healthcheck message types for communication with Podman */
#define HEALTHCHECK_MSG_STATUS_UPDATE -100

/* Global healthcheck timer (one per conmon instance) */
extern healthcheck_timer_t *active_healthcheck_timer;

/* Healthcheck subsystem management */
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
const char *healthcheck_status_to_string(int status);

/* Healthcheck status reporting */
bool healthcheck_send_status_update(const char *container_id, int status, int exit_code);

/* Healthcheck configuration validation */
bool healthcheck_validate_config(const healthcheck_config_t *config);

/* GLib timer callback function */
gboolean healthcheck_timer_callback(gpointer user_data);
gboolean healthcheck_delayed_start_callback(gpointer user_data);

#endif /* HEALTHCHECK_H */
