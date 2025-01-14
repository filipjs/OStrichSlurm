/*****************************************************************************\
 *  proc_req.c - process incoming messages to slurmctld
 *****************************************************************************
 *  Copyright (C) 2002-2007 The Regents of the University of California.
 *  Copyright (C) 2008-2011 Lawrence Livermore National Security.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Morris Jette <jette@llnl.gov>, et. al.
 *  CODE-OCEC-09-009. All rights reserved.
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://slurm.schedmd.com/>.
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
#endif

#ifdef WITH_PTHREADS
#  include <pthread.h>
#endif				/* WITH_PTHREADS */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "slurm/slurm_errno.h"

#include "src/common/assoc_mgr.h"
#include "src/common/checkpoint.h"
#include "src/common/daemonize.h"
#include "src/common/fd.h"
#include "src/common/forward.h"
#include "src/common/gres.h"
#include "src/common/hostlist.h"
#include "src/common/log.h"
#include "src/common/macros.h"
#include "src/common/node_select.h"
#include "src/common/pack.h"
#include "src/common/read_config.h"
#include "src/common/slurm_auth.h"
#include "src/common/slurm_cred.h"
#include "src/common/slurm_priority.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/slurm_topology.h"
#include "src/common/switch.h"
#include "src/common/xstring.h"
#include "src/common/slurm_ext_sensors.h"
#include "src/common/slurm_acct_gather.h"
#include "src/common/slurm_protocol_interface.h"

#include "src/slurmctld/agent.h"
#include "src/slurmctld/front_end.h"
#include "src/slurmctld/gang.h"
#include "src/slurmctld/job_scheduler.h"
#include "src/slurmctld/licenses.h"
#include "src/slurmctld/locks.h"
#include "src/slurmctld/proc_req.h"
#include "src/slurmctld/read_config.h"
#include "src/slurmctld/reservation.h"
#include "src/slurmctld/sched_plugin.h"
#include "src/slurmctld/slurmctld.h"
#include "src/slurmctld/srun_comm.h"
#include "src/slurmctld/state_save.h"
#include "src/slurmctld/trigger_mgr.h"

#include "src/plugins/select/bluegene/bg_enums.h"

static pthread_mutex_t rpc_mutex = PTHREAD_MUTEX_INITIALIZER;
static int rpc_type_size = 0;	/* Size of rpc_type_* arrays */
static uint16_t *rpc_type_id = NULL;
static uint32_t *rpc_type_cnt = NULL;
static uint64_t *rpc_type_time = NULL;
static int rpc_user_size = 0;	/* Size of rpc_user_* arrays */
static uint32_t *rpc_user_id = NULL;
static uint32_t *rpc_user_cnt = NULL;
static uint64_t *rpc_user_time = NULL;

static pthread_mutex_t throttle_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t throttle_cond = PTHREAD_COND_INITIALIZER;

static void         _fill_ctld_conf(slurm_ctl_conf_t * build_ptr);
static void         _kill_job_on_msg_fail(uint32_t job_id);
static int          _is_prolog_finished(uint32_t job_id);
static int 	    _launch_batch_step(job_desc_msg_t *job_desc_msg,
				       uid_t uid, uint32_t *step_id,
				       uint16_t protocol_version);
static int          _make_step_cred(struct step_record *step_rec,
				    slurm_cred_t **slurm_cred,
				    uint16_t protocol_version);
static void         _throttle_fini(int *active_rpc_cnt);
static void         _throttle_start(int *active_rpc_cnt);

inline static void  _slurm_rpc_accounting_first_reg(slurm_msg_t *msg);
inline static void  _slurm_rpc_accounting_register_ctld(slurm_msg_t *msg);
inline static void  _slurm_rpc_accounting_update_msg(slurm_msg_t *msg);
inline static void  _slurm_rpc_allocate_resources(slurm_msg_t * msg);
inline static void  _slurm_rpc_checkpoint(slurm_msg_t * msg);
inline static void  _slurm_rpc_checkpoint_comp(slurm_msg_t * msg);
inline static void  _slurm_rpc_checkpoint_task_comp(slurm_msg_t * msg);
inline static void  _slurm_rpc_delete_partition(slurm_msg_t * msg);
inline static void  _slurm_rpc_complete_job_allocation(slurm_msg_t * msg);
inline static void  _slurm_rpc_complete_batch_script(slurm_msg_t * msg);
inline static void  _slurm_rpc_complete_prolog(slurm_msg_t * msg);
inline static void  _slurm_rpc_dump_conf(slurm_msg_t * msg);
inline static void  _slurm_rpc_dump_front_end(slurm_msg_t * msg);
inline static void  _slurm_rpc_dump_jobs(slurm_msg_t * msg);
inline static void  _slurm_rpc_dump_jobs_user(slurm_msg_t * msg);
inline static void  _slurm_rpc_dump_job_single(slurm_msg_t * msg);
inline static void  _slurm_rpc_dump_licenses(slurm_msg_t * msg);
inline static void  _slurm_rpc_dump_nodes(slurm_msg_t * msg);
inline static void  _slurm_rpc_dump_node_single(slurm_msg_t * msg);
inline static void  _slurm_rpc_dump_partitions(slurm_msg_t * msg);
inline static void  _slurm_rpc_dump_spank(slurm_msg_t * msg);
inline static void  _slurm_rpc_dump_stats(slurm_msg_t * msg);
inline static void  _slurm_rpc_end_time(slurm_msg_t * msg);
inline static void  _slurm_rpc_epilog_complete(slurm_msg_t * msg);
inline static void  _slurm_rpc_get_shares(slurm_msg_t *msg);
inline static void  _slurm_rpc_get_topo(slurm_msg_t * msg);
inline static void  _slurm_rpc_get_priority_factors(slurm_msg_t *msg);
inline static void  _slurm_rpc_job_notify(slurm_msg_t * msg);
inline static void  _slurm_rpc_job_ready(slurm_msg_t * msg);
inline static void  _slurm_rpc_job_sbcast_cred(slurm_msg_t * msg);
inline static void  _slurm_rpc_job_step_kill(slurm_msg_t * msg);
inline static void  _slurm_rpc_job_step_create(slurm_msg_t * msg);
inline static void  _slurm_rpc_job_step_get_info(slurm_msg_t * msg);
inline static void  _slurm_rpc_job_will_run(slurm_msg_t * msg);
inline static void  _slurm_rpc_node_registration(slurm_msg_t * msg);
inline static void  _slurm_rpc_block_info(slurm_msg_t * msg);
inline static void  _slurm_rpc_job_alloc_info(slurm_msg_t * msg);
inline static void  _slurm_rpc_job_alloc_info_lite(slurm_msg_t * msg);
inline static void  _slurm_rpc_ping(slurm_msg_t * msg);
inline static void  _slurm_rpc_reboot_nodes(slurm_msg_t * msg);
inline static void  _slurm_rpc_reconfigure_controller(slurm_msg_t * msg);
inline static void  _slurm_rpc_resv_create(slurm_msg_t * msg);
inline static void  _slurm_rpc_resv_update(slurm_msg_t * msg);
inline static void  _slurm_rpc_resv_delete(slurm_msg_t * msg);
inline static void  _slurm_rpc_resv_show(slurm_msg_t * msg);
inline static void  _slurm_rpc_requeue(slurm_msg_t * msg);
inline static void  _slurm_rpc_takeover(slurm_msg_t * msg);
inline static void  _slurm_rpc_set_debug_flags(slurm_msg_t *msg);
inline static void  _slurm_rpc_set_debug_level(slurm_msg_t *msg);
inline static void  _slurm_rpc_set_schedlog_level(slurm_msg_t *msg);
inline static void  _slurm_rpc_shutdown_controller(slurm_msg_t * msg);
inline static void  _slurm_rpc_shutdown_controller_immediate(slurm_msg_t *
							     msg);
inline static void  _slurm_rpc_step_complete(slurm_msg_t * msg);
inline static void  _slurm_rpc_step_layout(slurm_msg_t * msg);
inline static void  _slurm_rpc_step_update(slurm_msg_t * msg);
inline static void  _slurm_rpc_submit_batch_job(slurm_msg_t * msg);
inline static void  _slurm_rpc_suspend(slurm_msg_t * msg);
inline static void  _slurm_rpc_trigger_clear(slurm_msg_t * msg);
inline static void  _slurm_rpc_trigger_get(slurm_msg_t * msg);
inline static void  _slurm_rpc_trigger_set(slurm_msg_t * msg);
inline static void  _slurm_rpc_trigger_pull(slurm_msg_t * msg);
inline static void  _slurm_rpc_update_front_end(slurm_msg_t * msg);
inline static void  _slurm_rpc_update_job(slurm_msg_t * msg);
inline static void  _slurm_rpc_update_node(slurm_msg_t * msg);
inline static void  _slurm_rpc_update_partition(slurm_msg_t * msg);
inline static void  _slurm_rpc_update_block(slurm_msg_t * msg);
inline static void  _slurm_rpc_kill_job2(slurm_msg_t *msg);

inline static void  _update_cred_key(void);

extern diag_stats_t slurmctld_diag_stats;

/*
 * slurmctld_req  - Process an individual RPC request
 * IN/OUT msg - the request message, data associated with the message is freed
 */
void slurmctld_req(slurm_msg_t *msg, connection_arg_t *arg)
{
	DEF_TIMERS;
	int i, rpc_type_index = -1, rpc_user_index = -1;
	uint32_t rpc_uid;

	/* Just to validate the cred */
	rpc_uid = (uint32_t) g_slurm_auth_get_uid(msg->auth_cred, NULL);
	if (g_slurm_auth_errno(msg->auth_cred) != SLURM_SUCCESS) {
		error("Bad authentication: %s",
		      g_slurm_auth_errstr(g_slurm_auth_errno(msg->auth_cred)));
		return;
	}

	START_TIMER;
	slurm_mutex_lock(&rpc_mutex);
	if (rpc_type_size == 0) {
		rpc_type_size = 100;  /* Capture info for first 100 RPC types */
		rpc_type_id   = xmalloc(sizeof(uint16_t) * rpc_type_size);
		rpc_type_cnt  = xmalloc(sizeof(uint32_t) * rpc_type_size);
		rpc_type_time = xmalloc(sizeof(uint64_t) * rpc_type_size);
	}
	for (i = 0; i < rpc_type_size; i++) {
		if (rpc_type_id[i] == 0)
			rpc_type_id[i] = msg->msg_type;
		else if (rpc_type_id[i] != msg->msg_type)
			continue;
		rpc_type_index = i;
		break;
	}
	if (rpc_user_size == 0) {
		rpc_user_size = 200;  /* Capture info for first 200 RPC users */
		rpc_user_id   = xmalloc(sizeof(uint32_t) * rpc_user_size);
		rpc_user_cnt  = xmalloc(sizeof(uint32_t) * rpc_user_size);
		rpc_user_time = xmalloc(sizeof(uint64_t) * rpc_user_size);
	}
	for (i = 0; i < rpc_user_size; i++) {
		if ((rpc_user_id[i] == 0) && (i != 0))
			rpc_user_id[i] = rpc_uid;
		else if (rpc_user_id[i] != rpc_uid)
			continue;
		rpc_user_index = i;
		break;
	}
	slurm_mutex_unlock(&rpc_mutex);

	/* Debug the protocol layer.
	 */
	if (slurmctld_conf.debug_flags & DEBUG_FLAG_PROTOCOL) {
		char *p;
		char inetbuf[64];

		p = rpc_num2string(msg->msg_type);
		_slurm_print_slurm_addr(&arg->cli_addr,
					inetbuf,
					sizeof(inetbuf));
		info("%s: received opcode %s from %s", __func__, p, inetbuf);
	}

	switch (msg->msg_type) {
	case REQUEST_RESOURCE_ALLOCATION:
		_slurm_rpc_allocate_resources(msg);
		slurm_free_job_desc_msg(msg->data);
		break;
	case REQUEST_BUILD_INFO:
		_slurm_rpc_dump_conf(msg);
		slurm_free_last_update_msg(msg->data);
		break;
	case REQUEST_JOB_INFO:
		_slurm_rpc_dump_jobs(msg);
		slurm_free_job_info_request_msg(msg->data);
		break;
	case REQUEST_JOB_USER_INFO:
		_slurm_rpc_dump_jobs_user(msg);
		slurm_free_job_user_id_msg(msg->data);
		break;
	case REQUEST_JOB_INFO_SINGLE:
		_slurm_rpc_dump_job_single(msg);
		slurm_free_job_id_msg(msg->data);
		break;
	case REQUEST_SHARE_INFO:
		_slurm_rpc_get_shares(msg);
		slurm_free_shares_request_msg(msg->data);
		break;
	case REQUEST_PRIORITY_FACTORS:
		_slurm_rpc_get_priority_factors(msg);
		slurm_free_priority_factors_request_msg(msg->data);
		break;
	case REQUEST_JOB_END_TIME:
		_slurm_rpc_end_time(msg);
		slurm_free_job_alloc_info_msg(msg->data);
		break;
	case REQUEST_FRONT_END_INFO:
		_slurm_rpc_dump_front_end(msg);
		slurm_free_front_end_info_request_msg(msg->data);
		break;
	case REQUEST_NODE_INFO:
		_slurm_rpc_dump_nodes(msg);
		slurm_free_node_info_request_msg(msg->data);
		break;
	case REQUEST_NODE_INFO_SINGLE:
		_slurm_rpc_dump_node_single(msg);
		slurm_free_node_info_single_msg(msg->data);
		break;
	case REQUEST_PARTITION_INFO:
		_slurm_rpc_dump_partitions(msg);
		slurm_free_part_info_request_msg(msg->data);
		break;
	case MESSAGE_EPILOG_COMPLETE:
		_slurm_rpc_epilog_complete(msg);
		slurm_free_epilog_complete_msg(msg->data);
		break;
	case REQUEST_CANCEL_JOB_STEP:
		_slurm_rpc_job_step_kill(msg);
		slurm_free_job_step_kill_msg(msg->data);
		break;
	case REQUEST_COMPLETE_JOB_ALLOCATION:
		_slurm_rpc_complete_job_allocation(msg);
		slurm_free_complete_job_allocation_msg(msg->data);
		break;
	case REQUEST_COMPLETE_PROLOG:
		_slurm_rpc_complete_prolog(msg);
		slurm_free_complete_prolog_msg(msg->data);
		break;
	case REQUEST_COMPLETE_BATCH_JOB:
	case REQUEST_COMPLETE_BATCH_SCRIPT:
		_slurm_rpc_complete_batch_script(msg);
		slurm_free_complete_batch_script_msg(msg->data);
		break;
	case REQUEST_JOB_STEP_CREATE:
		_slurm_rpc_job_step_create(msg);
		slurm_free_job_step_create_request_msg(msg->data);
		break;
	case REQUEST_JOB_STEP_INFO:
		_slurm_rpc_job_step_get_info(msg);
		slurm_free_job_step_info_request_msg(msg->data);
		break;
	case REQUEST_JOB_WILL_RUN:
		_slurm_rpc_job_will_run(msg);
		slurm_free_job_desc_msg(msg->data);
		break;
	case MESSAGE_NODE_REGISTRATION_STATUS:
		_slurm_rpc_node_registration(msg);
		slurm_free_node_registration_status_msg(msg->data);
		break;
	case REQUEST_JOB_ALLOCATION_INFO:
		_slurm_rpc_job_alloc_info(msg);
		slurm_free_job_alloc_info_msg(msg->data);
		break;
	case REQUEST_JOB_ALLOCATION_INFO_LITE:
		_slurm_rpc_job_alloc_info_lite(msg);
		slurm_free_job_alloc_info_msg(msg->data);
		break;
	case REQUEST_JOB_SBCAST_CRED:
		_slurm_rpc_job_sbcast_cred(msg);
		slurm_free_job_alloc_info_msg(msg->data);
		break;
	case REQUEST_PING:
		_slurm_rpc_ping(msg);
		/* No body to free */
		break;
	case REQUEST_RECONFIGURE:
		_slurm_rpc_reconfigure_controller(msg);
		/* No body to free */
		break;
	case REQUEST_CONTROL:
		_slurm_rpc_shutdown_controller(msg);
		/* No body to free */
		break;
	case REQUEST_TAKEOVER:
		_slurm_rpc_takeover(msg);
		/* No body to free */
		break;
	case REQUEST_SHUTDOWN:
		_slurm_rpc_shutdown_controller(msg);
		slurm_free_shutdown_msg(msg->data);
		break;
	case REQUEST_SHUTDOWN_IMMEDIATE:
		_slurm_rpc_shutdown_controller_immediate(msg);
		/* No body to free */
		break;
	case REQUEST_SUBMIT_BATCH_JOB:
		_slurm_rpc_submit_batch_job(msg);
		slurm_free_job_desc_msg(msg->data);
		break;
	case REQUEST_UPDATE_FRONT_END:
		_slurm_rpc_update_front_end(msg);
		slurm_free_update_front_end_msg(msg->data);
		break;
	case REQUEST_UPDATE_JOB:
		_slurm_rpc_update_job(msg);
		slurm_free_job_desc_msg(msg->data);
		break;
	case REQUEST_UPDATE_NODE:
		_slurm_rpc_update_node(msg);
		slurm_free_update_node_msg(msg->data);
		break;
	case REQUEST_CREATE_PARTITION:
	case REQUEST_UPDATE_PARTITION:
		_slurm_rpc_update_partition(msg);
		slurm_free_update_part_msg(msg->data);
		break;
	case REQUEST_DELETE_PARTITION:
		_slurm_rpc_delete_partition(msg);
		slurm_free_delete_part_msg(msg->data);
		break;
	case REQUEST_CREATE_RESERVATION:
		_slurm_rpc_resv_create(msg);
		slurm_free_resv_desc_msg(msg->data);
		break;
	case REQUEST_UPDATE_RESERVATION:
		_slurm_rpc_resv_update(msg);
		slurm_free_resv_desc_msg(msg->data);
		break;
	case REQUEST_DELETE_RESERVATION:
		_slurm_rpc_resv_delete(msg);
		slurm_free_resv_name_msg(msg->data);
		break;
	case REQUEST_UPDATE_BLOCK:
		_slurm_rpc_update_block(msg);
		slurm_free_block_info(msg->data);
		break;
	case REQUEST_RESERVATION_INFO:
		_slurm_rpc_resv_show(msg);
		slurm_free_resv_info_request_msg(msg->data);
		break;
	case REQUEST_NODE_REGISTRATION_STATUS:
		error("slurmctld is talking with itself. "
		      "SlurmctldPort == SlurmdPort");
		slurm_send_rc_msg(msg, EINVAL);
		break;
	case REQUEST_CHECKPOINT:
		_slurm_rpc_checkpoint(msg);
		slurm_free_checkpoint_msg(msg->data);
		break;
	case REQUEST_CHECKPOINT_COMP:
		_slurm_rpc_checkpoint_comp(msg);
		slurm_free_checkpoint_comp_msg(msg->data);
		break;
	case REQUEST_CHECKPOINT_TASK_COMP:
		_slurm_rpc_checkpoint_task_comp(msg);
		slurm_free_checkpoint_task_comp_msg(msg->data);
		break;
	case REQUEST_SUSPEND:
		_slurm_rpc_suspend(msg);
		slurm_free_suspend_msg(msg->data);
		break;
	case REQUEST_JOB_REQUEUE:
		_slurm_rpc_requeue(msg);
		slurm_free_requeue_msg(msg->data);
		break;
	case REQUEST_JOB_READY:
		_slurm_rpc_job_ready(msg);
		slurm_free_job_id_msg(msg->data);
		break;
	case REQUEST_BLOCK_INFO:
		_slurm_rpc_block_info(msg);
		slurm_free_block_info_request_msg(msg->data);
		break;
	case REQUEST_STEP_COMPLETE:
		_slurm_rpc_step_complete(msg);
		slurm_free_step_complete_msg(msg->data);
		break;
	case REQUEST_STEP_LAYOUT:
		_slurm_rpc_step_layout(msg);
		slurm_free_job_step_id_msg(msg->data);
		break;
	case REQUEST_UPDATE_JOB_STEP:
		_slurm_rpc_step_update(msg);
		slurm_free_update_step_msg(msg->data);
		break;
	case REQUEST_TRIGGER_SET:
		_slurm_rpc_trigger_set(msg);
		slurm_free_trigger_msg(msg->data);
		break;
	case REQUEST_TRIGGER_GET:
		_slurm_rpc_trigger_get(msg);
		slurm_free_trigger_msg(msg->data);
		break;
	case REQUEST_TRIGGER_CLEAR:
		_slurm_rpc_trigger_clear(msg);
		slurm_free_trigger_msg(msg->data);
		break;
	case REQUEST_TRIGGER_PULL:
		_slurm_rpc_trigger_pull(msg);
		slurm_free_trigger_msg(msg->data);
		break;
	case REQUEST_JOB_NOTIFY:
		_slurm_rpc_job_notify(msg);
		slurm_free_job_notify_msg(msg->data);
		break;
	case REQUEST_SET_DEBUG_FLAGS:
		_slurm_rpc_set_debug_flags(msg);
		slurm_free_set_debug_flags_msg(msg->data);
		break;
	case REQUEST_SET_DEBUG_LEVEL:
		_slurm_rpc_set_debug_level(msg);
		slurm_free_set_debug_level_msg(msg->data);
		break;
	case REQUEST_SET_SCHEDLOG_LEVEL:
		_slurm_rpc_set_schedlog_level(msg);
		slurm_free_set_debug_level_msg(msg->data);
		break;
	case ACCOUNTING_UPDATE_MSG:
		_slurm_rpc_accounting_update_msg(msg);
		slurm_free_accounting_update_msg(msg->data);
		break;
	case ACCOUNTING_FIRST_REG:
		_slurm_rpc_accounting_first_reg(msg);
		/* No body to free */
		break;
	case ACCOUNTING_REGISTER_CTLD:
		_slurm_rpc_accounting_register_ctld(msg);
		slurm_free_reboot_msg(msg->data);
		break;
	case REQUEST_TOPO_INFO:
		_slurm_rpc_get_topo(msg);
		/* No body to free */
		break;
	case REQUEST_SPANK_ENVIRONMENT:
		_slurm_rpc_dump_spank(msg);
		slurm_free_spank_env_request_msg(msg->data);
		break;
	case REQUEST_REBOOT_NODES:
		_slurm_rpc_reboot_nodes(msg);
		/* No body to free */
		break;
	case REQUEST_STATS_INFO:
		_slurm_rpc_dump_stats(msg);
		slurm_free_stats_info_request_msg(msg->data);
		break;
	case REQUEST_LICENSE_INFO:
		_slurm_rpc_dump_licenses(msg);
		slurm_free_license_info_request_msg(msg->data);
		break;
	 case REQUEST_KILL_JOB:
		_slurm_rpc_kill_job2(msg);
		slurm_free_job_step_kill_msg(msg->data);
		break;
	default:
		error("invalid RPC msg_type=%u", msg->msg_type);
		slurm_send_rc_msg(msg, EINVAL);
		break;
	}

	END_TIMER;
	slurm_mutex_lock(&rpc_mutex);
	if (rpc_type_index >= 0) {
		rpc_type_cnt[rpc_type_index]++;
		rpc_type_time[rpc_type_index] += DELTA_TIMER;
	}
	if (rpc_user_index >= 0) {
		rpc_user_cnt[rpc_user_index]++;
		rpc_user_time[rpc_user_index] += DELTA_TIMER;
	}
	slurm_mutex_unlock(&rpc_mutex);
}

/* These functions prevent certain RPCs from keeping the slurmctld write locks
 * constantly set, which can prevent other RPCs and system functions from being
 * processed. For example, a steady stream of batch submissions can prevent
 * squeue from responding or jobs from being scheduled. */
static void _throttle_start(int *active_rpc_cnt)
{
	slurm_mutex_lock(&throttle_mutex);
	while (1) {
		if (*active_rpc_cnt == 0) {
			(*active_rpc_cnt)++;
			break;
		}
		pthread_cond_wait(&throttle_cond, &throttle_mutex);
	}
	slurm_mutex_unlock(&throttle_mutex);
	usleep(1);
}
static void _throttle_fini(int *active_rpc_cnt)
{
	slurm_mutex_lock(&throttle_mutex);
	(*active_rpc_cnt)--;
	pthread_cond_broadcast(&throttle_cond);
	slurm_mutex_unlock(&throttle_mutex);
}

/*
 * _fill_ctld_conf - make a copy of current slurm configuration
 *	this is done with locks set so the data can change at other times
 * OUT conf_ptr - place to copy configuration to
 */
