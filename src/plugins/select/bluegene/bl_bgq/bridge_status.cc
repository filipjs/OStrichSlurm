/*****************************************************************************\
 *  bridge_status.cc
 *
 *****************************************************************************
 *  Copyright (C) 2011 Lawrence Livermore National Security.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Danny Auble <da@llnl.gov>
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

extern "C" {
#include "../ba_bgq/block_allocator.h"
#include "../bg_core.h"
#include "../bg_status.h"
#include "src/slurmctld/proc_req.h"
#include "src/slurmctld/trigger_mgr.h"
#include "src/slurmctld/locks.h"
}

#if defined HAVE_BG_FILES

#include <bgsched/bgsched.h>
#include <bgsched/Block.h>
#include <bgsched/NodeBoard.h>
#include <bgsched/Hardware.h>
#include <bgsched/core/core.h>
#include <boost/foreach.hpp>
#include <bgsched/realtime/Client.h>
#include <bgsched/realtime/ClientConfiguration.h>
#include <bgsched/realtime/ClientEventListener.h>
#include <bgsched/realtime/Filter.h>
#include "bridge_status.h"

#include <iostream>

using namespace std;
using namespace bgsched;
using namespace bgsched::core;
using namespace bgsched::realtime;
#endif

static bool bridge_status_inited = false;

#if defined HAVE_BG_FILES

/*
 * Handle compute block status changes as a result of a block allocate.
 */
typedef class event_handler: public bgsched::realtime::ClientEventListener {
public:
	/*
	 *  Handle a block state changed real-time event.
	 */
	void handleBlockStateChangedRealtimeEvent(
		const BlockStateChangedEventInfo& event);

	/*
	 *  Handle a midplane state changed real-time event.
	 */
	virtual void handleMidplaneStateChangedRealtimeEvent(
		const MidplaneStateChangedEventInfo& event);

	/*
	 * Handle a switch state changed real-time event.
	 */
	virtual void handleSwitchStateChangedRealtimeEvent(
		const SwitchStateChangedEventInfo& event);

	/*
	 * Handle a node board state changed real-time event.
	 */
	virtual void handleNodeBoardStateChangedRealtimeEvent(
		const NodeBoardStateChangedEventInfo& event);

	/*
	 * Handle a cable state changed real-time event.
	 */
	virtual void handleTorusCableStateChangedRealtimeEvent(
		const TorusCableStateChangedEventInfo& event);

} event_handler_t;

static List kill_job_list = NULL;
static pthread_t real_time_thread;
static pthread_t poll_thread;
static bgsched::realtime::Client *rt_client_ptr = NULL;
pthread_mutex_t rt_mutex = PTHREAD_MUTEX_INITIALIZER;

static void _handle_bad_midplane(const char *mp_coords,
				 EnumWrapper<Hardware::State> state)
{
	char bg_down_node[128];

	assert(mp_coords);

	snprintf(bg_down_node, sizeof(bg_down_node), "%s%s",
		 bg_conf->slurm_node_prefix, mp_coords);

	if (!node_already_down(bg_down_node)) {
		error("Midplane %s, state went to %d, marking midplane down.",
		      bg_down_node, state.toValue());
		slurm_drain_nodes(
			bg_down_node,
			(char *)"select_bluegene: MMCS midplane not UP",
			slurm_get_slurm_user_id());
	}
}

static void _handle_bad_switch(int dim, const char *mp_coords,
			       EnumWrapper<Hardware::State> state)
{
	char bg_down_node[128];

	assert(mp_coords);

	snprintf(bg_down_node, sizeof(bg_down_node), "%s%s",
		 bg_conf->slurm_node_prefix, mp_coords);

	if (!node_already_down(bg_down_node)) {
		error("Switch at dim '%d' on Midplane %s, state went to %d, "
		      "marking midplane down.",
		      dim, bg_down_node, state.toValue());
		slurm_drain_nodes(bg_down_node,
				  (char *)"select_bluegene: MMCS switch not UP",
				  slurm_get_slurm_user_id());
	}
}

