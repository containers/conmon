#pragma once
#if !defined(LOG_RATE_H)
#define LOG_RATE_H 

void log_rate_init();
bool log_rate_write_to_logs(stdpipe_t pipe, char *buf, ssize_t num_read);

#endif /* !defined(LOG_RATE_H) */