static void _fill_ctld_conf(slurm_ctl_conf_t * conf_ptr)
{
	char *licenses_used = get_licenses_used();  /* Do before config lock */
	slurm_ctl_conf_t *conf = slurm_conf_lock();

	memset(conf_ptr, 0, sizeof(slurm_ctl_conf_t));

	conf_ptr->last_update         = time(NULL);
	conf_ptr->accounting_storage_enforce =
		conf->accounting_storage_enforce;
	conf_ptr->accounting_storage_host =
		xstrdup(conf->accounting_storage_host);
	conf_ptr->accounting_storage_backup_host =
		xstrdup(conf->accounting_storage_backup_host);
	conf_ptr->accounting_storage_loc =
		xstrdup(conf->accounting_storage_loc);
	conf_ptr->accounting_storage_port = conf->accounting_storage_port;
	conf_ptr->accounting_storage_type =
		xstrdup(conf->accounting_storage_type);
	conf_ptr->accounting_storage_user =
		xstrdup(conf->accounting_storage_user);
	conf_ptr->acctng_store_job_comment = conf->acctng_store_job_comment;

	conf_ptr->acct_gather_conf = acct_gather_conf_values();
	conf_ptr->acct_gather_energy_type =
		xstrdup(conf->acct_gather_energy_type);
	conf_ptr->acct_gather_filesystem_type =
		xstrdup(conf->acct_gather_filesystem_type);
	conf_ptr->acct_gather_infiniband_type =
		xstrdup(conf->acct_gather_infiniband_type);
	conf_ptr->acct_gather_profile_type =
		xstrdup(conf->acct_gather_profile_type);
	conf_ptr->acct_gather_node_freq = conf->acct_gather_node_freq;

	conf_ptr->authinfo            = xstrdup(conf->authinfo);
	conf_ptr->authtype            = xstrdup(conf->authtype);

	conf_ptr->backup_addr         = xstrdup(conf->backup_addr);
	conf_ptr->backup_controller   = xstrdup(conf->backup_controller);
	conf_ptr->batch_start_timeout = conf->batch_start_timeout;
	conf_ptr->boot_time           = slurmctld_config.boot_time;

	conf_ptr->checkpoint_type     = xstrdup(conf->checkpoint_type);
	conf_ptr->chos_loc            = xstrdup(conf->chos_loc);
	conf_ptr->cluster_name        = xstrdup(conf->cluster_name);
	conf_ptr->complete_wait       = conf->complete_wait;
	conf_ptr->control_addr        = xstrdup(conf->control_addr);
	conf_ptr->control_machine     = xstrdup(conf->control_machine);
	conf_ptr->core_spec_plugin    = xstrdup(conf->core_spec_plugin);
	conf_ptr->cpu_freq_def        = conf->cpu_freq_def;
	conf_ptr->crypto_type         = xstrdup(conf->crypto_type);

	conf_ptr->def_mem_per_cpu     = conf->def_mem_per_cpu;
	conf_ptr->debug_flags         = conf->debug_flags;
	conf_ptr->disable_root_jobs   = conf->disable_root_jobs;
	conf_ptr->dynalloc_port       = conf->dynalloc_port;

	conf_ptr->enforce_part_limits = conf->enforce_part_limits;
	conf_ptr->epilog              = xstrdup(conf->epilog);
	conf_ptr->epilog_msg_time     = conf->epilog_msg_time;
	conf_ptr->epilog_slurmctld    = xstrdup(conf->epilog_slurmctld);
	ext_sensors_g_get_config(&conf_ptr->ext_sensors_conf);
	conf_ptr->ext_sensors_type    = xstrdup(conf->ext_sensors_type);
	conf_ptr->ext_sensors_freq    = conf->ext_sensors_freq;

	conf_ptr->fast_schedule       = conf->fast_schedule;
	conf_ptr->first_job_id        = conf->first_job_id;
	conf_ptr->fs_dampening_factor = conf->fs_dampening_factor;

	conf_ptr->gres_plugins        = xstrdup(conf->gres_plugins);
	conf_ptr->group_info          = conf->group_info;

	conf_ptr->inactive_limit      = conf->inactive_limit;

	conf_ptr->hash_val            = conf->hash_val;
	conf_ptr->health_check_interval = conf->health_check_interval;
	conf_ptr->health_check_node_state = conf->health_check_node_state;
	conf_ptr->health_check_program = xstrdup(conf->health_check_program);

	conf_ptr->job_acct_gather_freq  = xstrdup(conf->job_acct_gather_freq);
	conf_ptr->job_acct_gather_type  = xstrdup(conf->job_acct_gather_type);
	conf_ptr->job_acct_gather_params= xstrdup(conf->job_acct_gather_params);

	conf_ptr->job_ckpt_dir        = xstrdup(conf->job_ckpt_dir);
	conf_ptr->job_comp_host       = xstrdup(conf->job_comp_host);
	conf_ptr->job_comp_loc        = xstrdup(conf->job_comp_loc);
	conf_ptr->job_comp_port       = conf->job_comp_port;
	conf_ptr->job_comp_type       = xstrdup(conf->job_comp_type);
	conf_ptr->job_comp_user       = xstrdup(conf->job_comp_user);
	conf_ptr->job_container_plugin = xstrdup(conf->job_container_plugin);

	conf_ptr->job_credential_private_key =
		xstrdup(conf->job_credential_private_key);
	conf_ptr->job_credential_public_certificate =
		xstrdup(conf->job_credential_public_certificate);
	conf_ptr->job_file_append     = conf->job_file_append;
	conf_ptr->job_requeue         = conf->job_requeue;
	conf_ptr->job_submit_plugins  = xstrdup(conf->job_submit_plugins);

	conf_ptr->get_env_timeout     = conf->get_env_timeout;

	conf_ptr->keep_alive_time     = conf->keep_alive_time;
	conf_ptr->kill_wait           = conf->kill_wait;
	conf_ptr->kill_on_bad_exit    = conf->kill_on_bad_exit;

	conf_ptr->launch_type         = xstrdup(conf->launch_type);
	conf_ptr->layouts             = xstrdup(conf->layouts);
	conf_ptr->licenses            = xstrdup(conf->licenses);
	conf_ptr->licenses_used       = licenses_used;
	conf_ptr->log_fmt             = conf->log_fmt;

	conf_ptr->mail_prog           = xstrdup(conf->mail_prog);
	conf_ptr->max_array_sz        = conf->max_array_sz;
	conf_ptr->max_job_cnt         = conf->max_job_cnt;
	conf_ptr->max_job_id          = conf->max_job_id;
	conf_ptr->max_mem_per_cpu     = conf->max_mem_per_cpu;
	conf_ptr->max_step_cnt        = conf->max_step_cnt;
	conf_ptr->max_tasks_per_node  = conf->max_tasks_per_node;
	conf_ptr->mem_limit_enforce   = conf->mem_limit_enforce;
	conf_ptr->min_job_age         = conf->min_job_age;
	conf_ptr->mpi_default         = xstrdup(conf->mpi_default);
	conf_ptr->mpi_params          = xstrdup(conf->mpi_params);
	conf_ptr->msg_timeout         = conf->msg_timeout;

	conf_ptr->next_job_id         = get_next_job_id();
	conf_ptr->node_prefix         = xstrdup(conf->node_prefix);

	conf_ptr->over_time_limit     = conf->over_time_limit;

	conf_ptr->plugindir           = xstrdup(conf->plugindir);
	conf_ptr->plugstack           = xstrdup(conf->plugstack);

	conf_ptr->preempt_mode        = conf->preempt_mode;
	conf_ptr->preempt_type        = xstrdup(conf->preempt_type);
	conf_ptr->priority_decay_hl   = conf->priority_decay_hl;
	conf_ptr->priority_calc_period = conf->priority_calc_period;
	conf_ptr->priority_favor_small= conf->priority_favor_small;
	conf_ptr->priority_flags      = conf->priority_flags;
	conf_ptr->priority_max_age    = conf->priority_max_age;
	conf_ptr->priority_params     = xstrdup(conf->priority_params);
	conf_ptr->priority_reset_period = conf->priority_reset_period;
	conf_ptr->priority_type       = xstrdup(conf->priority_type);
	conf_ptr->priority_weight_age = conf->priority_weight_age;
	conf_ptr->priority_weight_fs  = conf->priority_weight_fs;
	conf_ptr->priority_weight_js  = conf->priority_weight_js;
	conf_ptr->priority_weight_part= conf->priority_weight_part;
	conf_ptr->priority_weight_qos = conf->priority_weight_qos;

	conf_ptr->private_data        = conf->private_data;
	conf_ptr->proctrack_type      = xstrdup(conf->proctrack_type);
	conf_ptr->prolog              = xstrdup(conf->prolog);
	conf_ptr->prolog_slurmctld    = xstrdup(conf->prolog_slurmctld);
	conf_ptr->prolog_flags        = conf->prolog_flags;
	conf_ptr->propagate_prio_process =
		slurmctld_conf.propagate_prio_process;
	conf_ptr->propagate_rlimits   = xstrdup(conf->propagate_rlimits);
	conf_ptr->propagate_rlimits_except = xstrdup(conf->
						     propagate_rlimits_except);

	conf_ptr->reboot_program      = xstrdup(conf->reboot_program);
	conf_ptr->reconfig_flags      = conf->reconfig_flags;
	conf_ptr->requeue_exit        = xstrdup(conf->requeue_exit);
	conf_ptr->requeue_exit_hold   = xstrdup(conf->requeue_exit_hold);
	conf_ptr->resume_program      = xstrdup(conf->resume_program);
	conf_ptr->resume_rate         = conf->resume_rate;
	conf_ptr->resume_timeout      = conf->resume_timeout;
	conf_ptr->resv_epilog         = xstrdup(conf->resv_epilog);
	conf_ptr->resv_over_run       = conf->resv_over_run;
	conf_ptr->resv_prolog         = xstrdup(conf->resv_prolog);
	conf_ptr->ret2service         = conf->ret2service;

	conf_ptr->salloc_default_command = xstrdup(conf->
						   salloc_default_command);
	if (conf->sched_params)
		conf_ptr->sched_params = xstrdup(conf->sched_params);
	else
		conf_ptr->sched_params = slurm_sched_g_get_conf();
	conf_ptr->schedport           = conf->schedport;
	conf_ptr->schedrootfltr       = conf->schedrootfltr;
	conf_ptr->sched_logfile       = xstrdup(conf->sched_logfile);
	conf_ptr->sched_log_level     = conf->sched_log_level;
	conf_ptr->sched_time_slice    = conf->sched_time_slice;
	conf_ptr->schedtype           = xstrdup(conf->schedtype);
	conf_ptr->select_type         = xstrdup(conf->select_type);
	select_g_get_info_from_plugin(SELECT_CONFIG_INFO, NULL,
				      &conf_ptr->select_conf_key_pairs);
	conf_ptr->select_type_param   = conf->select_type_param;
	conf_ptr->slurm_user_id       = conf->slurm_user_id;
	conf_ptr->slurm_user_name     = xstrdup(conf->slurm_user_name);
	conf_ptr->slurmctld_debug     = conf->slurmctld_debug;
	conf_ptr->slurmctld_logfile   = xstrdup(conf->slurmctld_logfile);
	conf_ptr->slurmctld_pidfile   = xstrdup(conf->slurmctld_pidfile);
	conf_ptr->slurmctld_plugstack = xstrdup(conf->slurmctld_plugstack);
	conf_ptr->slurmctld_port      = conf->slurmctld_port;
	conf_ptr->slurmctld_port_count = conf->slurmctld_port_count;
	conf_ptr->slurmctld_timeout   = conf->slurmctld_timeout;
	conf_ptr->slurmd_debug        = conf->slurmd_debug;
	conf_ptr->slurmd_logfile      = xstrdup(conf->slurmd_logfile);
	conf_ptr->slurmd_pidfile      = xstrdup(conf->slurmd_pidfile);
	conf_ptr->slurmd_plugstack    = xstrdup(conf->slurmd_plugstack);
	conf_ptr->slurmd_port         = conf->slurmd_port;
	conf_ptr->slurmd_spooldir     = xstrdup(conf->slurmd_spooldir);
	conf_ptr->slurmd_timeout      = conf->slurmd_timeout;
	conf_ptr->slurmd_user_id      = conf->slurmd_user_id;
	conf_ptr->slurmd_user_name    = xstrdup(conf->slurmd_user_name);
	conf_ptr->slurm_conf          = xstrdup(conf->slurm_conf);
	conf_ptr->srun_epilog         = xstrdup(conf->srun_epilog);

	conf_ptr->srun_port_range = xmalloc(2 * sizeof(uint16_t));
	if (conf->srun_port_range) {
		conf_ptr->srun_port_range[0] = conf->srun_port_range[0];
		conf_ptr->srun_port_range[1] = conf->srun_port_range[1];
	} else {
		conf_ptr->srun_port_range[0] = 0;
		conf_ptr->srun_port_range[1] = 0;
	}

	conf_ptr->srun_prolog         = xstrdup(conf->srun_prolog);
	conf_ptr->state_save_location = xstrdup(conf->state_save_location);
	conf_ptr->suspend_exc_nodes   = xstrdup(conf->suspend_exc_nodes);
	conf_ptr->suspend_exc_parts   = xstrdup(conf->suspend_exc_parts);
	conf_ptr->suspend_program     = xstrdup(conf->suspend_program);
	conf_ptr->suspend_rate        = conf->suspend_rate;
	conf_ptr->suspend_time        = conf->suspend_time;
	conf_ptr->suspend_timeout     = conf->suspend_timeout;
	conf_ptr->switch_type         = xstrdup(conf->switch_type);

	conf_ptr->task_epilog         = xstrdup(conf->task_epilog);
	conf_ptr->task_prolog         = xstrdup(conf->task_prolog);
	conf_ptr->task_plugin         = xstrdup(conf->task_plugin);
	conf_ptr->task_plugin_param   = conf->task_plugin_param;
	conf_ptr->tmp_fs              = xstrdup(conf->tmp_fs);
	conf_ptr->topology_plugin     = xstrdup(conf->topology_plugin);
	conf_ptr->track_wckey         = conf->track_wckey;
	conf_ptr->tree_width          = conf->tree_width;

	conf_ptr->wait_time           = conf->wait_time;

	conf_ptr->use_pam             = conf->use_pam;
	conf_ptr->use_spec_resources  = conf->use_spec_resources;
	conf_ptr->unkillable_program  = xstrdup(conf->unkillable_program);
	conf_ptr->unkillable_timeout  = conf->unkillable_timeout;
	conf_ptr->version             = xstrdup(SLURM_VERSION_STRING);
	conf_ptr->vsize_factor        = conf->vsize_factor;

	slurm_conf_unlock();
	return;
}

/*
 * validate_slurm_user - validate that the uid is authorized to see
 *      privileged data (either user root or SlurmUser)
 * IN uid - user to validate
 * RET true if permitted to run, false otherwise
 */
extern bool validate_slurm_user(uid_t uid)
{
	if ((uid == 0) || (uid == getuid()))
		return true;
	else
		return false;
}

/*
 * validate_super_user - validate that the uid is authorized at the
 *      root, SlurmUser, or SLURMDB_ADMIN_SUPER_USER level
 * IN uid - user to validate
 * RET true if permitted to run, false otherwise
 */
extern bool validate_super_user(uid_t uid)
{
	if ((uid == 0) || (uid == getuid()) ||
	    assoc_mgr_get_admin_level(acct_db_conn, uid) >=
	    SLURMDB_ADMIN_SUPER_USER)
		return true;
	else
		return false;
}

/*
 * validate_operator - validate that the uid is authorized at the
 *      root, SlurmUser, or SLURMDB_ADMIN_OPERATOR level
 * IN uid - user to validate
 * RET true if permitted to run, false otherwise
 */
extern bool validate_operator(uid_t uid)
{
	if ((uid == 0) || (uid == getuid()) ||
	    assoc_mgr_get_admin_level(acct_db_conn, uid) >=
	    SLURMDB_ADMIN_OPERATOR)
		return true;
	else
		return false;
}

/* _kill_job_on_msg_fail - The request to create a job record successed,
 *	but the reply message to srun failed. We kill the job to avoid
 *	leaving it orphaned */
static void _kill_job_on_msg_fail(uint32_t job_id)
{
	/* Locks: Write job, write node */
	slurmctld_lock_t job_write_lock = {
		NO_LOCK, WRITE_LOCK, WRITE_LOCK, NO_LOCK };

	error("Job allocate response msg send failure, killing JobId=%u",
	      job_id);
	lock_slurmctld(job_write_lock);
	job_complete(job_id, 0, false, false, 0);
	unlock_slurmctld(job_write_lock);
}

/* create a credential for a given job step, return error code */
static int _make_step_cred(struct step_record *step_ptr,
			   slurm_cred_t **slurm_cred, uint16_t protocol_version)
{
	slurm_cred_arg_t cred_arg;
	struct job_record* job_ptr = step_ptr->job_ptr;
	job_resources_t *job_resrcs_ptr = job_ptr->job_resrcs;

	xassert(job_resrcs_ptr && job_resrcs_ptr->cpus);

	memset(&cred_arg, 0, sizeof(slurm_cred_arg_t));

	cred_arg.jobid    = job_ptr->job_id;
	cred_arg.stepid   = step_ptr->step_id;
	cred_arg.uid      = job_ptr->user_id;

	cred_arg.job_core_bitmap = job_resrcs_ptr->core_bitmap;
	cred_arg.job_core_spec   = job_ptr->details->core_spec;
	cred_arg.job_hostlist    = job_resrcs_ptr->nodes;
	cred_arg.job_mem_limit   = job_ptr->details->pn_min_memory;
	cred_arg.job_nhosts      = job_resrcs_ptr->nhosts;
	cred_arg.job_gres_list   = job_ptr->gres_list;
	cred_arg.step_gres_list  = step_ptr->gres_list;

	cred_arg.step_core_bitmap = step_ptr->core_bitmap_job;
#ifdef HAVE_FRONT_END
	xassert(job_ptr->batch_host);
	cred_arg.step_hostlist   = job_ptr->batch_host;
#else
	cred_arg.step_hostlist   = step_ptr->step_layout->node_list;
#endif
	if (step_ptr->pn_min_memory)
		cred_arg.step_mem_limit  = step_ptr->pn_min_memory;

	cred_arg.cores_per_socket    = job_resrcs_ptr->cores_per_socket;
	cred_arg.sockets_per_node    = job_resrcs_ptr->sockets_per_node;
	cred_arg.sock_core_rep_count = job_resrcs_ptr->sock_core_rep_count;

	*slurm_cred = slurm_cred_create(slurmctld_config.cred_ctx, &cred_arg,
					protocol_version);
	if (*slurm_cred == NULL) {
		error("slurm_cred_create error");
		return ESLURM_INVALID_JOB_CREDENTIAL;
	}

	return SLURM_SUCCESS;
}

/* _slurm_rpc_allocate_resources:  process RPC to allocate resources for
 *	a job */
static void _slurm_rpc_allocate_resources(slurm_msg_t * msg)
{
	static int active_rpc_cnt = 0;
	int error_code = SLURM_SUCCESS;
	slurm_msg_t response_msg;
	DEF_TIMERS;
	job_desc_msg_t *job_desc_msg = (job_desc_msg_t *) msg->data;
	resource_allocation_response_msg_t alloc_msg;
	/* Locks: Read config, read job, read node, read partition */
	slurmctld_lock_t job_read_lock = {
		READ_LOCK, READ_LOCK, READ_LOCK, READ_LOCK };
	/* Locks: Read config, write job, write node, read partition */
	slurmctld_lock_t job_write_lock = {
		READ_LOCK, WRITE_LOCK, WRITE_LOCK, READ_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);
	int immediate = job_desc_msg->immediate;
	bool do_unlock = false;
	bool job_waiting = false;
	struct job_record *job_ptr = NULL;
	uint16_t port;	/* dummy value */
	slurm_addr_t resp_addr;
	char *err_msg = NULL;

	START_TIMER;
	if ((uid != job_desc_msg->user_id) && (!validate_slurm_user(uid))) {
		error_code = ESLURM_USER_ID_MISSING;
		error("Security violation, RESOURCE_ALLOCATE from uid=%d",
		      uid);
	}
	debug2("sched: Processing RPC: REQUEST_RESOURCE_ALLOCATION from uid=%d",
	       uid);

	/* do RPC call */
	if ((job_desc_msg->alloc_node == NULL) ||
	    (job_desc_msg->alloc_node[0] == '\0')) {
		error_code = ESLURM_INVALID_NODE_NAME;
		error("REQUEST_RESOURCE_ALLOCATE lacks alloc_node from uid=%d",
		      uid);
	}

	if (error_code == SLURM_SUCCESS) {
		/* Locks are for job_submit plugin use */
		lock_slurmctld(job_read_lock);
		error_code = validate_job_create_req(job_desc_msg,uid,&err_msg);
		unlock_slurmctld(job_read_lock);
	}

#if HAVE_ALPS_CRAY
	/*
	 * Catch attempts to nest salloc sessions. It is not possible to use an
	 * ALPS session which has the same alloc_sid, it fails even if PAGG
	 * container IDs are used.
	 */
	if (allocated_session_in_use(job_desc_msg)) {
		error_code = ESLURM_RESERVATION_BUSY;
		error("attempt to nest ALPS allocation on %s:%d by uid=%d",
			job_desc_msg->alloc_node, job_desc_msg->alloc_sid, uid);
	}
#endif
	if (!slurm_get_peer_addr(msg->conn_fd, &resp_addr)) {
		job_desc_msg->resp_host = xmalloc(16);
		slurm_get_ip_str(&resp_addr, &port,
				 job_desc_msg->resp_host, 16);
		dump_job_desc(job_desc_msg);
		if (error_code == SLURM_SUCCESS) {
			do_unlock = true;
			_throttle_start(&active_rpc_cnt);
			lock_slurmctld(job_write_lock);

			error_code = job_allocate(job_desc_msg, immediate,
						  false, NULL,
						  true, uid, &job_ptr,
						  &err_msg);
			/* unlock after finished using the job structure data */
			END_TIMER2("_slurm_rpc_allocate_resources");
		}
	} else if (errno)
		error_code = errno;
	else
		error_code = SLURM_ERROR;

	/* return result */
	if ((error_code == ESLURM_REQUESTED_PART_CONFIG_UNAVAILABLE) ||
	    (error_code == ESLURM_RESERVATION_NOT_USABLE) ||
	    (error_code == ESLURM_QOS_THRES) ||
	    (error_code == ESLURM_NODE_NOT_AVAIL) ||
	    (error_code == ESLURM_JOB_HELD))
		job_waiting = true;

	if ((error_code == SLURM_SUCCESS) ||
	    ((immediate == 0) && job_waiting)) {
		xassert(job_ptr);
		info("sched: _slurm_rpc_allocate_resources JobId=%u "
		     "NodeList=%s %s",job_ptr->job_id,
		     job_ptr->nodes, TIME_STR);

		/* send job_ID and node_name_ptr */
		if (job_ptr->job_resrcs && job_ptr->job_resrcs->cpu_array_cnt) {
			alloc_msg.num_cpu_groups = job_ptr->job_resrcs->
				cpu_array_cnt;
			alloc_msg.cpu_count_reps = xmalloc(sizeof(uint32_t) *
							   job_ptr->job_resrcs->
							   cpu_array_cnt);
			memcpy(alloc_msg.cpu_count_reps,
			       job_ptr->job_resrcs->cpu_array_reps,
			       (sizeof(uint32_t) * job_ptr->job_resrcs->
				cpu_array_cnt));
			alloc_msg.cpus_per_node  = xmalloc(sizeof(uint16_t) *
							   job_ptr->job_resrcs->
							   cpu_array_cnt);
			memcpy(alloc_msg.cpus_per_node,
			       job_ptr->job_resrcs->cpu_array_value,
			       (sizeof(uint16_t) * job_ptr->job_resrcs->
				cpu_array_cnt));
		} else {
			alloc_msg.num_cpu_groups = 0;
			alloc_msg.cpu_count_reps = NULL;
			alloc_msg.cpus_per_node  = NULL;
		}
		alloc_msg.error_code     = error_code;
		alloc_msg.job_id         = job_ptr->job_id;
		alloc_msg.node_cnt       = job_ptr->node_cnt;
		alloc_msg.node_list      = xstrdup(job_ptr->nodes);
		alloc_msg.partition      = xstrdup(job_ptr->partition);
		alloc_msg.alias_list     = xstrdup(job_ptr->alias_list);
		alloc_msg.select_jobinfo =
			select_g_select_jobinfo_copy(job_ptr->select_jobinfo);
		if (job_ptr->details) {
			alloc_msg.pn_min_memory = job_ptr->details->
						  pn_min_memory;
		} else {
			alloc_msg.pn_min_memory = 0;
		}
		unlock_slurmctld(job_write_lock);
		_throttle_fini(&active_rpc_cnt);

		slurm_msg_t_init(&response_msg);
		response_msg.flags = msg->flags;
		response_msg.msg_type = RESPONSE_RESOURCE_ALLOCATION;
		response_msg.data = &alloc_msg;

		if (slurm_send_node_msg(msg->conn_fd, &response_msg) < 0)
			_kill_job_on_msg_fail(job_ptr->job_id);
		xfree(alloc_msg.cpu_count_reps);
		xfree(alloc_msg.cpus_per_node);
		xfree(alloc_msg.node_list);
		xfree(alloc_msg.partition);
		xfree(alloc_msg.alias_list);
		select_g_select_jobinfo_free(alloc_msg.select_jobinfo);
		schedule_job_save();	/* has own locks */
		schedule_node_save();	/* has own locks */
	} else {	/* allocate error */
		if (do_unlock) {
			unlock_slurmctld(job_write_lock);
			_throttle_fini(&active_rpc_cnt);
		}
		info("_slurm_rpc_allocate_resources: %s ",
		     slurm_strerror(error_code));
		if (err_msg)
			slurm_send_rc_err_msg(msg, error_code, err_msg);
		else
			slurm_send_rc_msg(msg, error_code);
	}
	xfree(err_msg);
}