static void _handle_bad_nodeboard(const char *nb_name, const char* mp_coords,
				  EnumWrapper<Hardware::State> state)
{
	char bg_down_node[128];
	int io_start;

	assert(nb_name);
	assert(mp_coords);

	/* From the first nodecard id we can figure
	   out where to start from with the alloc of ionodes.
	*/
	io_start = atoi((char*)nb_name+1);
	io_start *= bg_conf->io_ratio;

	/* On small systems with less than a midplane the
	   database may see the nodecards there but in missing
	   state.  To avoid getting a bunch of warnings here just
	   skip over the ones missing.
	*/
	if (io_start >= bg_conf->ionodes_per_mp) {
		if (state == Hardware::Missing)
			debug3("Nodeboard %s is missing",
			       nb_name);
		else
			error("We don't have the system configured "
			      "for this nodecard %s, we only have "
			      "%d ionodes and this starts at %d",
			      nb_name, bg_conf->ionodes_per_mp, io_start);
		return;
	}

	/* if (!ionode_bitmap) */
	/* 	ionode_bitmap = bit_alloc(bg_conf->ionodes_per_mp); */
	/* info("setting %s start %d of %d", */
	/*      nb_name,  io_start, bg_conf->ionodes_per_mp); */
	/* bit_nset(ionode_bitmap, io_start, io_start+io_cnt); */

	/* we have to handle each nodecard separately to make
	   sure we don't create holes in the system */
	snprintf(bg_down_node, sizeof(bg_down_node), "%s%s",
		 bg_conf->slurm_node_prefix, mp_coords);

	if (down_nodecard(bg_down_node, io_start, 0) == SLURM_SUCCESS)
		debug("nodeboard %s on %s is in an error state (%d)",
		      nb_name, bg_down_node, state.toValue());
	else
		debug2("nodeboard %s on %s is in an error state (%d), "
		       "but error was returned when trying to make it so",
		       nb_name, bg_down_node, state.toValue());
	return;
}

static void _handle_cable_change(int dim, ba_mp_t *ba_mp,
				 EnumWrapper<Hardware::State> state)
{
	if (state == Hardware::Available) {
		/* no change */
		if (!(ba_mp->axis_switch[dim].usage & BG_SWITCH_CABLE_ERROR))
			return;
		ba_mp->axis_switch[dim].usage &= (~BG_SWITCH_CABLE_ERROR_SET);
		info("Cable in dim '%u' on Midplane %s, "
		     "has returned to service",
		     dim, ba_mp->coord_str);
		/* Don't resume any blocks in the error, Admins will
		   do this when they make sure it is ready.  Really
		   only matters for static blocks.  On a dynamic
		   system no block will be left around if a cable is bad.
		*/
	} else if (!(ba_mp->axis_switch[dim].usage & BG_SWITCH_CABLE_ERROR)) {
		bg_record_t *bg_record = NULL, *smallest_bg_record = NULL;
		ListIterator itr;
		List delete_list = NULL;
		ba_mp_t *next_ba_mp = ba_mp->next_mp[dim];
		bool delete_it = 0;
		ba_mp->axis_switch[dim].usage |= BG_SWITCH_CABLE_ERROR_SET;
		error("Cable at dim '%d' on Midplane %s, "
		      "state went to %d, marking cable down.",
		      dim, ba_mp->coord_str, state.toValue());

		/* Now handle potential overlapping blocks. */
		if (bg_conf->layout_mode == LAYOUT_DYNAMIC)
			delete_it = 1;

		slurm_mutex_lock(&block_state_mutex);
		delete_list = list_create(NULL);
		itr = list_iterator_create(bg_lists->main);
		while ((bg_record = (bg_record_t *)list_next(itr))) {
			if (bg_record->mp_count == 1)
				continue;
			if (!bit_test(bg_record->mp_bitmap, ba_mp->index))
				continue;
			if (!bit_test(bg_record->mp_bitmap, next_ba_mp->index))
				continue;
			/* This block uses the wire so we need to take
			 * care of it.  We only need to put one block
			 * in an error, so pick the smallest one.
			 */
			if ((bg_conf->layout_mode != LAYOUT_DYNAMIC)
			    && (!smallest_bg_record
				|| (smallest_bg_record->mp_count
				    > bg_record->mp_count)))
				smallest_bg_record = bg_record;

			list_push(delete_list, bg_record);
		}
		list_iterator_destroy(itr);
		slurm_mutex_unlock(&block_state_mutex);

		free_block_list(NO_VAL, delete_list, delete_it, 0);
		list_destroy(delete_list);

		if (smallest_bg_record) {
			char reason[200];
			snprintf(reason, sizeof(reason),
				 "Cable going from %s -> %s went into "
				 "an error state (%d).", ba_mp->coord_str,
				 next_ba_mp->coord_str, state.toValue());
			put_block_in_error_state(smallest_bg_record, reason);
		}
	}
}


