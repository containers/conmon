#ifndef SECCOMP_NOTIFY_H
#define SECCOMP_NOTIFY_H

#include "seccomp_notify_plugin.h"

#ifdef USE_SECCOMP

struct seccomp_notify_context_s;

gboolean seccomp_cb(int fd, GIOCondition condition, G_GNUC_UNUSED gpointer user_data);

int seccomp_notify_plugins_load(struct seccomp_notify_context_s **out, const char *plugins, struct seccomp_notify_conf_s *conf);
int seccomp_notify_plugins_event(struct seccomp_notify_context_s *ctx, int seccomp_fd);
int seccomp_notify_plugins_free(struct seccomp_notify_context_s *ctx);

#define cleanup_seccomp_notify_context __attribute__((cleanup(cleanup_seccomp_notify_pluginsp)))
void cleanup_seccomp_notify_pluginsp(void *p);

#endif // USE_SECCOMP
gboolean seccomp_accept_cb(int fd, G_GNUC_UNUSED GIOCondition condition, G_GNUC_UNUSED gpointer user_data);
#endif // SECCOMP_NOTIFY_H