/* _slurm_rpc_dump_conf - process RPC for Slurm configuration information */
static void _slurm_rpc_dump_conf(slurm_msg_t * msg)
{
	DEF_TIMERS;
	slurm_msg_t response_msg;
	last_update_msg_t *last_time_msg = (last_update_msg_t *) msg->data;
	slurm_ctl_conf_info_msg_t config_tbl;
	/* Locks: Read config, partition*/
	slurmctld_lock_t config_read_lock = {
		READ_LOCK, NO_LOCK, NO_LOCK, READ_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);

	START_TIMER;
	debug2("Processing RPC: REQUEST_BUILD_INFO from uid=%d",
	       uid);
	lock_slurmctld(config_read_lock);

	/* check to see if configuration data has changed */
	if ((last_time_msg->last_update - 1) >= slurmctld_conf.last_update) {
		unlock_slurmctld(config_read_lock);
		debug2("_slurm_rpc_dump_conf, no change");
		slurm_send_rc_msg(msg, SLURM_NO_CHANGE_IN_DATA);
	} else {
		_fill_ctld_conf(&config_tbl);
		unlock_slurmctld(config_read_lock);
		END_TIMER2("_slurm_rpc_dump_conf");

		/* init response_msg structure */
		slurm_msg_t_init(&response_msg);
		response_msg.flags = msg->flags;
		response_msg.protocol_version = msg->protocol_version;
		response_msg.address = msg->address;
		response_msg.msg_type = RESPONSE_BUILD_INFO;
		response_msg.data = &config_tbl;

		/* send message */
		slurm_send_node_msg(msg->conn_fd, &response_msg);
		free_slurm_conf(&config_tbl, false);
	}
}

/* _slurm_rpc_dump_jobs - process RPC for job state information */
static void _slurm_rpc_dump_jobs(slurm_msg_t * msg)
{
	DEF_TIMERS;
	char *dump;
	int dump_size;
	slurm_msg_t response_msg;
	job_info_request_msg_t *job_info_request_msg =
		(job_info_request_msg_t *) msg->data;
	/* Locks: Read config job, write node (for hiding) */
	slurmctld_lock_t job_read_lock = {
		READ_LOCK, READ_LOCK, NO_LOCK, WRITE_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);

	START_TIMER;
	debug3("Processing RPC: REQUEST_JOB_INFO from uid=%d", uid);
	lock_slurmctld(job_read_lock);

	if ((job_info_request_msg->last_update - 1) >= last_job_update) {
		unlock_slurmctld(job_read_lock);
		debug3("_slurm_rpc_dump_jobs, no change");
		slurm_send_rc_msg(msg, SLURM_NO_CHANGE_IN_DATA);
	} else {
		pack_all_jobs(&dump, &dump_size,
			      job_info_request_msg->show_flags,
			      g_slurm_auth_get_uid(msg->auth_cred, NULL),
			      NO_VAL, msg->protocol_version);
		unlock_slurmctld(job_read_lock);
		END_TIMER2("_slurm_rpc_dump_jobs");
#if 0
		info("_slurm_rpc_dump_jobs, size=%d %s", dump_size, TIME_STR);
#endif

		/* init response_msg structure */
		slurm_msg_t_init(&response_msg);
		response_msg.flags = msg->flags;
		response_msg.protocol_version = msg->protocol_version;
		response_msg.address = msg->address;
		response_msg.msg_type = RESPONSE_JOB_INFO;
		response_msg.data = dump;
		response_msg.data_size = dump_size;

		/* send message */
		slurm_send_node_msg(msg->conn_fd, &response_msg);
		xfree(dump);
	}
}

/* _slurm_rpc_dump_jobs - process RPC for job state information */
static void _slurm_rpc_dump_jobs_user(slurm_msg_t * msg)
{
	DEF_TIMERS;
	char *dump;
	int dump_size;
	slurm_msg_t response_msg;
	job_user_id_msg_t *job_info_request_msg =
		(job_user_id_msg_t *) msg->data;
	/* Locks: Read config job, write node (for hiding) */
	slurmctld_lock_t job_read_lock = {
		READ_LOCK, READ_LOCK, NO_LOCK, WRITE_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);

	START_TIMER;
	debug3("Processing RPC: REQUEST_JOB_USER_INFO from uid=%d", uid);
	lock_slurmctld(job_read_lock);
	pack_all_jobs(&dump, &dump_size,
		      job_info_request_msg->show_flags,
		      g_slurm_auth_get_uid(msg->auth_cred, NULL),
		      job_info_request_msg->user_id, msg->protocol_version);
	unlock_slurmctld(job_read_lock);
	END_TIMER2("_slurm_rpc_dump_job_user");
#if 0
	info("_slurm_rpc_dump_user_jobs, size=%d %s", dump_size, TIME_STR);
#endif

	/* init response_msg structure */
	slurm_msg_t_init(&response_msg);
	response_msg.flags = msg->flags;
	response_msg.protocol_version = msg->protocol_version;
	response_msg.address = msg->address;
	response_msg.msg_type = RESPONSE_JOB_INFO;
	response_msg.data = dump;
	response_msg.data_size = dump_size;

	/* send message */
	slurm_send_node_msg(msg->conn_fd, &response_msg);
	xfree(dump);
}

/* _slurm_rpc_dump_job_single - process RPC for one job's state information */
static void _slurm_rpc_dump_job_single(slurm_msg_t * msg)
{
	DEF_TIMERS;
	char *dump = NULL;
	int dump_size, rc;
	slurm_msg_t response_msg;
	job_id_msg_t *job_id_msg = (job_id_msg_t *) msg->data;
	/* Locks: Read config, job, and node info */
	slurmctld_lock_t job_read_lock = {
		READ_LOCK, READ_LOCK, NO_LOCK, READ_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);

	START_TIMER;
	debug3("Processing RPC: REQUEST_JOB_INFO_SINGLE from uid=%d", uid);
	lock_slurmctld(job_read_lock);

	rc = pack_one_job(&dump, &dump_size, job_id_msg->job_id,
			  job_id_msg->show_flags,
			  g_slurm_auth_get_uid(msg->auth_cred, NULL),
			  msg->protocol_version);
	unlock_slurmctld(job_read_lock);
	END_TIMER2("_slurm_rpc_dump_job_single");
#if 0
	info("_slurm_rpc_dump_job_single, size=%d %s", dump_size, TIME_STR);
#endif

	/* init response_msg structure */
	if (rc != SLURM_SUCCESS) {
		slurm_send_rc_msg(msg, rc);
	} else {
		slurm_msg_t_init(&response_msg);
		response_msg.flags = msg->flags;
		response_msg.protocol_version = msg->protocol_version;
		response_msg.address = msg->address;
		response_msg.msg_type = RESPONSE_JOB_INFO;
		response_msg.data = dump;
		response_msg.data_size = dump_size;
		slurm_send_node_msg(msg->conn_fd, &response_msg);
	}
	xfree(dump);
}

static void  _slurm_rpc_get_shares(slurm_msg_t *msg)
{
	DEF_TIMERS;
	shares_request_msg_t *req_msg = (shares_request_msg_t *) msg->data;
	shares_response_msg_t resp_msg;
	slurm_msg_t response_msg;

	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);

	START_TIMER;
	debug2("Processing RPC: REQUEST_SHARE_INFO from uid=%d", uid);
	resp_msg.assoc_shares_list = assoc_mgr_get_shares(acct_db_conn,
							  uid,
							  req_msg->acct_list,
							  req_msg->user_list);
	slurm_msg_t_init(&response_msg);
	response_msg.flags = msg->flags;
	response_msg.protocol_version = msg->protocol_version;
	response_msg.address  = msg->address;
	response_msg.msg_type = RESPONSE_SHARE_INFO;
	response_msg.data     = &resp_msg;
	slurm_send_node_msg(msg->conn_fd, &response_msg);
	if (resp_msg.assoc_shares_list)
		list_destroy(resp_msg.assoc_shares_list);
	END_TIMER2("_slurm_rpc_get_share");
	debug2("_slurm_rpc_get_shares %s", TIME_STR);
}

static void  _slurm_rpc_get_priority_factors(slurm_msg_t *msg)
{
	DEF_TIMERS;
	priority_factors_request_msg_t *req_msg =
		(priority_factors_request_msg_t *) msg->data;
	priority_factors_response_msg_t resp_msg;
	slurm_msg_t response_msg;

	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);

	START_TIMER;
	debug2("Processing RPC: REQUEST_PRIORITY_FACTORS from uid=%d", uid);
	resp_msg.priority_factors_list = priority_g_get_priority_factors_list(
		req_msg, uid);
	slurm_msg_t_init(&response_msg);
	response_msg.flags = msg->flags;
	response_msg.protocol_version = msg->protocol_version;
	response_msg.address  = msg->address;
	response_msg.msg_type = RESPONSE_PRIORITY_FACTORS;
	response_msg.data     = &resp_msg;
	slurm_send_node_msg(msg->conn_fd, &response_msg);
	if (resp_msg.priority_factors_list)
		list_destroy(resp_msg.priority_factors_list);
	END_TIMER2("_slurm_rpc_get_priority_factors");
	debug2("_slurm_rpc_get_priority_factors %s", TIME_STR);
}

/* _slurm_rpc_end_time - Process RPC for job end time */
static void _slurm_rpc_end_time(slurm_msg_t * msg)
{
	DEF_TIMERS;
	job_alloc_info_msg_t *time_req_msg =
		(job_alloc_info_msg_t *) msg->data;
	srun_timeout_msg_t timeout_msg;
	slurm_msg_t response_msg;
	int rc;
	/* Locks: Read job */
	slurmctld_lock_t job_read_lock = {
		NO_LOCK, READ_LOCK, NO_LOCK, NO_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);

	START_TIMER;
	debug2("Processing RPC: REQUEST_JOB_END_TIME from uid=%d", uid);
	lock_slurmctld(job_read_lock);
	rc = job_end_time(time_req_msg, &timeout_msg);
	unlock_slurmctld(job_read_lock);
	END_TIMER2("_slurm_rpc_end_time");

	if (rc != SLURM_SUCCESS) {
		slurm_send_rc_msg(msg, rc);
	} else {
		slurm_msg_t_init(&response_msg);
		response_msg.flags = msg->flags;
		response_msg.protocol_version = msg->protocol_version;
		response_msg.address  = msg->address;
		response_msg.msg_type = SRUN_TIMEOUT;
		response_msg.data     = &timeout_msg;
		slurm_send_node_msg(msg->conn_fd, &response_msg);
	}
	debug2("_slurm_rpc_end_time jobid=%u %s",
	       time_req_msg->job_id, TIME_STR);
}

/* _slurm_rpc_dump_front_end - process RPC for front_end state information */
static void _slurm_rpc_dump_front_end(slurm_msg_t * msg)
{
	DEF_TIMERS;
	char *dump = NULL;
	int dump_size = 0;
	slurm_msg_t response_msg;
	front_end_info_request_msg_t *front_end_req_msg =
		(front_end_info_request_msg_t *) msg->data;
	/* Locks: Read config, read node */
	slurmctld_lock_t node_read_lock = {
		READ_LOCK, NO_LOCK, NO_LOCK, READ_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);

	START_TIMER;
	debug3("Processing RPC: REQUEST_FRONT_END_INFO from uid=%d", uid);
	lock_slurmctld(node_read_lock);

	if ((front_end_req_msg->last_update - 1) >= last_front_end_update) {
		unlock_slurmctld(node_read_lock);
		debug3("_slurm_rpc_dump_front_end, no change");
		slurm_send_rc_msg(msg, SLURM_NO_CHANGE_IN_DATA);
	} else {
		pack_all_front_end(&dump, &dump_size, uid,
				   msg->protocol_version);
		unlock_slurmctld(node_read_lock);
		END_TIMER2("_slurm_rpc_dump_front_end");
		debug2("_slurm_rpc_dump_front_end, size=%d %s",
		       dump_size, TIME_STR);

		/* init response_msg structure */
		slurm_msg_t_init(&response_msg);
		response_msg.flags = msg->flags;
		response_msg.protocol_version = msg->protocol_version;
		response_msg.address = msg->address;
		response_msg.msg_type = RESPONSE_FRONT_END_INFO;
		response_msg.data = dump;
		response_msg.data_size = dump_size;

		/* send message */
		slurm_send_node_msg(msg->conn_fd, &response_msg);
		xfree(dump);
	}
}

/* _slurm_rpc_dump_nodes - dump RPC for node state information */
static void _slurm_rpc_dump_nodes(slurm_msg_t * msg)
{
	DEF_TIMERS;
	char *dump;
	int dump_size;
	slurm_msg_t response_msg;
	node_info_request_msg_t *node_req_msg =
		(node_info_request_msg_t *) msg->data;
	/* Locks: Read config, write node (reset allocated CPU count in some
	 * select plugins) */
	slurmctld_lock_t node_write_lock = {
		READ_LOCK, NO_LOCK, WRITE_LOCK, NO_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);

	START_TIMER;
	debug3("Processing RPC: REQUEST_NODE_INFO from uid=%d", uid);
	lock_slurmctld(node_write_lock);

	if ((slurmctld_conf.private_data & PRIVATE_DATA_NODES) &&
	    (!validate_operator(uid))) {
		unlock_slurmctld(node_write_lock);
		error("Security violation, REQUEST_NODE_INFO RPC from uid=%d",
		      uid);
		slurm_send_rc_msg(msg, ESLURM_ACCESS_DENIED);
		return;
	}

	select_g_select_nodeinfo_set_all();

	if ((node_req_msg->last_update - 1) >= last_node_update) {
		unlock_slurmctld(node_write_lock);
		debug3("_slurm_rpc_dump_nodes, no change");
		slurm_send_rc_msg(msg, SLURM_NO_CHANGE_IN_DATA);
	} else {
		pack_all_node(&dump, &dump_size, node_req_msg->show_flags,
			      uid, msg->protocol_version);
		unlock_slurmctld(node_write_lock);
		END_TIMER2("_slurm_rpc_dump_nodes");
#if 0
		info("_slurm_rpc_dump_nodes, size=%d %s", dump_size, TIME_STR);
#endif

		/* init response_msg structure */
		slurm_msg_t_init(&response_msg);
		response_msg.flags = msg->flags;
		response_msg.protocol_version = msg->protocol_version;
		response_msg.address = msg->address;
		response_msg.msg_type = RESPONSE_NODE_INFO;
		response_msg.data = dump;
		response_msg.data_size = dump_size;

		/* send message */
		slurm_send_node_msg(msg->conn_fd, &response_msg);
		xfree(dump);
	}
}

/* _slurm_rpc_dump_node_single - done RPC state information for one node */
static void _slurm_rpc_dump_node_single(slurm_msg_t * msg)
{
	DEF_TIMERS;
	char *dump;
	int dump_size;
	slurm_msg_t response_msg;
	node_info_single_msg_t *node_req_msg =
		(node_info_single_msg_t *) msg->data;
	/* Locks: Read config, read node */
	slurmctld_lock_t node_read_lock = {
		READ_LOCK, NO_LOCK, READ_LOCK, NO_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);

	START_TIMER;
	debug3("Processing RPC: REQUEST_NODE_INFO_SINGLE from uid=%d", uid);
	lock_slurmctld(node_read_lock);

	if ((slurmctld_conf.private_data & PRIVATE_DATA_NODES) &&
	    (!validate_operator(uid))) {
		unlock_slurmctld(node_read_lock);
		error("Security violation, REQUEST_NODE_INFO_SINGLE RPC from "
		      "uid=%d", uid);
		slurm_send_rc_msg(msg, ESLURM_ACCESS_DENIED);
		return;
	}

#if 0
	/* This function updates each node's alloc_cpus count and too slow for
	 * our use here. Node write lock is needed if this function is used */
	select_g_select_nodeinfo_set_all();
#endif
	pack_one_node(&dump, &dump_size, node_req_msg->show_flags,
		      uid, node_req_msg->node_name, msg->protocol_version);
	unlock_slurmctld(node_read_lock);
	END_TIMER2("_slurm_rpc_dump_node_single");
#if 0
	info("_slurm_rpc_dump_node_single, name=%s size=%d %s",
	     node_req_msg->node_name, dump_size, TIME_STR);
#endif

	/* init response_msg structure */
	slurm_msg_t_init(&response_msg);
	response_msg.flags = msg->flags;
	response_msg.protocol_version = msg->protocol_version;
	response_msg.address = msg->address;
	response_msg.msg_type = RESPONSE_NODE_INFO;
	response_msg.data = dump;
	response_msg.data_size = dump_size;

	/* send message */
	slurm_send_node_msg(msg->conn_fd, &response_msg);
	xfree(dump);
}

/* _slurm_rpc_dump_partitions - process RPC for partition state information */
static void _slurm_rpc_dump_partitions(slurm_msg_t * msg)
{
	DEF_TIMERS;
	char *dump;
	int dump_size;
	slurm_msg_t response_msg;
	part_info_request_msg_t  *part_req_msg;

	/* Locks: Read configuration and partition */
	slurmctld_lock_t part_read_lock = {
		READ_LOCK, NO_LOCK, NO_LOCK, READ_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);

	START_TIMER;
	debug2("Processing RPC: REQUEST_PARTITION_INFO uid=%d", uid);
	part_req_msg = (part_info_request_msg_t  *) msg->data;
	lock_slurmctld(part_read_lock);

	if ((slurmctld_conf.private_data & PRIVATE_DATA_PARTITIONS) &&
	    !validate_operator(uid)) {
		unlock_slurmctld(part_read_lock);
		debug2("Security violation, PARTITION_INFO RPC from uid=%d",
		       uid);
		slurm_send_rc_msg(msg, ESLURM_ACCESS_DENIED);
	} else if ((part_req_msg->last_update - 1) >= last_part_update) {
		unlock_slurmctld(part_read_lock);
		debug2("_slurm_rpc_dump_partitions, no change");
		slurm_send_rc_msg(msg, SLURM_NO_CHANGE_IN_DATA);
	} else {
		pack_all_part(&dump, &dump_size, part_req_msg->show_flags,
			      uid, msg->protocol_version);
		unlock_slurmctld(part_read_lock);
		END_TIMER2("_slurm_rpc_dump_partitions");
		debug2("_slurm_rpc_dump_partitions, size=%d %s",
		       dump_size, TIME_STR);

		/* init response_msg structure */
		slurm_msg_t_init(&response_msg);
		response_msg.flags = msg->flags;
		response_msg.protocol_version = msg->protocol_version;
		response_msg.address = msg->address;
		response_msg.msg_type = RESPONSE_PARTITION_INFO;
		response_msg.data = dump;
		response_msg.data_size = dump_size;

		/* send message */
		slurm_send_node_msg(msg->conn_fd, &response_msg);
		xfree(dump);
	}
}

/* _slurm_rpc_epilog_complete - process RPC noting the completion of
 * the epilog denoting the completion of a job it its entirety */
static void  _slurm_rpc_epilog_complete(slurm_msg_t * msg)
{
	static time_t config_update = 0;
	static bool defer_sched = false;
	DEF_TIMERS;
	/* Locks: Read configuration, write job, write node */
	slurmctld_lock_t job_write_lock = {
		READ_LOCK, WRITE_LOCK, WRITE_LOCK, NO_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);
	epilog_complete_msg_t *epilog_msg =
		(epilog_complete_msg_t *) msg->data;
	bool run_scheduler = false;
	struct job_record  *job_ptr;
	char jbuf[JBUFSIZ];

	START_TIMER;
	debug2("Processing RPC: MESSAGE_EPILOG_COMPLETE uid=%d", uid);
	if (!validate_slurm_user(uid)) {
		error("Security violation, EPILOG_COMPLETE RPC from uid=%d",
		      uid);
		return;
	}

	job_ptr = find_job_record(epilog_msg->job_id);
	if (config_update != slurmctld_conf.last_update) {
		char *sched_params = slurm_get_sched_params();
		defer_sched = (sched_params && strstr(sched_params,"defer"));
		xfree(sched_params);
	}

	lock_slurmctld(job_write_lock);
	if (job_epilog_complete(epilog_msg->job_id, epilog_msg->node_name,
				epilog_msg->return_code))
		run_scheduler = true;
	unlock_slurmctld(job_write_lock);
	END_TIMER2("_slurm_rpc_epilog_complete");

	if (epilog_msg->return_code)
		error("%s: epilog error %s Node=%s Err=%s %s",
		      __func__, jobid2str(job_ptr, jbuf),
		      epilog_msg->node_name,
		      slurm_strerror(epilog_msg->return_code), TIME_STR);
	else
		debug2("%s: %s Node=%s %s",
		       __func__, jobid2str(job_ptr, jbuf),
		       epilog_msg->node_name, TIME_STR);

	/* Functions below provide their own locking */
	if (run_scheduler) {
		/*
		 * In defer mode, avoid triggering the scheduler logic
		 * for every epilog complete message.
		 * As one epilog message is sent from every node of each
		 * job at termination, the number of simultaneous schedule
		 * calls can be very high for large machine or large number
		 * of managed jobs.
		 */
		if (!defer_sched)
			(void) schedule(0);	/* Has own locking */
		schedule_node_save();		/* Has own locking */
		schedule_job_save();		/* Has own locking */
	}

	/* NOTE: RPC has no response */
}

/* _slurm_rpc_job_step_kill - process RPC to cancel an entire job or
 * an individual job step */
static void _slurm_rpc_job_step_kill(slurm_msg_t * msg)
{
	static int active_rpc_cnt = 0;
	int error_code = SLURM_SUCCESS;
	DEF_TIMERS;
	job_step_kill_msg_t *job_step_kill_msg =
		(job_step_kill_msg_t *) msg->data;
	/* Locks: Read config, write job, write node */
	slurmctld_lock_t job_write_lock = {
		READ_LOCK, WRITE_LOCK, WRITE_LOCK, NO_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);
	struct job_record *job_ptr;

	START_TIMER;
	debug2("Processing RPC: REQUEST_CANCEL_JOB_STEP uid=%d", uid);
	_throttle_start(&active_rpc_cnt);
	lock_slurmctld(job_write_lock);
	job_ptr = find_job_record(job_step_kill_msg->job_id);
	trace_job(job_ptr, __func__, "enter");

	/* do RPC call */
	if (job_step_kill_msg->job_step_id == SLURM_BATCH_SCRIPT) {
		/* NOTE: SLURM_BATCH_SCRIPT == NO_VAL */
		error_code = job_signal(job_step_kill_msg->job_id,
					job_step_kill_msg->signal,
					job_step_kill_msg->flags, uid,
					false);
		unlock_slurmctld(job_write_lock);
		_throttle_fini(&active_rpc_cnt);
		END_TIMER2("_slurm_rpc_job_step_kill");

		/* return result */
		if (error_code) {
			info("Signal %u JobId=%u by UID=%u: %s",
			     job_step_kill_msg->signal,
			     job_step_kill_msg->job_id, uid,
			     slurm_strerror(error_code));
			slurm_send_rc_msg(msg, error_code);
		} else {
			if (job_step_kill_msg->signal == SIGKILL) {
				info("%s: Cancel of JobId=%u by UID=%u, %s",
				     __func__, job_step_kill_msg->job_id, uid,
				     TIME_STR);
				slurmctld_diag_stats.jobs_canceled++;
			} else {
				info("%s: Signal %u of JobId=%u by UID=%u, %s",
				     __func__, job_step_kill_msg->signal,
				     job_step_kill_msg->job_id, uid, TIME_STR);
			}
			slurm_send_rc_msg(msg, SLURM_SUCCESS);

			/* Below function provides its own locking */
			schedule_job_save();
		}
	} else {
		error_code = job_step_signal(job_step_kill_msg->job_id,
					     job_step_kill_msg->job_step_id,
					     job_step_kill_msg->signal,
					     uid);
		unlock_slurmctld(job_write_lock);
		_throttle_fini(&active_rpc_cnt);
		END_TIMER2("_slurm_rpc_job_step_kill");

		/* return result */
		if (error_code) {
			info("Signal %u of StepId=%u.%u by UID=%u: %s",
			     job_step_kill_msg->signal,
			     job_step_kill_msg->job_id,
			     job_step_kill_msg->job_step_id, uid,
			     slurm_strerror(error_code));
			slurm_send_rc_msg(msg, error_code);
		} else {
			if (job_step_kill_msg->signal == SIGKILL) {
				info("%s: Cancel of StepId=%u.%u by "
				     "UID=%u %s", __func__,
				     job_step_kill_msg->job_id,
				     job_step_kill_msg->job_step_id, uid,
				     TIME_STR);
			} else {
				info("%s: Signal %u of StepId=%u.%u by UID=%u %s",
				     __func__, job_step_kill_msg->signal,
				     job_step_kill_msg->job_id,
				     job_step_kill_msg->job_step_id, uid,
				     TIME_STR);
			}
			slurm_send_rc_msg(msg, SLURM_SUCCESS);

			/* Below function provides its own locking */
			schedule_job_save();
		}
	}
	trace_job(job_ptr, __func__, "return");
}

/* _slurm_rpc_complete_job_allocation - process RPC to note the
 *	completion of a job allocation */