void event_handler::handleBlockStateChangedRealtimeEvent(
        const BlockStateChangedEventInfo& event)
{
	bg_record_t *bg_record = NULL;
	const char *bg_block_id = event.getBlockName().c_str();

	if (!bg_lists->main)
		return;

	slurm_mutex_lock(&block_state_mutex);
	bg_record = find_bg_record_in_list(bg_lists->main, bg_block_id);
	if (!bg_record) {
		slurm_mutex_unlock(&block_state_mutex);
		info("bridge_status: bg_record %s isn't in the main list",
		     bg_block_id);
		return;
	}

	bg_status_update_block_state(bg_record,
				     bridge_translate_status(event.getStatus()),
				     kill_job_list);

	slurm_mutex_unlock(&block_state_mutex);

	bg_status_process_kill_job_list(kill_job_list);

	last_bg_update = time(NULL);
}

void event_handler::handleMidplaneStateChangedRealtimeEvent(
	const MidplaneStateChangedEventInfo& event)
{
	Coordinates ibm_coords = event.getMidplaneCoordinates();
	uint16_t coords[SYSTEM_DIMENSIONS];
	ba_mp_t *ba_mp;
	int dim;

	for (dim = 0; dim < SYSTEM_DIMENSIONS; dim++)
		coords[dim] = ibm_coords[dim];

	ba_mp = coord2ba_mp(coords);

	if (!ba_mp) {
		error("Midplane %s, state went from %d to %d, "
		      "but is not in our system",
		      event.getLocation().c_str(),
		      event.getPreviousState(),
		      event.getState());
	}

	if (event.getState() == Hardware::Available) {
		/* Don't do anything, wait for admin to fix things,
		 * just note things are better. */

		info("Midplane %s(%s), has returned to service",
		     event.getLocation().c_str(),
		     ba_mp->coord_str);
		return;
	}

	/* Else mark the midplane down */
	_handle_bad_midplane(ba_mp->coord_str, event.getState());

	return;

}

void event_handler::handleSwitchStateChangedRealtimeEvent(
	const SwitchStateChangedEventInfo& event)
{
	Coordinates ibm_coords = event.getMidplaneCoordinates();
	uint16_t coords[SYSTEM_DIMENSIONS];
	int dim;
	ba_mp_t *ba_mp;


	for (dim = 0; dim < SYSTEM_DIMENSIONS; dim++)
		coords[dim] = ibm_coords[dim];

	dim = event.getDimension();
	ba_mp = coord2ba_mp(coords);

	if (!ba_mp) {
		error("Switch in dim '%d' on Midplane %s, state "
		      "went from %d to %d, but is not in our system",
		      dim, event.getMidplaneLocation().c_str(),
		      event.getPreviousState(),
		      event.getState());
	}

	if (event.getState() == Hardware::Available) {
		/* Don't do anything, wait for admin to fix things,
		 * just note things are better. */

		info("Switch in dim '%u' on Midplane %s(%s), "
		     "has returned to service",
		     dim, event.getMidplaneLocation().c_str(),
		     ba_mp->coord_str);
		return;
	}

	/* Else mark the midplane down */
	_handle_bad_switch(dim, ba_mp->coord_str, event.getState());

	return;
}

void event_handler::handleNodeBoardStateChangedRealtimeEvent(
	const NodeBoardStateChangedEventInfo& event)
{
	const char *mp_name = event.getLocation().substr(0,6).c_str();
	const char *nb_name = event.getLocation().substr(7,3).c_str();
	Coordinates ibm_coords = event.getMidplaneCoordinates();
	uint16_t coords[SYSTEM_DIMENSIONS];
	int dim;
	ba_mp_t *ba_mp;

	for (dim = 0; dim < SYSTEM_DIMENSIONS; dim++)
		coords[dim] = ibm_coords[dim];

	ba_mp = coord2ba_mp(coords);

	if (!ba_mp) {
		error("Nodeboard '%s' on Midplane %s, state went from %d to %d,"
		      "but is not in our system",
		      nb_name, mp_name,
		      event.getPreviousState(),
		      event.getState());
	}

	if (event.getState() == Hardware::Available) {
		/* Don't do anything, wait for admin to fix things,
		 * just note things are better. */

		info("Nodeboard '%s' on Midplane %s(%s), "
		     "has returned to service",
		     nb_name, mp_name,
		     ba_mp->coord_str);
		return;
	}

	_handle_bad_nodeboard(nb_name, ba_mp->coord_str, event.getState());

	return;
}

