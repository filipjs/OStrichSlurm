/*****************************************************************************\
 *  load_leveler.c - Provide an srun command line interface over LoadLeveler.
 *****************************************************************************
 *  Copyright (C) 2011-2012 SchedMD <http://www.schedmd.com>.
 *  Written by Morris Jette <jette@schedmd.com>
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://www.schedmd.com/slurmdocs/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#ifdef HAVE_CONFIG_H
#  include "config.h"
#  ifdef HAVE_PTY_H
#    include <pty.h>
#  endif
#endif

#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/param.h>	/* MAXPATHLEN */
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <utmp.h>

#include "slurm/slurm.h"
#include "slurm/slurm_errno.h"

#include "src/common/fd.h"
#include "src/common/hostlist.h"
#include "src/common/jobacct_common.h"
#include "src/common/log.h"
#include "src/common/mpi.h"
#include "src/common/net.h"
#include "src/common/pack.h"
#include "src/common/plugstack.h"
#include "src/common/read_config.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/slurm_protocol_defs.h"
#include "src/common/slurm_rlimits_info.h"
#include "src/common/switch.h"
#include "src/common/uid.h"
#include "src/common/xmalloc.h"
#include "src/common/xsignal.h"
#include "src/common/xstring.h"

#include "src/srun/load_leveler.h"
#include "src/srun/opt.h"

extern char **environ;

#ifdef USE_LOADLEVELER
/* PTY_MODE indicates if the srun back-end is to spawn its task using a
 * psuedo-terminate for stdio/out/err. If set, then stdout and stderr are
 * combined into a single data stream, but the output is flushed regularly.
 * If not set, then I/O may not be flushed very regularly. We might want this
 * to be configurable by job. */
#define PTY_MODE false

/* Set this to generate debugging information for srun front-end/back-end
 *	program communications */
#define _DEBUG_SRUN 0

/* Timeout for srun front-end/back-end messages in usec */
#define MSG_TIMEOUT 5000000

/* Timeout in seconds for select call, if no I/O then test for existance
 * of job this frequently */
#define SELECT_TIMEOUT 10

static slurm_fd_t global_signal_conn = SLURM_SOCKET_ERROR;
static bool disable_status = false;
static bool quit_on_intr = false;
static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
static int srun_state = 0;	/* 0=starting, 1=running, 2=ending */
static char *srun_jobid = NULL;
static char *cmd_fname = NULL;
static char *stepid_fname = NULL;

typedef struct srun_child_wait_data {
	int dummy_pipe;
	bool *job_fini_ptr;
	pid_t pid;
	slurm_fd_t signal_socket;
	int *status_ptr;
} srun_child_wait_data_t;

/*****************************************************************************\
 * Local helper functions for srun front-end/back-end support
 * NOTE: These functions are needed even without llapi.h
\*****************************************************************************/

/* srun back-end function, read message from signal_socket and signal the
 *	specified process
 * signal_socket IN - socket to read message from
 * pid IN - process to be signalled
 * RETURN true on EOF */
static bool _be_proc_signal(slurm_fd_t signal_socket, pid_t pid)
{
	uint32_t sig_num = 0;
	int read_len;
	bool fini_rc = true;

	/* Read and process message header */
	read_len = slurm_read_stream(signal_socket, (char *)&sig_num,
				     sizeof(sig_num));
	if (read_len == -1) {
		error("signal read error: %m");
		/* Error, treat like EOF */
	} else if (read_len == 0) {
		/* EOF */
	} else if (read_len < sizeof(sig_num)) {
		error("signal read header, bad size (%d < %lu)",
		      read_len, sizeof(sig_num));
		/* Can not recover, treat like EOF */
	} else {
		if (kill(pid, (int) sig_num))
			error("signal(%u): %m", sig_num);
		else {
#if _DEBUG_SRUN
			info("signal(%u) sent", sig_num);
#endif
		}
		fini_rc = false;
	}

	return fini_rc;
}

/* srun back-end function, read message from stdin_socket and write to pipe.
 * stdin_pipe IN   - pipe to spawned process, write message here
 * stdin_socket IN - socket to read message from
 * RETURN true on EOF */
static bool _be_proc_stdin(int stdin_pipe, slurm_fd_t stdin_socket)
{
	char *buf;
	uint32_t buf_len = 0;
	int read_len;
	bool fini_rc = true;

	/* Read and process message header */
	read_len = slurm_read_stream(stdin_socket, (char *)&buf_len,
				     sizeof(buf_len));
	if (read_len == -1) {
		if (errno != SLURM_PROTOCOL_SOCKET_ZERO_BYTES_SENT)
			error("stdin read header error: %m");
		return fini_rc;	/* Error, treat like EOF */
	}
	if (read_len == 0)
		return fini_rc;	/* Abnornal EOF */
	if (read_len < sizeof(buf_len)) {
		error("stdin read header, bad size (%d < %lu)",
		      read_len, sizeof(buf_len));
		return fini_rc;	/* Can not recover, treat like EOF */
	}
	if (buf_len == NO_VAL) {
#if _DEBUG_SRUN
		info("stdin EOF");
#endif
		return fini_rc;	/* EOF received */
	}

	/* Read and process message data */
	fini_rc = false;
	buf = xmalloc(buf_len+1);
	read_len = slurm_read_stream(stdin_socket, buf, buf_len);
	if (read_len < 0) {
		error("stdin read buffer: %m");
	} else if (read_len < buf_len) {
		error("stdin read short (%d < %d)", read_len, buf_len);
	} else {
		int offset = 0, write_len;
		buf[read_len] = '\0';
#if _DEBUG_SRUN
		info("stdin:%s:%d", buf, read_len);
#endif
		while (offset < read_len) {
			write_len = write(stdin_pipe, buf+offset,
					  read_len-offset);
			if (write_len < 0) {
				if ((errno == EAGAIN) && (errno == EINTR))
					continue;
				error("stdin write: %m");
				fini_rc = false;
				break;
			} else {
				offset += write_len;
			}
		}
	}
	xfree(buf);

	return fini_rc;
}

/* srun back-end function, read message from stdout or stderr pipe and write
 *	it to a socket.
 * stdio_pipe IN   - stdout or stderr pipe to read data from
 * stdio_socket IN - stdout or stderr pipe to write data to
 * stdio_name IN   - "stdout" or "stderr" for logging
 * RETURN true on EOF
 */
static bool _be_proc_stdio(int stdio_pipe, slurm_fd_t stdio_socket,
			   char *stdio_name)
{
	char buf[16 * 1024];
	uint32_t buf_len;
	int read_len, write_len;
	bool fini_rc = false;

	read_len = read(stdio_pipe, buf, (sizeof(buf) - 1));
	if (read_len > 0) {
		buf_len =  (uint32_t) read_len;
		write_len = slurm_write_stream(stdio_socket, (char *)&buf_len,
					       sizeof(buf_len));
		if (write_len < 0) {
			error("%s write_stream: %m", stdio_name);
		} else {
			write_len = slurm_write_stream(stdio_socket, buf,
						       buf_len);
		}
#if _DEBUG_SRUN
		buf[buf_len] = '\0';
		info("%s:%s:%u", stdio_name, buf, buf_len);
#endif
	} else if (read_len < 0) {
		if ((errno ==  EAGAIN) || (errno == EINTR)) {
			debug("%s read: %m", stdio_name);
		} else if (errno == EIO) {	/* This from PTY mode */
			debug("%s read: %m", stdio_name);
			fini_rc = true;	/* Can not recover from error */
		} else {
			error("%s read: %m", stdio_name);
			fini_rc = true;	/* Can not recover from error */
		}
	} else {	/* read_len == 0 means EOF */
#if _DEBUG_SRUN
		info("%s EOF", stdio_name);
#endif
		buf_len =  NO_VAL;
		write_len = slurm_write_stream(stdio_socket, (char *)&buf_len,
					       sizeof(buf_len));
		fini_rc = true;
	}

	return fini_rc;
}

