/*****************************************************************************\
 *  job_submit_pbs.c - Translate PBS job options specifications to the Slurm
 *			equivalents, particularly job dependencies.
 *****************************************************************************
 *  Copyright (C) 2013 SchedMD LLC.
 *  Written by Morris Jette <jette@schedmd.com>
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

#if HAVE_CONFIG_H
#   include "config.h"
#endif

#if HAVE_STDINT_H
#  include <stdint.h>
#endif

#if HAVE_INTTYPES_H
#  include <inttypes.h>
#endif

#include <stdio.h>

#include <sys/types.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <pthread.h>

#include "slurm/slurm.h"
#include "slurm/slurm_errno.h"

#include "src/common/slurm_xlator.h"
#include "src/slurmctld/slurmctld.h"

#define _DEBUG 0

/*
 * These variables are required by the generic plugin interface.  If they
 * are not found in the plugin, the plugin loader will ignore it.
 *
 * plugin_name - a string giving a human-readable description of the
 * plugin.  There is no maximum length, but the symbol must refer to
 * a valid string.
 *
 * plugin_type - a string suggesting the type of the plugin or its
 * applicability to a particular form of data or method of data handling.
 * If the low-level plugin API is used, the contents of this string are
 * unimportant and may be anything.  SLURM uses the higher-level plugin
 * interface which requires this string to be of the form
 *
 *	<application>/<method>
 *
 * where <application> is a description of the intended application of
 * the plugin (e.g., "auth" for SLURM authentication) and <method> is a
 * description of how this plugin satisfies that application.  SLURM will
 * only load authentication plugins if the plugin_type string has a prefix
 * of "auth/".
 *
 * plugin_version   - specifies the version number of the plugin.
 * min_plug_version - specifies the minumum version number of incoming
 *                    messages that this plugin can accept
 */
const char plugin_name[]       	= "Job submit PBS plugin";
const char plugin_type[]       	= "job_submit/pbs";
const uint32_t plugin_version   = 100;
const uint32_t min_plug_version = 100;

int init (void)
{
	return SLURM_SUCCESS;
}

int fini (void)
{
	return SLURM_SUCCESS;
}

/* Translate PBS job dependencies to Slurm equivalents to the exptned possible
 *
 * PBS option		Slurm nearest equivalent
 * ===========		========================
 * after		after
 * afterok		afterok
 * afternotok		afternotok
 * afterany		after
 * before		N/A
 * beforeok		N/A
 * beforenotok		N/A
 * beforeany		N/A
 * N/A			expand
 * on			N/A
 * N/A			singleton
 */
static void _xlate_dependency(struct job_descriptor *job_desc)
{
	char *result = NULL;
	char *last_ptr = NULL, *tok;

	if (!job_desc->dependency)
		return;

#if _DEBUG
	info("dependency  in:%s", job_desc->dependency);
#endif

	tok = strtok_r(job_desc->dependency, ",", &last_ptr);
	while (tok) {
		if (!strncmp(tok, "after", 5)  ||
		    !strncmp(tok, "expand", 6) ||
		    !strncmp(tok, "singleton", 9)) {
			if (result)
				xstrcat(result, ",");
			xstrcat(result, tok);
		} else {
			info("%s: discarding job dependency option %s",
			     plugin_type, tok);
		}
		tok = strtok_r(NULL, ",", &last_ptr);
	}
#if _DEBUG
	info("dependency out:%s", result);
#endif
	xfree(job_desc->dependency);
	job_desc->dependency = result;
}

extern int job_submit(struct job_descriptor *job_desc, uint32_t submit_uid)
{
	_xlate_dependency(job_desc);
	return SLURM_SUCCESS;
}

/* Lua script hook called for "modify job" event. */
extern int job_modify(struct job_descriptor *job_desc,
		      struct job_record *job_ptr, uint32_t submit_uid)
{
	_xlate_dependency(job_desc);
	return SLURM_SUCCESS;
}