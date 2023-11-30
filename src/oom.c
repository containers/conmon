#define _GNU_SOURCE

#include "oom.h"
#include "utils.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

int old_oom_score = 0;

static void write_oom_adjust(int oom_score, int *old_value)
{
#ifdef __linux__
	char fmt_oom_score[16];
	int oom_score_fd = open("/proc/self/oom_score_adj", O_RDWR | O_CLOEXEC);
	if (oom_score_fd < 0) {
		ndebugf("failed to open /proc/self/oom_score_adj: %s\n", strerror(errno));
		return;
	}
	if (old_value) {
		if (read(oom_score_fd, fmt_oom_score, sizeof(fmt_oom_score)) < 0) {
			ndebugf("failed to read from /proc/self/oom_score_adj: %s\n", strerror(errno));
		}
		*old_value = atoi(fmt_oom_score);
	}
	sprintf(fmt_oom_score, "%d", oom_score);
	if (write(oom_score_fd, fmt_oom_score, strlen(fmt_oom_score)) < 0) {
		ndebugf("failed to write to /proc/self/oom_score_adj: %s\n", strerror(errno));
	}
	close(oom_score_fd);
#else
	(void)oom_score;
	(void)old_value;
#endif
}

void attempt_oom_adjust(int oom_score)
{
	write_oom_adjust(oom_score, &old_oom_score);
}

void reset_oom_adjust()
{
	write_oom_adjust(old_oom_score, NULL);
}
