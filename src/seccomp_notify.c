#define _GNU_SOURCE
#if __STDC_VERSION__ >= 199901L
/* C99 or later */
#else
#error conmon.c requires C99 or later
#endif

#include <errno.h>
#include <sys/ioctl.h>
#include <dlfcn.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <signal.h>
#include <sys/socket.h>

#include "cli.h" // opt_bundle_path
#include "utils.h"
#include "cmsg.h"

#ifdef USE_SECCOMP

#include <sys/sysmacros.h>
#include <linux/seccomp.h>
#include <seccomp.h>

#include "seccomp_notify.h"


#ifndef SECCOMP_USER_NOTIF_FLAG_CONTINUE
#define SECCOMP_USER_NOTIF_FLAG_CONTINUE (1UL << 0)
#endif

static struct seccomp_notify_context_s *seccomp_notify_ctx;

struct plugin {
	void *handle;
	void *opaque;
	run_oci_seccomp_notify_handle_request_cb handle_request_cb;
};

struct seccomp_notify_context_s {
	struct plugin *plugins;
	size_t n_plugins;

	struct seccomp_notif_resp *sresp;
	struct seccomp_notif *sreq;
	struct seccomp_notif_sizes sizes;
};

static inline void *xmalloc0(size_t size);
static void cleanup_seccomp_plugins();

static int seccomp_syscall(unsigned int op, unsigned int flags, void *args);

