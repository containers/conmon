#include "oom.h"
#include "utils.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#define OOM_SCORE "-1000"

void attempt_oom_adjust()
{
	int oom_score_fd = open("/proc/self/oom_score_adj", O_WRONLY);
	if (oom_score_fd < 0) {
		ndebugf("failed to open /proc/self/oom_score_adj: %s\n", strerror(errno));
		return;
	}
	if (write(oom_score_fd, OOM_SCORE, strlen(OOM_SCORE)) < 0) {
		ndebugf("failed to write to /proc/self/oom_score_adj: %s\n", strerror(errno));
	}
	close(oom_score_fd);
}