static void _slurm_rpc_complete_job_allocation(slurm_msg_t * msg)
{
	static int active_rpc_cnt = 0;
	int error_code = SLURM_SUCCESS;
	DEF_TIMERS;
	complete_job_allocation_msg_t *comp_msg =
		(complete_job_allocation_msg_t *) msg->data;
	/* Locks: Write job, write node */
	slurmctld_lock_t job_write_lock = {
		NO_LOCK, WRITE_LOCK, WRITE_LOCK, NO_LOCK
	};
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);
	struct job_record *job_ptr;
	char jbuf[JBUFSIZ];

	/* init */
	START_TIMER;
	debug2("Processing RPC: REQUEST_COMPLETE_JOB_ALLOCATION from "
	       "uid=%u, JobId=%u rc=%d",
	       uid, comp_msg->job_id, comp_msg->job_rc);

	job_ptr = find_job_record(comp_msg->job_id);
	trace_job(job_ptr, __func__, "enter");
	_throttle_start(&active_rpc_cnt);
	lock_slurmctld(job_write_lock);

	/* do RPC call */
	/* Mark job and/or job step complete */
	error_code = job_complete(comp_msg->job_id, uid,
				  false, false, comp_msg->job_rc);
	unlock_slurmctld(job_write_lock);
	_throttle_fini(&active_rpc_cnt);
	END_TIMER2("_slurm_rpc_complete_job_allocation");

	/* return result */
	if (error_code) {
		info("%s: %s error %s ",
		     __func__, jobid2str(job_ptr, jbuf),
		     slurm_strerror(error_code));
		slurm_send_rc_msg(msg, error_code);
	} else {
		debug2("%s: %s %s", __func__,
		       jobid2str(job_ptr, jbuf),
		       TIME_STR);
		slurm_send_rc_msg(msg, SLURM_SUCCESS);
		(void) schedule_job_save();	/* Has own locking */
		(void) schedule_node_save();	/* Has own locking */
	}

	trace_job(job_ptr, __func__, "return");
}

/* _slurm_rpc_complete_prolog - process RPC to note the
 *	completion of a prolog */
static void _slurm_rpc_complete_prolog(slurm_msg_t * msg)
{
	int error_code = SLURM_SUCCESS;
	DEF_TIMERS;
	complete_prolog_msg_t *comp_msg =
		(complete_prolog_msg_t *) msg->data;
	/* Locks: Write job, write node */
	slurmctld_lock_t job_write_lock = {
		NO_LOCK, WRITE_LOCK, NO_LOCK, NO_LOCK
	};

	/* init */
	START_TIMER;
	debug2("Processing RPC: REQUEST_COMPLETE_PROLOG from "
	       "JobId=%u", comp_msg->job_id);

	lock_slurmctld(job_write_lock);
	error_code = prolog_complete(comp_msg->job_id, comp_msg->prolog_rc);
	unlock_slurmctld(job_write_lock);

	END_TIMER2("_slurm_rpc_complete_prolog");

	/* return result */
	if (error_code) {
		info("_slurm_rpc_complete_prolog JobId=%u: %s ",
		     comp_msg->job_id, slurm_strerror(error_code));
		slurm_send_rc_msg(msg, error_code);
	} else {
		debug2("_slurm_rpc_complete_prolog JobId=%u %s",
		       comp_msg->job_id, TIME_STR);
		slurm_send_rc_msg(msg, SLURM_SUCCESS);
	}
}

/* _slurm_rpc_complete_batch - process RPC from slurmstepd to note the
 *	completion of a batch script */
static void _slurm_rpc_complete_batch_script(slurm_msg_t * msg)
{
	static int active_rpc_cnt = 0;
	int error_code = SLURM_SUCCESS, i;
	DEF_TIMERS;
	complete_batch_script_msg_t *comp_msg =
		(complete_batch_script_msg_t *) msg->data;
	/* Locks: Write job, write node */
	slurmctld_lock_t job_write_lock = {
		NO_LOCK, WRITE_LOCK, WRITE_LOCK, NO_LOCK
	};
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);
	bool job_requeue = false;
	bool dump_job = false, dump_node = false, run_sched = false;
	struct job_record *job_ptr = NULL;
	char *msg_title = "node(s)";
	char *nodes = comp_msg->node_name;
#ifdef HAVE_BG
	update_block_msg_t block_desc;
	memset(&block_desc, 0, sizeof(update_block_msg_t));
#endif
	/* init */
	START_TIMER;
	debug2("Processing RPC: REQUEST_COMPLETE_BATCH_SCRIPT from "
	       "uid=%u JobId=%u",
	       uid, comp_msg->job_id);

	if (!validate_slurm_user(uid)) {
		error("A non superuser %u tried to complete batch job %u",
		      uid, comp_msg->job_id);
		/* Only the slurmstepd can complete a batch script */
		END_TIMER2("_slurm_rpc_complete_batch_script");
		return;
	}

	_throttle_start(&active_rpc_cnt);
	lock_slurmctld(job_write_lock);
	job_ptr = find_job_record(comp_msg->job_id);

	if (job_ptr && job_ptr->batch_host && comp_msg->node_name &&
	    strcmp(job_ptr->batch_host, comp_msg->node_name)) {
		/* This can be the result of the slurmd on the batch_host
		 * failing, but the slurmstepd continuing to run. Then the
		 * batch job is requeued and started on a different node.
		 * The end result is one batch complete RPC from each node. */
		error("Batch completion for job %u sent from wrong node "
		      "(%s rather than %s), ignored request",
		      comp_msg->job_id,
		      comp_msg->node_name, job_ptr->batch_host);
		unlock_slurmctld(job_write_lock);
		_throttle_fini(&active_rpc_cnt);
		slurm_send_rc_msg(msg, error_code);
		return;
	}

	/* Send batch step info to accounting, only if the job is
	 * still completing.  If the job was requeued because of node
	 * failure (state == pending) an epilog script might not of
	 * ran so we already finished the last instance of the job so
	 * this would be put on the requeued instance which is
	 * incorrect.
	 * NOTE: Do not use IS_JOB_PENDING since that doesn't take
	 * into account the COMPLETING FLAG which is valid, but not
	 * always set yet when the step exits normally.
	 */
	if (association_based_accounting && job_ptr
	    && (job_ptr->job_state != JOB_PENDING)) {
		struct step_record batch_step;
		memset(&batch_step, 0, sizeof(struct step_record));
		batch_step.job_ptr = job_ptr;
		batch_step.step_id = SLURM_BATCH_SCRIPT;
		batch_step.jobacct = comp_msg->jobacct;
		batch_step.exit_code = comp_msg->job_rc;
#ifdef HAVE_FRONT_END
		nodes = job_ptr->nodes;
#endif
		batch_step.gres = nodes;
		node_name2bitmap(batch_step.gres, false,
				 &batch_step.step_node_bitmap);
		batch_step.requid = -1;
		batch_step.start_time = job_ptr->start_time;
		batch_step.name = "batch";
		batch_step.select_jobinfo = job_ptr->select_jobinfo;

		jobacct_storage_g_step_start(acct_db_conn, &batch_step);
		jobacct_storage_g_step_complete(acct_db_conn, &batch_step);
		FREE_NULL_BITMAP(batch_step.step_node_bitmap);
	}

#ifdef HAVE_FRONT_END
	if (job_ptr && job_ptr->front_end_ptr)
		nodes = job_ptr->front_end_ptr->name;
	msg_title = "front_end";
#endif

	/* do RPC call */
	/* First set node DOWN if fatal error */
	if ((comp_msg->slurm_rc == ESLURM_ALREADY_DONE) ||
	    (comp_msg->slurm_rc == ESLURMD_CREDENTIAL_REVOKED)) {
		/* race condition on job termination, not a real error */
		info("slurmd error running JobId=%u from %s=%s: %s",
		     comp_msg->job_id,
		     msg_title, nodes,
		     slurm_strerror(comp_msg->slurm_rc));
		comp_msg->slurm_rc = SLURM_SUCCESS;
#ifdef HAVE_ALPS_CRAY
	} else if (comp_msg->slurm_rc == ESLURM_RESERVATION_NOT_USABLE) {
		/*
		 * Confirmation of ALPS reservation failed.
		 *
		 * This is non-fatal, it may be a transient error (e.g. ALPS
		 * temporary unavailable). Give job one more chance to run.
		 */
		error("ALPS reservation for JobId %u failed: %s",
			comp_msg->job_id, slurm_strerror(comp_msg->slurm_rc));
		dump_job = job_requeue = true;
#endif
	/* Handle non-fatal errors here. All others drain the node. */
	} else if ((comp_msg->slurm_rc == SLURM_COMMUNICATIONS_SEND_ERROR) ||
		   (comp_msg->slurm_rc == ESLURM_USER_ID_MISSING) ||
		   (comp_msg->slurm_rc == ESLURMD_UID_NOT_FOUND)  ||
		   (comp_msg->slurm_rc == ESLURMD_GID_NOT_FOUND)  ||
		   (comp_msg->slurm_rc == ESLURMD_INVALID_ACCT_FREQ)) {
		error("Slurmd error running JobId=%u on %s=%s: %s",
		      comp_msg->job_id, msg_title, nodes,
		      slurm_strerror(comp_msg->slurm_rc));
	} else if (comp_msg->slurm_rc != SLURM_SUCCESS) {
		error("slurmd error running JobId=%u on %s=%s: %s",
		      comp_msg->job_id,
		      msg_title, nodes,
		      slurm_strerror(comp_msg->slurm_rc));
		slurmctld_diag_stats.jobs_failed++;
		if (error_code == SLURM_SUCCESS) {
#ifdef HAVE_BG
			if (job_ptr) {
				select_g_select_jobinfo_get(
					job_ptr->select_jobinfo,
					SELECT_JOBDATA_BLOCK_ID,
					&block_desc.bg_block_id);
			}
#else
#ifdef HAVE_FRONT_END
			if (job_ptr && job_ptr->front_end_ptr) {
				update_front_end_msg_t update_node_msg;
				memset(&update_node_msg, 0,
				       sizeof(update_front_end_msg_t));
				update_node_msg.name = job_ptr->front_end_ptr->
						       name;
				update_node_msg.node_state = NODE_STATE_DRAIN;
				update_node_msg.reason =
					"batch job complete failure";
				error_code = update_front_end(&update_node_msg);
			}
#else
			error_code = drain_nodes(comp_msg->node_name,
						 "batch job complete failure",
						 getuid());
#endif	/* !HAVE_FRONT_END */
#endif	/* !HAVE_BG */
			if ((comp_msg->job_rc != SLURM_SUCCESS) && job_ptr &&
			    job_ptr->details && job_ptr->details->requeue)
				job_requeue = true;
			dump_job = true;
			dump_node = true;
		}
	}

	/* Mark job allocation complete */
	if (msg->msg_type == REQUEST_COMPLETE_BATCH_JOB)
		job_epilog_complete(comp_msg->job_id, comp_msg->node_name, 0);
	i = job_complete(comp_msg->job_id, uid, job_requeue, false,
			 comp_msg->job_rc);
	error_code = MAX(error_code, i);
	unlock_slurmctld(job_write_lock);
	_throttle_fini(&active_rpc_cnt);

#ifdef HAVE_BG
	if (block_desc.bg_block_id) {
		block_desc.reason = slurm_strerror(comp_msg->slurm_rc);
		block_desc.state = BG_BLOCK_ERROR_FLAG;
		i = select_g_update_block(&block_desc);
		error_code = MAX(error_code, i);
		xfree(block_desc.bg_block_id);
	}
#endif

	/* this has to be done after the job_complete */

	END_TIMER2("_slurm_rpc_complete_batch_script");

	/* return result */
	if (error_code) {
		debug2("_slurm_rpc_complete_batch_script JobId=%u: %s ",
		       comp_msg->job_id,
		       slurm_strerror(error_code));
		slurm_send_rc_msg(msg, error_code);
	} else {
		debug2("_slurm_rpc_complete_batch_script JobId=%u %s",
		       comp_msg->job_id, TIME_STR);
		slurmctld_diag_stats.jobs_completed++;
		dump_job = true;
		if (replace_batch_job(msg, job_ptr))
			run_sched = true;
	}

	if (run_sched)
		(void) schedule(0);		/* Has own locking */
	if (dump_job)
		(void) schedule_job_save();	/* Has own locking */
	if (dump_node)
		(void) schedule_node_save();	/* Has own locking */
}

/* _slurm_rpc_job_step_create - process RPC to create/register a job step
 *	with the step_mgr */
static void _slurm_rpc_job_step_create(slurm_msg_t * msg)
{
	static int active_rpc_cnt = 0;
	int error_code = SLURM_SUCCESS;
	DEF_TIMERS;
	slurm_msg_t resp;
	struct step_record *step_rec;
	job_step_create_response_msg_t job_step_resp;
	job_step_create_request_msg_t *req_step_msg =
		(job_step_create_request_msg_t *) msg->data;
	slurm_cred_t *slurm_cred = (slurm_cred_t *) NULL;
	/* Locks: Write jobs, read nodes */
	slurmctld_lock_t job_write_lock = {
		NO_LOCK, WRITE_LOCK, READ_LOCK, NO_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);

	START_TIMER;
	debug2("Processing RPC: REQUEST_JOB_STEP_CREATE from uid=%d", uid);

	dump_step_desc(req_step_msg);
	if (uid && (uid != req_step_msg->user_id)) {
		error("Security violation, JOB_STEP_CREATE RPC from uid=%d "
		      "to run as uid %u",
		      uid, req_step_msg->user_id);
		slurm_send_rc_msg(msg, ESLURM_USER_ID_MISSING);
		return;
	}

#if defined HAVE_FRONT_END && !defined HAVE_BGQ	&& !defined HAVE_ALPS_CRAY
	/* Limited job step support */
	/* Non-super users not permitted to run job steps on front-end.
	 * A single slurmd can not handle a heavy load. */
	if (!validate_slurm_user(uid)) {
		info("Attempt to execute job step by uid=%d", uid);
		slurm_send_rc_msg(msg, ESLURM_NO_STEPS);
		return;
	}
#endif

	_throttle_start(&active_rpc_cnt);
	lock_slurmctld(job_write_lock);
	error_code = step_create(req_step_msg, &step_rec, false);

	if (error_code == SLURM_SUCCESS) {
		error_code = _make_step_cred(step_rec, &slurm_cred,
					     msg->protocol_version);
		ext_sensors_g_get_stepstartdata(step_rec);
	}
	END_TIMER2("_slurm_rpc_job_step_create");

	/* return result */
	if (error_code) {
		unlock_slurmctld(job_write_lock);
		_throttle_fini(&active_rpc_cnt);
		if ((error_code == ESLURM_PROLOG_RUNNING) ||
		    (error_code == ESLURM_DISABLED)) {	/* job suspended */
			debug("_slurm_rpc_job_step_create for job %u: %s",
			      req_step_msg->job_id, slurm_strerror(error_code));
		} else {
			info("_slurm_rpc_job_step_create for job %u: %s",
			     req_step_msg->job_id, slurm_strerror(error_code));
		}
		slurm_send_rc_msg(msg, error_code);
	} else {
		slurm_step_layout_t *layout = step_rec->step_layout;

		info("sched: _slurm_rpc_job_step_create: StepId=%u.%u %s %s",
		     step_rec->job_ptr->job_id, step_rec->step_id,
		     req_step_msg->node_list, TIME_STR);

		job_step_resp.job_step_id = step_rec->step_id;
		job_step_resp.resv_ports  = step_rec->resv_ports;
		job_step_resp.step_layout = layout;
#ifdef HAVE_FRONT_END
		if (step_rec->job_ptr->batch_host) {
			job_step_resp.step_layout->front_end =
				xstrdup(step_rec->job_ptr->batch_host);
		}
#endif
		job_step_resp.cred           = slurm_cred;
		job_step_resp.select_jobinfo = step_rec->select_jobinfo;
		job_step_resp.switch_job     = step_rec->switch_job;

		unlock_slurmctld(job_write_lock);
		_throttle_fini(&active_rpc_cnt);
		slurm_msg_t_init(&resp);
		resp.flags = msg->flags;
		resp.protocol_version = msg->protocol_version;
		resp.address = msg->address;
		resp.msg_type = RESPONSE_JOB_STEP_CREATE;
		resp.data = &job_step_resp;

		slurm_send_node_msg(msg->conn_fd, &resp);
		slurm_cred_destroy(slurm_cred);
		schedule_job_save();	/* Sets own locks */
	}
}

/* _slurm_rpc_job_step_get_info - process request for job step info */
static void _slurm_rpc_job_step_get_info(slurm_msg_t * msg)
{
	DEF_TIMERS;
	void *resp_buffer = NULL;
	int resp_buffer_size = 0;
	int error_code = SLURM_SUCCESS;
	job_step_info_request_msg_t *request =
		(job_step_info_request_msg_t *) msg->data;
	/* Locks: Read config, job, write partition (for filtering) */
	slurmctld_lock_t job_read_lock = {
		READ_LOCK, READ_LOCK, NO_LOCK, WRITE_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);

	START_TIMER;
	debug3("Processing RPC: REQUEST_JOB_STEP_INFO from uid=%d", uid);

	lock_slurmctld(job_read_lock);

	if ((request->last_update - 1) >= last_job_update) {
		unlock_slurmctld(job_read_lock);
		debug3("_slurm_rpc_job_step_get_info, no change");
		error_code = SLURM_NO_CHANGE_IN_DATA;
	} else {
		Buf buffer = init_buf(BUF_SIZE);
		error_code = pack_ctld_job_step_info_response_msg(
			request->job_id, request->step_id,
			uid, request->show_flags, buffer,
			msg->protocol_version);
		unlock_slurmctld(job_read_lock);
		END_TIMER2("_slurm_rpc_job_step_get_info");
		if (error_code) {
			/* job_id:step_id not found or otherwise *\
			   \* error message is printed elsewhere    */
			debug3("_slurm_rpc_job_step_get_info: %s",
			       slurm_strerror(error_code));
			free_buf(buffer);
		} else {
			resp_buffer_size = get_buf_offset(buffer);
			resp_buffer = xfer_buf_data(buffer);
			debug3("_slurm_rpc_job_step_get_info size=%d %s",
			       resp_buffer_size, TIME_STR);
		}
	}

	if (error_code)
		slurm_send_rc_msg(msg, error_code);
	else {
		slurm_msg_t response_msg;

		slurm_msg_t_init(&response_msg);
		response_msg.flags = msg->flags;
		response_msg.protocol_version = msg->protocol_version;
		response_msg.address = msg->address;
		response_msg.msg_type = RESPONSE_JOB_STEP_INFO;
		response_msg.data = resp_buffer;
		response_msg.data_size = resp_buffer_size;
		slurm_send_node_msg(msg->conn_fd, &response_msg);
		xfree(resp_buffer);
	}
}

static bool _is_valid_will_run_user(job_desc_msg_t *job_desc_msg, uid_t uid)
{
	char *account = NULL;

	if ((uid == job_desc_msg->user_id) || validate_operator(uid))
		return true;

	if (job_desc_msg->job_id != NO_VAL) {
		struct job_record *job_ptr;
		job_ptr = find_job_record(job_desc_msg->job_id);
		if (job_ptr)
			account = job_ptr->account;
	} else if (job_desc_msg->account)
		account = job_desc_msg->account;

	if (account && assoc_mgr_is_user_acct_coord(acct_db_conn, uid, account))
		return true;

	return false;
}

/* _slurm_rpc_job_will_run - process RPC to determine if job with given
 *	configuration can be initiated */
static void _slurm_rpc_job_will_run(slurm_msg_t * msg)
{
	/* init */
	DEF_TIMERS;
	int error_code = SLURM_SUCCESS;
	struct job_record *job_ptr = NULL;
	job_desc_msg_t *job_desc_msg = (job_desc_msg_t *) msg->data;
	/* Locks: Read config, read job, read node, read partition */
	slurmctld_lock_t job_read_lock = {
		READ_LOCK, READ_LOCK, READ_LOCK, READ_LOCK };
	/* Locks: Write job, read node, read partition */
	slurmctld_lock_t job_write_lock = {
		NO_LOCK, WRITE_LOCK, READ_LOCK, READ_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);
	uint16_t port;	/* dummy value */
	slurm_addr_t resp_addr;
	will_run_response_msg_t *resp = NULL;
	char *err_msg = NULL;

	START_TIMER;
	debug2("Processing RPC: REQUEST_JOB_WILL_RUN from uid=%d", uid);

	/* do RPC call */
	if (!_is_valid_will_run_user(job_desc_msg, uid)) {
		error_code = ESLURM_USER_ID_MISSING;
		error("Security violation, JOB_WILL_RUN RPC from uid=%d", uid);
	}
	if ((job_desc_msg->alloc_node == NULL)
	    ||  (job_desc_msg->alloc_node[0] == '\0')) {
		error_code = ESLURM_INVALID_NODE_NAME;
		error("REQUEST_JOB_WILL_RUN lacks alloc_node from uid=%d", uid);
	}

	if (error_code == SLURM_SUCCESS) {
		/* Locks are for job_submit plugin use */
		lock_slurmctld(job_read_lock);
		error_code = validate_job_create_req(job_desc_msg,uid,&err_msg);
		unlock_slurmctld(job_read_lock);
	}

	if (!slurm_get_peer_addr(msg->conn_fd, &resp_addr)) {
		job_desc_msg->resp_host = xmalloc(16);
		slurm_get_ip_str(&resp_addr, &port,
				 job_desc_msg->resp_host, 16);
		dump_job_desc(job_desc_msg);
		if (error_code == SLURM_SUCCESS) {
			lock_slurmctld(job_write_lock);
			if (job_desc_msg->job_id == NO_VAL) {
				error_code = job_allocate(job_desc_msg, false,
							  true, &resp,
							  true, uid, &job_ptr,
							  &err_msg);
			} else {	/* existing job test */
				error_code = job_start_data(job_desc_msg,
							    &resp);
			}
			unlock_slurmctld(job_write_lock);
			END_TIMER2("_slurm_rpc_job_will_run");
		}
	} else if (errno)
		error_code = errno;
	else
		error_code = SLURM_ERROR;

	/* return result */
	if (error_code) {
		debug2("_slurm_rpc_job_will_run: %s",
		       slurm_strerror(error_code));
		if (err_msg)
			slurm_send_rc_err_msg(msg, error_code, err_msg);
		else
			slurm_send_rc_msg(msg, error_code);
	} else if (resp) {
		slurm_msg_t response_msg;
		/* init response_msg structure */
		slurm_msg_t_init(&response_msg);
		response_msg.flags = msg->flags;
		response_msg.protocol_version = msg->protocol_version;
		response_msg.address = msg->address;
		response_msg.msg_type = RESPONSE_JOB_WILL_RUN;
		response_msg.data = resp;
		slurm_send_node_msg(msg->conn_fd, &response_msg);
		slurm_free_will_run_response_msg(resp);
		debug2("_slurm_rpc_job_will_run success %s", TIME_STR);
	} else {
		debug2("_slurm_rpc_job_will_run success %s", TIME_STR);
		if (job_desc_msg->job_id == NO_VAL)
			slurm_send_rc_msg(msg, SLURM_SUCCESS);
	}
	xfree(err_msg);
}

/* _slurm_rpc_node_registration - process RPC to determine if a node's
 *	actual configuration satisfies the configured specification */
static void _slurm_rpc_node_registration(slurm_msg_t * msg)
{
	/* init */
	DEF_TIMERS;
	int error_code = SLURM_SUCCESS;
	bool newly_up = false;
	slurm_node_registration_status_msg_t *node_reg_stat_msg =
		(slurm_node_registration_status_msg_t *) msg->data;
	/* Locks: Read config, write job, write node */
	slurmctld_lock_t job_write_lock = {
		READ_LOCK, WRITE_LOCK, WRITE_LOCK, NO_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);

	START_TIMER;
	debug2("Processing RPC: MESSAGE_NODE_REGISTRATION_STATUS from uid=%d",
	       uid);
	if (!validate_slurm_user(uid)) {
		error_code = ESLURM_USER_ID_MISSING;
		error("Security violation, NODE_REGISTER RPC from uid=%d", uid);
	}

	if (msg->protocol_version != SLURM_PROTOCOL_VERSION)
		info("Node %s appears to have a different version "
		     "of Slurm than ours.  Please update at your earliest "
		     "convenience.", node_reg_stat_msg->node_name);

	if (error_code == SLURM_SUCCESS) {
		/* do RPC call */
		if (!(slurmctld_conf.debug_flags & DEBUG_FLAG_NO_CONF_HASH) &&
		    (node_reg_stat_msg->hash_val != NO_VAL) &&
		    (node_reg_stat_msg->hash_val != slurm_get_hash_val())) {
			error("Node %s appears to have a different slurm.conf "
			      "than the slurmctld.  This could cause issues "
			      "with communication and functionality.  "
			      "Please review both files and make sure they "
			      "are the same.  If this is expected ignore, and "
			      "set DebugFlags=NO_CONF_HASH in your slurm.conf.",
			      node_reg_stat_msg->node_name);
		}
		lock_slurmctld(job_write_lock);
#ifdef HAVE_FRONT_END		/* Operates only on front-end */
		error_code = validate_nodes_via_front_end(node_reg_stat_msg,
							  msg->protocol_version,
							  &newly_up);
#else
		validate_jobs_on_node(node_reg_stat_msg);
		error_code = validate_node_specs(node_reg_stat_msg,
						 msg->protocol_version,
						 &newly_up);
#endif
		unlock_slurmctld(job_write_lock);
		END_TIMER2("_slurm_rpc_node_registration");
		if (newly_up) {
			queue_job_scheduler();
		}
	}

	/* return result */
	if (error_code) {
		error("_slurm_rpc_node_registration node=%s: %s",
		      node_reg_stat_msg->node_name,
		      slurm_strerror(error_code));
		slurm_send_rc_msg(msg, error_code);
	} else {
		debug2("_slurm_rpc_node_registration complete for %s %s",
		       node_reg_stat_msg->node_name, TIME_STR);
		slurm_send_rc_msg(msg, SLURM_SUCCESS);
	}
}