gboolean seccomp_cb(int fd, GIOCondition condition, G_GNUC_UNUSED gpointer user_data)
{
	if (condition & G_IO_IN) {
		if (seccomp_notify_ctx == NULL)
			return G_SOURCE_REMOVE;

		int ret = seccomp_notify_plugins_event(seccomp_notify_ctx, fd);
		return ret == 0 ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

gboolean seccomp_accept_cb(int fd, G_GNUC_UNUSED GIOCondition condition, G_GNUC_UNUSED gpointer user_data)
{
	ndebugf("about to accept from seccomp_socket_fd: %d", fd);
	int connfd = accept4(fd, NULL, NULL, SOCK_CLOEXEC);
	if (connfd < 0) {
		nwarn("Failed to accept console-socket connection");
		return G_SOURCE_CONTINUE;
	}

	struct file_t listener = recvfd(connfd);
	close(connfd);

	_cleanup_free_ char *oci_config_path = g_strdup_printf("%s/config.json", opt_bundle_path);
	if (oci_config_path == NULL) {
		nwarn("Failed to allocate memory");
		return G_SOURCE_CONTINUE;
	}

	struct seccomp_notify_conf_s conf = {
		.runtime_root_path = NULL,
		.name = opt_name,
		.bundle_path = opt_bundle_path,
		.oci_config_path = oci_config_path,
	};
	int ret = seccomp_notify_plugins_load(&seccomp_notify_ctx, opt_seccomp_notify_plugins, &conf);
	if (ret < 0) {
		nwarn("Failed to initialize seccomp notify plugins");
		return G_SOURCE_CONTINUE;
	}

	g_unix_set_fd_nonblocking(listener.fd, TRUE, NULL);
	g_unix_fd_add(listener.fd, G_IO_IN | G_IO_HUP, seccomp_cb, NULL);
	atexit(cleanup_seccomp_plugins);

	return G_SOURCE_CONTINUE;
}

int seccomp_notify_plugins_load(struct seccomp_notify_context_s **out, const char *plugins, struct seccomp_notify_conf_s *conf)
{
	cleanup_seccomp_notify_context struct seccomp_notify_context_s *ctx = xmalloc0(sizeof *ctx);
	_cleanup_free_ char *b = NULL;
	char *it, *saveptr;
	size_t s;

	if (seccomp_syscall(SECCOMP_GET_NOTIF_SIZES, 0, &ctx->sizes) < 0) {
		pexit("Failed to get notifications size");
		return -1;
	}

	ctx->sreq = xmalloc0(ctx->sizes.seccomp_notif);
	ctx->sresp = xmalloc0(ctx->sizes.seccomp_notif_resp);

	ctx->n_plugins = 1;
	for (it = b; it; it = strchr(it, ':'))
		ctx->n_plugins++;

	ctx->plugins = xmalloc0(sizeof(struct plugin) * (ctx->n_plugins + 1));

	b = strdup(plugins);
	if (b == NULL) {
		pexit("Failed to strdup");
		return -1;
	}
	for (s = 0, it = strtok_r(b, ":", &saveptr); it; s++, it = strtok_r(NULL, ":", &saveptr)) {
		run_oci_seccomp_notify_plugin_version_cb version_cb;
		run_oci_seccomp_notify_start_cb start_cb;
		void *opq = NULL;

		ctx->plugins[s].handle = dlopen(it, RTLD_NOW);
		if (ctx->plugins[s].handle == NULL) {
			pexitf("cannot load `%s`: %s", it, dlerror());
			return -1;
		}

		version_cb = (run_oci_seccomp_notify_plugin_version_cb)dlsym(ctx->plugins[s].handle, "run_oci_seccomp_notify_version");
		if (version_cb != NULL) {
			int version;

			version = version_cb();
			if (version != 1) {
				pexitf("invalid version supported by the plugin `%s`", it);
				return -1;
			}
		}

		ctx->plugins[s].handle_request_cb =
			(run_oci_seccomp_notify_handle_request_cb)dlsym(ctx->plugins[s].handle, "run_oci_seccomp_notify_handle_request");
		if (ctx->plugins[s].handle_request_cb == NULL) {
			pexitf("plugin `%s` doesn't export `run_oci_seccomp_notify_handle_request`", it);
			return -1;
		}

		start_cb = (run_oci_seccomp_notify_start_cb)dlsym(ctx->plugins[s].handle, "run_oci_seccomp_notify_start");
		if (start_cb) {
			int ret;

			ret = start_cb(&opq, conf, sizeof(*conf));
			if (ret != 0) {
				pexitf("error loading `%s`", it);
				return -1;
			}
		}
		ctx->plugins[s].opaque = opq;
	}

	/* Change ownership.  */
	*out = ctx;
	ctx = NULL;
	return 0;
}

int seccomp_notify_plugins_event(struct seccomp_notify_context_s *ctx, int seccomp_fd)
{
	size_t i;
	int ret;
	bool handled = false;

	memset(ctx->sreq, 0, ctx->sizes.seccomp_notif);
	memset(ctx->sresp, 0, ctx->sizes.seccomp_notif_resp);

	ret = ioctl(seccomp_fd, SECCOMP_IOCTL_NOTIF_RECV, ctx->sreq);
	if (ret < 0) {
		if (errno == ENOENT)
			return 0;
		nwarnf("Failed to read notification from %d", seccomp_fd);
		return -1;
	}

	for (i = 0; i < ctx->n_plugins; i++) {
		if (ctx->plugins[i].handle_request_cb) {
			int resp_handled = 0;
			int ret;

			ret = ctx->plugins[i].handle_request_cb(ctx->plugins[i].opaque, &ctx->sizes, ctx->sreq, ctx->sresp, seccomp_fd,
								&resp_handled);
			if (ret != 0) {
				nwarnf("Failed to handle seccomp notification from fd %d", seccomp_fd);
				return -1;
			}

			switch (resp_handled) {
			case RUN_OCI_SECCOMP_NOTIFY_HANDLE_NOT_HANDLED:
				break;

			case RUN_OCI_SECCOMP_NOTIFY_HANDLE_SEND_RESPONSE:
				handled = true;
				break;

			/* The plugin will take care of it.  */
			case RUN_OCI_SECCOMP_NOTIFY_HANDLE_DELAYED_RESPONSE:
				return 0;

			case RUN_OCI_SECCOMP_NOTIFY_HANDLE_SEND_RESPONSE_AND_CONTINUE:
				ctx->sresp->flags |= SECCOMP_USER_NOTIF_FLAG_CONTINUE;
				handled = true;
				break;

			default:
				pexitf("Unknown handler action specified %d", handled);
				return -1;
			}
		}
	}

	/* No plugin could handle the request.  */
	if (!handled) {
		ctx->sresp->error = -ENOTSUP;
		ctx->sresp->flags = 0;
	}

	ctx->sresp->id = ctx->sreq->id;
	ret = ioctl(seccomp_fd, SECCOMP_IOCTL_NOTIF_SEND, ctx->sresp);
	if (ret < 0) {
		if (errno == ENOENT)
			return 0;
		nwarnf("Failed to send seccomp notification on fd %d", seccomp_fd);
		return -errno;
	}
	return 0;
}

int seccomp_notify_plugins_free(struct seccomp_notify_context_s *ctx)
{
	size_t i;

	if (ctx == NULL) {
		nwarnf("Invalid seccomp notification context");
		return -1;
	}

	free(ctx->sreq);
	free(ctx->sresp);

	for (i = 0; i < ctx->n_plugins; i++) {
		if (ctx->plugins && ctx->plugins[i].handle) {
			run_oci_seccomp_notify_stop_cb cb;

			cb = (run_oci_seccomp_notify_stop_cb)dlsym(ctx->plugins[i].handle, "run_oci_seccomp_notify_stop");
			if (cb)
				cb(ctx->plugins[i].opaque);
			dlclose(ctx->plugins[i].handle);
		}
	}

	free(ctx);

	return 0;
}

static void cleanup_seccomp_plugins()
{
	if (seccomp_notify_ctx) {
		seccomp_notify_plugins_free(seccomp_notify_ctx);
		seccomp_notify_ctx = NULL;
	}
}

void cleanup_seccomp_notify_pluginsp(void *p)
{
	struct seccomp_notify_context_s **pp = p;
	if (*pp) {
		seccomp_notify_plugins_free(*pp);
		*pp = NULL;
	}
}

static inline void *xmalloc0(size_t size)
{
	void *res = calloc(1, size);
	if (res == NULL)
		pexitf("calloc");
	return res;
}

static int seccomp_syscall(unsigned int op, unsigned int flags, void *args)
{
	errno = 0;
	return syscall(__NR_seccomp, op, flags, args);
}
#else
gboolean seccomp_accept_cb(G_GNUC_UNUSED int fd, G_GNUC_UNUSED GIOCondition condition, G_GNUC_UNUSED gpointer user_data)
{
	pexit("seccomp support not available");
	return G_SOURCE_REMOVE;
}
#endif