/* srun front-end function, read message from local stdin and write it to
 *	stdin_socket socket.
 * stdin_fd IN - the local stdin file descriptor to read message from
 * stdin_socket IN - the socket to write the message to
 * RETURN true on EOF
 */
static bool _fe_proc_stdin(slurm_fd_t stdin_fd, slurm_fd_t stdin_socket)
{
	char buf[16 * 1024];
	int buf_len, in_len;
	uint32_t msg_len = 0;

	in_len = read(stdin_fd, buf, (sizeof(buf) - 1));
	if (in_len < 0) {
		error("stdin read: %m");
		return false;
	}
	if (in_len == 0) {
#if _DEBUG_SRUN
		info("stdin EOF");
#endif
		msg_len = NO_VAL;
	} else {
		msg_len = in_len;
	}

	buf_len = slurm_write_stream_timeout(stdin_socket, (char *)&msg_len,
					     sizeof(msg_len), MSG_TIMEOUT);
	/* NOTE: Do not change test below
	 * (-1 < sizeof(msg_len)) is false since
	 * -1 gets converted to unsigned long first */
	if ((buf_len < 0) || (buf_len < sizeof(msg_len))) {
		error("stdin write: %m");
		return false;
	}
	if (msg_len == NO_VAL)
		return true;

	buf_len = slurm_write_stream_timeout(stdin_socket, buf, msg_len,
					     MSG_TIMEOUT);
	if ((buf_len < 0) || (buf_len < msg_len)) {
		error("stdin write: %m");
	} else {
#if _DEBUG_SRUN
		buf[msg_len] = '\0';
		info("stdin:%s:%d", buf, msg_len);
#endif
	}
	return false;
}

/* srun front-end exit code recording function
 * signal_socket IN - socket to read exit code from
 * RETURN the exit code from the remote process
 */
static int _fe_proc_exit(slurm_fd_t signal_socket)
{
	int return_code = 0, status;
	size_t buf_len;
	uint32_t status_32;

	buf_len = slurm_read_stream_timeout(signal_socket, (char *)&status_32,
					    sizeof(status_32), MSG_TIMEOUT);
	if (buf_len < sizeof(status_32)) {
		error("signal ERROR: %m");
		return 1;
	}

	status = status_32;
	if (WIFEXITED(status))
		return_code = WEXITSTATUS(status);

#if _DEBUG_SRUN
	if (WIFEXITED(status)) {
		info("exit status: %d", return_code);
	} else if (WIFSIGNALED(status))
		info("exit signaled: %d", WTERMSIG(status));
	else
		info("exit code: %d", status);
#endif

	return return_code;
}

/* srun front-end I/O function, read message from stdout/stderr socket and
 *	write it to local stdout/stderr file descriptor
 * stdio_socket IN - socket to read message from
 * stdio_fd IN - file descriptor to write to
 * stdio_name IN   - "stdout" or "stderr" for logging
 * RETURN true on EOF
*/
static bool _fe_proc_stdio(slurm_fd_t stdio_socket, int stdio_fd,
			   char *stdio_name)
{
	char *buf;
	uint32_t msg_len = 0;
	int buf_inx = 0, buf_len;

	buf_len = slurm_read_stream_timeout(stdio_socket, (char *)&msg_len,
					    sizeof(msg_len), MSG_TIMEOUT);
	if ((buf_len < 0) || (buf_len < sizeof(msg_len))) {
		error("%s read header: %m", stdio_name);
		return true;
	}
	if (msg_len == NO_VAL) {
#if _DEBUG_SRUN
		info("%s EOF", stdio_name);
#endif
		return true;
	}

	buf = xmalloc(msg_len + 1);
	buf_len = slurm_read_stream_timeout(stdio_socket, buf+buf_inx, msg_len,
					    MSG_TIMEOUT);
	if ((buf_len < 0) || (buf_len < msg_len)) {
		error("%s read buffer: %m", stdio_name);
		if (buf_len < 0)
			return true;
	} else if (buf_len == 0) {
#if _DEBUG_SRUN
		info("%s EOF", stdio_name);
#endif
		return true;
	}
#if _DEBUG_SRUN
	buf[buf_len] = '\0';
	info("%s:%s:%d", stdio_name, buf, buf_len);
#else
{
	int offset = 0, write_len;
	while (offset < buf_len) {
		write_len = write(stdio_fd, buf+offset, buf_len-offset);
		if (write_len < 0) {
			if ((errno == EAGAIN) && (errno == EINTR))
				continue;
			error("%s write: %m", stdio_name);
			break;
		} else {
			offset += write_len;
		}
	}
}
#endif
	xfree(buf);
	return false;
}


/* Test that the front-end job still exists */
static void _fe_test_job_state(void)
{
	int i, rc;
	job_info_t *job_ptr;
	job_info_msg_t *job_info_msg;
	bool job_active = false;

	rc = slurm_load_job(&job_info_msg, srun_jobid, SHOW_ALL);
	if (rc != SLURM_SUCCESS)
		return;

	for (i = 0; i < job_info_msg->record_count; i++) {
		if (strcmp(job_info_msg->job_array[i].job_id, srun_jobid))
			continue;
		job_ptr = &job_info_msg->job_array[i];
		if (job_ptr->job_state < JOB_COMPLETE)
			job_active = true;
		break;
	}
	slurm_free_job_info_msg(job_info_msg);

	if (!job_active) {
		info("job %s completed, aborting", srun_jobid);
		pthread_mutex_lock(&state_mutex);
		srun_state = 2;
		pthread_mutex_unlock(&state_mutex);
	}
}

/* write the exit status of spawned back-end process to the srun front-end */
static void _be_proc_status(int status, slurm_fd_t signal_socket)
{
	uint32_t status_32;

	status_32 = (uint32_t) status;
	if (slurm_write_stream(signal_socket, (char *)&status_32,
			       sizeof(status_32)) < 0) {
		error("slurm_write_stream(exit_status): %m");
	}
}

/* Generate and return a pseudo-random authentication key */
static uint32_t _gen_auth_key(void)
{
	struct timeval tv;
	uint32_t key;

	gettimeofday(&tv, NULL);
	key  = (tv.tv_sec % 1000) * 1000000;
	key += tv.tv_usec;

	return key;
}

/* Thread spawned by _wait_be_func(). See that function for details. */
static void *_wait_be_thread(void *x)
{
	srun_child_wait_data_t *thread_data = (srun_child_wait_data_t *)x;

	waitpid(thread_data->pid, thread_data->status_ptr, 0);
	_be_proc_status(*(thread_data->status_ptr), thread_data->signal_socket);
	*(thread_data->job_fini_ptr) = true;
	while (write(thread_data->dummy_pipe, "", 1) == -1) {
		if ((errno == EAGAIN) || (errno == EINTR))
			continue;
		error("write(dummy_pipe): %m");
		break;
	}

	return NULL;
}

/*
 * Wait for back-end process completion and send exit code to front-end
 * pid IN - process ID to wait for
 * signal_socket IN - socket used to transmit exit code
 * status_ptr IN - pointer to place for recording process exit status
 * job_fini_ptr IN - flag to set upon job completion
 * dummy_pipe IN - file just used to wake main process
 * RETURN - ID of spawned thread
 */
static pthread_t _wait_be_func(pid_t pid, slurm_fd_t signal_socket,
			       int *status_ptr, bool *job_fini_ptr,
			       int dummy_pipe)
{
	static srun_child_wait_data_t thread_data;
	pthread_attr_t thread_attr;
	pthread_t thread_id = 0;

	slurm_attr_init(&thread_attr);
	thread_data.dummy_pipe = dummy_pipe;
	thread_data.job_fini_ptr = job_fini_ptr;
	thread_data.pid = pid;
	thread_data.signal_socket = signal_socket;
	thread_data.status_ptr = status_ptr;
	if (pthread_create(&thread_id, &thread_attr, _wait_be_thread,
                           &thread_data)) {
		error("pthread_create: %m");
	}
	slurm_attr_destroy(&thread_attr);
	return thread_id;
}