/* _slurm_rpc_job_alloc_info - process RPC to get details on existing job */
static void _slurm_rpc_job_alloc_info(slurm_msg_t * msg)
{
	int error_code = SLURM_SUCCESS;
	slurm_msg_t response_msg;
	struct job_record *job_ptr;
	DEF_TIMERS;
	job_alloc_info_msg_t *job_info_msg =
		(job_alloc_info_msg_t *) msg->data;
	job_alloc_info_response_msg_t job_info_resp_msg;
	/* Locks: Read config, job, read node */
	slurmctld_lock_t job_read_lock = {
		READ_LOCK, READ_LOCK, READ_LOCK, NO_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);
	bool do_unlock = false;

	START_TIMER;
	debug2("Processing RPC: REQUEST_JOB_ALLOCATION_INFO from uid=%d", uid);

	/* do RPC call */
	do_unlock = true;
	lock_slurmctld(job_read_lock);
	error_code = job_alloc_info(uid, job_info_msg->job_id, &job_ptr);
	END_TIMER2("_slurm_rpc_job_alloc_info");

	/* return result */
	if (error_code || (job_ptr == NULL) || (job_ptr->job_resrcs == NULL)) {
		if (do_unlock)
			unlock_slurmctld(job_read_lock);
		debug2("_slurm_rpc_job_alloc_info: JobId=%u, uid=%u: %s",
		       job_info_msg->job_id, uid,
		       slurm_strerror(error_code));
		slurm_send_rc_msg(msg, error_code);
	} else {
		info("_slurm_rpc_job_alloc_info JobId=%u NodeList=%s %s",
		     job_info_msg->job_id, job_ptr->nodes, TIME_STR);

		/* send job_ID  and node_name_ptr */
		job_info_resp_msg.num_cpu_groups = job_ptr->job_resrcs->
			cpu_array_cnt;
		job_info_resp_msg.cpu_count_reps =
			xmalloc(sizeof(uint32_t) *
				job_ptr->job_resrcs->cpu_array_cnt);
		memcpy(job_info_resp_msg.cpu_count_reps,
		       job_ptr->job_resrcs->cpu_array_reps,
		       (sizeof(uint32_t) * job_ptr->job_resrcs->cpu_array_cnt));
		job_info_resp_msg.cpus_per_node  =
			xmalloc(sizeof(uint16_t) *
				job_ptr->job_resrcs->cpu_array_cnt);
		memcpy(job_info_resp_msg.cpus_per_node,
		       job_ptr->job_resrcs->cpu_array_value,
		       (sizeof(uint16_t) * job_ptr->job_resrcs->cpu_array_cnt));
		job_info_resp_msg.error_code     = error_code;
		job_info_resp_msg.job_id         = job_info_msg->job_id;
		job_info_resp_msg.node_addr      =
			xmalloc(sizeof(slurm_addr_t) * job_ptr->node_cnt);
		memcpy(job_info_resp_msg.node_addr, job_ptr->node_addr,
		       (sizeof(slurm_addr_t) * job_ptr->node_cnt));
		job_info_resp_msg.node_cnt       = job_ptr->node_cnt;
		job_info_resp_msg.node_list      = xstrdup(job_ptr->nodes);
		job_info_resp_msg.select_jobinfo =
			select_g_select_jobinfo_copy(job_ptr->select_jobinfo);
		unlock_slurmctld(job_read_lock);

		slurm_msg_t_init(&response_msg);
		response_msg.flags = msg->flags;
		response_msg.protocol_version = msg->protocol_version;
		response_msg.msg_type    = RESPONSE_JOB_ALLOCATION_INFO;
		response_msg.data        = &job_info_resp_msg;

		slurm_send_node_msg(msg->conn_fd, &response_msg);
		select_g_select_jobinfo_free(job_info_resp_msg.select_jobinfo);
		xfree(job_info_resp_msg.cpu_count_reps);
		xfree(job_info_resp_msg.cpus_per_node);
		xfree(job_info_resp_msg.node_addr);
		xfree(job_info_resp_msg.node_list);
	}
}

/* _slurm_rpc_job_alloc_info_lite - process RPC to get minor details
   on existing job */
static void _slurm_rpc_job_alloc_info_lite(slurm_msg_t * msg)
{
	int error_code = SLURM_SUCCESS, i, j;
	slurm_msg_t response_msg;
	struct job_record *job_ptr;
	DEF_TIMERS;
	job_alloc_info_msg_t *job_info_msg =
		(job_alloc_info_msg_t *) msg->data;
	resource_allocation_response_msg_t job_info_resp_msg;
	/* Locks: Read config, job, read node */
	slurmctld_lock_t job_read_lock = {
		READ_LOCK, READ_LOCK, READ_LOCK, NO_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);
	bool do_unlock = false;

	START_TIMER;
	debug2("Processing RPC: REQUEST_JOB_ALLOCATION_INFO_LITE from uid=%d",
	       uid);

	/* do RPC call */
	do_unlock = true;
	lock_slurmctld(job_read_lock);
	error_code = job_alloc_info(uid, job_info_msg->job_id, &job_ptr);
	END_TIMER2("_slurm_rpc_job_alloc_info_lite");

	/* return result */
	if (error_code || (job_ptr == NULL) || (job_ptr->job_resrcs == NULL)) {
		if (do_unlock)
			unlock_slurmctld(job_read_lock);
		debug2("_slurm_rpc_job_alloc_info_lite: JobId=%u, uid=%u: %s",
		       job_info_msg->job_id, uid, slurm_strerror(error_code));
		slurm_send_rc_msg(msg, error_code);
	} else {
		debug("_slurm_rpc_job_alloc_info_lite JobId=%u NodeList=%s %s",
		      job_info_msg->job_id, job_ptr->nodes, TIME_STR);

		/* send job_ID and node_name_ptr */
		if (bit_equal(job_ptr->node_bitmap,
			      job_ptr->job_resrcs->node_bitmap)) {
			job_info_resp_msg.num_cpu_groups = job_ptr->job_resrcs->
				cpu_array_cnt;
			job_info_resp_msg.cpu_count_reps =
				xmalloc(sizeof(uint32_t) *
					job_ptr->job_resrcs->
					cpu_array_cnt);
			memcpy(job_info_resp_msg.cpu_count_reps,
			       job_ptr->job_resrcs->cpu_array_reps,
			       (sizeof(uint32_t) *
				job_ptr->job_resrcs->cpu_array_cnt));
			job_info_resp_msg.cpus_per_node  =
				xmalloc(sizeof(uint16_t) *
					job_ptr->job_resrcs->
					cpu_array_cnt);
			memcpy(job_info_resp_msg.cpus_per_node,
			       job_ptr->job_resrcs->cpu_array_value,
			       (sizeof(uint16_t) *
				job_ptr->job_resrcs->cpu_array_cnt));
		} else {
			/* Job has changed size, rebuild CPU count info */
			job_info_resp_msg.num_cpu_groups = job_ptr->node_cnt;
			job_info_resp_msg.cpu_count_reps =
				xmalloc(sizeof(uint32_t) *
					job_ptr->node_cnt);
			job_info_resp_msg.cpus_per_node =
				xmalloc(sizeof(uint32_t) *
					job_ptr->node_cnt);
			for (i=0, j=-1; i<job_ptr->job_resrcs->nhosts; i++) {
				if (job_ptr->job_resrcs->cpus[i] == 0)
					continue;
				if ((j == -1) ||
				    (job_info_resp_msg.cpus_per_node[j] !=
				     job_ptr->job_resrcs->cpus[i])) {
					j++;
					job_info_resp_msg.cpus_per_node[j] =
						job_ptr->job_resrcs->cpus[i];
					job_info_resp_msg.cpu_count_reps[j] = 1;
				} else {
					job_info_resp_msg.cpu_count_reps[j]++;
				}
			}
			job_info_resp_msg.num_cpu_groups = j + 1;
		}
		job_info_resp_msg.alias_list     = xstrdup(job_ptr->alias_list);
		job_info_resp_msg.error_code     = error_code;
		job_info_resp_msg.job_id         = job_info_msg->job_id;
		job_info_resp_msg.node_cnt       = job_ptr->node_cnt;
		job_info_resp_msg.node_list      = xstrdup(job_ptr->nodes);
		job_info_resp_msg.partition      = xstrdup(job_ptr->partition);
		job_info_resp_msg.select_jobinfo =
			select_g_select_jobinfo_copy(job_ptr->select_jobinfo);
		unlock_slurmctld(job_read_lock);

		slurm_msg_t_init(&response_msg);
		response_msg.flags = msg->flags;
		response_msg.protocol_version = msg->protocol_version;
		response_msg.msg_type    = RESPONSE_JOB_ALLOCATION_INFO_LITE;
		response_msg.data        = &job_info_resp_msg;

		slurm_send_node_msg(msg->conn_fd, &response_msg);

		xfree(job_info_resp_msg.cpu_count_reps);
		xfree(job_info_resp_msg.cpus_per_node);
		xfree(job_info_resp_msg.alias_list);
		xfree(job_info_resp_msg.node_list);
		xfree(job_info_resp_msg.partition);
		select_g_select_jobinfo_free(job_info_resp_msg.select_jobinfo);
	}
}

/* _slurm_rpc_job_sbcast_cred - process RPC to get details on existing job
 *	plus sbcast credential */
static void _slurm_rpc_job_sbcast_cred(slurm_msg_t * msg)
{
	int error_code = SLURM_SUCCESS;
	slurm_msg_t response_msg;
	struct job_record *job_ptr;
	DEF_TIMERS;
	job_alloc_info_msg_t *job_info_msg =
		(job_alloc_info_msg_t *) msg->data;
	job_sbcast_cred_msg_t job_info_resp_msg;
	sbcast_cred_t *sbcast_cred;
	/* Locks: Read config, job, read node */
	slurmctld_lock_t job_read_lock = {
		READ_LOCK, READ_LOCK, READ_LOCK, NO_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);

	START_TIMER;
	debug2("Processing RPC: REQUEST_JOB_SBCAST_CRED from uid=%d", uid);

	/* do RPC call */
	lock_slurmctld(job_read_lock);
	error_code = job_alloc_info(uid, job_info_msg->job_id, &job_ptr);
	END_TIMER2("_slurm_rpc_job_alloc_info");

	/* return result */
	if (error_code || (job_ptr == NULL)) {
		unlock_slurmctld(job_read_lock);
		debug2("_slurm_rpc_job_sbcast_cred: JobId=%u, uid=%u: %s",
		       job_info_msg->job_id, uid,
		       slurm_strerror(error_code));
		slurm_send_rc_msg(msg, error_code);
	} else if ((sbcast_cred =
		    create_sbcast_cred(slurmctld_config.cred_ctx,
				       job_ptr->job_id,
				       job_ptr->nodes,
				       job_ptr->end_time)) == NULL){
		unlock_slurmctld(job_read_lock);
		error("_slurm_rpc_job_sbcast_cred JobId=%u cred create error",
		      job_info_msg->job_id);
		slurm_send_rc_msg(msg, SLURM_ERROR);
	} else {
		info("_slurm_rpc_job_sbcast_cred JobId=%u NodeList=%s %s",
		     job_info_msg->job_id, job_ptr->nodes, TIME_STR);

		job_info_resp_msg.job_id         = job_ptr->job_id;
		job_info_resp_msg.node_addr      =
			xmalloc(sizeof(slurm_addr_t) * job_ptr->node_cnt);
		memcpy(job_info_resp_msg.node_addr, job_ptr->node_addr,
		       (sizeof(slurm_addr_t) * job_ptr->node_cnt));
		job_info_resp_msg.node_cnt       = job_ptr->node_cnt;
		job_info_resp_msg.node_list      = xstrdup(job_ptr->nodes);
		job_info_resp_msg.sbcast_cred    = sbcast_cred;
		unlock_slurmctld(job_read_lock);

		slurm_msg_t_init(&response_msg);
		response_msg.flags = msg->flags;
		response_msg.protocol_version = msg->protocol_version;
		response_msg.msg_type    = RESPONSE_JOB_SBCAST_CRED;
		response_msg.data        = &job_info_resp_msg;

		slurm_send_node_msg(msg->conn_fd, &response_msg);
		xfree(job_info_resp_msg.node_addr);
		xfree(job_info_resp_msg.node_list);
		delete_sbcast_cred(sbcast_cred);
	}
}

/* _slurm_rpc_ping - process ping RPC */
static void _slurm_rpc_ping(slurm_msg_t * msg)
{
	/* We could authenticate here, if desired */

	/* return result */
	slurm_send_rc_msg(msg, SLURM_SUCCESS);
}


/* _slurm_rpc_reconfigure_controller - process RPC to re-initialize
 *	slurmctld from configuration file
 * Anything you add to this function must be added to the
 * slurm_reconfigure function inside controller.c try
 * to keep these in sync.
 */
static void _slurm_rpc_reconfigure_controller(slurm_msg_t * msg)
{
	int error_code = SLURM_SUCCESS;
	static bool in_progress = false;
	DEF_TIMERS;
	/* Locks: Write configuration, job, node and partition */
	slurmctld_lock_t config_write_lock = {
		WRITE_LOCK, WRITE_LOCK, WRITE_LOCK, WRITE_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);

	START_TIMER;
	info("Processing RPC: REQUEST_RECONFIGURE from uid=%d", uid);
	if (!validate_super_user(uid)) {
		error("Security violation, RECONFIGURE RPC from uid=%d", uid);
		error_code = ESLURM_USER_ID_MISSING;
	}
	if (in_progress || slurmctld_config.shutdown_time)
		error_code = EINPROGRESS;

	/* do RPC call */
	if (error_code == SLURM_SUCCESS) {
		debug("sched: begin reconfiguration");
		lock_slurmctld(config_write_lock);
		in_progress = true;
		error_code = read_slurm_conf(1, true);
		if (error_code == SLURM_SUCCESS) {
			_update_cred_key();
			set_slurmctld_state_loc();
			msg_to_slurmd(REQUEST_RECONFIGURE);
		}
		in_progress = false;
		slurm_sched_g_partition_change();	/* notify sched plugin */
		unlock_slurmctld(config_write_lock);
		assoc_mgr_set_missing_uids();
		start_power_mgr(&slurmctld_config.thread_id_power);
		trigger_reconfig();
	}
	END_TIMER2("_slurm_rpc_reconfigure_controller");

	/* return result */
	if (error_code) {
		error("_slurm_rpc_reconfigure_controller: %s",
		      slurm_strerror(error_code));
		slurm_send_rc_msg(msg, error_code);
	} else {
		info("_slurm_rpc_reconfigure_controller: completed %s",
		     TIME_STR);
		slurm_send_rc_msg(msg, SLURM_SUCCESS);
		acct_storage_g_reconfig(acct_db_conn, 0);
		priority_g_reconfig(false);	/* notify priority plugin too */
		save_all_state();		/* has its own locks */
		queue_job_scheduler();
	}
}

/* _slurm_rpc_takeover - process takeover RPC */
static void _slurm_rpc_takeover(slurm_msg_t * msg)
{
	int error_code = SLURM_SUCCESS;
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);

	/* We could authenticate here, if desired */
	if (!validate_super_user(uid)) {
		error("Security violation, TAKEOVER RPC from uid=%d", uid);
		error_code = ESLURM_USER_ID_MISSING;
	} else {
		/* takeover is not possible in controller mode */
		/* return success */
		info("Performing RPC: REQUEST_TAKEOVER : "
		     "already in controller mode - skipping");
	}

	slurm_send_rc_msg(msg, error_code);

}

/* _slurm_rpc_shutdown_controller - process RPC to shutdown slurmctld */
static void _slurm_rpc_shutdown_controller(slurm_msg_t * msg)
{
	int error_code = SLURM_SUCCESS, i;
	uint16_t options = 0;
	shutdown_msg_t *shutdown_msg = (shutdown_msg_t *) msg->data;
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);
	/* Locks: Read node */
	slurmctld_lock_t node_read_lock = {
		NO_LOCK, NO_LOCK, READ_LOCK, NO_LOCK };

	if (!validate_super_user(uid)) {
		error("Security violation, SHUTDOWN RPC from uid=%d", uid);
		error_code = ESLURM_USER_ID_MISSING;
	}
	if (error_code);
	else if (msg->msg_type == REQUEST_CONTROL) {
		info("Performing RPC: REQUEST_CONTROL");
		/* resume backup mode */
		slurmctld_config.resume_backup = true;
	} else {
		info("Performing RPC: REQUEST_SHUTDOWN");
		options = shutdown_msg->options;
	}

	/* do RPC call */
	if (error_code)
		;
	else if (options == 1)
		info("performing immeditate shutdown without state save");
	else if (slurmctld_config.shutdown_time)
		debug2("shutdown RPC issued when already in progress");
	else {
		if ((msg->msg_type == REQUEST_SHUTDOWN) &&
		    (options == 0)) {
			/* This means (msg->msg_type != REQUEST_CONTROL) */
			lock_slurmctld(node_read_lock);
			msg_to_slurmd(REQUEST_SHUTDOWN);
			unlock_slurmctld(node_read_lock);
		}
		if (slurmctld_config.thread_id_sig)	/* signal clean-up */
			pthread_kill(slurmctld_config.thread_id_sig, SIGTERM);
		else {
			error("thread_id_sig undefined, hard shutdown");
			slurmctld_config.shutdown_time = time(NULL);
			/* send REQUEST_SHUTDOWN_IMMEDIATE RPC */
			slurmctld_shutdown();
		}
	}

	if (msg->msg_type == REQUEST_CONTROL) {
		/* Wait for workload to dry up before sending reply.
		 * One thread should remain, this one. */
		for (i = 1; i < (CONTROL_TIMEOUT * 10); i++) {
			if (slurmctld_config.server_thread_count <= 1)
				break;
			usleep(100000);
		}
		if (slurmctld_config.server_thread_count > 1)
			error("REQUEST_CONTROL reply with %d active threads",
			      slurmctld_config.server_thread_count);
		/* save_all_state();	performed by _slurmctld_background */
	}


	slurm_send_rc_msg(msg, error_code);
	if ((error_code == SLURM_SUCCESS) && (options == 1) &&
	    (slurmctld_config.thread_id_sig))
		pthread_kill(slurmctld_config.thread_id_sig, SIGABRT);
}

/* _slurm_rpc_shutdown_controller_immediate - process RPC to shutdown
 *	slurmctld */
static void _slurm_rpc_shutdown_controller_immediate(slurm_msg_t * msg)
{
	int error_code = SLURM_SUCCESS;
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);

	if (!validate_super_user(uid)) {
		error("Security violation, SHUTDOWN_IMMEDIATE RPC from uid=%d",
		      uid);
		error_code = ESLURM_USER_ID_MISSING;
	}

	/* do RPC call */
	/* No op: just used to knock loose accept RPC thread */
	if (error_code == SLURM_SUCCESS)
		debug("Performing RPC: REQUEST_SHUTDOWN_IMMEDIATE");
}

/* _slurm_rpc_step_complete - process step completion RPC to note the
 *      completion of a job step on at least some nodes.
 *	If the job step is complete, it may
 *	represent the termination of an entire job */
static void _slurm_rpc_step_complete(slurm_msg_t *msg)
{
	static int active_rpc_cnt = 0;
	int error_code = SLURM_SUCCESS, rc, rem;
	uint32_t step_rc;
	DEF_TIMERS;
	step_complete_msg_t *req = (step_complete_msg_t *)msg->data;
	/* Locks: Write job, write node */
	slurmctld_lock_t job_write_lock = {
		NO_LOCK, WRITE_LOCK, WRITE_LOCK, NO_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);
	bool dump_job = false, dump_node = false;

	/* init */
	START_TIMER;
	debug("Processing RPC: REQUEST_STEP_COMPLETE for %u.%u "
	      "nodes %u-%u rc=%u uid=%d",
	      req->job_id, req->job_step_id,
	      req->range_first, req->range_last,
	      req->step_rc, uid);

	_throttle_start(&active_rpc_cnt);
	lock_slurmctld(job_write_lock);
	rc = step_partial_comp(req, uid, &rem, &step_rc);

	if (rc || rem) {	/* some error or not totally done */
		/* Note: Error printed within step_partial_comp */
		unlock_slurmctld(job_write_lock);
		_throttle_fini(&active_rpc_cnt);
		slurm_send_rc_msg(msg, rc);
		if (!rc)	/* partition completion */
			schedule_job_save();	/* Has own locking */
		return;
	}

	if (req->job_step_id == SLURM_BATCH_SCRIPT) {
		/* FIXME: test for error, possibly cause batch job requeue */
		error_code = job_complete(req->job_id, uid, false,
					  false, step_rc);
		unlock_slurmctld(job_write_lock);
		_throttle_fini(&active_rpc_cnt);
		END_TIMER2("_slurm_rpc_step_complete");

		/* return result */
		if (error_code) {
			info("_slurm_rpc_step_complete JobId=%u: %s",
			     req->job_id, slurm_strerror(error_code));
			slurm_send_rc_msg(msg, error_code);
		} else {
			debug2("sched: _slurm_rpc_step_complete JobId=%u: %s",
			       req->job_id, TIME_STR);
			slurm_send_rc_msg(msg, SLURM_SUCCESS);
			dump_job = true;
		}
	} else {
		error_code = job_step_complete(req->job_id, req->job_step_id,
					       uid, false, step_rc);
		unlock_slurmctld(job_write_lock);
		_throttle_fini(&active_rpc_cnt);
		END_TIMER2("_slurm_rpc_step_complete");

		/* return result */
		if (error_code) {
			info("_slurm_rpc_step_complete 1 StepId=%u.%u %s",
			     req->job_id, req->job_step_id,
			     slurm_strerror(error_code));
			slurm_send_rc_msg(msg, error_code);
		} else {
			info("sched: _slurm_rpc_step_complete StepId=%u.%u %s",
			     req->job_id, req->job_step_id, TIME_STR);
			slurm_send_rc_msg(msg, SLURM_SUCCESS);
			dump_job = true;
		}
	}
	if (dump_job)
		(void) schedule_job_save();	/* Has own locking */
	if (dump_node)
		(void) schedule_node_save();	/* Has own locking */
}

/* _slurm_rpc_step_layout - return the step layout structure for
 *      a job step, if it currently exists
 */
static void _slurm_rpc_step_layout(slurm_msg_t *msg)
{
	int error_code = SLURM_SUCCESS;
	slurm_msg_t response_msg;
	DEF_TIMERS;
	job_step_id_msg_t *req = (job_step_id_msg_t *)msg->data;
	slurm_step_layout_t *step_layout = NULL;
	/* Locks: Read config job, write node */
	slurmctld_lock_t job_read_lock = {
		READ_LOCK, READ_LOCK, READ_LOCK, NO_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);
	struct job_record *job_ptr = NULL;
	struct step_record *step_ptr = NULL;

	START_TIMER;
	debug2("Processing RPC: REQUEST_STEP_LAYOUT, from uid=%d", uid);

	lock_slurmctld(job_read_lock);
	error_code = job_alloc_info(uid, req->job_id, &job_ptr);
	END_TIMER2("_slurm_rpc_step_layout");
	/* return result */
	if (error_code || (job_ptr == NULL)) {
		unlock_slurmctld(job_read_lock);
		if (error_code == ESLURM_ACCESS_DENIED) {
			error("Security vioation, REQUEST_STEP_LAYOUT for "
			      "JobId=%u from uid=%u", req->job_id, uid);
		} else {
			debug2("_slurm_rpc_step_layout: JobId=%u, uid=%u: %s",
			       req->job_id, uid,
			       slurm_strerror(error_code));
		}
		slurm_send_rc_msg(msg, error_code);
		return;
	}

	step_ptr = find_step_record(job_ptr, req->step_id);
	if (!step_ptr) {
		unlock_slurmctld(job_read_lock);
		debug2("_slurm_rpc_step_layout: "
		       "JobId=%u.%u Not Found",
		       req->job_id, req->step_id);
		slurm_send_rc_msg(msg, ESLURM_INVALID_JOB_ID);
		return;
	}
	step_layout = slurm_step_layout_copy(step_ptr->step_layout);
#ifdef HAVE_FRONT_END
	if (job_ptr->batch_host)
		step_layout->front_end = xstrdup(job_ptr->batch_host);
#endif
	unlock_slurmctld(job_read_lock);

	slurm_msg_t_init(&response_msg);
	response_msg.flags = msg->flags;
	response_msg.protocol_version = msg->protocol_version;
	response_msg.msg_type    = RESPONSE_STEP_LAYOUT;
	response_msg.data        = step_layout;

	slurm_send_node_msg(msg->conn_fd, &response_msg);
	slurm_step_layout_destroy(step_layout);
}

