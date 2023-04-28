/*
 * Copyright 2016 SUSE LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* NOTE: This code comes directly from runc/libcontainer/utils/cmsg.c. */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "cmsg.h"

#if __STDC_VERSION__ >= 199901L
/* C99 or later */
#else
#error cmsg.c requires C99 or later
#endif

#ifdef __FreeBSD__
#define ECOMM EINVAL
#endif

#define error(s) \
	do { \
		fprintf(stderr, "conmon: %s %s\n", s, strerror(errno)); \
		errno = ECOMM; \
		goto err; /* return value */ \
	} while (0)

#define errorf(fmt, ...) \
	do { \
		fprintf(stderr, "conmon: " fmt ": %s\n", ##__VA_ARGS__, strerror(errno)); \
		errno = ECOMM; \
		goto err; /* return value */ \
	} while (0)

/*
 * Sends a file descriptor along the sockfd provided. Returns the return
 * value of sendmsg(2). Any synchronisation and preparation of state
 * should be done external to this (we expect the other side to be in
 * recvfd() in the code).
 */
ssize_t sendfd(int sockfd, struct file_t file)
{
	struct msghdr msg = {0};
	struct iovec iov[1] = {{0}};
	struct cmsghdr *cmsg;
	int *fdptr;

	union {
		char buf[CMSG_SPACE(sizeof(file.fd))];
		struct cmsghdr align;
	} u;

	/*
	 * We need to send some other data along with the ancillary data,
	 * otherwise the other side won't receive any data. This is very
	 * well-hidden in the documentation (and only applies to
	 * SOCK_STREAM). See the bottom part of unix(7).
	 */
	iov[0].iov_base = file.name;
	iov[0].iov_len = strlen(file.name) + 1;

	msg.msg_name = NULL;
	msg.msg_namelen = 0;
	msg.msg_iov = iov;
	msg.msg_iovlen = 1;
	msg.msg_control = u.buf;
	msg.msg_controllen = sizeof(u.buf);

	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));

	fdptr = (int *)CMSG_DATA(cmsg);
	memcpy(fdptr, &file.fd, sizeof(int));

	return sendmsg(sockfd, &msg, 0);
}

/*
 * Receives a file descriptor from the sockfd provided. Returns the file
 * descriptor as sent from sendfd(). It will return the file descriptor
 * or die (literally) trying. Any synchronisation and preparation of
 * state should be done external to this (we expect the other side to be
 * in sendfd() in the code).
 */
struct file_t recvfd(int sockfd)
{
	struct msghdr msg = {0};
	struct iovec iov[1] = {{0}};
	struct cmsghdr *cmsg;
	struct file_t file = {0};
	int *fdptr;
	int olderrno;

	union {
		char buf[CMSG_SPACE(sizeof(file.fd))];
		struct cmsghdr align;
	} u;

	/* Allocate a buffer. */
	/* TODO: Make this dynamic with MSG_PEEK. */
	file.name = malloc(TAG_BUFFER);
	if (!file.name)
		error("recvfd: failed to allocate file.tag buffer");

	/*
	 * We need to "receive" the non-ancillary data even though we don't
	 * plan to use it at all. Otherwise, things won't work as expected.
	 * See unix(7) and other well-hidden documentation.
	 */
	iov[0].iov_base = file.name;
	iov[0].iov_len = TAG_BUFFER;

	msg.msg_name = NULL;
	msg.msg_namelen = 0;
	msg.msg_iov = iov;
	msg.msg_iovlen = 1;
	msg.msg_control = u.buf;
	msg.msg_controllen = sizeof(u.buf);

	ssize_t ret = recvmsg(sockfd, &msg, 0);
	if (ret < 0)
		goto err;

	cmsg = CMSG_FIRSTHDR(&msg);
	if (!cmsg)
		error("recvfd: got NULL from CMSG_FIRSTHDR");
	if (cmsg->cmsg_level != SOL_SOCKET)
		errorf("recvfd: expected SOL_SOCKET in cmsg: %d", cmsg->cmsg_level);
	if (cmsg->cmsg_type != SCM_RIGHTS)
		errorf("recvfd: expected SCM_RIGHTS in cmsg: %d", cmsg->cmsg_type);
	if (cmsg->cmsg_len != CMSG_LEN(sizeof(int)))
		errorf("recvfd: expected correct CMSG_LEN in cmsg: %lu", (unsigned long)cmsg->cmsg_len);

	fdptr = (int *)CMSG_DATA(cmsg);
	if (!fdptr || *fdptr < 0)
		error("recvfd: received invalid pointer");

	file.fd = *fdptr;
	return file;

err:
	olderrno = errno;
	free(file.name);
	errno = olderrno;
	return (struct file_t){.name = NULL, .fd = -1};
}