void event_handler::handleTorusCableStateChangedRealtimeEvent(
	const TorusCableStateChangedEventInfo& event)
{
	Coordinates ibm_coords = event.getFromMidplaneCoordinates();
	uint16_t coords[SYSTEM_DIMENSIONS];
	int dim;
	ba_mp_t *from_ba_mp;


	for (dim = 0; dim < SYSTEM_DIMENSIONS; dim++)
		coords[dim] = ibm_coords[dim];

	dim = event.getDimension();
	from_ba_mp = coord2ba_mp(coords);
	if (!from_ba_mp) {
		error("Cable in dim '%d' on Midplane %s, state "
		      "went from %d to %d, but is not in our system",
		      dim, event.getFromMidplaneLocation().c_str(),
		      event.getPreviousState(),
		      event.getState());
	}

	/* Else mark the midplane down */
	_handle_cable_change(dim, from_ba_mp, event.getState());

	return;
}


static int _real_time_connect(void)
{
	int rc = SLURM_ERROR;
	int count = 0;
	int sleep_value = 5;

	while (bridge_status_inited && (rc != SLURM_SUCCESS)) {
		try {
			rt_client_ptr->connect();
			rc = SLURM_SUCCESS;
		} catch (...) {
			rc = SLURM_ERROR;
			error("couldn't connect to the real_time server, "
			      "trying for %d seconds.", count * sleep_value);
			sleep(sleep_value);
			count++;
		}
	}

	return rc;
}

static void *_real_time(void *no_data)
{
	event_handler_t event_hand;
	int rc = SLURM_SUCCESS;
	bool failed = false;
	Filter::BlockStatuses block_statuses;
  	Filter rt_filter(Filter::createNone());

	rt_filter.setNodeBoards(true);
	rt_filter.setSwitches(true);
	rt_filter.setBlocks(true);

	rt_filter.setMidplanes(true);
	rt_filter.setTorusCables(true);

	block_statuses.insert(Block::Free);
	block_statuses.insert(Block::Booting);
	block_statuses.insert(Block::Initialized);
	block_statuses.insert(Block::Terminating);
	rt_filter.setBlockStatuses(&block_statuses);

	rt_client_ptr->addListener(event_hand);

	rc = _real_time_connect();

	while (bridge_status_inited) {
		bgsched::realtime::Filter::Id filter_id; // Assigned filter id

		slurm_mutex_lock(&rt_mutex);
		if (!bridge_status_inited) {
			slurm_mutex_unlock(&rt_mutex);
			break;
		}

		if (rc == SLURM_SUCCESS) {
			rt_client_ptr->setFilter(rt_filter, &filter_id, NULL);
			rt_client_ptr->requestUpdates(NULL);
			rt_client_ptr->receiveMessages(NULL, NULL, &failed);
		} else
			failed = true;

		slurm_mutex_unlock(&rt_mutex);

		if (bridge_status_inited && failed) {
			error("Disconnected from real-time events. "
			      "Will try to reconnect.");
			rc = _real_time_connect();
			if (rc == SLURM_SUCCESS) {
				info("real-time server connected again");
				failed = false;
			}
		}
	}
	return NULL;
}

static void _do_block_poll(void)
{
	bg_record_t *bg_record;
	ListIterator itr;
	int updated = 0;

	if (!bg_lists->main)
		return;

	slurm_mutex_lock(&block_state_mutex);
	itr = list_iterator_create(bg_lists->main);
	while ((bg_record = (bg_record_t *) list_next(itr))) {
		BlockFilter filter;
		Block::Ptrs vec;

		if ((bg_record->magic != BLOCK_MAGIC)
		    || !bg_record->bg_block_id)
			continue;

		filter.setName(string(bg_record->bg_block_id));

		vec = getBlocks(filter, BlockSort::AnyOrder);
		if (vec.empty()) {
			debug("block %s not found, removing "
			      "from slurm", bg_record->bg_block_id);
			list_remove(itr);
			destroy_bg_record(bg_record);
			continue;
		}
		const Block::Ptr &block_ptr = *(vec.begin());

		if (bg_status_update_block_state(
			    bg_record,
			    bridge_translate_status(
				    block_ptr->getStatus().toValue()),
			    kill_job_list))
			updated = 1;
	}
	slurm_mutex_unlock(&block_state_mutex);

	bg_status_process_kill_job_list(kill_job_list);

	if (updated == 1)
		last_bg_update = time(NULL);

}

