#pragma once
#if !defined(LOG_RATE_H)
#define LOG_RATE_H

typedef enum {
	BACKPRESSURE,
	DROP,
	IGNORE,
	PASSTHROUGH
} log_policy_t;

bool log_rate_parse_policy(const char* policy_string, log_policy_t* policy);
bool log_rate_parse_rate_limit(const char* rate_limit_string, size_t* rate_limit);
void log_rate_init(log_policy_t policy, size_t rate_limit);
bool log_rate_write_to_logs(stdpipe_t pipe, char* buf, ssize_t num_read);

#endif /* !defined(LOG_RATE_H) */
