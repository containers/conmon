#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <errno.h>

#include "utils.h"
#include "log_rate.h"
#include "ctr_logging.h"

#define IO_BUF_SIZE      65536
#define SECS_PER_PERIOD  1
#define BILLION          1000000000

static const struct timespec secs_per_period = {SECS_PER_PERIOD, 0};
static size_t bytes_written_this_period = 0;
static struct timespec start_of_this_period;
static log_policy_t log_policy;
static size_t bytes_per_period;
static bool dropping = false;
static struct timespec drop_until;

static int64_t add_timespecs_nano(const struct timespec* first, const struct timespec* second);
static struct timespec add_timespecs(const struct timespec* first, const struct timespec* second);
static struct timespec subtract_timespecs(const struct timespec* first, const struct timespec* second);
static int64_t subtract_timespecs_nano(const struct timespec* first, const struct timespec* second);
static void write_io_bufs(stdpipe_t pipe, char* buf, ssize_t count);
static void sleep_for_the_rest_of_this_period();
static void start_new_period();

bool log_rate_parse_policy(const char* policy_string, log_policy_t* policy) {
	if (policy_string == NULL) {
		*policy = PASSTHROUGH;
		return true;
	}
	if (!strcmp(policy_string, "backpressure")) {
		*policy = BACKPRESSURE;
		return true;
	} else if (!strcmp(policy_string, "drop")) {
		*policy = DROP;
		return true;
	} else if (!strcmp(policy_string, "ignore")) {
		*policy = IGNORE;
		return true;
	} else if (!strcmp(policy_string, "passthrough")) {
		*policy = PASSTHROUGH;
		return true;
	} else {
		return false;
	}
}

bool log_rate_parse_rate_limit(const char* rate_limit_string, size_t* rate_limit) {
	if (rate_limit_string == NULL) {
		rate_limit = 0;
		return true;
	}
	char* endptr;
	size_t unscaled_rate_limit = strtol(rate_limit_string, &endptr, 10);
	if (errno != 0) {
		return false;
	}
	size_t scale = 1;
	switch (*endptr) {
	case '\0':
		break;
	case 'K':
		scale = (size_t)1024;
		break;
	case 'M':
		scale = (size_t)1024 * 1024;
		break;
	case 'G':
		scale = (size_t)1024 * 1024 * 1024;
		break;
	case 'T':
		scale = (size_t)1024 * 1024 * 1024 * 1024;
		break;
	default:
		return false;
	}
	*rate_limit = unscaled_rate_limit * scale;
	return true;
}

void log_rate_init(log_policy_t policy, size_t rate_limit) {
	log_policy = policy;
	bytes_per_period = rate_limit;
	start_new_period();
}

bool log_rate_write_to_logs(stdpipe_t pipe, char *buf, ssize_t num_read) {
	struct timespec now;
	switch (log_policy) {
	case BACKPRESSURE:
		break;
	case DROP:
		if (dropping) {
			clock_gettime(CLOCK_MONOTONIC, &now);
			int64_t diff_nano = subtract_timespecs_nano(&now, &drop_until);
			if (diff_nano < 0) {
				return true;
			} else {
				dropping = false;
				start_new_period();
			}
		}
		break;
	case IGNORE:
		return true;
	case PASSTHROUGH:
		write_to_logs(pipe, buf, num_read);
		return true;
	}
	char* buf_start = buf;
	ssize_t bytes_remaining = num_read;
	clock_gettime(CLOCK_MONOTONIC, &now);
	int64_t diff_nano = subtract_timespecs_nano(&now, &start_of_this_period);
	if (diff_nano < SECS_PER_PERIOD * BILLION) {
		ssize_t bytes_we_can_write = bytes_per_period - bytes_written_this_period;
		if (num_read <= bytes_we_can_write) {
			write_io_bufs(pipe, buf_start, num_read);
			bytes_written_this_period += num_read;
			return true;
		} else {
			write_io_bufs(pipe, buf_start, bytes_we_can_write);
			bytes_written_this_period += bytes_we_can_write;
			buf_start += bytes_we_can_write;
			bytes_remaining = num_read - bytes_we_can_write;
			sleep_for_the_rest_of_this_period();
			start_new_period();
		}
	} else {
		start_new_period();
	}

	ssize_t chunks = bytes_remaining / bytes_per_period;
	ssize_t remainder = bytes_remaining % bytes_per_period;

	for (ssize_t i = 0; i < chunks; ++i) {
		write_io_bufs(pipe, buf_start + i * bytes_per_period, bytes_per_period);
		sleep_for_the_rest_of_this_period();
		start_new_period();
	}
	if (remainder != 0) {
	    if (bytes_written_this_period + remainder > bytes_per_period) {
		    sleep_for_the_rest_of_this_period();
		    start_new_period();
	    }
	    write_io_bufs(pipe, buf_start + (chunks * bytes_per_period), remainder);
	    bytes_written_this_period += remainder;
	}
	return true;
}

int64_t add_timespecs_nano(const struct timespec* first, const struct timespec* second) {
	return (first->tv_sec + second->tv_sec) * BILLION + first->tv_nsec + second->tv_nsec;
}

struct timespec add_timespecs(const struct timespec* first, const struct timespec* second) {
	int64_t sum_nanoseconds = add_timespecs_nano(first, second);
	struct timespec ret = {sum_nanoseconds / BILLION, sum_nanoseconds % BILLION};
	return ret;
}

struct timespec subtract_timespecs(const struct timespec* first, const struct timespec* second) {
	int64_t diff_nanoseconds = subtract_timespecs_nano(first, second);
	struct timespec ret = {diff_nanoseconds / BILLION, diff_nanoseconds % BILLION};
	return ret;
}

int64_t subtract_timespecs_nano(const struct timespec* first, const struct timespec* second) {
	return (first->tv_sec - second->tv_sec) * BILLION + first->tv_nsec - second->tv_nsec;
}

void write_io_bufs(stdpipe_t pipe, char* buf, ssize_t count) {
	ssize_t chunks = count / IO_BUF_SIZE;
	ssize_t remainder = count % IO_BUF_SIZE;
	for (int i = 0; i < chunks; ++i) {
		write_to_logs(pipe, buf + (i * IO_BUF_SIZE), IO_BUF_SIZE);
	}
	write_to_logs(pipe, buf + count - remainder, remainder);
}

void sleep_for_the_rest_of_this_period() {
	int ret;
	struct timespec now, sleep, diff;
	clock_gettime(CLOCK_MONOTONIC, &now);
	diff = subtract_timespecs(&now, &start_of_this_period);
	sleep = subtract_timespecs(&secs_per_period, &diff);
	if (sleep.tv_sec < 0 || sleep.tv_nsec < 0) {
		return;
	}
	if (log_policy == DROP) {
		dropping = true;
		drop_until = add_timespecs(&start_of_this_period, &secs_per_period);
		return;
	}
	do {
		ret = nanosleep(&sleep, &sleep);
	} while (ret == -1 && errno == EINTR);
}

void start_new_period() {
	bytes_written_this_period = 0;
	clock_gettime(CLOCK_MONOTONIC, &start_of_this_period);
}