/*
 * A collection of signal handling functions follow
 */
static void _default_sigaction(int sig)
{
	struct sigaction act;
	if (sigaction(sig, NULL, &act)) {
		error("sigaction(%d): %m", sig);
		return;
	}
	if (act.sa_handler != SIG_IGN)
		return;

	act.sa_handler = SIG_DFL;
	if (sigaction(sig, &act, NULL))
		error("sigaction(%d): %m", sig);
}

static void _print_step_state(void)
{
	if (srun_state == 0)
		info("job step is starting");
	else if (srun_state == 1)
		info("job step is running");
	else if (srun_state == 2)
		info("job step is terminating");
}

static long _diff_tv_str(struct timeval *tv1,struct timeval *tv2)
{
	long delta_t;

	delta_t  = MIN((tv2->tv_sec - tv1->tv_sec), 10);
	delta_t *= 1000000;
	delta_t +=  tv2->tv_usec - tv1->tv_usec;
	return delta_t;
}

static void _handle_intr(void)
{
	static struct timeval last_intr = { 0, 0 };
	struct timeval now;

	gettimeofday(&now, NULL);
	if (!quit_on_intr && _diff_tv_str(&last_intr, &now) > 1000000) {
		if  (opt.disable_status) {
			info("sending Ctrl-C to job");
			srun_send_signal(SIGINT);
		} else if (srun_state < 2) {
			info("interrupt (one more within 1 sec to abort)");
			_print_step_state();
		} else {
			info("interrupt (abort already in progress)");
			_print_step_state();
		}
		last_intr = now;
	} else  { /* second Ctrl-C in half as many seconds */
		info("aborting job step");
		pthread_mutex_lock(&state_mutex);
		srun_state = 2;
		pthread_mutex_unlock(&state_mutex);
		srun_send_signal(SIGKILL);
	}
}

static void _signal_handler(int signo)
{
	debug("got signal %d", signo);
	switch (signo) {
		case SIGHUP:
		case SIGTERM:
		case SIGQUIT:
			pthread_mutex_lock(&state_mutex);
			srun_state = 2;
			pthread_mutex_unlock(&state_mutex);
			break;
		case SIGINT:
			_handle_intr();
	}
}

static void _setup_signal_handler(void)
{
	int i;
	static int sig_array[] = {
		SIGINT,  SIGQUIT, SIGTERM, SIGHUP, 0 };

	/* Make sure no required signals are ignored (possibly inherited) */
	(void) xsignal_unblock(sig_array);
	for (i = 0; sig_array[i]; i++) {
		_default_sigaction(sig_array[i]);
		(void) xsignal(sig_array[i], _signal_handler);
	}
}

/*
 * A collection of job environment handling functions follow
 */
static Buf _fe_build_env(void)
{
	int i;
	char cwd[MAXPATHLEN + 1];
	Buf buf;

	if (!(buf = init_buf(2048)))
		fatal("init_buf(), malloc failure");

	if ((getcwd(cwd, MAXPATHLEN)) == NULL)
		fatal("getcwd failed: %m");
	packstr(cwd, buf);

	if (environ) {
		for (i = 0; environ[i]; i++)
			packstr(environ[i], buf);
	} else {
		error("no environment variables are set");
	}

	return buf;
}

/* Send buffer with job environment across a socket.
 * Return true on success, false on failure */
static bool _fe_send_env(Buf env_buf, slurm_fd_t stderr_socket)
{
	int buf_len;
	char *buf = get_buf_data(env_buf);
	uint32_t msg_len = get_buf_offset(env_buf);

	buf_len = slurm_write_stream_timeout(stderr_socket, (char *)&msg_len,
					     sizeof(msg_len), MSG_TIMEOUT);
	/* NOTE: Do not change test below
	 * (-1 < sizeof(msg_len)) is false since
	 * -1 gets converted to unsigned long first */
	if ((buf_len < 0) || (buf_len < sizeof(msg_len))) {
		error("environment write: %m");
		return false;
	}

	buf_len = slurm_write_stream_timeout(stderr_socket, buf, msg_len,
					     MSG_TIMEOUT);
	if ((buf_len < 0) || (buf_len < msg_len)) {
		error("environment write: %m");
		return false;
	}

	return true;
}

/* Read buffer with job environment from a socket.
 * Return true on success, false on failure */
static bool _be_get_env(slurm_fd_t stderr_socket)
{
	Buf env_buf;
	char *buf, *cwd, *env;
	uint32_t buf_len = 0, u32_tmp = 0;
	int read_len;
	bool rc;

	/* Read and process message header */
	read_len = slurm_read_stream(stderr_socket, (char *)&buf_len,
				     sizeof(buf_len));
	if (read_len == -1) {
		if (errno != SLURM_PROTOCOL_SOCKET_ZERO_BYTES_SENT)
			error("environment read header error: %m");
		return false;
	}
	if (read_len == 0)
		return false;	/* Abnornal EOF */
	if (read_len < sizeof(buf_len)) {
		error("environment read header, bad size (%d < %lu)",
		      read_len, sizeof(buf_len));
		return false;	/* Can not recover, treat like EOF */
	}

	/* Read and process message data */
	rc = false;
	buf = xmalloc(buf_len+1);
	read_len = slurm_read_stream(stderr_socket, buf, buf_len);
	if (read_len < 0) {
		error("environment read buffer: %m");
	} else if (read_len < buf_len) {
		error("environment read short (%d < %d)", read_len, buf_len);
	} else {
		rc = true;
		env_buf = create_buf(buf, buf_len);
		if (unpackstr_ptr(&cwd, &u32_tmp, env_buf)) {
			error("job environment not read properly");
		} else {
			if (chdir(cwd))
				error("chdir(%s): %m", cwd);
		}
		while (!unpackstr_ptr(&env, &u32_tmp, env_buf)) {
			char *sep, *work_env = xstrdup(env);
			sep = strchr(work_env, '=');
			if (sep) {
				sep[0] = '\0';
				if (setenv(work_env, sep+1, 1)) {
					error("setenv(%s,%s): %m",
					      work_env, sep+1);
				}
			} else
				error("bad job environment variable: %s", env);
			xfree(work_env);
		}
	}
	xfree(buf);

	return rc;
}

/*
 * Socket connection authentication logic
 */
static bool _xmit_key(slurm_fd_t socket_conn, uint32_t auth_key)
{
	int i;

	i = slurm_write_stream_timeout(socket_conn, (char *) &auth_key,
				       sizeof(auth_key), MSG_TIMEOUT);
	if ((i < 0) || (i <  sizeof(auth_key))) {
		error("auth_key write: %m");
		return false;
	}

	return true;
}

