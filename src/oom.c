#include "oom.h"
#include "utils.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

void attempt_oom_adjust(const char *const oom_score)
{
#ifdef __linux__
	int oom_score_fd = open("/proc/self/oom_score_adj", O_WRONLY);
	if (oom_score_fd < 0) {
		ndebugf("failed to open /proc/self/oom_score_adj: %s\n", strerror(errno));
		return;
	}
	if (write(oom_score_fd, oom_score, strlen(oom_score)) < 0) {
		ndebugf("failed to write to /proc/self/oom_score_adj: %s\n", strerror(errno));
	}
	close(oom_score_fd);
#else
	(void)oom_score;
#endif
}