/* _slurm_rpc_step_update - update a job step
 */
static void _slurm_rpc_step_update(slurm_msg_t *msg)
{
	DEF_TIMERS;
	step_update_request_msg_t *req =
		(step_update_request_msg_t *) msg->data;
	/* Locks: Write job */
	slurmctld_lock_t job_write_lock = {
		NO_LOCK, WRITE_LOCK, NO_LOCK, NO_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);
	int rc;

	START_TIMER;
	debug2("Processing RPC: REQUEST_STEP_UPDATE, from uid=%d", uid);

	lock_slurmctld(job_write_lock);
	rc = update_step(req, uid);
	unlock_slurmctld(job_write_lock);
	END_TIMER2("_slurm_rpc_step_update");

	slurm_send_rc_msg(msg, rc);
}

/* _slurm_rpc_submit_batch_job - process RPC to submit a batch job */
static void _slurm_rpc_submit_batch_job(slurm_msg_t * msg)
{
	static int active_rpc_cnt = 0;
	int error_code = SLURM_SUCCESS;
	DEF_TIMERS;
	uint32_t step_id = 0;
	struct job_record *job_ptr = NULL;
	slurm_msg_t response_msg;
	submit_response_msg_t submit_msg;
	job_desc_msg_t *job_desc_msg = (job_desc_msg_t *) msg->data;
	/* Locks: Read config, read job, read node, read partition */
	slurmctld_lock_t job_read_lock = {
		READ_LOCK, READ_LOCK, READ_LOCK, READ_LOCK };
	/* Locks: Write job, read node, read partition */
	slurmctld_lock_t job_write_lock = {
		NO_LOCK, WRITE_LOCK, READ_LOCK, READ_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);
	char *err_msg = NULL;

	START_TIMER;
	debug2("Processing RPC: REQUEST_SUBMIT_BATCH_JOB from uid=%d", uid);

	slurm_msg_t_init(&response_msg);
	response_msg.flags = msg->flags;
	response_msg.protocol_version = msg->protocol_version;

	/* do RPC call */
	if ( (uid != job_desc_msg->user_id) && (!validate_super_user(uid)) ) {
		/* NOTE: Super root can submit a batch job for any user */
		error_code = ESLURM_USER_ID_MISSING;
		error("Security violation, SUBMIT_JOB from uid=%d", uid);
	}
	if ((job_desc_msg->alloc_node == NULL) ||
	    (job_desc_msg->alloc_node[0] == '\0')) {
		error_code = ESLURM_INVALID_NODE_NAME;
		error("REQUEST_SUBMIT_BATCH_JOB lacks alloc_node from uid=%d", uid);
	}

	if (error_code == SLURM_SUCCESS) {
		/* Locks are for job_submit plugin use */
		lock_slurmctld(job_read_lock);
		error_code = validate_job_create_req(job_desc_msg,uid,&err_msg);
		unlock_slurmctld(job_read_lock);
	}

	dump_job_desc(job_desc_msg);
	if (error_code == SLURM_SUCCESS) {
		_throttle_start(&active_rpc_cnt);
		lock_slurmctld(job_write_lock);
		START_TIMER;	/* Restart after we have locks */
		if (job_desc_msg->job_id != SLURM_BATCH_SCRIPT) {
			job_ptr = find_job_record(job_desc_msg->job_id);
			if (job_ptr && IS_JOB_FINISHED(job_ptr)) {
				if (IS_JOB_COMPLETING(job_ptr)) {
					info("Attempt to re-use active "
					     "job id %u", job_ptr->job_id);
					slurm_send_rc_msg(
						msg,
						ESLURM_DUPLICATE_JOB_ID);
					unlock_slurmctld(job_write_lock);
					_throttle_fini(&active_rpc_cnt);
					goto fini;
				}
				job_ptr = NULL;	/* OK to re-use job id */
			}
		} else
			job_ptr = NULL;

		if (job_ptr) {	/* Active job allocation */
#if defined HAVE_FRONT_END && !defined HAVE_BGQ	&& !defined HAVE_ALPS_CRAY
			/* Limited job step support */
			/* Non-super users not permitted to run job steps on
			 * front-end. A single slurmd can not handle a heavy
			 * load. */
			if (!validate_slurm_user(uid)) {
				info("Attempt to execute batch job step by "
				     "uid=%d", uid);
				slurm_send_rc_msg(msg, ESLURM_NO_STEPS);
				unlock_slurmctld(job_write_lock);
				_throttle_fini(&active_rpc_cnt);
				goto fini;
			}
#endif

			if (job_ptr->user_id != uid) {
				error("Security violation, uid=%d attempting "
				      "to execute a step within job %u owned "
				      "by user %u",
		 		      uid, job_ptr->job_id,
				      job_ptr->user_id);
				slurm_send_rc_msg(msg, ESLURM_USER_ID_MISSING);
				unlock_slurmctld(job_write_lock);
				_throttle_fini(&active_rpc_cnt);
				goto fini;
			}
			if (job_ptr->details &&
			    job_ptr->details->prolog_running) {
				slurm_send_rc_msg(msg, EAGAIN);
				unlock_slurmctld(job_write_lock);
				_throttle_fini(&active_rpc_cnt);
				goto fini;
			}

			error_code = _launch_batch_step(job_desc_msg, uid,
							&step_id,
							msg->protocol_version);
			unlock_slurmctld(job_write_lock);
			_throttle_fini(&active_rpc_cnt);
			END_TIMER2("_slurm_rpc_submit_batch_job");

			if (error_code != SLURM_SUCCESS) {
				info("_launch_batch_step: %s",
				     slurm_strerror(error_code));
				slurm_send_rc_msg(msg, error_code);
			} else {
				info("_launch_batch_step StepId=%u.%u %s",
				     job_desc_msg->job_id, step_id,
				     TIME_STR);
				submit_msg.job_id     = job_desc_msg->job_id;
				submit_msg.step_id    = step_id;
				submit_msg.error_code = error_code;
				response_msg.msg_type =
					RESPONSE_SUBMIT_BATCH_JOB;

				response_msg.data = &submit_msg;
				slurm_send_node_msg(msg->conn_fd,
						    &response_msg);
				schedule_job_save();
			}
			goto fini;
		}

		/* Create new job allocation */
		error_code = job_allocate(job_desc_msg,
					  job_desc_msg->immediate, false,
					  NULL, 0, uid, &job_ptr, &err_msg);
		unlock_slurmctld(job_write_lock);
		_throttle_fini(&active_rpc_cnt);
		END_TIMER2("_slurm_rpc_submit_batch_job");
		if (job_desc_msg->immediate && (error_code != SLURM_SUCCESS))
			error_code = ESLURM_CAN_NOT_START_IMMEDIATELY;
	}

	/* return result */
	if ((error_code != SLURM_SUCCESS) &&
	    (error_code != ESLURM_JOB_HELD) &&
	    (error_code != ESLURM_NODE_NOT_AVAIL) &&
	    (error_code != ESLURM_QOS_THRES) &&
	    (error_code != ESLURM_RESERVATION_NOT_USABLE) &&
	    (error_code != ESLURM_REQUESTED_PART_CONFIG_UNAVAILABLE)) {
		info("_slurm_rpc_submit_batch_job: %s",
		     slurm_strerror(error_code));
		if (err_msg)
			slurm_send_rc_err_msg(msg, error_code, err_msg);
		else
			slurm_send_rc_msg(msg, error_code);
	} else if (!job_ptr) {	/* Mostly to avoid CLANG error */
		fatal("job_allocate failed to allocate job, rc=%d",error_code);
	} else {
		info("_slurm_rpc_submit_batch_job JobId=%u %s",
		     job_ptr->job_id, TIME_STR);
		/* send job_ID */
		submit_msg.job_id     = job_ptr->job_id;
		submit_msg.step_id    = SLURM_BATCH_SCRIPT;
		submit_msg.error_code = error_code;
		response_msg.msg_type = RESPONSE_SUBMIT_BATCH_JOB;
		response_msg.data = &submit_msg;
		slurm_send_node_msg(msg->conn_fd, &response_msg);

		schedule_job_save();	/* Has own locks */
		schedule_node_save();	/* Has own locks */
		queue_job_scheduler();
	}

fini:	xfree(err_msg);
}

/* _slurm_rpc_update_job - process RPC to update the configuration of a
 * job (e.g. priority)
 */
static void _slurm_rpc_update_job(slurm_msg_t * msg)
{
	int error_code;
	DEF_TIMERS;
	job_desc_msg_t *job_desc_msg = (job_desc_msg_t *) msg->data;
	/* Locks: Write job, read node, read partition */
	slurmctld_lock_t job_write_lock = {
		NO_LOCK, WRITE_LOCK, READ_LOCK, READ_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);

	START_TIMER;
	debug2("Processing RPC: REQUEST_UPDATE_JOB from uid=%d", uid);

	/* do RPC call */
	dump_job_desc(job_desc_msg);
	/* Insure everything that may be written to database is lower case */
	xstrtolower(job_desc_msg->account);
	xstrtolower(job_desc_msg->wckey);
	lock_slurmctld(job_write_lock);
	if (job_desc_msg->job_id_str)
		error_code = update_job_str(msg, uid);
	else
		error_code = update_job(msg, uid);
	unlock_slurmctld(job_write_lock);
	END_TIMER2("_slurm_rpc_update_job");

	/* return result */
	if (error_code) {
		info("_slurm_rpc_update_job JobId=%u uid=%d: %s",
		     job_desc_msg->job_id, uid, slurm_strerror(error_code));
	} else {
		info("_slurm_rpc_update_job complete JobId=%u uid=%d %s",
		     job_desc_msg->job_id, uid, TIME_STR);
		/* Below functions provide their own locking */
		schedule_job_save();
		schedule_node_save();
		queue_job_scheduler();
	}
}

/*
 * slurm_drain_nodes - process a request to drain a list of nodes,
 *	no-op for nodes already drained or draining
 * node_list IN - list of nodes to drain
 * reason IN - reason to drain the nodes
 * reason_uid IN - who set the reason
 * RET SLURM_SUCCESS or error code
 * NOTE: This is utilzed by plugins and not via RPC and it sets its
 *	own locks.
 */
extern int slurm_drain_nodes(char *node_list, char *reason, uint32_t reason_uid)
{
	int error_code;
	DEF_TIMERS;
	/* Locks: Write  node */
	slurmctld_lock_t node_write_lock = {
		NO_LOCK, NO_LOCK, WRITE_LOCK, NO_LOCK };

	START_TIMER;
	lock_slurmctld(node_write_lock);
	error_code = drain_nodes(node_list, reason, reason_uid);
	unlock_slurmctld(node_write_lock);
	END_TIMER2("slurm_drain_nodes");

	return error_code;
}

/*
 * slurm_fail_job - terminate a job due to a launch failure
 *      no-op for jobs already terminated
 * job_id IN - slurm job id
 * IN job_state - desired job state (JOB_BOOT_FAIL, JOB_NODE_FAIL, etc.)
 * RET SLURM_SUCCESS or error code
 * NOTE: This is utilzed by plugins and not via RPC and it sets its
 *      own locks.
 */
extern int slurm_fail_job(uint32_t job_id, uint16_t job_state)
{
	int error_code;
	DEF_TIMERS;
	/* Locks: Write job and node */
	slurmctld_lock_t job_write_lock = {
		NO_LOCK, WRITE_LOCK, WRITE_LOCK, NO_LOCK };

	START_TIMER;
	lock_slurmctld(job_write_lock);
	error_code = job_fail(job_id, job_state);
	unlock_slurmctld(job_write_lock);
	END_TIMER2("slurm_fail_job");

	return error_code;
}

/*
 * _slurm_rpc_update_front_end - process RPC to update the configuration of a
 *	front_end node (e.g. UP/DOWN)
 */
static void _slurm_rpc_update_front_end(slurm_msg_t * msg)
{
	int error_code = SLURM_SUCCESS;
	DEF_TIMERS;
	update_front_end_msg_t *update_front_end_msg_ptr =
		(update_front_end_msg_t *) msg->data;
	/* Locks: write node */
	slurmctld_lock_t node_write_lock = {
		NO_LOCK, NO_LOCK, WRITE_LOCK, NO_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);

	START_TIMER;
	debug2("Processing RPC: REQUEST_UPDATE_FRONT_END from uid=%d", uid);
	if (!validate_super_user(uid)) {
		error_code = ESLURM_USER_ID_MISSING;
		error("Security violation, UPDATE_FRONT_END RPC from uid=%d",
		      uid);
	}

	if (error_code == SLURM_SUCCESS) {
		/* do RPC call */
		lock_slurmctld(node_write_lock);
		error_code = update_front_end(update_front_end_msg_ptr);
		unlock_slurmctld(node_write_lock);
		END_TIMER2("_slurm_rpc_update_front_end");
	}

	/* return result */
	if (error_code) {
		info("_slurm_rpc_update_front_end for %s: %s",
		     update_front_end_msg_ptr->name,
		     slurm_strerror(error_code));
		slurm_send_rc_msg(msg, error_code);
	} else {
		debug2("_slurm_rpc_update_front_end complete for %s %s",
		       update_front_end_msg_ptr->name, TIME_STR);
		slurm_send_rc_msg(msg, SLURM_SUCCESS);
	}
}

/*
 * _slurm_rpc_update_node - process RPC to update the configuration of a
 *	node (e.g. UP/DOWN)
 */
static void _slurm_rpc_update_node(slurm_msg_t * msg)
{
	int error_code = SLURM_SUCCESS;
	DEF_TIMERS;
	update_node_msg_t *update_node_msg_ptr =
		(update_node_msg_t *) msg->data;
	/* Locks: Write job and write node */
	slurmctld_lock_t node_write_lock = {
		NO_LOCK, WRITE_LOCK, WRITE_LOCK, NO_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);

	START_TIMER;
	debug2("Processing RPC: REQUEST_UPDATE_NODE from uid=%d", uid);
	if (!validate_super_user(uid)) {
		error_code = ESLURM_USER_ID_MISSING;
		error("Security violation, UPDATE_NODE RPC from uid=%d", uid);
	}

	if (error_code == SLURM_SUCCESS) {
		/* do RPC call */
		lock_slurmctld(node_write_lock);
		error_code = update_node(update_node_msg_ptr);
		unlock_slurmctld(node_write_lock);
		END_TIMER2("_slurm_rpc_update_node");
	}

	/* return result */
	if (error_code) {
		info("_slurm_rpc_update_node for %s: %s",
		     update_node_msg_ptr->node_names,
		     slurm_strerror(error_code));
		slurm_send_rc_msg(msg, error_code);
	} else {
		debug2("_slurm_rpc_update_node complete for %s %s",
		       update_node_msg_ptr->node_names, TIME_STR);
		slurm_send_rc_msg(msg, SLURM_SUCCESS);
	}

	/* Below functions provide their own locks */
	schedule_node_save();
	queue_job_scheduler();
	trigger_reconfig();
}

/* _slurm_rpc_update_partition - process RPC to update the configuration
 *	of a partition (e.g. UP/DOWN) */
static void _slurm_rpc_update_partition(slurm_msg_t * msg)
{
	int error_code = SLURM_SUCCESS;
	DEF_TIMERS;
	update_part_msg_t *part_desc_ptr = (update_part_msg_t *) msg->data;
	/* Locks: Read config, write job, read node, write partition
	 * NOTE: job write lock due to gang scheduler support */
	slurmctld_lock_t part_write_lock = {
		READ_LOCK, WRITE_LOCK, READ_LOCK, WRITE_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);

	START_TIMER;
	debug2("Processing RPC: REQUEST_UPDATE_PARTITION from uid=%d", uid);
	if (!validate_super_user(uid)) {
		error_code = ESLURM_USER_ID_MISSING;
		error("Security violation, UPDATE_PARTITION RPC from uid=%d",
		      uid);
	}

	if (error_code == SLURM_SUCCESS) {
		/* do RPC call */
		if (msg->msg_type == REQUEST_CREATE_PARTITION) {
			lock_slurmctld(part_write_lock);
			error_code = update_part(part_desc_ptr, true);
			unlock_slurmctld(part_write_lock);
		} else {
			lock_slurmctld(part_write_lock);
			error_code = update_part(part_desc_ptr, false);
			unlock_slurmctld(part_write_lock);
		}
		END_TIMER2("_slurm_rpc_update_partition");
	}

	/* return result */
	if (error_code) {
		info("_slurm_rpc_update_partition partition=%s: %s",
		     part_desc_ptr->name, slurm_strerror(error_code));
		slurm_send_rc_msg(msg, error_code);
	} else {
		debug2("_slurm_rpc_update_partition complete for %s %s",
		       part_desc_ptr->name, TIME_STR);
		slurm_send_rc_msg(msg, SLURM_SUCCESS);

		schedule_part_save();		/* Has its locking */
		queue_job_scheduler();
	}
}

/* _slurm_rpc_delete_partition - process RPC to delete a partition */
static void _slurm_rpc_delete_partition(slurm_msg_t * msg)
{
	/* init */
	int error_code = SLURM_SUCCESS;
	DEF_TIMERS;
	delete_part_msg_t *part_desc_ptr = (delete_part_msg_t *) msg->data;
	/* Locks: write job, read node, write partition */
	slurmctld_lock_t part_write_lock = {
		NO_LOCK, WRITE_LOCK, READ_LOCK, WRITE_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);

	START_TIMER;
	debug2("Processing RPC: REQUEST_DELETE_PARTITION from uid=%d", uid);
	if (!validate_super_user(uid)) {
		error_code = ESLURM_USER_ID_MISSING;
		error("Security violation, DELETE_PARTITION RPC from uid=%d",
		      uid);
	}

	if (error_code == SLURM_SUCCESS) {
		/* do RPC call */
		lock_slurmctld(part_write_lock);
		error_code = delete_partition(part_desc_ptr);
		unlock_slurmctld(part_write_lock);
		END_TIMER2("_slurm_rpc_delete_partition");
	}

	/* return result */
	if (error_code) {
		info("_slurm_rpc_delete_partition partition=%s: %s",
		     part_desc_ptr->name, slurm_strerror(error_code));
		slurm_send_rc_msg(msg, error_code);
	} else {
		info("_slurm_rpc_delete_partition complete for %s %s",
		     part_desc_ptr->name, TIME_STR);
		slurm_send_rc_msg(msg, SLURM_SUCCESS);

		save_all_state();	/* Has own locking */
		queue_job_scheduler();
	}
}

/* _slurm_rpc_resv_create - process RPC to create a reservation */
static void _slurm_rpc_resv_create(slurm_msg_t * msg)
{
	int error_code = SLURM_SUCCESS;
	DEF_TIMERS;
	resv_desc_msg_t *resv_desc_ptr = (resv_desc_msg_t *)
		msg->data;
	/* Locks: write node, read partition */
	slurmctld_lock_t node_write_lock = {
		NO_LOCK, NO_LOCK, WRITE_LOCK, READ_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);

	START_TIMER;
	debug2("Processing RPC: REQUEST_CREATE_RESERVATION from uid=%d", uid);
	if (!validate_operator(uid)) {
		error_code = ESLURM_USER_ID_MISSING;
		error("Security violation, CREATE_RESERVATION RPC from uid=%d",
		      uid);
	}

	if (error_code == SLURM_SUCCESS) {
		/* do RPC call */
		lock_slurmctld(node_write_lock);
		error_code = create_resv(resv_desc_ptr);
		unlock_slurmctld(node_write_lock);
		END_TIMER2("_slurm_rpc_resv_create");
	}

	/* return result */
	if (error_code) {
		if (resv_desc_ptr->name) {
			info("_slurm_rpc_resv_create reservation=%s: %s",
			     resv_desc_ptr->name, slurm_strerror(error_code));
		} else {
			info("_slurm_rpc_resv_create: %s",
			     slurm_strerror(error_code));
		}
		slurm_send_rc_msg(msg, error_code);
	} else {
		slurm_msg_t response_msg;
		reservation_name_msg_t resv_resp_msg;

		debug2("_slurm_rpc_resv_create complete for %s %s",
		       resv_desc_ptr->name, TIME_STR);
		/* send reservation name */
		slurm_msg_t_init(&response_msg);
		response_msg.flags = msg->flags;
		response_msg.protocol_version = msg->protocol_version;
		resv_resp_msg.name    = resv_desc_ptr->name;
		response_msg.msg_type = RESPONSE_CREATE_RESERVATION;
		response_msg.data     = &resv_resp_msg;
		slurm_send_node_msg(msg->conn_fd, &response_msg);

		queue_job_scheduler();
	}
}

/* _slurm_rpc_resv_update - process RPC to update a reservation */
static void _slurm_rpc_resv_update(slurm_msg_t * msg)
{
	int error_code = SLURM_SUCCESS;
	DEF_TIMERS;
	resv_desc_msg_t *resv_desc_ptr = (resv_desc_msg_t *)
		msg->data;
	/* Locks: write node, read partition */
	slurmctld_lock_t node_write_lock = {
		NO_LOCK, NO_LOCK, WRITE_LOCK, READ_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);

	START_TIMER;
	debug2("Processing RPC: REQUEST_UPDATE_RESERVATION from uid=%d", uid);
	if (!validate_operator(uid)) {
		error_code = ESLURM_USER_ID_MISSING;
		error("Security violation, UPDATE_RESERVATION RPC from uid=%d",
		      uid);
	}

	if (error_code == SLURM_SUCCESS) {
		/* do RPC call */
		lock_slurmctld(node_write_lock);
		error_code = update_resv(resv_desc_ptr);
		unlock_slurmctld(node_write_lock);
		END_TIMER2("_slurm_rpc_resv_update");
	}

	/* return result */
	if (error_code) {
		info("_slurm_rpc_resv_update reservation=%s: %s",
		     resv_desc_ptr->name, slurm_strerror(error_code));
		slurm_send_rc_msg(msg, error_code);
	} else {
		debug2("_slurm_rpc_resv_update complete for %s %s",
		       resv_desc_ptr->name, TIME_STR);
		slurm_send_rc_msg(msg, SLURM_SUCCESS);

		queue_job_scheduler();
	}
}

/* _slurm_rpc_resv_delete - process RPC to delete a reservation */
static void _slurm_rpc_resv_delete(slurm_msg_t * msg)
{
	/* init */
	int error_code = SLURM_SUCCESS;
	DEF_TIMERS;
	reservation_name_msg_t *resv_desc_ptr = (reservation_name_msg_t *)
		msg->data;
	/* Locks: read job, write node */
	slurmctld_lock_t node_write_lock = {
		NO_LOCK, READ_LOCK, WRITE_LOCK, NO_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);

	START_TIMER;
	debug2("Processing RPC: REQUEST_DELETE_RESERVTION from uid=%d", uid);
	if (!validate_operator(uid)) {
		error_code = ESLURM_USER_ID_MISSING;
		error("Security violation, DELETE_RESERVTION RPC from uid=%d",
		      uid);
	}

	if (error_code == SLURM_SUCCESS) {
		/* do RPC call */
		lock_slurmctld(node_write_lock);
		error_code = delete_resv(resv_desc_ptr);
		unlock_slurmctld(node_write_lock);
		END_TIMER2("_slurm_rpc_resv_delete");
	}

	/* return result */
	if (error_code) {
		info("_slurm_rpc_delete_reservation partition=%s: %s",
		     resv_desc_ptr->name, slurm_strerror(error_code));
		slurm_send_rc_msg(msg, error_code);
	} else {
		info("_slurm_rpc_delete_reservation complete for %s %s",
		     resv_desc_ptr->name, TIME_STR);
		slurm_send_rc_msg(msg, SLURM_SUCCESS);

		queue_job_scheduler();
	}
}

/* _slurm_rpc_resv_show - process RPC to dump reservation info */
static void _slurm_rpc_resv_show(slurm_msg_t * msg)
{
	resv_info_request_msg_t *resv_req_msg = (resv_info_request_msg_t *)
		msg->data;
	DEF_TIMERS;
	/* Locks: read node */
	slurmctld_lock_t node_read_lock = {
		NO_LOCK, NO_LOCK, READ_LOCK, NO_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);
	slurm_msg_t response_msg;
	char *dump;
	int dump_size;

	START_TIMER;
	debug2("Processing RPC: REQUEST_RESERVATION_INFO from uid=%d", uid);
	if ((resv_req_msg->last_update - 1) >= last_resv_update) {
		debug2("_slurm_rpc_resv_show, no change");
		slurm_send_rc_msg(msg, SLURM_NO_CHANGE_IN_DATA);
	} else {
		lock_slurmctld(node_read_lock);
		show_resv(&dump, &dump_size, uid, msg->protocol_version);
		unlock_slurmctld(node_read_lock);
		END_TIMER2("_slurm_rpc_resv_show");

		/* init response_msg structure */
		slurm_msg_t_init(&response_msg);
		response_msg.flags = msg->flags;
		response_msg.protocol_version = msg->protocol_version;
		response_msg.address = msg->address;
		response_msg.msg_type = RESPONSE_RESERVATION_INFO;
		response_msg.data = dump;
		response_msg.data_size = dump_size;

		/* send message */
		slurm_send_node_msg(msg->conn_fd, &response_msg);
		xfree(dump);
	}
}

