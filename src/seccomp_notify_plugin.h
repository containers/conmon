#ifndef SECCOMP_NOTIFY_PLUGIN_H

#ifdef USE_SECCOMP

#include <linux/seccomp.h>

struct seccomp_notify_conf_s {
	const char *runtime_root_path;
	const char *name;
	const char *bundle_path;
	const char *oci_config_path;
};

/* The plugin doesn't know how to handle the request.  */
#define RUN_OCI_SECCOMP_NOTIFY_HANDLE_NOT_HANDLED 0
/* The plugin filled the response and it is ready to write.  */
#define RUN_OCI_SECCOMP_NOTIFY_HANDLE_SEND_RESPONSE 1
/* The plugin will handle the request and write directly to the fd.  */
#define RUN_OCI_SECCOMP_NOTIFY_HANDLE_DELAYED_RESPONSE 2
/* Specify SECCOMP_USER_NOTIF_FLAG_CONTINUE in the flags.  */
#define RUN_OCI_SECCOMP_NOTIFY_HANDLE_SEND_RESPONSE_AND_CONTINUE 3

/* Configure the plugin.  Return an opaque pointer that will be used for successive calls.  */
typedef int (*run_oci_seccomp_notify_start_cb)(void **opaque, struct seccomp_notify_conf_s *conf, size_t size_configuration);

/* Try to handle a single request.  It MUST be defined.
   HANDLED specifies how the request was handled by the plugin:
   0: not handled, try next plugin or return ENOTSUP if it is the last plugin.
   RUN_OCI_SECCOMP_NOTIFY_HANDLE_SEND_RESPONSE: sresp filled and ready to be notified to seccomp.
   RUN_OCI_SECCOMP_NOTIFY_HANDLE_DELAYED_RESPONSE: the notification will be handled internally by the plugin and forwarded to seccomp_fd. It
   is useful for asynchronous handling.
*/
typedef int (*run_oci_seccomp_notify_handle_request_cb)(void *opaque, struct seccomp_notif_sizes *sizes, struct seccomp_notif *sreq,
							struct seccomp_notif_resp *sresp, int seccomp_fd, int *handled);

/* Stop the plugin.  The opaque value is the return value from run_oci_seccomp_notify_start.  */
typedef int (*run_oci_seccomp_notify_stop_cb)(void *opaque);

/* Retrieve the API version used by the plugin.  It MUST return 1. */
typedef int (*run_oci_seccomp_notify_plugin_version_cb)();

#endif // USE_SECCOMP
#endif // SECCOMP_NOTIFY_PLUGIN_H