static void _handle_midplane_update(ComputeHardware::ConstPtr bgq,
				    ba_mp_t *ba_mp)
{
	Coordinates::Coordinates coords(ba_mp->coord[A], ba_mp->coord[X],
					ba_mp->coord[Y], ba_mp->coord[Z]);
	Midplane::ConstPtr mp_ptr = bgq->getMidplane(coords);
	int i;
	Dimension dim;

	if (mp_ptr->getState() != Hardware::Available) {
		_handle_bad_midplane(ba_mp->coord_str, mp_ptr->getState());
		/* no reason to continue */
		return;
	}

	for (i=0; i<16; i++) {
		NodeBoard::ConstPtr nodeboard = mp_ptr->getNodeBoard(i);
		if (nodeboard->getState() != Hardware::Available)
			_handle_bad_nodeboard(
				nodeboard->getLocation().substr(7,3).c_str(),
				ba_mp->coord_str, nodeboard->getState());
	}

	for (dim=Dimension::A; dim<=Dimension::D; dim++) {
		Switch::ConstPtr my_switch = mp_ptr->getSwitch(dim);
		if (my_switch->getState() != Hardware::Available)
			_handle_bad_switch(dim,
					   ba_mp->coord_str,
					   my_switch->getState());
		else {
			Cable::ConstPtr my_cable = my_switch->getCable();
			_handle_cable_change(dim, ba_mp,
					     my_switch->getState());
		}
	}
}

static void _do_hardware_poll(int level, uint16_t *coords,
			      ComputeHardware::ConstPtr bgqsys)
{
	ba_mp_t *ba_mp;

	if (!ba_main_grid || (level > SYSTEM_DIMENSIONS))
		return;

	if (level < SYSTEM_DIMENSIONS) {
		for (coords[level] = 0;
		     coords[level] < DIM_SIZE[level];
		     coords[level]++) {
			/* handle the outter dims here */
			_do_hardware_poll(level+1, coords, bgqsys);
		}
		return;
	}
	ba_mp = coord2ba_mp(coords);
	if (ba_mp)
		_handle_midplane_update(bgqsys, ba_mp);
}

static void *_poll(void *no_data)
{
	event_handler_t event_hand;
	time_t last_ran = time(NULL);
	time_t curr_time;

	while (bridge_status_inited) {
		//debug("polling waiting until realtime dies");
		slurm_mutex_lock(&rt_mutex);
		if (!bridge_status_inited) {
			slurm_mutex_unlock(&rt_mutex);
			break;
		}
		//debug("polling taking over, realtime is dead");
		curr_time = time(NULL);
		_do_block_poll();
		/* only do every 30 seconds */
		if ((curr_time - 30) >= last_ran) {
			ComputeHardware::ConstPtr bgqsys = getComputeHardware();
			uint16_t coords[SYSTEM_DIMENSIONS];
			_do_hardware_poll(0, coords, bgqsys);
		}

		slurm_mutex_unlock(&rt_mutex);
		last_ran = time(NULL);
		sleep(1);
	}
	return NULL;
}

#endif

extern int bridge_status_init(void)
{
	if (bridge_status_inited)
		return SLURM_ERROR;

	bridge_status_inited = true;

#if defined HAVE_BG_FILES
	pthread_attr_t thread_attr;

	if (!kill_job_list)
		kill_job_list = bg_status_create_kill_job_list();

	rt_client_ptr = new(bgsched::realtime::Client);

	slurm_attr_init(&thread_attr);
	if (pthread_create(&real_time_thread, &thread_attr, _real_time, NULL))
		fatal("pthread_create error %m");
	slurm_attr_init(&thread_attr);
	if (pthread_create(&poll_thread, &thread_attr, _poll, NULL))
		fatal("pthread_create error %m");
	slurm_attr_destroy(&thread_attr);
#endif
	return SLURM_SUCCESS;
}

extern int bridge_status_fini(void)
{
	if (!bridge_status_inited)
		return SLURM_ERROR;

	bridge_status_inited = false;
#if defined HAVE_BG_FILES
	/* make the rt connection end. */
	rt_client_ptr->disconnect();

	if (kill_job_list) {
		list_destroy(kill_job_list);
		kill_job_list = NULL;
	}

	if (real_time_thread) {
		pthread_join(real_time_thread, NULL);
		real_time_thread = 0;
	}

	if (poll_thread) {
		pthread_join(poll_thread, NULL);
		poll_thread = 0;
	}
	pthread_mutex_destroy(&rt_mutex);
	delete(rt_client_ptr);
#endif
	return SLURM_SUCCESS;
}

/*
 * This could potentially lock the node lock in the slurmctld with
 * slurm_drain_node, so if slurmctld_locked is called we will call the
 * drainning function without locking the lock again.
 */
extern int bridge_block_check_mp_states(char *bg_block_id,
					bool slurmctld_locked)
{
	return SLURM_SUCCESS;
}