/* _slurm_rpc_update_block - process RPC to update the configuration
 *	of a block (e.g. FREE/ERROR/DELETE) */
static void _slurm_rpc_update_block(slurm_msg_t * msg)
{
	int error_code = SLURM_SUCCESS;
	DEF_TIMERS;
	update_block_msg_t *block_desc_ptr = (update_block_msg_t *) msg->data;
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);
	char *name = NULL;
	START_TIMER;

	debug2("Processing RPC: REQUEST_UPDATE_BLOCK from uid=%d", uid);
	if (!validate_super_user(uid)) {
		error_code = ESLURM_USER_ID_MISSING;
		error("Security violation, UPDATE_BLOCK RPC from uid=%d", uid);
		if (block_desc_ptr->bg_block_id) {
			name = block_desc_ptr->bg_block_id;
		} else if (block_desc_ptr->mp_str) {
			name = block_desc_ptr->mp_str;
		}
	}

	if (error_code == SLURM_SUCCESS) {
		/* do RPC call */
		if (block_desc_ptr->bg_block_id) {
			error_code = select_g_update_block(block_desc_ptr);
			END_TIMER2("_slurm_rpc_update_block");
			name = block_desc_ptr->bg_block_id;
		} else if (block_desc_ptr->mp_str) {
			error_code = select_g_update_sub_node(block_desc_ptr);
			END_TIMER2("_slurm_rpc_update_subbp");
			name = block_desc_ptr->mp_str;
		} else {
			error("Unknown update for blocks");
			error_code = SLURM_ERROR;
			END_TIMER2("_slurm_rpc_update_block");
		}
	}

	/* return result */
	if (error_code) {
		info("_slurm_rpc_update_block %s: %s",
		     name,
		     slurm_strerror(error_code));
		slurm_send_rc_msg(msg, error_code);
	} else {
		debug2("_slurm_rpc_update_block complete for %s %s",
		       name, TIME_STR);
		slurm_send_rc_msg(msg, SLURM_SUCCESS);
	}
}

/* determine of nodes are ready for the job */
static void _slurm_rpc_job_ready(slurm_msg_t * msg)
{
	int error_code, result;
	job_id_msg_t *id_msg = (job_id_msg_t *) msg->data;
	DEF_TIMERS;
	/* Locks: read job */
	slurmctld_lock_t job_read_lock = {
		NO_LOCK, READ_LOCK, NO_LOCK, NO_LOCK };
	slurm_msg_t response_msg;
	return_code_msg_t rc_msg;

	START_TIMER;
	lock_slurmctld(job_read_lock);
	error_code = job_node_ready(id_msg->job_id, &result);
	unlock_slurmctld(job_read_lock);
	END_TIMER2("_slurm_rpc_job_ready");

	if (error_code) {
		debug2("_slurm_rpc_job_ready: %s",
		       slurm_strerror(error_code));
		slurm_send_rc_msg(msg, error_code);
	} else {
		debug2("_slurm_rpc_job_ready(%u)=%d %s", id_msg->job_id,
		       result, TIME_STR);
		slurm_msg_t_init(&response_msg);
		response_msg.flags = msg->flags;
		response_msg.protocol_version = msg->protocol_version;
		response_msg.address = msg->address;
		rc_msg.return_code = result;
		response_msg.data = &rc_msg;
		if(_is_prolog_finished(id_msg->job_id)) {
			response_msg.msg_type = RESPONSE_JOB_READY;
		} else {
			response_msg.msg_type = RESPONSE_PROLOG_EXECUTING;
		}
		slurm_send_node_msg(msg->conn_fd, &response_msg);
	}
}

/* Check if prolog has already finished */
static int _is_prolog_finished(uint32_t job_id) {
	int is_running = 0;
	struct job_record  *job_ptr;

	slurmctld_lock_t job_read_lock = {
		NO_LOCK, READ_LOCK, NO_LOCK, NO_LOCK };
	lock_slurmctld(job_read_lock);
	job_ptr = find_job_record(job_id);
	if (job_ptr) {
		is_running = (job_ptr->state_reason != WAIT_PROLOG);
	}
	unlock_slurmctld(job_read_lock);
	return is_running;
}

/* get node select info plugin */
static void  _slurm_rpc_block_info(slurm_msg_t * msg)
{
	int error_code = SLURM_SUCCESS;
	Buf buffer = NULL;
	block_info_request_msg_t *sel_req_msg =
		(block_info_request_msg_t *) msg->data;
	slurm_msg_t response_msg;
	/* Locks: read config */
	slurmctld_lock_t config_read_lock = {
		READ_LOCK, NO_LOCK, NO_LOCK, NO_LOCK };
	DEF_TIMERS;
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);

	START_TIMER;
	debug2("Processing RPC: REQUEST_BLOCK_INFO from uid=%d", uid);
	lock_slurmctld(config_read_lock);
	if ((slurmctld_conf.private_data & PRIVATE_DATA_NODES) &&
	    !validate_operator(uid)) {
		error_code = ESLURM_ACCESS_DENIED;
		error("Security violation, REQUEST_BLOCK_INFO RPC from uid=%d",
		      uid);
	}
	unlock_slurmctld(config_read_lock);
	if (error_code == SLURM_SUCCESS) {
		error_code = select_g_pack_select_info(
			sel_req_msg->last_update, sel_req_msg->show_flags,
			&buffer, msg->protocol_version);
	}
	END_TIMER2("_slurm_rpc_block_info");

	if (error_code) {
		debug3("_slurm_rpc_block_info: %s",
		       slurm_strerror(error_code));
		slurm_send_rc_msg(msg, error_code);
	} else {
		/* init response_msg structure */
		slurm_msg_t_init(&response_msg);
		response_msg.flags = msg->flags;
		response_msg.protocol_version = msg->protocol_version;
		response_msg.address = msg->address;
		response_msg.msg_type = RESPONSE_BLOCK_INFO;
		response_msg.data = get_buf_data(buffer);
		response_msg.data_size = get_buf_offset(buffer);
		/* send message */
		slurm_send_node_msg(msg->conn_fd, &response_msg);

		if (buffer)
			free_buf(buffer);
	}
}

/* Reset the job credential key based upon configuration parameters.
 * NOTE: READ lock_slurmctld config before entry */
static void _update_cred_key(void)
{
	slurm_cred_ctx_key_update(slurmctld_config.cred_ctx,
				  slurmctld_conf.job_credential_private_key);
}

inline static void _slurm_rpc_suspend(slurm_msg_t * msg)
{
	int error_code = SLURM_SUCCESS;
	DEF_TIMERS;
	suspend_msg_t *sus_ptr = (suspend_msg_t *) msg->data;
	/* Locks: write job and node */
	slurmctld_lock_t job_write_lock = {
		NO_LOCK, WRITE_LOCK, WRITE_LOCK, NO_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);
	char *op;

	START_TIMER;
	switch (sus_ptr->op) {
	case SUSPEND_JOB:
		op = "suspend";
		break;
	case RESUME_JOB:
		op = "resume";
		break;
	default:
		op = "unknown";
	}
	info("Processing RPC: REQUEST_SUSPEND(%s) from uid=%u",
	     op, (unsigned int) uid);

	lock_slurmctld(job_write_lock);
	if (sus_ptr->job_id_str) {
		error_code = job_suspend2(sus_ptr, uid, msg->conn_fd, true,
					  msg->protocol_version);
	} else {
		error_code = job_suspend(sus_ptr, uid, msg->conn_fd, true,
					 msg->protocol_version);
	}
	unlock_slurmctld(job_write_lock);
	END_TIMER2("_slurm_rpc_suspend");

	if (error_code) {
		info("_slurm_rpc_suspend(%s) for %s %s", op,
		     sus_ptr->job_id_str, slurm_strerror(error_code));
	} else {
		info("_slurm_rpc_suspend(%s) for %s %s", op,
		     sus_ptr->job_id_str, TIME_STR);

		schedule_job_save();	/* Has own locking */
		if (sus_ptr->op == SUSPEND_JOB)
			queue_job_scheduler();
	}
}

inline static void _slurm_rpc_requeue(slurm_msg_t * msg)
{
	int error_code = SLURM_SUCCESS;
	DEF_TIMERS;
	requeue_msg_t *req_ptr = (requeue_msg_t *)msg->data;
	/* Locks: write job and node */
	slurmctld_lock_t job_write_lock = {
		NO_LOCK, WRITE_LOCK, WRITE_LOCK, NO_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);

	START_TIMER;

	info("%s: Processing RPC: REQUEST_JOB_REQUEUE from uid=%d", __func__,
	     uid);

	lock_slurmctld(job_write_lock);
	if (req_ptr->job_id_str) {
		error_code = job_requeue2(uid, req_ptr, msg->conn_fd,
					  msg->protocol_version, false);
	} else {
		error_code = job_requeue(uid, req_ptr->job_id, msg->conn_fd,
					 msg->protocol_version, false,
					 req_ptr->state);
	}
	unlock_slurmctld(job_write_lock);
	END_TIMER2("_slurm_rpc_requeue");

	if (error_code) {
		info("%s: %u: %s", __func__, req_ptr->job_id,
		     slurm_strerror(error_code));
	}

	/* Functions below provide their own locking
	 */
	schedule_job_save();
}

/* Assorted checkpoint operations */
inline static void  _slurm_rpc_checkpoint(slurm_msg_t * msg)
{
	int error_code = SLURM_SUCCESS;
	DEF_TIMERS;
	checkpoint_msg_t *ckpt_ptr = (checkpoint_msg_t *) msg->data;
	/* Locks: write job lock, read node lock */
	slurmctld_lock_t job_write_lock = {
		NO_LOCK, WRITE_LOCK, READ_LOCK, NO_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);
	char *op;

	START_TIMER;
	switch (ckpt_ptr->op) {
	case CHECK_ABLE:
		op = "able";
		break;
	case CHECK_CREATE:
		op = "create";
		break;
	case CHECK_DISABLE:
		op = "disable";
		break;
	case CHECK_ENABLE:
		op = "enable";
		break;
	case CHECK_ERROR:
		op = "error";
		break;
	case CHECK_REQUEUE:
		op = "requeue";
		break;
	case CHECK_RESTART:
		op = "restart";
		break;
	case CHECK_VACATE:
		op = "vacate";
		break;
	default:
		op = "unknown";
	}
	debug2("Processing RPC: REQUEST_CHECKPOINT(%s) from uid=%u",
	       op, (unsigned int) uid);

	/* do RPC call and send reply */
	lock_slurmctld(job_write_lock);
	if (ckpt_ptr->op == CHECK_RESTART) {
		error_code = job_restart(ckpt_ptr, uid, msg->conn_fd,
					 msg->protocol_version);
	} else if (ckpt_ptr->step_id == SLURM_BATCH_SCRIPT) {
		error_code = job_checkpoint(ckpt_ptr, uid, msg->conn_fd,
					    msg->protocol_version);
	} else {
		error_code = job_step_checkpoint(ckpt_ptr, uid, msg->conn_fd,
						 msg->protocol_version);
	}
	unlock_slurmctld(job_write_lock);
	END_TIMER2("_slurm_rpc_checkpoint");

	if (error_code) {
		if (ckpt_ptr->step_id == SLURM_BATCH_SCRIPT) {
			info("_slurm_rpc_checkpoint %s %u: %s", op,
			     ckpt_ptr->job_id, slurm_strerror(error_code));
		} else {
			info("_slurm_rpc_checkpoint %s %u.%u: %s", op,
			     ckpt_ptr->job_id, ckpt_ptr->step_id,
			     slurm_strerror(error_code));
		}
	} else {
		if (ckpt_ptr->step_id == SLURM_BATCH_SCRIPT) {
			info("_slurm_rpc_checkpoint %s for %u %s", op,
			     ckpt_ptr->job_id, TIME_STR);
		} else {
			info("_slurm_rpc_checkpoint %s for %u.%u %s", op,
			     ckpt_ptr->job_id, ckpt_ptr->step_id, TIME_STR);
		}
		if ((ckpt_ptr->op != CHECK_ABLE) &&
		    (ckpt_ptr->op != CHECK_ERROR)) {
			/* job state changed, save it */
			/* NOTE: This function provides it own locks */
			schedule_job_save();
		}
	}
}

inline static void  _slurm_rpc_checkpoint_comp(slurm_msg_t * msg)
{
	int error_code = SLURM_SUCCESS;
	DEF_TIMERS;
	checkpoint_comp_msg_t *ckpt_ptr = (checkpoint_comp_msg_t *) msg->data;
	/* Locks: read job */
	slurmctld_lock_t job_read_lock = {
		NO_LOCK, READ_LOCK, NO_LOCK, NO_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);

	START_TIMER;
	debug2("Processing RPC: REQUEST_CHECKPOINT_COMP from uid=%d", uid);

	/* do RPC call and send reply */
	lock_slurmctld(job_read_lock);
	error_code = job_step_checkpoint_comp(ckpt_ptr, uid, msg->conn_fd,
					      msg->protocol_version);
	unlock_slurmctld(job_read_lock);
	END_TIMER2("_slurm_rpc_checkpoint_comp");

	if (error_code) {
		info("_slurm_rpc_checkpoint_comp %u.%u: %s",
		     ckpt_ptr->job_id, ckpt_ptr->step_id,
		     slurm_strerror(error_code));
	} else {
		info("_slurm_rpc_checkpoint_comp %u.%u %s",
		     ckpt_ptr->job_id, ckpt_ptr->step_id, TIME_STR);
	}
}

inline static void  _slurm_rpc_checkpoint_task_comp(slurm_msg_t * msg)
{
	int error_code = SLURM_SUCCESS;
	DEF_TIMERS;
	checkpoint_task_comp_msg_t *ckpt_ptr;
	/* Locks: read job */
	slurmctld_lock_t job_read_lock = {
		NO_LOCK, READ_LOCK, NO_LOCK, NO_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);

	ckpt_ptr = (checkpoint_task_comp_msg_t *) msg->data;
	START_TIMER;
	debug2("Processing RPC: REQUEST_CHECKPOINT_TASK_COMP from uid=%d", uid);

	/* do RPC call and send reply */
	lock_slurmctld(job_read_lock);
	error_code = job_step_checkpoint_task_comp(ckpt_ptr, uid, msg->conn_fd,
						   msg->protocol_version);
	unlock_slurmctld(job_read_lock);
	END_TIMER2("_slurm_rpc_checkpoint_task_comp");

	if (error_code) {
		info("_slurm_rpc_checkpoint_task_comp %u.%u: %s",
		     ckpt_ptr->job_id, ckpt_ptr->step_id,
		     slurm_strerror(error_code));
	} else {
		info("_slurm_rpc_checkpoint_task_comp %u.%u %s",
		     ckpt_ptr->job_id, ckpt_ptr->step_id, TIME_STR);
	}
}

/* Copy an array of type char **, xmalloc() the array and xstrdup() the
 * strings in the array */
extern char **
xduparray(uint32_t size, char ** array)
{
	int i;
	char ** result;

	if (size == 0)
		return (char **)NULL;

	result = (char **) xmalloc(sizeof(char *) * size);
	for (i=0; i<size; i++)
		result[i] = xstrdup(array[i]);

	return result;
}

/* Like xduparray(), but performs one xmalloc().  The output format of this
 * must be identical to _read_data_array_from_file() */
static char **
_xduparray2(uint32_t size, char ** array)
{
	int i, len = 0;
	char *ptr, ** result;

	if (size == 0)
		return (char **) NULL;

	for (i=0; i<size; i++)
		len += (strlen(array[i]) + 1);
	ptr = xmalloc(len);
	result = (char **) xmalloc(sizeof(char *) * size);

	for (i=0; i<size; i++) {
		result[i] = ptr;
		len = strlen(array[i]);
		strcpy(ptr, array[i]);
		ptr += (len + 1);
	}

	return result;
}

/* _launch_batch_step
 * IN: job_desc_msg from _slurm_rpc_submit_batch_job() but with jobid set
 *     which means it's trying to launch within a pre-existing allocation.
 * IN: uid launching this batch job, which has already been validated.
 * OUT: SLURM error code if launch fails, or SLURM_SUCCESS
 */
static int _launch_batch_step(job_desc_msg_t *job_desc_msg, uid_t uid,
			      uint32_t *step_id, uint16_t protocol_version)
{
	struct job_record  *job_ptr;
	time_t now = time(NULL);
	int error_code = SLURM_SUCCESS;

	batch_job_launch_msg_t *launch_msg_ptr;
	agent_arg_t *agent_arg_ptr;
	struct node_record *node_ptr;

	if (job_desc_msg->array_inx && job_desc_msg->array_inx[0])
		return ESLURM_INVALID_ARRAY;

	/*
	 * Create a job step. Note that a credential is not necessary,
	 * since the slurmctld will be submitting this job directly to
	 * the slurmd.
	 */
	job_step_create_request_msg_t req_step_msg;
	struct step_record *step_rec;

	/*
	 * As far as the step record in slurmctld goes, we are just
	 * launching a batch script which will be run on a single
	 * processor on a single node. The actual launch request sent
	 * to the slurmd should contain the proper allocation values
	 * for subsequent srun jobs within the batch script.
	 */
	memset(&req_step_msg, 0, sizeof(job_step_create_request_msg_t));
	req_step_msg.job_id = job_desc_msg->job_id;
	req_step_msg.user_id = uid;
	req_step_msg.min_nodes = 1;
	req_step_msg.max_nodes = 0;
	req_step_msg.cpu_count = 1;
	req_step_msg.num_tasks = 1;
	req_step_msg.task_dist = SLURM_DIST_CYCLIC;
	req_step_msg.name = job_desc_msg->name;

	error_code = step_create(&req_step_msg, &step_rec, true);
	xfree(req_step_msg.node_list);	/* may be set by step_create */

	if (error_code != SLURM_SUCCESS)
		return error_code;

	/*
	 * TODO: check all instances of step_record to ensure there's no
	 * problem with a null switch_job_info pointer.
	 */

	/* Get the allocation in order to construct the batch job
	 * launch request for the slurmd.
	 */

	job_ptr = step_rec->job_ptr;

	/* TODO: need to address batch job step request options such as
	 * the ability to run a batch job on a subset of the nodes in the
	 * current allocation.
	 * TODO: validate the specific batch job request vs. the
	 * existing allocation. Note that subsequent srun steps within
	 * the batch script will work within the full allocation, but
	 * the batch step options can still provide default settings via
	 * environment variables
	 *
	 * NOTE: for now we are *ignoring* most of the job_desc_msg
	 *       allocation-related settings. At some point we
	 *       should perform better error-checking, otherwise
	 *       the submitter will make some invalid assumptions
	 *       about how this job actually ran.
	 */
	job_ptr->time_last_active = now;


	/* Launch the batch job */
	node_ptr = find_first_node_record(job_ptr->node_bitmap);
	if (node_ptr == NULL) {
		delete_step_record(job_ptr, step_rec->step_id);
		return ESLURM_INVALID_JOB_ID;
	}

	/* Initialization of data structures */
	launch_msg_ptr = (batch_job_launch_msg_t *)
		xmalloc(sizeof(batch_job_launch_msg_t));
	launch_msg_ptr->job_id = job_ptr->job_id;
	launch_msg_ptr->step_id = step_rec->step_id;
	launch_msg_ptr->gid = job_ptr->group_id;
	launch_msg_ptr->uid = uid;
	launch_msg_ptr->nodes = xstrdup(job_ptr->alias_list);
	launch_msg_ptr->partition = xstrdup(job_ptr->partition);
	launch_msg_ptr->restart_cnt = job_ptr->restart_cnt;
	if (job_ptr->details) {
		launch_msg_ptr->pn_min_memory = job_ptr->details->
						pn_min_memory;
	}

	if (make_batch_job_cred(launch_msg_ptr, job_ptr, protocol_version)) {
		error("aborting batch step %u.%u", job_ptr->job_id,
		      job_ptr->group_id);
		xfree(launch_msg_ptr->nodes);
		xfree(launch_msg_ptr);
		delete_step_record(job_ptr, step_rec->step_id);
		return SLURM_ERROR;
	}

	launch_msg_ptr->std_err = xstrdup(job_desc_msg->std_err);
	launch_msg_ptr->std_in = xstrdup(job_desc_msg->std_in);
	launch_msg_ptr->std_out = xstrdup(job_desc_msg->std_out);
	launch_msg_ptr->acctg_freq = xstrdup(job_desc_msg->acctg_freq);
	launch_msg_ptr->open_mode = job_desc_msg->open_mode;
	launch_msg_ptr->work_dir = xstrdup(job_desc_msg->work_dir);
	launch_msg_ptr->argc = job_desc_msg->argc;
	launch_msg_ptr->argv = xduparray(job_desc_msg->argc,
					 job_desc_msg->argv);
	launch_msg_ptr->array_job_id = job_ptr->array_job_id;
	launch_msg_ptr->array_task_id = job_ptr->array_task_id;
	launch_msg_ptr->spank_job_env_size = job_ptr->spank_job_env_size;
	launch_msg_ptr->spank_job_env = xduparray(job_ptr->spank_job_env_size,
						  job_ptr->spank_job_env);
	launch_msg_ptr->script = xstrdup(job_desc_msg->script);
	launch_msg_ptr->environment = _xduparray2(job_desc_msg->env_size,
						  job_desc_msg->environment);
	launch_msg_ptr->envc = job_desc_msg->env_size;
	launch_msg_ptr->job_mem = job_desc_msg->pn_min_memory;
	launch_msg_ptr->cpus_per_task = job_desc_msg->cpus_per_task;

	/* job_ptr->total_cpus represents the total number of CPUs available
	 * for this step (overcommit not supported yet). If job_desc_msg
	 * contains a reasonable min_cpus request, use that value;
	 * otherwise default to the allocation processor request.
	 */
	launch_msg_ptr->ntasks = job_ptr->total_cpus;
	if (job_desc_msg->min_cpus > 0 &&
	    job_desc_msg->min_cpus < launch_msg_ptr->ntasks)
		launch_msg_ptr->ntasks = job_desc_msg->min_cpus;

	launch_msg_ptr->num_cpu_groups = job_ptr->job_resrcs->cpu_array_cnt;
	launch_msg_ptr->cpus_per_node  =
		xmalloc(sizeof(uint16_t) * job_ptr->job_resrcs->cpu_array_cnt);
	memcpy(launch_msg_ptr->cpus_per_node,
	       job_ptr->job_resrcs->cpu_array_value,
	       (sizeof(uint16_t) * job_ptr->job_resrcs->cpu_array_cnt));
	launch_msg_ptr->cpu_count_reps  =
		xmalloc(sizeof(uint32_t) * job_ptr->job_resrcs->cpu_array_cnt);
	memcpy(launch_msg_ptr->cpu_count_reps,
	       job_ptr->job_resrcs->cpu_array_reps,
	       (sizeof(uint32_t) * job_ptr->job_resrcs->cpu_array_cnt));
	launch_msg_ptr->select_jobinfo = select_g_select_jobinfo_copy(
		job_ptr->select_jobinfo);

	/* FIXME: for some reason these CPU arrays total all the CPUs
	 * actually allocated, rather than totaling up to the requested
	 * CPU count for the allocation.
	 * This means that SLURM_TASKS_PER_NODE will not match with
	 * SLURM_NTASKS in the batch script environment.
	 */

	agent_arg_ptr = (agent_arg_t *) xmalloc(sizeof(agent_arg_t));
	agent_arg_ptr->node_count = 1;
	agent_arg_ptr->retry = 0;
	xassert(job_ptr->batch_host);
#ifdef HAVE_FRONT_END
	if (job_ptr->front_end_ptr)
		agent_arg_ptr->protocol_version =
			job_ptr->front_end_ptr->protocol_version;
#else
	if ((node_ptr = find_node_record(job_ptr->batch_host)))
		agent_arg_ptr->protocol_version = node_ptr->protocol_version;
#endif

	agent_arg_ptr->hostlist = hostlist_create(job_ptr->batch_host);
	if (!agent_arg_ptr->hostlist)
		fatal("Invalid batch host: %s", job_ptr->batch_host);
	agent_arg_ptr->msg_type = REQUEST_BATCH_JOB_LAUNCH;
	agent_arg_ptr->msg_args = (void *) launch_msg_ptr;

	/* Launch the RPC via agent */
	agent_queue_request(agent_arg_ptr);

	*step_id = step_rec->step_id;
	return SLURM_SUCCESS;
}

inline static void  _slurm_rpc_trigger_clear(slurm_msg_t * msg)
{
	int rc;
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);
	trigger_info_msg_t * trigger_ptr = (trigger_info_msg_t *) msg->data;
	DEF_TIMERS;

	START_TIMER;
	debug("Processing RPC: REQUEST_TRIGGER_CLEAR from uid=%d", uid);

	rc = trigger_clear(uid, trigger_ptr);
	END_TIMER2("_slurm_rpc_trigger_clear");

	slurm_send_rc_msg(msg, rc);
}