static bool _validate_connect(slurm_fd_t socket_conn, uint32_t auth_key)
{
	struct timeval tv;
	fd_set read_fds;
	uint32_t read_key;
	bool valid = false;
	int i, n_fds;

	n_fds = socket_conn;
	while (1) {
		FD_ZERO(&read_fds);
		FD_SET(socket_conn, &read_fds);

		tv.tv_sec = 2;
		tv.tv_usec = 0;
		i = select((n_fds + 1), &read_fds, NULL, NULL, &tv);
		if (i == 0)
			break;
		if (i < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		i = slurm_read_stream(socket_conn, (char *)&read_key,
				      sizeof(read_key));
		if ((i == sizeof(read_key)) && (read_key == auth_key))
			valid = true;
		break;
	}

	return valid;
}

/* Given a program name, return its communication protocol */
static char *_get_cmd_protocol(char *cmd)
{
	int stdout_pipe[2] = {-1, -1}, stderr_pipe[2] = {-1, -1};
	int read_size, buf_rem = 16 * 1024, offset = 0, status;
	pid_t pid;
	char *buf, *protocol = "mpi";

	if ((pipe(stdout_pipe) == -1) || (pipe(stderr_pipe) == -1)) {
		error("pipe: %m");
		return "mpi";
	}

	pid = fork();
	if (pid < 0) {
		error("fork: %m");
		return "mpi";
	} else if (pid == 0) {
		if ((dup2(stdout_pipe[1], 1) == -1) ||
		    (dup2(stderr_pipe[1], 2) == -1)) {
			error("dup2: %m");
			return NULL;
		}
		(void) close(0);	/* stdin */
		(void) close(stdout_pipe[0]);
		(void) close(stdout_pipe[1]);
		(void) close(stderr_pipe[0]);
		(void) close(stderr_pipe[1]);

		execlp("/usr/bin/ldd", "ldd", cmd, NULL);
		error("execv(ldd) error: %m");
		return NULL;
	}

	(void) close(stdout_pipe[1]);
	(void) close(stderr_pipe[1]);
	buf = xmalloc(buf_rem);
	while ((read_size = read(stdout_pipe[0], &buf[offset], buf_rem))) {
		if (read_size > 0) {
			buf_rem -= read_size;
			offset  += read_size;
			if (buf_rem == 0)
				break;
		} else if ((errno != EAGAIN) || (errno != EINTR)) {
			error("read(pipe): %m");
			break;
		}
	}
	if (strstr(buf, "libmpi.so"))
		protocol = "mpi";
	else if (strstr(buf, "libshmem.so"))
		protocol = "shmem";
	else if (strstr(buf, "libxlpgas.so"))
		protocol = "pgas";
	else if (strstr(buf, "libpami.so"))
		protocol = "pami";
	else if (strstr(buf, "liblapi.so"))
		protocol = "lapi";
	xfree(buf);
	while ((waitpid(pid, &status, 0) == -1) && (errno == EINTR))
		;
	(void) close(stdout_pipe[0]);
	(void) close(stderr_pipe[0]);

	return protocol;
}

/*
 * Parse a multi-prog input file line
 * line IN - line to parse
 * num_task OUT - number of tasks to be started
 * cmd OUT - command to execute, caller must xfree this
 * args OUT - arguments to the command, caller must xfree this
 */
static void _parse_prog_line(char *in_line, int *num_tasks, char **cmd,
			     char **args)
{
	int i;
	int first_arg_inx = 0, last_arg_inx = 0;
	int first_cmd_inx,  last_cmd_inx;
	int first_task_inx, last_task_inx;
	hostset_t hs;

	/* Get the task ID string */
	for (i = 0; in_line[i]; i++) {
		if (!isspace(in_line[i]))
			break;
	}
	if (in_line[i] == '#')
		goto fini;
	if (!isdigit(in_line[i]))
		goto bad_line;
	first_task_inx = i;
	for (i++; in_line[i]; i++) {
		if (isspace(in_line[i]))
			break;
	}
	if (!isspace(in_line[i]))
		goto bad_line;
	last_task_inx = i;

	/* Get the command */
	for (i++; in_line[i]; i++) {
		if (!isspace(in_line[i]))
			break;
	}
	if (in_line[i] == '\0')
		goto bad_line;
	first_cmd_inx = i;
	for (i++; in_line[i]; i++) {
		if (isspace(in_line[i]))
			break;
	}
	if (!isspace(in_line[i]))
		goto bad_line;
	last_cmd_inx = i;

	/* Get the command's arguments */
	for (i++; in_line[i]; i++) {
		if (!isspace(in_line[i]))
			break;
	}
	if (in_line[i])
		first_arg_inx = i;
	for ( ; in_line[i]; i++) {
		if (in_line[i] == '\n') {
			last_arg_inx = i;
			break;
		}
	}

	/* Now transfer data to the function arguments */
	in_line[last_task_inx] = '\0';
	hs = hostset_create(in_line + first_task_inx);
	in_line[last_task_inx] = ' ';
	if (!hs)
		goto bad_line;
	*num_tasks = hostset_count(hs);
	hostset_destroy(hs);

	in_line[last_cmd_inx] = '\0';
	*cmd = xstrdup(in_line + first_cmd_inx);
	in_line[last_cmd_inx] = ' ';

	if (last_arg_inx)
		in_line[last_arg_inx] = '\0';
	if (first_arg_inx)
		*args = xstrdup(in_line + first_arg_inx);
	else
		*args = NULL;
	if (last_arg_inx)
		in_line[last_arg_inx] = '\n';
	return;

bad_line:
	error("invalid input line: %s", in_line);
fini:	*num_tasks = -1;
	return;	
}

/*
 * Either get or set a POE command line,
 * line IN/OUT - line to set or get
 * length IN - size of line in bytes
 * step_id IN - -1 if input line, otherwise the step ID to output
 * RET true if more lines to get
 */
static bool _multi_prog_parse(char *line, int length, int step_id)
{
	static int cmd_count = 0, inx = 0, total_tasks = 0;
	static char **args = NULL, **cmd = NULL;
	static int *num_tasks = NULL;
	int i;

	if (step_id < 0) {
		char *tmp_args = NULL, *tmp_cmd = NULL;
		int tmp_tasks = -1;
		_parse_prog_line(line, &tmp_tasks, &tmp_cmd, &tmp_args);

		if (tmp_tasks < 0) {
			if (line[0] != '#')
				error("bad line %s", line);
			return true;
		}

		xrealloc(args, (sizeof(char *) * (cmd_count + 1)));
		xrealloc(cmd,  (sizeof(char *) * (cmd_count + 1)));
		xrealloc(num_tasks, (sizeof(int) * (cmd_count + 1)));
		args[cmd_count] = tmp_args;
		cmd[cmd_count]  = tmp_cmd;
		num_tasks[cmd_count] = tmp_tasks;
		total_tasks += tmp_tasks;
		cmd_count++;
		return true;
	} else if (inx >= cmd_count) {
		for (i = 0; i < cmd_count; i++) {
			xfree(args[i]);
			xfree(cmd[i]);
		}
		xfree(args);
		xfree(cmd);
		xfree(num_tasks);
		cmd_count = 0;
		inx = 0;
		total_tasks = 0;
		return false;
	} else if (args[inx]) {
		/* <cmd>@<step_id>%<total_tasks>%<protocol>:<num_tasks> <args...> */
		snprintf(line, length, "%s@%d%c%d%c%s:%d %s",
			 cmd[inx], step_id, '%', total_tasks, '%',
			 _get_cmd_protocol(cmd[inx]), num_tasks[inx],
			 args[inx]);
		inx++;
		return true;
	} else {
		/* <cmd>@<step_id>%<total_tasks>%<protocol>:<num_tasks> */
		snprintf(line, length, "%s@%d%c%d%c%s:%d",
			 cmd[inx], step_id, '%', total_tasks, '%',
			 _get_cmd_protocol(cmd[inx]), num_tasks[inx]);
		inx++;
		return true;
	}
}

/* Return the next available step ID */
static int _get_next_stepid(char *job_id, char *dname, int dname_size)
{
	int fd, i, rc, step_id;
	char *work_dir;
	ssize_t io_size = 0;
	char buf[16];

	/* NOTE: Directory must be shared between nodes for cmd_file to work */
	if (!(work_dir = getenv("HOME"))) {
		work_dir = xmalloc(512);
		if (!getcwd(work_dir, 512))
			fatal("getcwd(): %m");
		snprintf(dname, dname_size, "%s/.slurm_loadl", work_dir);
		xfree(work_dir);
	} else {
		snprintf(dname, dname_size, "%s/.slurm_loadl", work_dir);
	}
	mkdir(dname, 0700);

	/* Create or open our stepid file */
	if (!stepid_fname) {
		stepid_fname = xmalloc(strlen(dname) + strlen(job_id) + 32);
		sprintf(stepid_fname, "%s/slurm_stepid_%s", dname, job_id);
	}
	while (((fd = open(stepid_fname, O_CREAT|O_EXCL|O_RDWR, 0600)) < 0) &&
	       (errno == EINTR))
		;
	if ((fd < 0) && (errno == EEXIST))
		fd = open(stepid_fname, O_RDWR);
	if (fd < 0)
		fatal("open(%s): %m", stepid_fname);

	/* Set exclusive lock on the file */
	for (i = 0; ; i++) {
		rc = flock(fd, LOCK_EX | LOCK_NB);
		if (rc == 0)
			break;
		if (i > 10)
			fatal("flock(%s): %m", stepid_fname);
		usleep(100);
	}

	/* Read latest step ID from the file */
	for (i = 0; ; i++) {
		io_size = read(fd, buf, sizeof(buf));
		if (io_size >= 0)
			break;
		if (i > 10) {
			flock(fd, LOCK_UN);
			fatal("read(%s): %m", stepid_fname);
		}
	}
	if (io_size > 0)
		step_id = atoi(buf) + 1;
	else
		step_id = 1;

	/* Write new step ID value */
	snprintf(buf, sizeof(buf), "%d", step_id);
	for (i = 0; ; i++) {
		lseek(fd, 0, SEEK_SET);
		io_size = write(fd, buf, sizeof(buf));
		if (io_size == sizeof(buf))
			break;
		if (i > 10) {
			flock(fd, LOCK_UN);
			fatal("write(%s): %m", stepid_fname);
		}
	}

	/* Unlock the file */
	for (i = 0; ; i++) {
		rc = flock(fd, LOCK_UN);
		if (rc == 0)
			break;
		if (i > 10)
			fatal("flock(%s): %m", stepid_fname);
	}

	return step_id;
}

/* Build a POE command line based upon srun options (using global variables) */
extern char *build_poe_command(char *job_id)
{
	int i, step_id;
	char *cmd_line = NULL, *tmp_str;
	char dname[512], value[32];
	bool need_cmdfile = false;
	char *protocol = "mpi";

	/*
	 * In order to support MPMD or job steps smaller than the job
	 * allocation size, specify a command file using the poe option
	 * -cmdfile or MP_CMDFILE env var. See page 43 here:
	 * http://publib.boulder.ibm.com/epubs/pdf/c2367811.pdf
	 * The command file should contain one more more lines of the following
	 * form:
	 * <cmd>@<step_id>%<total_tasks>%<protocol>:<num_tasks> <args>
	 * IBM is working to eliminate the need to specify protocol, but until
	 * then it might be determined as follows:
	 *
	 * We are currently looking at 'ldd <program>' and checking the name of
	 * the MPI and PAMI libraries and on x86, also checking to see if Intel
	 * MPI library is used.
	 * This is done at runtime in PMD and depending on '-mpilib' option and
	 * '-config' option used, change the LD_LIBRARY_PATH to properly
	 * support the different PE Runtime levels the customer have installed
	 * on their cluster.
	 *
	 * There is precedence order that would be important if multiple
	 * libraries are listed in the 'ldd output' as long as you know it is
	 * not a mixed protocol (i.e. Openshmem + MPI, UPC + MPI, etc)
	 * application.
	 * 1) If MPI library is found (libmpi.so) -> use 'mpi'
	 * 2) if Openshmem library is found (libshmem.so) -> use 'shmem'
	 * 3) if UPC runtime library is found (libxlpgas.so) -> use 'pgas'
	 * 4) if only PAMI library is found (libpami.so) -> use 'pami'
	 * 5) if only LAPI library is found (liblapi.so) -> use 'lapi'
	 */
	if (opt.multi_prog)
		need_cmdfile = true;
	if (opt.ntasks_set && !need_cmdfile) {
		tmp_str = getenv("SLURM_NPROCS");
		if (!tmp_str)
			tmp_str = getenv("SLURM_NNODES");
		if (tmp_str && (opt.ntasks != atoi(tmp_str)))
			need_cmdfile = true;
	}

	if (opt.multi_prog) {
		protocol = "multi";
	} else {
		protocol = _get_cmd_protocol(opt.argv[0]);
	}
	debug("cmd:%s protcol:%s", opt.argv[0], protocol);

	step_id = _get_next_stepid(job_id, dname, sizeof(dname));
	xstrcat(cmd_line, "poe");
	if (need_cmdfile) {
		char *buf;
		int fd, i, j, k;

		/* NOTE: The command file needs to be in a directory that can
		 * be read from the compute node(s), so /tmp does not work.
		 * We use the user's home directory (based upon "HOME"
		 * environment variable) otherwise use current working
		 * directory. The file name contains the job ID and step ID. */
		xstrfmtcat(cmd_fname,
			   "%s/slurm_cmdfile_%s.%d", dname, job_id, step_id);
		while ((fd = creat(cmd_fname, 0600)) < 0) {
			if (errno == EINTR)
				continue;
			fatal("creat(%s): %m", cmd_fname);
		}

		i = strlen(opt.argv[0]) + 128;
		buf = xmalloc(i);
		if (opt.multi_prog) {
			char in_line[512];
			FILE *fp = fopen(opt.argv[0], "r");
			if (!fp)
				fatal("fopen(%s): %m", opt.argv[0]);
			/* Read and parse SLURM MPMD format file here */
			while (fgets(in_line, sizeof(in_line), fp))
				_multi_prog_parse(in_line, 512, -1);
			fclose(fp);
			/* Write LoadLeveler MPMD format file here */
			while (_multi_prog_parse(in_line, 512, step_id))
				j = xstrfmtcat(buf, "%s\n", in_line);
		} else {
			/* <cmd>@<step_id>%<total_tasks>%<protocol>:<num_tasks> <args...>*/
			xstrfmtcat(buf, "%s@%d%c%d%c%s:%d",
				   opt.argv[0], step_id, '%',
				   opt.ntasks, '%', protocol, opt.ntasks);
			for (i = 1; i < opt.argc; i++) /* start at argv[1] */
				xstrfmtcat(buf, " %s", opt.argv[i]);
			xstrfmtcat(buf, "\n");
		}
		i = 0;
		j = strlen(buf);
		while ((k = write(fd, &buf[i], j))) {
			if (k > 0) {
				i += k;
				j -= k;
			} else if ((errno != EAGAIN) && (errno != EINTR)) {
				error("write(cmdfile): %m");
				break;
			}
		}
		(void) close(fd);
		setenv("MP_NEWJOB", "parallel", 1);
		setenv("MP_CMDFILE", cmd_fname, 1);
	} else {
		xstrfmtcat(cmd_line, " %s", opt.argv[0]);
		/* Each token gets double quotes around it in case any
		 * arguments contain spaces */
		for (i = 1; i < opt.argc; i++) {
			xstrfmtcat(cmd_line, " \"%s\"", opt.argv[i]);
		}
	}

	if (opt.network) {
		if (strstr(opt.network, "dedicated"))
			setenv("MP_ADAPTER_USE", "dedicated", 1);
		else if (strstr(opt.network, "shared"))
			setenv("MP_ADAPTER_USE", "shared", 1);
	}
	if (opt.cpu_bind_type) {
		if ((opt.cpu_bind_type & CPU_BIND_TO_THREADS) ||
		    (opt.cpu_bind_type & CPU_BIND_TO_CORES)) {
			setenv("MP_BINDPROC", "yes", 1);
		}
	}
	if (opt.shared != (uint16_t) NO_VAL) {
		if (opt.shared)
			setenv("MP_CPU_USE", "unique", 1);
		else
			setenv("MP_CPU_USE", "multiple", 1);
	}
	if (opt.network) {
		if (strstr(opt.network, "hfi"))
			setenv("MP_DEVTYPE", "hfi", 1);
		else if  (strstr(opt.network, "ib"))
			setenv("MP_DEVTYPE", "ib", 1);
	}
	if (opt.network) {
		if (strstr(opt.network, "sn_all"))
			setenv("MP_EUIDEVICE", "sn_all", 1);
		else if (strstr(opt.network, "sn_single"))
			setenv("MP_EUIDEVICE", "sn_single", 1);
		else if ((tmp_str = strstr(opt.network, "eth"))) {
			char buf[5];
			strncpy(buf, tmp_str, 5);
			buf[4] = '\0';
			setenv("MP_EUIDEVICE", buf, 1);
		}
	}
	if (opt.network) {
		if (strstr(opt.network, "ip") || strstr(opt.network, "ip"))
			setenv("MP_EUILIB", "IP", 1);
		else if (strstr(opt.network, "us") ||
			 strstr(opt.network, "US"))
			setenv("MP_EUILIB", "US", 1);
	}
	if (opt.nodelist) {
/* FIXME: Need to generate hostlist file on compute node,
 * presumably using environment variables to set up */
		char *fname = NULL, *host_name, *host_line;
		pid_t pid = getpid();
		hostlist_t hl;
		int fd, len, offset, wrote;
		hl = hostlist_create(opt.nodelist);
		if (!hl)
			fatal("Invalid nodelist: %s", opt.nodelist);
		xstrfmtcat(fname, "slurm_hostlist.%u", (uint32_t) pid);
		if ((fd = creat(fname, 0600)) < 0)
			fatal("creat(%s): %m", fname);
		while ((host_name = hostlist_shift(hl))) {
			host_line = NULL;
			xstrfmtcat(host_line, "%s\n", host_name);
			free(host_name);
			len = strlen(host_line) + 1;
			offset = 0;
			while (len > offset) {
				wrote = write(fd, host_line + offset,
					      len - offset);
				if (wrote < 0) {
					if ((errno == EAGAIN) ||
					    (errno == EINTR))
						continue;
					fatal("write(%s): %m", fname);
				}
				offset += wrote;
			}
			xfree(host_line);
		}
		hostlist_destroy(hl);
		info("wrote hostlist file at %s", fname);
		xfree(fname);
		close(fd);
	}
	if (opt.msg_timeout) {
		snprintf(value, sizeof(value), "%d", opt.msg_timeout);
		setenv("MP_TIMEOUT", value, 1);
	}
	if (opt.immediate)
		setenv("MP_RETRY", "0", 1);
	if (_verbose) {
		int info_level = MIN((_verbose + 1), 6);
		snprintf(value, sizeof(value), "%d", info_level);
		setenv("MP_INFOLEVEL", value, 1);
	}
	if (opt.labelio)
		setenv("MP_LABELIO", "yes", 0);
	if (!strcmp(protocol, "multi"))
		setenv("MP_MSG_API", "mpi", 0);
	else if (!strcmp(protocol, "mpi"))
		setenv("MP_MSG_API", "mpi", 0);
	else if (!strcmp(protocol, "lapi"))
		setenv("MP_MSG_API", "lapi", 0);
	else if (!strcmp(protocol, "pami"))
		setenv("MP_MSG_API", "pami", 0);
	else if (!strcmp(protocol, "upc"))
		setenv("MP_MSG_API", "upc", 0);
	else if (!strcmp(protocol, "shmem")) {
		setenv("MP_MSG_API", "shmem,xmi", 0);
		setenv("MP_USE_BULK_XFER", "no", 0);
	}
	if (opt.min_nodes != NO_VAL) {
		snprintf(value, sizeof(value), "%u", opt.min_nodes);
		setenv("MP_NODES", value, 1);
	}
	if (opt.ntasks) {
		snprintf(value, sizeof(value), "%u", opt.ntasks);
		setenv("MP_PROCS", value, 1);
	}
	if (opt.cpu_bind_type) {
		if (opt.cpu_bind_type & CPU_BIND_TO_THREADS)
			setenv("MP_TASK_AFFINITY", "cpu", 1);
		else if (opt.cpu_bind_type & CPU_BIND_TO_CORES)
			setenv("MP_TASK_AFFINITY", "core", 1);
		else if (opt.cpus_per_task) {
			snprintf(value, sizeof(value), "cpu:%d",
				 opt.cpus_per_task);
			setenv("MP_TASK_AFFINITY", value, 1);
		}
	}
	if (opt.ntasks_per_node != NO_VAL) {
		snprintf(value, sizeof(value), "%u", opt.ntasks_per_node);
		setenv("MP_TASKS_PER_NODE", value, 1);
	}
	if (opt.unbuffered) {
		setenv("MP_STDERRMODE", "unordered", 1);
		setenv("MP_STDOUTMODE", "unordered", 1);
	}

	disable_status = opt.disable_status;
	quit_on_intr = opt.quit_on_intr;
	srun_jobid = xstrdup(opt.jobid);

#if _DEBUG_SRUN
	info("cmd_line:%s", cmd_line);
#endif
	return cmd_line;
}

/* Find a string like "\" \"" or "\" \0" and return printer to the space
 * or return NULL if not found */
static char *_find_srun_sep(char *cmd_line)
{
	int i, len = strlen(cmd_line);

	for (i = 0; i < (len - 2); i++) {
		if ((cmd_line[i] == '"') &&
		    (cmd_line[i+1] == ' ') &&
		    ((cmd_line[i+2] == '"') || (cmd_line[i+2] == '\0')))
			return cmd_line + i + 1;
	}

	return NULL;
}

/* If srun is executed from within a batch job, we need to fork/exec
 * POE with the proper environment variables and signal handling.
 * cmd_line format is: poe <cmd> "<arg1>" "<arg2>" ... */
static int _srun_spawn_batch(char *cmd_line)
{
	char **argv, *begin, *sep;
	int i = strlen(cmd_line) / 2 + 2;

	argv = (char **) xmalloc(sizeof(char *) * i);
	begin = cmd_line;
	i = 0;
	while (1) {
		if (i < 2)
			sep = strchr(begin, ' ');
		else
			sep = _find_srun_sep(begin);
		if (sep)
			sep[0] = '\0';
		if (i >= 2) {	/* Exclude quotes */
			int len = strlen(begin);
			begin[len - 1] = '\0';
			begin++;
		}
		argv[i++] = begin;
		if (sep)
			begin = sep + 1;
		else
			break;
	}

	return execvp(argv[0], argv);
}

/*
 * srun_front_end - Open stdin/out/err socket connections to communicate with
 *	a remote node process and spawn a remote job to claim that connection
 *	and execute the user's command.
 *
 * cmd_line IN - Command execute line
 * srun_alloc IN - TRUE if this srun commanmd created the job allocation
 * RETURN - remote processes exit code or -1 if some internal error
 */
extern int srun_front_end (char *cmd_line, bool  srun_alloc)
{
	uint16_t port_e, port_o, port_s;
	slurm_addr_t addr_e, addr_o, addr_s;
	slurm_addr_t stderr_addr, stdout_addr, signal_addr;
	slurm_fd_t local_stdin = 0;
	slurm_fd_t stdout_socket = -1;
	slurm_fd_t stderr_socket = -1;
	slurm_fd_t signal_socket = -1;
	slurm_fd_t stderr_conn = SLURM_SOCKET_ERROR;
	slurm_fd_t stdout_conn = SLURM_SOCKET_ERROR;
	slurm_fd_t signal_conn = SLURM_SOCKET_ERROR;
	bool job_fini = false, stderr_fini = false, stdout_fini = false;
	fd_set except_fds, read_fds;
	char *exec_line = NULL, hostname[1024];
	int i, n_fds, status = -1;
	bool pty = PTY_MODE;
	struct timeval tv;
	Buf local_env;
	uint32_t auth_key;

	if (!getenv("SLURM_BE_KEY") || !getenv("SLURM_BE_SOCKET")) {
		/* We should be running within a batch script */
		return _srun_spawn_batch(cmd_line);
	}

	if (!getenv("SLURM_BE_KEY") || !getenv("SLURM_BE_SOCKET")) {
		error("Environment variables SLURM_BE_KEY or "
		      "SLURM_BE_SOCKET not found");
		goto fini;
	}

	if (!cmd_line || !cmd_line[0]) {
		error("no command to execute");
		goto fini;
	}

	_setup_signal_handler();

	/* Open sockets for back-end program to communicate with */
	/* Socket for stdin/stdout */
	if ((stdout_socket = slurm_init_msg_engine_port(0)) < 0) {
		error("init_msg_engine_port: %m");
		goto fini;
	}
	if (slurm_get_stream_addr(stdout_socket, &addr_o) < 0) {
		error("slurm_get_stream_addr: %m");
		goto fini;
	}
	port_o = ntohs(((struct sockaddr_in) addr_o).sin_port);

	/* Socket for stderr */
	if ((stderr_socket = slurm_init_msg_engine_port(0)) < 0) {
		error("init_msg_engine_port: %m");
		goto fini;
	}
	if (slurm_get_stream_addr(stderr_socket, &addr_e) < 0) {
		error("slurm_get_stream_addr: %m");
		goto fini;
	}
	port_e = ntohs(((struct sockaddr_in) addr_e).sin_port);

	/* Socket for signals and exit code */
	if ((signal_socket = slurm_init_msg_engine_port(0)) < 0) {
		error("init_msg_engine_port: %m");
		goto fini;
	}
	if (slurm_get_stream_addr(signal_socket, &addr_s) < 0) {
		error("slurm_get_stream_addr: %m");
		goto fini;
	}
	port_s = ntohs(((struct sockaddr_in) addr_s).sin_port);
	auth_key = _gen_auth_key();

	/* Generate back-end execute line */
	gethostname_short(hostname, sizeof(hostname));
	xstrfmtcat(exec_line, "%s/bin/srun --srun-be %s %hu %hu %hu %u %s",
		   SLURM_PREFIX, hostname, port_o, port_e, port_s, auth_key,
		   cmd_line);
#if _DEBUG_SRUN
	printf("%s\n", exec_line);
#endif
	i = salloc_be_spawn(exec_line);
	xfree(exec_line);
	if (i)
		goto fini;

	local_env = _fe_build_env();

	/* Wait for the back-end to start and poll on the job's existance */
	n_fds = stdout_socket;
	while ((srun_state < 2) && (stdout_conn == SLURM_SOCKET_ERROR)) {
		FD_ZERO(&except_fds);
		FD_SET(stdout_socket, &except_fds);
		FD_ZERO(&read_fds);
		FD_SET(stdout_socket, &read_fds);

		tv.tv_sec = SELECT_TIMEOUT;
		tv.tv_usec = 0;
		i = select((n_fds + 1), &read_fds, NULL, &except_fds, &tv);
		if (i == -1) {
			if (errno == EINTR)
				continue;
			error("select: %m");
			break;
		} else if (i == 0) {
			/* Periodically test for abnormal job termination */
			_fe_test_job_state();
			continue;
		} else {	/* i > 0, ready for I/O */
			break;
		}
	}

	/* Accept connections from the back-end */
	while (srun_state < 2) {
		stdout_conn = slurm_accept_stream(stdout_socket, &stdout_addr);
		if (stdout_conn != SLURM_SOCKET_ERROR) {
			if (!_validate_connect(stdout_conn, auth_key))
				continue;
			break;
		}
		if (errno != EINTR) {
			error("slurm_accept_stream: %m");
			goto fini;
		}
	}
	if (pty) {
		stderr_fini = true;
	} else {
		while (srun_state < 2) {
			stderr_conn = slurm_accept_stream(stderr_socket,
							  &stderr_addr);
			if (stderr_conn != SLURM_SOCKET_ERROR) {
				if (!_validate_connect(stderr_conn, auth_key))
					continue;
				break;
			}
			if (errno != EINTR) {
				error("slurm_accept_stream: %m");
				goto fini;
			}
		}
	}
	while (srun_state < 2) {
		signal_conn = slurm_accept_stream(signal_socket, &signal_addr);
		if (signal_conn != SLURM_SOCKET_ERROR) {
			if (!_validate_connect(signal_conn, auth_key))
				continue;
			break;
		}
		if (errno != EINTR) {
			error("slurm_accept_stream: %m");
			goto fini;
		}
	}
	pthread_mutex_lock(&state_mutex);
	if (srun_state < 2)
		srun_state = 1;
	pthread_mutex_unlock(&state_mutex);

	global_signal_conn = signal_conn;
	if (srun_state < 2)
		(void) _fe_send_env(local_env, stderr_conn);

	n_fds = local_stdin;
	n_fds = MAX(stderr_conn, n_fds);
	n_fds = MAX(stdout_conn, n_fds);
	n_fds = MAX(signal_conn, n_fds);
	while ((srun_state < 2) &&
	       (!(job_fini && stderr_fini && stdout_fini))) {
		FD_ZERO(&except_fds);
		FD_ZERO(&read_fds);
		if (local_stdin >= 0) {
			FD_SET(local_stdin, &except_fds);
			FD_SET(local_stdin, &read_fds);
		}
		if (!stdout_fini)
			FD_SET(stdout_conn, &read_fds);
		if (!stderr_fini)
			FD_SET(stderr_conn, &read_fds);
		FD_SET(signal_conn, &read_fds);

		tv.tv_sec = SELECT_TIMEOUT;
		tv.tv_usec = 0;
		i = select((n_fds + 1), &read_fds, NULL, &except_fds, &tv);
		if (i == -1) {
			if (errno == EINTR)
				continue;
			error("select: %m");
			break;
		}
		if (i == 0) {
			/* Periodically test for abnormal job termination */
			_fe_test_job_state();
			continue;
		}
		if ((local_stdin >= 0) &&
		    (FD_ISSET(local_stdin, &except_fds) ||
		     FD_ISSET(local_stdin, &read_fds))) {
			if (_fe_proc_stdin(local_stdin, stdout_conn)) {
				local_stdin = -1;
			}
		}
		if (FD_ISSET(stdout_conn, &read_fds) &&
		    _fe_proc_stdio(stdout_conn, 1, "stdout")) {
			/* Remote stderr closed */
			stdout_fini = true;
		}
		if (FD_ISSET(stderr_conn, &read_fds) &&
		    _fe_proc_stdio(stderr_conn, 2, "stderr")) {
			/* Remote stderr closed */
			stderr_fini = true;
		}
		if (FD_ISSET(signal_conn, &read_fds)) {
			i = _fe_proc_exit(signal_conn);
			status = MAX(status, i);
			job_fini = true;
		}
	}

fini:	pthread_mutex_lock(&state_mutex);
	srun_state = 2;
	pthread_mutex_unlock(&state_mutex);
	if (cmd_fname)
		(void) unlink(cmd_fname);
	if (srun_alloc && stepid_fname)
		(void) unlink(stepid_fname);
	if (stdout_conn != SLURM_SOCKET_ERROR)
		slurm_close_accepted_conn(stdout_conn);
	if (stderr_conn != SLURM_SOCKET_ERROR)
		slurm_close_accepted_conn(stderr_conn);
	if (signal_conn != SLURM_SOCKET_ERROR) {
		global_signal_conn = SLURM_SOCKET_ERROR;
		slurm_close_accepted_conn(signal_conn);
	}
	if (stdout_socket >= 0)
		slurm_shutdown_msg_engine(stdout_socket);
	if (stderr_socket >= 0)
		slurm_shutdown_msg_engine(stderr_socket);
	if (signal_socket >= 0)
		slurm_shutdown_msg_engine(signal_socket);

	return status;
}


/* srun front-end signal processing function, send a signal to back-end
 *	program
 * sig_num IN - signal to send
 * RETURN 0 on success, -1 on error
*/
extern int srun_send_signal(int sig_num)
{
	int buf_len;
	uint32_t sig_msg = (uint32_t) sig_num;

	if (global_signal_conn == SLURM_SOCKET_ERROR) {
		error("signal write: back-end not connected");
		return -1;
	}
	buf_len = slurm_write_stream_timeout(global_signal_conn,
					     (char *)&sig_msg, sizeof(sig_msg),
					     MSG_TIMEOUT);

	/* NOTE: Do not change test below
	 * (-1 < sizeof(sig_msg)) is false since
	 * -1 gets converted to unsigned long first */
	if ((buf_len < 0) || (buf_len < sizeof(sig_msg))) {
		error("signal write: %m");
		return -1;
	}

#if _DEBUG_SRUN
	info("signal %d sent", sig_num);
#endif
	return 0;
}

/*
 * srun_back_end - Open stdin/out/err socket connections to communicate with
 *	the srun command that spawned this one, forward its stdin/out/err
 *	communications back, forward signals, and return the program's exit
 *	code.
 *
 * argc IN - Count of elements in argv
 * argv IN - [0]:  Our executable name (e.g. srun)
 *	     [1]:  "--srun-be" (argument to spawn srun backend)
 *	     [2]:  Hostname or address of front-end
 *	     [3]:  Port number for stdin/out
 *	     [4]:  Port number for stderr
 *	     [5]:  Port number for signals/exit status
 *	     [6]:  Authentication key
 *	     [7]:  Program to be spawned for user
 *	     [8+]: Arguments to spawned program
 * RETURN - remote processes exit code
 */
extern int srun_back_end (int argc, char **argv)
{
	char *host = NULL;
	uint16_t port_e = 0, port_o = 0, port_s = 0;
	slurm_addr_t addr_e, addr_o, addr_s;
	slurm_fd_t stderr_socket = 0, stdin_socket = 0, stdout_socket = -1;
	slurm_fd_t signal_socket = 0;
	bool job_fini = false;
	bool signal_fini = false, stderr_fini = false, stdout_fini = false;
	int dummy_pipe[2] = {-1, -1}, stderr_pipe[2] = {-1, -1};
	int stdin_pipe[2] = {-1, -1}, stdout_pipe[2] = {-1, -1};
	fd_set read_fds;
	pid_t pid;
	int i, n_fds, status = 0;
	bool pty = PTY_MODE;
	uint32_t auth_key;

	if (argc >= 8) {
		host   = argv[2];
		port_o = atoi(argv[3]);
		port_e = atoi(argv[4]);
		port_s = atoi(argv[5]);
		auth_key = atoi(argv[6]);
	}
	if ((argc < 8) || (port_o == 0) || (port_e == 0) || (port_s == 0)) {
		error("Usage: srun --srun-be <srun_host> <srun_stdin/out_port> "
		      "<srun_stderr_port> <signal/exit_status_port> "
		      "<auth_key> <program> <args ...>\n");
		return 1;
	}

	/* Set up stdin/out on first port,
	 * Set up environment/stderr on second port,
	 * Signals and exit code use third port */
	slurm_set_addr(&addr_o, port_o, host);
	stdout_socket = slurm_open_stream(&addr_o);
	if (stdout_socket < 0) {
		error("slurm_open_msg_conn(%s:%hu): %m", host, port_o);
		return 1;
	}
	_xmit_key(stdout_socket, auth_key);
	stdin_socket = stdout_socket;

	slurm_set_addr(&addr_e, port_e, host);
	stderr_socket = slurm_open_stream(&addr_e);
	if (stderr_socket < 0) {
		error("slurm_open_msg_conn(%s:%hu): %m", host, port_e);
		return 1;
	}
	_xmit_key(stderr_socket, auth_key);

	slurm_set_addr(&addr_s, port_s, host);
	signal_socket = slurm_open_stream(&addr_s);
	if (signal_socket < 0) {
		error("slurm_open_msg_conn(%s:%hu): %m", host, port_s);
		return 1;
	}
	_xmit_key(signal_socket, auth_key);

	_be_get_env(stderr_socket);

	if (pty) {
		if (openpty(&stdin_pipe[1], &stdin_pipe[0],
			    NULL, NULL, NULL) < 0) {
			error("stdin openpty: %m");
			return 1;
		}
		stdout_pipe[0] = dup(stdin_pipe[1]);
		stdout_pipe[1] = dup(stdin_pipe[0]);
		if ((stdout_pipe[0] == -1) || (stdout_pipe[1] == -1)) {
			error("dup(openpty): %m");
			return 1;
		}
		/* In PTY mode, stderr goes to the same stream as stdout */
		stderr_pipe[0] = stderr_pipe[1] = -1;
		stderr_fini = true;
	} else if ((pipe(stdin_pipe)  == -1) ||
		   (pipe(stdout_pipe) == -1) ||
		   (pipe(stderr_pipe) == -1)) {
		error("pipe: %m");
		return 1;
	}

	pid = fork();
	if (pid < 0) {
		error("fork: %m");
		return 1;
	} else if (pid == 0) {
		if (pty) {
			login_tty(stdin_pipe[0]);
		} else {
			if ((dup2(stdin_pipe[0],  0) == -1) ||
			    (dup2(stdout_pipe[1], 1) == -1) ||
			    (dup2(stderr_pipe[1], 2) == -1)) {
				error("dup2: %m");
				return 1;
			}
			(void) close(stderr_pipe[0]);
			(void) close(stderr_pipe[1]);
		}
		(void) close(stdin_pipe[0]);
		(void) close(stdin_pipe[1]);
		(void) close(stdout_pipe[0]);
		(void) close(stdout_pipe[1]);

		execvp(argv[7], argv+7);
		error("execv(%s) error: %m", argv[7]);
		return 1;
	}

	(void) close(stdin_pipe[0]);
	(void) close(stdout_pipe[1]);
	(void) close(stderr_pipe[1]);

	/* NOTE: dummy_pipe is only used to wake the select() function in the
	 * loop below when the spawned process terminates */
	if (pipe(dummy_pipe) == -1)
		error("pipe: %m");
	_wait_be_func(pid, signal_socket, &status, &job_fini, dummy_pipe[1]);

	n_fds = dummy_pipe[0];
	n_fds = MAX(signal_socket,  n_fds);
	n_fds = MAX(stdin_socket,   n_fds);
	n_fds = MAX(stderr_pipe[0], n_fds);
	n_fds = MAX(stdout_pipe[0], n_fds);

	while ( !(job_fini && stderr_fini && stdout_fini) ) {
		FD_ZERO(&read_fds);
		FD_SET(dummy_pipe[0], &read_fds);
		if (!signal_fini)
			FD_SET(signal_socket,  &read_fds);
		if (stdin_pipe[1] >= 0)
			FD_SET(stdin_socket,   &read_fds);
		if (!stderr_fini)
			FD_SET(stderr_pipe[0], &read_fds);
		if (!stdout_fini)
			FD_SET(stdout_pipe[0], &read_fds);

		i = select((n_fds + 1), &read_fds, NULL, NULL, NULL);
		if (i == -1) {
			if (errno == EINTR)
				continue;
			error("select: %m");
			break;
		}
		if ((signal_socket >= 0) &&
		    FD_ISSET(signal_socket, &read_fds) &&
		    _be_proc_signal(signal_socket, pid)) {
			/* Remote signal_socket closed */
			signal_fini = true;
		}
		if ((stdin_socket >= 0) && FD_ISSET(stdin_socket, &read_fds) &&
		    _be_proc_stdin(stdin_pipe[1], stdin_socket)) {
			/* Remote stdin closed */
			(void) close(stdin_pipe[1]);
			stdin_pipe[1] = -1;
		}
		if (FD_ISSET(stderr_pipe[0], &read_fds) &&
		    _be_proc_stdio(stderr_pipe[0], stderr_socket, "stderr")) {
			/* Remote stderr closed */
			stderr_fini = true;
		}
		if (FD_ISSET(stdout_pipe[0], &read_fds) &&
		    _be_proc_stdio(stdout_pipe[0], stdout_socket, "stdout")) {
			/* Remote stdout closed */
			stdout_fini = true;
		}
	}

	(void) close(dummy_pipe[0]);
	(void) close(dummy_pipe[1]);
	if (stdin_pipe[1] >= 0)
		(void) close(stdin_pipe[1]);
	(void) close(stderr_pipe[0]);
	(void) close(stdout_pipe[0]);
	slurm_close_stream(signal_socket);
	slurm_close_stream(stderr_socket);
	slurm_close_stream(stdout_socket);

	if (WIFEXITED(status))
		exit(WEXITSTATUS(status));
	exit(0);
}

#endif	/* USE_LOADLEVELER */