inline static void  _slurm_rpc_trigger_get(slurm_msg_t * msg)
{
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);
	trigger_info_msg_t *resp_data;
	trigger_info_msg_t * trigger_ptr = (trigger_info_msg_t *) msg->data;
	slurm_msg_t response_msg;
	DEF_TIMERS;

	START_TIMER;
	debug("Processing RPC: REQUEST_TRIGGER_GET from uid=%d", uid);

	resp_data = trigger_get(uid, trigger_ptr);
	END_TIMER2("_slurm_rpc_trigger_get");

	slurm_msg_t_init(&response_msg);
	response_msg.flags = msg->flags;
	response_msg.protocol_version = msg->protocol_version;
	response_msg.address  = msg->address;
	response_msg.msg_type = RESPONSE_TRIGGER_GET;
	response_msg.data     = resp_data;
	slurm_send_node_msg(msg->conn_fd, &response_msg);
	slurm_free_trigger_msg(resp_data);
}

inline static void  _slurm_rpc_trigger_set(slurm_msg_t * msg)
{
	int rc;
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);
	gid_t gid = g_slurm_auth_get_gid(msg->auth_cred, NULL);;
	trigger_info_msg_t * trigger_ptr = (trigger_info_msg_t *) msg->data;
	DEF_TIMERS;

	START_TIMER;
	debug("Processing RPC: REQUEST_TRIGGER_SET from uid=%d", uid);

	rc = trigger_set(uid, gid, trigger_ptr);
	END_TIMER2("_slurm_rpc_trigger_set");

	slurm_send_rc_msg(msg, rc);
}

inline static void  _slurm_rpc_trigger_pull(slurm_msg_t * msg)
{
	int rc;
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);
	trigger_info_msg_t * trigger_ptr = (trigger_info_msg_t *) msg->data;
	DEF_TIMERS;

	START_TIMER;

	/* NOTE: No locking required here, trigger_pull only needs to lock
	 * it's own internal trigger structure */
	debug("Processing RPC: REQUEST_TRIGGER_PULL from uid=%u",
	      (unsigned int) uid);
	if (!validate_slurm_user(uid)) {
		rc = ESLURM_USER_ID_MISSING;
		error("Security violation, REQUEST_TRIGGER_PULL RPC from uid=%d",
		      uid);
	} else
		rc = trigger_pull(trigger_ptr);
	END_TIMER2("_slurm_rpc_trigger_pull");

	slurm_send_rc_msg(msg, rc);
}

inline static void  _slurm_rpc_get_topo(slurm_msg_t * msg)
{
	topo_info_response_msg_t *topo_resp_msg;
	slurm_msg_t response_msg;
	int i;
	/* Locks: read node lock */
	slurmctld_lock_t node_read_lock = {
		NO_LOCK, NO_LOCK, READ_LOCK, NO_LOCK };
	DEF_TIMERS;

	START_TIMER;
	lock_slurmctld(node_read_lock);
	topo_resp_msg = xmalloc(sizeof(topo_info_response_msg_t));
	topo_resp_msg->record_count = switch_record_cnt;
	topo_resp_msg->topo_array = xmalloc(sizeof(topo_info_t) *
					    topo_resp_msg->record_count);
	for (i=0; i<topo_resp_msg->record_count; i++) {
		topo_resp_msg->topo_array[i].level      =
			switch_record_table[i].level;
		topo_resp_msg->topo_array[i].link_speed =
			switch_record_table[i].link_speed;
		topo_resp_msg->topo_array[i].name       =
			xstrdup(switch_record_table[i].name);
		topo_resp_msg->topo_array[i].nodes      =
			xstrdup(switch_record_table[i].nodes);
		topo_resp_msg->topo_array[i].switches   =
			xstrdup(switch_record_table[i].switches);
	}
	unlock_slurmctld(node_read_lock);
	END_TIMER2("_slurm_rpc_get_topo");

	slurm_msg_t_init(&response_msg);
	response_msg.flags = msg->flags;
	response_msg.protocol_version = msg->protocol_version;
	response_msg.address  = msg->address;
	response_msg.msg_type = RESPONSE_TOPO_INFO;
	response_msg.data     = topo_resp_msg;
	slurm_send_node_msg(msg->conn_fd, &response_msg);
	slurm_free_topo_info_msg(topo_resp_msg);
}

inline static void  _slurm_rpc_job_notify(slurm_msg_t * msg)
{
	int error_code;
	/* Locks: read job */
	slurmctld_lock_t job_read_lock = {
		NO_LOCK, READ_LOCK, NO_LOCK, NO_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);
	job_notify_msg_t * notify_msg = (job_notify_msg_t *) msg->data;
	struct job_record *job_ptr;
	DEF_TIMERS;

	START_TIMER;
	debug("Processing RPC: REQUEST_JOB_NOTIFY from uid=%d", uid);

	/* do RPC call */
	lock_slurmctld(job_read_lock);
	job_ptr = find_job_record(notify_msg->job_id);
	if (!job_ptr)
		error_code = ESLURM_INVALID_JOB_ID;
	else if ((job_ptr->user_id == uid) || validate_slurm_user(uid))
		error_code = srun_user_message(job_ptr, notify_msg->message);
	else {
		error_code = ESLURM_USER_ID_MISSING;
		error("Security violation, REQUEST_JOB_NOTIFY RPC "
		      "from uid=%d for jobid %u owner %d",
		      uid, notify_msg->job_id, job_ptr->user_id);
	}
	unlock_slurmctld(job_read_lock);

	END_TIMER2("_slurm_rpc_job_notify");
	slurm_send_rc_msg(msg, error_code);
}

inline static void  _slurm_rpc_set_debug_flags(slurm_msg_t *msg)
{
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);
	slurmctld_lock_t config_write_lock =
		{ WRITE_LOCK, NO_LOCK, NO_LOCK, NO_LOCK };
	set_debug_flags_msg_t *request_msg =
		(set_debug_flags_msg_t *) msg->data;
	uint64_t debug_flags;
	char *flag_string;

	debug2("Processing RPC: REQUEST_SET_DEBUG_FLAGS from uid=%d", uid);
	if (!validate_super_user(uid)) {
		error("set debug flags request from non-super user uid=%d",
		      uid);
		slurm_send_rc_msg(msg, EACCES);
		return;
	}

	lock_slurmctld (config_write_lock);
	debug_flags  = slurmctld_conf.debug_flags;
	debug_flags &= (~request_msg->debug_flags_minus);
	debug_flags |= request_msg->debug_flags_plus;
	slurm_set_debug_flags(debug_flags);
	slurmctld_conf.last_update = time(NULL);

	/* Reset cached debug_flags values */
	log_set_debug_flags();
	gs_reconfig();
	gres_plugin_reconfig(NULL);
	acct_storage_g_reconfig(acct_db_conn, 0);
	priority_g_reconfig(false);
	select_g_reconfigure();
	(void) slurm_sched_g_reconfig();
	(void) switch_g_reconfig();

	unlock_slurmctld (config_write_lock);
	flag_string = debug_flags2str(debug_flags);
	info("Set DebugFlags to %s", flag_string ? flag_string : "none");
	xfree(flag_string);
	slurm_send_rc_msg(msg, SLURM_SUCCESS);
}

inline static void  _slurm_rpc_set_debug_level(slurm_msg_t *msg)
{
	int debug_level, old_debug_level;
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);
	slurmctld_lock_t config_write_lock =
		{ WRITE_LOCK, NO_LOCK, NO_LOCK, NO_LOCK };
	set_debug_level_msg_t *request_msg =
		(set_debug_level_msg_t *) msg->data;
	log_options_t log_opts = LOG_OPTS_INITIALIZER;
	slurm_ctl_conf_t *conf;

	debug2("Processing RPC: REQUEST_SET_DEBUG_LEVEL from uid=%d", uid);
	if (!validate_super_user(uid)) {
		error("set debug level request from non-super user uid=%d",
		      uid);
		slurm_send_rc_msg(msg, EACCES);
		return;
	}

	/* NOTE: not offset by LOG_LEVEL_INFO, since it's inconveniet
	 * to provide negative values for scontrol */
	debug_level = MIN (request_msg->debug_level, (LOG_LEVEL_END - 1));
	debug_level = MAX (debug_level, LOG_LEVEL_QUIET);

	lock_slurmctld (config_write_lock);
	if (slurmctld_config.daemonize) {
		log_opts.stderr_level = LOG_LEVEL_QUIET;
		if (slurmctld_conf.slurmctld_logfile) {
			log_opts.logfile_level = debug_level;
			log_opts.syslog_level = LOG_LEVEL_QUIET;
		} else {
			log_opts.syslog_level = debug_level;
			log_opts.logfile_level = LOG_LEVEL_QUIET;
		}
	} else {
		log_opts.syslog_level = LOG_LEVEL_QUIET;
		log_opts.stderr_level = debug_level;
		if (slurmctld_conf.slurmctld_logfile)
			log_opts.logfile_level = debug_level;
		else
			log_opts.logfile_level = LOG_LEVEL_QUIET;
	}
	log_alter(log_opts, LOG_DAEMON, slurmctld_conf.slurmctld_logfile);
	unlock_slurmctld (config_write_lock);

	conf = slurm_conf_lock();
	old_debug_level = conf->slurmctld_debug;
	conf->slurmctld_debug = debug_level;
	slurm_conf_unlock();
	slurmctld_conf.last_update = time(NULL);
	if (debug_level != old_debug_level)
		info("Set debug level to %d", debug_level);

	slurm_send_rc_msg(msg, SLURM_SUCCESS);
}

inline static void  _slurm_rpc_set_schedlog_level(slurm_msg_t *msg)
{
	int schedlog_level, old_schedlog_level;
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);
	slurmctld_lock_t config_read_lock =
		{ READ_LOCK, NO_LOCK, NO_LOCK, NO_LOCK };
	set_debug_level_msg_t *request_msg =
		(set_debug_level_msg_t *) msg->data;
	log_options_t log_opts = SCHEDLOG_OPTS_INITIALIZER;
	slurm_ctl_conf_t *conf;

	debug2("Processing RPC: REQUEST_SET_SCHEDLOG_LEVEL from uid=%d", uid);
	if (!validate_super_user(uid)) {
		error("set scheduler log level request from non-super user "
		      "uid=%d", uid);
		slurm_send_rc_msg(msg, EACCES);
		return;
	}

	/*
	 * If slurmctld_conf.sched_logfile is NULL, then this operation
	 *  will fail, since there is no sched logfile for which to alter
	 *  the log level. (Calling sched_log_alter with a NULL filename
	 *  is likely to cause a segfault at the next sched log call)
	 *  So just give up and return "Operation Disabled"
	 */
	if (slurmctld_conf.sched_logfile == NULL) {
		error("set scheduler log level failed: no log file!");
		slurm_send_rc_msg (msg, ESLURM_DISABLED);
		return;
	}

	schedlog_level = MIN (request_msg->debug_level, (LOG_LEVEL_QUIET + 1));
	schedlog_level = MAX (schedlog_level, LOG_LEVEL_QUIET);

	lock_slurmctld (config_read_lock);
	log_opts.logfile_level = schedlog_level;
	sched_log_alter(log_opts, LOG_DAEMON, slurmctld_conf.sched_logfile);
 	unlock_slurmctld (config_read_lock);

	conf = slurm_conf_lock();
	old_schedlog_level = conf->sched_log_level;
	conf->sched_log_level = schedlog_level;
	slurm_conf_unlock();
	slurmctld_conf.last_update = time(NULL);
	if (schedlog_level != old_schedlog_level)
		info("sched: Set scheduler log level to %d", schedlog_level);

	slurm_send_rc_msg(msg, SLURM_SUCCESS);
}

inline static void  _slurm_rpc_accounting_update_msg(slurm_msg_t *msg)
{
	int rc = SLURM_SUCCESS;
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);
	accounting_update_msg_t *update_ptr =
		(accounting_update_msg_t *) msg->data;
	DEF_TIMERS;

	START_TIMER;
	debug2("Processing RPC: ACCOUNTING_UPDATE_MSG from uid=%d", uid);

	if (!validate_super_user(uid)) {
		error("Update Association request from non-super user uid=%d",
		      uid);
		slurm_send_rc_msg(msg, EACCES);
		return;
	}

	/* Send message back to the caller letting him know we got it.
	   There is no need to wait since the end result would be the
	   same if we wait or not since the update has already
	   happened in the database.
	*/

	slurm_send_rc_msg(msg, rc);

	if (update_ptr->update_list && list_count(update_ptr->update_list))
		rc = assoc_mgr_update(update_ptr->update_list);

	END_TIMER2("_slurm_rpc_accounting_update_msg");

	if (rc != SLURM_SUCCESS)
		error("assoc_mgr_update gave error: %s", slurm_strerror(rc));
}

/* _slurm_rpc_reboot_nodes - process RPC to schedule nodes reboot */
inline static void _slurm_rpc_reboot_nodes(slurm_msg_t * msg)
{
	int rc;
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);
#ifndef HAVE_FRONT_END
	int i;
	struct node_record *node_ptr;
	reboot_msg_t *reboot_msg = (reboot_msg_t *)msg->data;
	char *nodelist = NULL;
	bitstr_t *bitmap = NULL;
	/* Locks: write node lock */
	slurmctld_lock_t node_write_lock = {
		NO_LOCK, NO_LOCK, WRITE_LOCK, NO_LOCK };
#endif
	DEF_TIMERS;

	START_TIMER;
	debug2("Processing RPC: REQUEST_REBOOT_NODES from uid=%d", uid);
	if (!validate_super_user(uid)) {
		error("Security violation, REBOOT_NODES RPC from uid=%d", uid);
		slurm_send_rc_msg(msg, EACCES);
		return;
	}
#ifdef HAVE_FRONT_END
	rc = ESLURM_NOT_SUPPORTED;
#else
	/* do RPC call */
	if (reboot_msg)
		nodelist = reboot_msg->node_list;
	if (!nodelist || !strcasecmp(nodelist, "ALL")) {
		bitmap = bit_alloc(node_record_count);
		bit_nset(bitmap, 0, (node_record_count - 1));
	} else if (node_name2bitmap(nodelist, false, &bitmap) != 0) {
		FREE_NULL_BITMAP(bitmap);
		error("Invalid node list in REBOOT_NODES request");
		slurm_send_rc_msg(msg, ESLURM_INVALID_NODE_NAME);
		return;
	}

	lock_slurmctld(node_write_lock);
	for (i = 0, node_ptr = node_record_table_ptr;
	     i < node_record_count; i++, node_ptr++) {
		if (bit_test(bitmap, i) == 0)
			continue;
		if (IS_NODE_MAINT(node_ptr)) /* already on maintenance */
			continue;
		if (IS_NODE_FUTURE(node_ptr) || IS_NODE_DOWN(node_ptr))
			continue;
		if (IS_NODE_CLOUD(node_ptr) && IS_NODE_POWER_SAVE(node_ptr))
			continue;
		node_ptr->node_state |= NODE_STATE_MAINT;
		want_nodes_reboot = true;
	}

	if (want_nodes_reboot == true)
		schedule_node_save();

	unlock_slurmctld(node_write_lock);
	FREE_NULL_BITMAP(bitmap);
	rc = SLURM_SUCCESS;
#endif
	END_TIMER2("_slurm_rpc_reboot_nodes");
	slurm_send_rc_msg(msg, rc);
}

inline static void  _slurm_rpc_accounting_first_reg(slurm_msg_t *msg)
{
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);
	time_t event_time = time(NULL);

	DEF_TIMERS;

	START_TIMER;
	debug2("Processing RPC: ACCOUNTING_FIRST_REG from uid=%d", uid);
	if (!validate_super_user(uid)) {
		error("First Registration request from non-super user uid=%d",
		      uid);
		return;
	}

	send_all_to_accounting(event_time);

	END_TIMER2("_slurm_rpc_accounting_first_reg");
}

inline static void  _slurm_rpc_accounting_register_ctld(slurm_msg_t *msg)
{
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);

	DEF_TIMERS;

	START_TIMER;
	debug2("Processing RPC: ACCOUNTING_REGISTER_CTLD from uid=%d", uid);
	if (!validate_super_user(uid)) {
		error("Registration request from non-super user uid=%d",
		      uid);
		return;
	}

	clusteracct_storage_g_register_ctld(acct_db_conn,
					    slurmctld_conf.slurmctld_port);

	END_TIMER2("_slurm_rpc_accounting_register_ctld");
}

inline static void _slurm_rpc_dump_spank(slurm_msg_t * msg)
{
	int rc = SLURM_SUCCESS;
	spank_env_request_msg_t *spank_req_msg = (spank_env_request_msg_t *)
						 msg->data;
	spank_env_responce_msg_t *spank_resp_msg = NULL;
	/* Locks: read job */
	slurmctld_lock_t job_read_lock = {
		NO_LOCK, READ_LOCK, NO_LOCK, NO_LOCK };
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);
	slurm_msg_t response_msg;
	DEF_TIMERS;

	START_TIMER;
	debug("Processing RPC: REQUEST_SPANK_ENVIRONMENT from uid=%d JobId=%u",
	      uid, spank_req_msg->job_id);
	if (!validate_slurm_user(uid)) {
		rc = ESLURM_USER_ID_MISSING;
		error("Security violation, REQUEST_SPANK_ENVIRONMENT RPC "
		      "from uid=%d", uid);
	}

	if (rc == SLURM_SUCCESS) {
		/* do RPC call */
		struct job_record *job_ptr;
		uint32_t i;

		lock_slurmctld(job_read_lock);
		job_ptr = find_job_record(spank_req_msg->job_id);
		if (job_ptr) {
			spank_resp_msg =
				xmalloc(sizeof(spank_env_responce_msg_t));
			spank_resp_msg->spank_job_env_size =
				job_ptr->spank_job_env_size;
			spank_resp_msg->spank_job_env = xmalloc(
				spank_resp_msg->spank_job_env_size *
				sizeof(char *));
			for (i = 0; i < spank_resp_msg->spank_job_env_size; i++)
				spank_resp_msg->spank_job_env[i] = xstrdup(
					job_ptr->spank_job_env[i]);
		} else {
			rc = ESLURM_INVALID_JOB_ID;
		}
		unlock_slurmctld(job_read_lock);
	}
	END_TIMER2("_slurm_rpc_dump_spank");

	if (rc == SLURM_SUCCESS) {
		slurm_msg_t_init(&response_msg);
		response_msg.flags = msg->flags;
		response_msg.protocol_version = msg->protocol_version;
		response_msg.address  = msg->address;
		response_msg.msg_type = RESPONCE_SPANK_ENVIRONMENT;
		response_msg.data     = spank_resp_msg;
		slurm_send_node_msg(msg->conn_fd, &response_msg);
		slurm_free_spank_env_responce_msg(spank_resp_msg);
	} else {
		slurm_send_rc_msg(msg, rc);
	}
}

static void _clear_rpc_stats(void)
{
	int i;

	slurm_mutex_lock(&rpc_mutex);
	for (i = 0; i < rpc_type_size; i++) {
		rpc_type_cnt[i] = 0;
		rpc_type_id[i] = 0;
		rpc_type_time[i] = 0;
	}
	for (i = 0; i < rpc_user_size; i++) {
		rpc_user_cnt[i] = 0;
		rpc_user_id[i] = 0;
		rpc_user_time[i] = 0;
	}
	slurm_mutex_unlock(&rpc_mutex);
}

static void _pack_rpc_stats(int resp, char **buffer_ptr, int *buffer_size,
			    uint16_t protocol_version)
{
	uint32_t i;
	Buf buffer;

	if (protocol_version < SLURM_14_11_PROTOCOL_VERSION)
		return;

	slurm_mutex_lock(&rpc_mutex);
	buffer = create_buf(*buffer_ptr, *buffer_size);
	set_buf_offset(buffer, *buffer_size);
	for (i = 0; i < rpc_type_size; i++) {
		if (rpc_type_id[i] == 0)
			break;
	}
	pack32(i, buffer);
	pack16_array(rpc_type_id,   i, buffer);
	pack32_array(rpc_type_cnt,  i, buffer);
	pack64_array(rpc_type_time, i, buffer);

	for (i = 1; i < rpc_user_size; i++) {
		if (rpc_user_id[i] == 0)
			break;
	}
	pack32(i, buffer);
	pack32_array(rpc_user_id,   i, buffer);
	pack32_array(rpc_user_cnt,  i, buffer);
	pack64_array(rpc_user_time, i, buffer);
	slurm_mutex_unlock(&rpc_mutex);

	*buffer_size = get_buf_offset(buffer);
	buffer_ptr[0] = xfer_buf_data(buffer);
}

/* _slurm_rpc_dump_stats - process RPC for statistics information */
inline static void _slurm_rpc_dump_stats(slurm_msg_t * msg)
{
	char *dump;
	int dump_size;
	stats_info_request_msg_t *request_msg;
	slurm_msg_t response_msg;
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);

	request_msg = (stats_info_request_msg_t *)msg->data;

	if ((request_msg->command_id == STAT_COMMAND_RESET) &&
	    !validate_slurm_user(uid)) {
		error("Security violation: REQUEST_STATS_INFO reset "
		      "from uid=%d", uid);
		slurm_send_rc_msg(msg, ESLURM_ACCESS_DENIED);
		return;
	}

	debug2("Processing RPC: REQUEST_STATS_INFO (command: %u)",
	       request_msg->command_id);

	slurm_msg_t_init(&response_msg);
	response_msg.protocol_version = msg->protocol_version;
	response_msg.address = msg->address;
	response_msg.msg_type = RESPONSE_STATS_INFO;

	if (request_msg->command_id == STAT_COMMAND_RESET) {
		reset_stats(1);
		_clear_rpc_stats();
		pack_all_stat(0, &dump, &dump_size, msg->protocol_version);
		_pack_rpc_stats(0, &dump, &dump_size, msg->protocol_version);
		response_msg.data = dump;
		response_msg.data_size = dump_size;
	} else {
		pack_all_stat(1, &dump, &dump_size, msg->protocol_version);
		_pack_rpc_stats(1, &dump, &dump_size, msg->protocol_version);
		response_msg.data = dump;
		response_msg.data_size = dump_size;
	}

	/* send message */
	slurm_send_node_msg(msg->conn_fd, &response_msg);
	xfree(dump);
}

/* _slurm_rpc_dump_licenses()
 *
 * Pack the io buffer and send it back to the library.
 */
inline static void
_slurm_rpc_dump_licenses(slurm_msg_t * msg)
{
	DEF_TIMERS;
	char *dump;
	int dump_size;
	slurm_msg_t response_msg;
	license_info_request_msg_t  *lic_req_msg;
	uid_t uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);

	START_TIMER;
	debug2("%s: Processing RPC: REQUEST_LICENSE_INFO uid=%d",
	       __func__, uid);
	lic_req_msg = (license_info_request_msg_t *)msg->data;

	if ((lic_req_msg->last_update - 1) >= last_license_update) {
		/* Dont send unnecessary data
		 */
		debug2("%s: no change SLURM_NO_CHANGE_IN_DATA", __func__);
		slurm_send_rc_msg(msg, SLURM_NO_CHANGE_IN_DATA);

		return;
	}

	get_all_license_info(&dump, &dump_size, uid, msg->protocol_version);

	END_TIMER2("_slurm_rpc_dump_licenses");
	debug2("%s: size=%d %s", __func__, dump_size, TIME_STR);

	/* init response_msg structure
	 */
	slurm_msg_t_init(&response_msg);

	response_msg.flags = msg->flags;
	response_msg.protocol_version = msg->protocol_version;
	response_msg.address = msg->address;
	response_msg.msg_type = RESPONSE_LICENSE_INFO;
	response_msg.data = dump;
	response_msg.data_size = dump_size;

	/* send message
	 */
	slurm_send_node_msg(msg->conn_fd, &response_msg);
	xfree(dump);
	/* Ciao!
	 */
}

/* Free memory used to track RPC usage by type and user */
extern void free_rpc_stats(void)
{
	slurm_mutex_lock(&rpc_mutex);
	xfree(rpc_type_cnt);
	xfree(rpc_type_id);
	xfree(rpc_type_time);
	rpc_type_size = 0;

	xfree(rpc_user_cnt);
	xfree(rpc_user_id);
	xfree(rpc_user_time);
	rpc_user_size = 0;
	slurm_mutex_unlock(&rpc_mutex);
}

/* _slurm_rpc_kill_job2()
 */
inline static void
_slurm_rpc_kill_job2(slurm_msg_t *msg)
{
	DEF_TIMERS;
	job_step_kill_msg_t *kill;
	slurmctld_lock_t lock = {READ_LOCK, WRITE_LOCK,
				 WRITE_LOCK, NO_LOCK };
	uid_t uid;
	int cc;

	kill = 	(job_step_kill_msg_t *)msg->data;
	uid = g_slurm_auth_get_uid(msg->auth_cred, NULL);

	START_TIMER;
	debug("%s: REQUEST_KILL_JOB job %s uid %d",
	      __func__, kill->sjob_id, uid);

	lock_slurmctld(lock);

	cc = job_str_signal(kill->sjob_id,
			    kill->signal,
			    kill->flags,
			    uid,
			    0);
	if (cc == ESLURM_ALREADY_DONE) {
		debug2("%s: job_str_signal() job %s sig %d returned %s",
		       __func__, kill->sjob_id,
		       kill->signal, slurm_strerror(cc));
	} else if (cc != SLURM_SUCCESS) {
		error("%s: job_str_signal() job %s sig %d returned %s",
		      __func__, kill->sjob_id,
		      kill->signal, slurm_strerror(cc));
	}

	slurm_send_rc_msg(msg, cc);

	unlock_slurmctld(lock);
	END_TIMER2("_slurm_rpc_kill_job2");
}
