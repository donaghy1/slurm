/*****************************************************************************\
 *  select_bluegene.c - node selection plugin for Blue Gene system.
 *****************************************************************************
 *  Copyright (C) 2004-2007 The Regents of the University of California.
 *  Copyright (C) 2008-2009 Lawrence Livermore National Security.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Dan Phung <phung4@llnl.gov> Danny Auble <da@llnl.gov>
 *  CODE-OCEC-09-009. All rights reserved.
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <https://computing.llnl.gov/linux/slurm/>.
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

#include "src/common/slurm_xlator.h"
#include "bg_core.h"
#include "bg_read_config.h"
#include "bg_defined_block.h"

#ifdef HAVE_BGQ
# include "ba_bgq/block_allocator.h"
#else
# include "ba/block_allocator.h"
#endif

//#include "src/common/uid.h"
#include "src/slurmctld/trigger_mgr.h"
#include <fcntl.h>

#define HUGE_BUF_SIZE (1024*16)

/* These are defined here so when we link with something other than
 * the slurmctld we will have these symbols defined.  They will get
 * overwritten when linking with the slurmctld.
 */
#if defined (__APPLE__)
slurm_ctl_conf_t slurmctld_conf __attribute__((weak_import));
struct node_record *node_record_table_ptr  __attribute__((weak_import)) = NULL;
int bg_recover __attribute__((weak_import)) = NOT_FROM_CONTROLLER;
List part_list  __attribute__((weak_import)) = NULL;
int node_record_count __attribute__((weak_import));
time_t last_node_update __attribute__((weak_import));
time_t last_job_update __attribute__((weak_import));
char *alpha_num  __attribute__((weak_import)) =
	"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
void *acct_db_conn  __attribute__((weak_import)) = NULL;
char *slurmctld_cluster_name  __attribute__((weak_import)) = NULL;
slurmdb_cluster_rec_t *working_cluster_rec  __attribute__((weak_import)) = NULL;
#else
slurm_ctl_conf_t slurmctld_conf;
struct node_record *node_record_table_ptr = NULL;
int bg_recover = NOT_FROM_CONTROLLER;
List part_list = NULL;
int node_record_count;
time_t last_node_update;
time_t last_job_update;
char *alpha_num = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
void *acct_db_conn = NULL;
char *slurmctld_cluster_name = NULL;
slurmdb_cluster_rec_t *working_cluster_rec = NULL;
#endif

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
 * the plugin (e.g., "select" for SLURM node selection) and <method>
 * is a description of how this plugin satisfies that application.  SLURM will
 * only load select plugins if the plugin_type string has a
 * prefix of "select/".
 *
 * plugin_version - an unsigned 32-bit integer giving the version number
 * of the plugin.  If major and minor revisions are desired, the major
 * version number may be multiplied by a suitable magnitude constant such
 * as 100 or 1000.  Various SLURM versions will likely require a certain
 * minimum version for their plugins as the node selection API matures.
 */
const char plugin_name[]       	= "BlueGene node selection plugin";
const char plugin_type[]       	= "select/bluegene";
const uint32_t plugin_id	= 100;
const uint32_t plugin_version	= 200;

/* Global variables */
bg_config_t *bg_conf = NULL;
bg_lists_t *bg_lists = NULL;
time_t last_bg_update;
pthread_mutex_t block_state_mutex = PTHREAD_MUTEX_INITIALIZER;
int blocks_are_created = 0;
int num_unused_cpus = 0;

extern int select_p_alter_node_cnt(enum select_node_cnt type, void *data);

static void _destroy_bg_config(bg_config_t *bg_conf)
{
	if (bg_conf) {
		if (bg_conf->blrts_list) {
			list_destroy(bg_conf->blrts_list);
			bg_conf->blrts_list = NULL;
		}

		xfree(bg_conf->bridge_api_file);

		xfree(bg_conf->default_blrtsimage);
		xfree(bg_conf->default_linuximage);
		xfree(bg_conf->default_mloaderimage);
		xfree(bg_conf->default_ramdiskimage);

		if (bg_conf->linux_list) {
			list_destroy(bg_conf->linux_list);
			bg_conf->linux_list = NULL;
		}

		if (bg_conf->mloader_list) {
			list_destroy(bg_conf->mloader_list);
			bg_conf->mloader_list = NULL;
		}

		if (bg_conf->ramdisk_list) {
			list_destroy(bg_conf->ramdisk_list);
			bg_conf->ramdisk_list = NULL;
		}

		xfree(bg_conf->slurm_user_name);
		xfree(bg_conf->slurm_node_prefix);
		xfree(bg_conf);
	}
}

static void _destroy_bg_lists(bg_lists_t *bg_lists)
{
	if (bg_lists) {
		if (bg_lists->booted) {
			list_destroy(bg_lists->booted);
			bg_lists->booted = NULL;
		}

		if (bg_lists->job_running) {
			list_destroy(bg_lists->job_running);
			bg_lists->job_running = NULL;
			num_unused_cpus = 0;
		}

		if (bg_lists->main) {
			list_destroy(bg_lists->main);
			bg_lists->main = NULL;
		}

		if (bg_lists->valid_small32) {
			list_destroy(bg_lists->valid_small32);
			bg_lists->valid_small32 = NULL;
		}
		if (bg_lists->valid_small64) {
			list_destroy(bg_lists->valid_small64);
			bg_lists->valid_small64 = NULL;
		}
		if (bg_lists->valid_small128) {
			list_destroy(bg_lists->valid_small128);
			bg_lists->valid_small128 = NULL;
		}
		if (bg_lists->valid_small256) {
			list_destroy(bg_lists->valid_small256);
			bg_lists->valid_small256 = NULL;
		}

		xfree(bg_lists);
	}
}

#ifdef HAVE_BG
static int _delete_old_blocks(List curr_block_list, List found_block_list)
{
	ListIterator itr_curr, itr_found;
	bg_record_t *found_record = NULL, *init_record = NULL;
	List destroy_list = list_create(NULL);

	xassert(curr_block_list);
	xassert(found_block_list);

	slurm_mutex_lock(&block_state_mutex);
	if (!bg_recover) {
		info("removing all current blocks (clean start)");
		itr_curr = list_iterator_create(curr_block_list);
		while ((init_record = list_next(itr_curr))) {
			list_remove(itr_curr);
			/* The block needs to exist in the main list
			 * just to make sure we query the state. */
			if (!(found_record = find_bg_record_in_list(
				      bg_lists->main,
				      init_record->bg_block_id)))
				list_push(bg_lists->main, init_record);
			else {
				destroy_bg_record(init_record);
				init_record = found_record;
			}
			/* Make sure this block isn't in an
			   error state since if it is it won't
			   disappear. */
			if (init_record->state & BG_BLOCK_ERROR_FLAG)
				resume_block(init_record);
			list_push(destroy_list, init_record);
		}
		list_iterator_destroy(itr_curr);
	} else {
		info("removing unspecified blocks");
		itr_curr = list_iterator_create(curr_block_list);
		while ((init_record = list_next(itr_curr))) {
			itr_found = list_iterator_create(found_block_list);
			while ((found_record = list_next(itr_found))) {
				if (!strcmp(init_record->bg_block_id,
					    found_record->bg_block_id)) {
					/* don't delete this one */
					break;
				}
			}
			list_iterator_destroy(itr_found);

			if (found_record == NULL) {
				list_remove(itr_curr);
				/* The block needs to exist in the main list
				 * just to make sure we query the state. */
				if (!(found_record = find_bg_record_in_list(
					      bg_lists->main,
					      init_record->bg_block_id)))
					list_push(bg_lists->main, init_record);
				else {
					destroy_bg_record(init_record);
					init_record = found_record;
				}
				/* Make sure this block isn't in an
				   error state since if it is it won't
				   disappear. */
				if (init_record->state & BG_BLOCK_ERROR_FLAG)
					resume_block(init_record);
				list_push(destroy_list, init_record);
			}
		}
		list_iterator_destroy(itr_curr);
	}
	slurm_mutex_unlock(&block_state_mutex);

	free_block_list(NO_VAL, destroy_list, 1, 0);
	list_destroy(destroy_list);

	return SLURM_SUCCESS;
}

static void _set_bg_lists()
{
	if (!bg_lists)
		bg_lists = xmalloc(sizeof(bg_lists_t));

	slurm_mutex_lock(&block_state_mutex);

	if (bg_lists->booted)
		list_destroy(bg_lists->booted);
	bg_lists->booted = list_create(NULL);

	if (bg_lists->job_running)
		list_destroy(bg_lists->job_running);
	bg_lists->job_running = list_create(NULL);

	if (bg_lists->main)
		list_destroy(bg_lists->main);
	bg_lists->main = list_create(destroy_bg_record);

	slurm_mutex_unlock(&block_state_mutex);

}

/*
 * _validate_config_blocks - Match slurm configuration information with
 *                           current BG block configuration.
 * IN/OUT curr_block_list -  List of blocks already existing on the system.
 * IN/OUT found_block_list - List of blocks found on the system
 *                              that are listed in the bluegene.conf.
 * NOTE: Both of the lists above should be created with list_create(NULL)
 *       since the bg_lists->main will contain the complete list of pointers
 *       and be destroyed with it.
 *
 * RET - SLURM_SUCCESS if they match, else an error
 * code. Writes bg_block_id into bg_lists->main records.
 */

static int _validate_config_blocks(List curr_block_list,
				   List found_block_list, char *dir)
{
	int rc = SLURM_ERROR;
	bg_record_t* bg_record = NULL;
	bg_record_t* init_bg_record = NULL;
	int full_created = 0;
	ListIterator itr_conf;
	ListIterator itr_curr;
	char tmp_char[256];
	int dim;

	xassert(curr_block_list);
	xassert(found_block_list);

#ifdef HAVE_BG_FILES
	/* read current bg block info into curr_block_list This
	 * happens in the state load before this in emulation mode */
	if (bridge_blocks_load_curr(curr_block_list) == SLURM_ERROR)
		return SLURM_ERROR;
	/* since we only care about error states here we don't care
	   about the return code this must be done after the bg_lists->main
	   is created */
	load_state_file(curr_block_list, dir);
#else
	/* read in state from last run. */
	if ((rc = load_state_file(curr_block_list, dir)) != SLURM_SUCCESS)
		return rc;
	/* This needs to be reset to SLURM_ERROR or it will never we
	   that way again ;). */
	rc = SLURM_ERROR;
#endif
	if (!bg_recover)
		return SLURM_ERROR;

	itr_curr = list_iterator_create(curr_block_list);
	itr_conf = list_iterator_create(bg_lists->main);
	while ((bg_record = list_next(itr_conf))) {
		list_iterator_reset(itr_curr);
		while ((init_bg_record = list_next(itr_curr))) {
			if (!bit_equal(bg_record->mp_bitmap,
				       init_bg_record->mp_bitmap))
				continue; /* wrong nodes */
			if (!bit_equal(bg_record->ionode_bitmap,
				       init_bg_record->ionode_bitmap))
				continue;
			if ((bg_record->conn_type[0] < SELECT_SMALL)
			    && (init_bg_record->conn_type[0] < SELECT_SMALL)) {
				for (dim = 0; dim < SYSTEM_DIMENSIONS; dim++) {
					if (bg_record->conn_type[dim]
					    != init_bg_record->conn_type[dim])
						break; /* wrong conn_type */
				}
				if (dim < SYSTEM_DIMENSIONS)
					continue;
			}
			copy_bg_record(init_bg_record, bg_record);
			/* remove from the curr list since we just
			   matched it no reason to keep it around
			   anymore */
			list_delete_item(itr_curr);
			break;
		}

		if (!bg_record->bg_block_id) {
			format_node_name(bg_record, tmp_char,
					 sizeof(tmp_char));
			info("Block found in bluegene.conf to be "
			     "created: Nodes:%s",
			     tmp_char);
			rc = SLURM_ERROR;
		} else {
			if (bg_record->full_block)
				full_created = 1;

			list_push(found_block_list, bg_record);
			format_node_name(bg_record, tmp_char,
					 sizeof(tmp_char));
			info("Existing: BlockID:%s Nodes:%s Conn:%s",
			     bg_record->bg_block_id,
			     tmp_char,
			     conn_type_string(bg_record->conn_type[0]));
			if (bg_record->state & BG_BLOCK_ERROR_FLAG)
				put_block_in_error_state(bg_record, NULL);
			else if (((bg_record->state == BG_BLOCK_INITED)
				  || (bg_record->state == BG_BLOCK_BOOTING))
				 && !block_ptr_exist_in_list(bg_lists->booted,
							     bg_record))
				list_push(bg_lists->booted, bg_record);
		}
	}
	if (bg_conf->layout_mode == LAYOUT_DYNAMIC)
		goto finished;

	if (!full_created) {
		list_iterator_reset(itr_curr);
		while ((init_bg_record = list_next(itr_curr))) {
			if (init_bg_record->full_block) {
				list_remove(itr_curr);
				bg_record = init_bg_record;
				list_append(bg_lists->main, bg_record);
				list_push(found_block_list, bg_record);
				format_node_name(bg_record, tmp_char,
						 sizeof(tmp_char));
				info("Existing: BlockID:%s Nodes:%s Conn:%s",
				     bg_record->bg_block_id,
				     tmp_char,
				     conn_type_string(bg_record->conn_type[0]));
				if (bg_record->state & BG_BLOCK_ERROR_FLAG)
					put_block_in_error_state(
						bg_record, NULL);
				else if (((bg_record->state
					     == BG_BLOCK_INITED)
					    || (bg_record->state
						== BG_BLOCK_BOOTING))
				    && !block_ptr_exist_in_list(
					    bg_lists->booted, bg_record))
					list_push(bg_lists->booted, bg_record);
				break;
			}
		}
	}

finished:
	list_iterator_destroy(itr_conf);
	list_iterator_destroy(itr_curr);
	if (!list_count(curr_block_list))
		rc = SLURM_SUCCESS;
	return rc;
}

/* Pack all relevent information about a block */
static void _pack_block(bg_record_t *bg_record, Buf buffer,
			uint16_t protocol_version)
{
#ifdef HAVE_BGQ
	int dim;
#endif
	uint32_t count = NO_VAL;
	block_job_info_t *job;
	ListIterator itr;

	if (protocol_version >= SLURM_2_3_PROTOCOL_VERSION) {
		packstr(bg_record->bg_block_id, buffer);
		packstr(bg_record->blrtsimage, buffer);
		pack_bit_fmt(bg_record->mp_bitmap, buffer);
#ifdef HAVE_BGQ
		pack32(SYSTEM_DIMENSIONS, buffer);
		for (dim=0; dim<SYSTEM_DIMENSIONS; dim++)
			pack16(bg_record->conn_type[dim], buffer);
#else
		pack32(1, buffer); /* for dimensions of conn_type */
		pack16(bg_record->conn_type[0], buffer);
#endif
		packstr(bg_record->ionode_str, buffer);
		pack_bit_fmt(bg_record->ionode_bitmap, buffer);

		if (bg_record->job_list)
			count = list_count(bg_record->job_list);
		pack32(count, buffer);
		if (count && count != NO_VAL) {
			itr = list_iterator_create(bg_record->job_list);
			while ((job = list_next(itr))) {
				slurm_pack_block_job_info(job, buffer,
							  protocol_version);
			}
			list_iterator_destroy(itr);
		}
		count = NO_VAL;

		pack32((uint32_t)bg_record->job_running, buffer);
		packstr(bg_record->linuximage, buffer);
		packstr(bg_record->mloaderimage, buffer);
		packstr(bg_record->mp_str, buffer);
		packstr(bg_record->mp_used_str, buffer);
		pack32((uint32_t)bg_record->cnode_cnt, buffer);
		pack16((uint16_t)bg_record->node_use, buffer);
		packstr(bg_record->user_name, buffer);
		packstr(bg_record->ramdiskimage, buffer);
		packstr(bg_record->reason, buffer);
		pack16((uint16_t)bg_record->state, buffer);
		pack_bit_fmt(bg_record->mp_used_bitmap, buffer);
	} else if (protocol_version >= SLURM_2_2_PROTOCOL_VERSION) {
		packstr(bg_record->bg_block_id, buffer);
#ifdef HAVE_BGL
		packstr(bg_record->blrtsimage, buffer);
#endif
		pack_bit_fmt(bg_record->mp_bitmap, buffer);
		pack16((uint16_t)bg_record->conn_type[0], buffer);
		packstr(bg_record->ionode_str, buffer);
		pack_bit_fmt(bg_record->ionode_bitmap, buffer);
		pack32((uint32_t)bg_record->job_running, buffer);
		packstr(bg_record->linuximage, buffer);
		packstr(bg_record->mloaderimage, buffer);
		packstr(bg_record->mp_str, buffer);
		pack32((uint32_t)bg_record->cnode_cnt, buffer);
#ifdef HAVE_BGL
		pack16((uint16_t)bg_record->node_use, buffer);
#endif
		packstr(bg_record->user_name, buffer);
		packstr(bg_record->ramdiskimage, buffer);
		packstr(bg_record->reason, buffer);
		pack16((uint16_t)bg_record->state, buffer);
	} else if (protocol_version >= SLURM_2_1_PROTOCOL_VERSION) {
		packstr(bg_record->bg_block_id, buffer);
#ifdef HAVE_BGL
		packstr(bg_record->blrtsimage, buffer);
#endif
		pack_bit_fmt(bg_record->mp_bitmap, buffer);
		pack16((uint16_t)bg_record->conn_type[0], buffer);
		packstr(bg_record->ionode_str, buffer);
		pack_bit_fmt(bg_record->ionode_bitmap, buffer);
		pack32((uint32_t)bg_record->job_running, buffer);
		packstr(bg_record->linuximage, buffer);
		packstr(bg_record->mloaderimage, buffer);
		packstr(bg_record->mp_str, buffer);
		pack32((uint32_t)bg_record->cnode_cnt, buffer);
#ifdef HAVE_BGL
		pack16((uint16_t)bg_record->node_use, buffer);
#endif
		packstr(bg_record->user_name, buffer);
		packstr(bg_record->ramdiskimage, buffer);
		pack16((uint16_t)bg_record->state, buffer);
	}
}

static List _get_config(void)
{
	config_key_pair_t *key_pair;
	List my_list = list_create(destroy_config_key_pair);

	if (!my_list)
		fatal("malloc failure on list_create");

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("MidPlaneNodeCnt");
	key_pair->value = xstrdup_printf("%u", bg_conf->mp_cnode_cnt);
	list_append(my_list, key_pair);

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("NodeCPUCnt");
	key_pair->value = xstrdup_printf("%u", bg_conf->cpu_ratio);
	list_append(my_list, key_pair);


#ifdef HAVE_BGL
	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("BlrtsImage");
	key_pair->value = xstrdup(bg_conf->default_blrtsimage);
	list_append(my_list, key_pair);

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("LinuxImage");
	key_pair->value = xstrdup(bg_conf->default_linuximage);
	list_append(my_list, key_pair);

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("RamDiskImage");
	key_pair->value = xstrdup(bg_conf->default_ramdiskimage);
	list_append(my_list, key_pair);
#elif defined HAVE_BGP
	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("CnloadImage");
	key_pair->value = xstrdup(bg_conf->default_linuximage);
	list_append(my_list, key_pair);

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("IoloadImage");
	key_pair->value = xstrdup(bg_conf->default_ramdiskimage);
	list_append(my_list, key_pair);
#endif

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("BridgeAPILogFile");
	key_pair->value = xstrdup(bg_conf->bridge_api_file);
	list_append(my_list, key_pair);

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("BridgeAPIVerbose");
	key_pair->value = xstrdup_printf("%u", bg_conf->bridge_api_verb);
	list_append(my_list, key_pair);

	if (bg_conf->deny_pass) {
		key_pair = xmalloc(sizeof(config_key_pair_t));
		key_pair->name = xstrdup("DenyPassThrough");
		if (bg_conf->deny_pass & PASS_DENY_A)
			xstrcat(key_pair->value, "A,");
		if (bg_conf->deny_pass & PASS_DENY_X)
			xstrcat(key_pair->value, "X,");
		if (bg_conf->deny_pass & PASS_DENY_Y)
			xstrcat(key_pair->value, "Y,");
		if (bg_conf->deny_pass & PASS_DENY_Z)
			xstrcat(key_pair->value, "Z,");
		if (key_pair->value)
			key_pair->value[strlen(key_pair->value)-1] = '\0';
		list_append(my_list, key_pair);
	}

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("LayoutMode");
	switch(bg_conf->layout_mode) {
	case LAYOUT_STATIC:
		key_pair->value = xstrdup("Static");
		break;
	case LAYOUT_OVERLAP:
		key_pair->value = xstrdup("Overlap");
		break;
	case LAYOUT_DYNAMIC:
		key_pair->value = xstrdup("Dynamic");
		break;
	default:
		key_pair->value = xstrdup("Unknown");
		break;
	}
	list_append(my_list, key_pair);

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("MloaderImage");
	key_pair->value = xstrdup(bg_conf->default_mloaderimage);
	list_append(my_list, key_pair);

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("NodeCardNodeCnt");
	key_pair->value = xstrdup_printf("%u", bg_conf->nodecard_cnode_cnt);
	list_append(my_list, key_pair);

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("IONodesPerMP");
	key_pair->value = xstrdup_printf("%u", bg_conf->ionodes_per_mp);
	list_append(my_list, key_pair);

	list_sort(my_list, (ListCmpF) sort_key_pairs);

	return my_list;
}
#endif

/*
 * init() is called when the plugin is loaded, before any other functions
 * are called.  Put global initialization here.
 */
extern int init(void)
{

#ifdef HAVE_BG
	if (!bg_conf) {
		/* This is needed on all systems where srun wraps the
		   bluegene calling program (i.e. runjob).
		*/
		bg_conf = xmalloc(sizeof(bg_config_t));
		/* set some defaults for most systems */
		bg_conf->mp_cnode_cnt = 512;
		bg_conf->quarter_cnode_cnt = 128;
		bg_conf->nodecard_cnode_cnt = 32;
		bg_conf->mp_nodecard_cnt = bg_conf->mp_cnode_cnt
			/ bg_conf->nodecard_cnode_cnt;
	}
	if (bg_recover != NOT_FROM_CONTROLLER) {
#if defined HAVE_BG_L_P && (SYSTEM_DIMENSIONS != 3)
		fatal("SYSTEM_DIMENSIONS value (%d) invalid for BlueGene",
		      SYSTEM_DIMENSIONS);
#elif defined HAVE_BGQ && (SYSTEM_DIMENSIONS != 4)
		fatal("SYSTEM_DIMENSIONS value (%d) invalid for BGQ",
		      SYSTEM_DIMENSIONS);
#endif

#if defined HAVE_BG_FILES && defined HAVE_BG_L_P
#ifdef HAVE_BGL
	        if (!getenv("CLASSPATH") || !getenv("DB2INSTANCE")
		    || !getenv("VWSPATH"))
			fatal("db2profile has not been "
			      "run to setup DB2 environment");

		if ((SELECT_COPROCESSOR_MODE  != RM_PARTITION_COPROCESSOR_MODE)
		    || (SELECT_VIRTUAL_NODE_MODE
			!= RM_PARTITION_VIRTUAL_NODE_MODE))
			fatal("enum node_use_type out of sync with rm_api.h");
#endif
		if ((SELECT_MESH  != RM_MESH)
		    || (SELECT_TORUS != RM_TORUS)
		    || (SELECT_NAV   != RM_NAV))
			fatal("enum conn_type out of sync with rm_api.h");
#endif

		verbose("%s loading...", plugin_name);
		/* if this is coming from something other than the controller
		   we don't want to read the config or anything like that. */
		_set_bg_lists();

		xfree(bg_conf->slurm_user_name);
		xfree(bg_conf->slurm_node_prefix);
		slurm_conf_lock();
		xassert(slurmctld_conf.slurm_user_name);
		xassert(slurmctld_conf.node_prefix);
		bg_conf->slurm_user_name =
			xstrdup(slurmctld_conf.slurm_user_name);
		bg_conf->slurm_node_prefix =
			xstrdup(slurmctld_conf.node_prefix);
		bg_conf->slurm_debug_flags = slurmctld_conf.debug_flags;
		bg_conf->slurm_debug_level = slurmctld_conf.slurmctld_debug;
		slurm_conf_unlock();

		if (bg_conf->blrts_list)
			list_destroy(bg_conf->blrts_list);
		bg_conf->blrts_list = list_create(destroy_image);
		if (bg_conf->linux_list)
			list_destroy(bg_conf->linux_list);
		bg_conf->linux_list = list_create(destroy_image);
		if (bg_conf->mloader_list)
			list_destroy(bg_conf->mloader_list);
		bg_conf->mloader_list = list_create(destroy_image);
		if (bg_conf->ramdisk_list)
			list_destroy(bg_conf->ramdisk_list);
		bg_conf->ramdisk_list = list_create(destroy_image);

		ba_init(NULL, 1);

		verbose("BlueGene plugin loaded successfully");
	}
	verbose("%s loaded", plugin_name);
#else
	if (bg_recover != NOT_FROM_CONTROLLER)
		fatal("select/bluegene is incompatible with a "
		      "non BlueGene system");
#endif
	return SLURM_SUCCESS;
}

extern int fini ( void )
{
	int rc = SLURM_SUCCESS;

	_destroy_bg_config(bg_conf);
	_destroy_bg_lists(bg_lists);

	ba_fini();

	return rc;
}

/*
 * The remainder of this file implements the standard SLURM
 * node selection API.
 */

/* We rely upon DB2 to save and restore BlueGene state */
extern int select_p_state_save(char *dir_name)
{
#ifdef HAVE_BG
	ListIterator itr;
	bg_record_t *bg_record = NULL;
	int error_code = 0, log_fd;
	char *old_file, *new_file, *reg_file;
	uint32_t blocks_packed = 0, tmp_offset, block_offset;
	Buf buffer = init_buf(BUF_SIZE);
	DEF_TIMERS;

	debug("bluegene: select_p_state_save");
	START_TIMER;
	/* write header: time */
	packstr(BLOCK_STATE_VERSION, buffer);
	block_offset = get_buf_offset(buffer);
	pack32(blocks_packed, buffer);
	pack_time(time(NULL), buffer);

	/* write block records to buffer */
	slurm_mutex_lock(&block_state_mutex);
	itr = list_iterator_create(bg_lists->main);
	while ((bg_record = list_next(itr))) {
		if (bg_record->magic != BLOCK_MAGIC)
			continue;
		/* on real bluegene systems we only want to keep track of
		 * the blocks in an error state
		 */
#if defined HAVE_BG_FILES
		if (!(bg_record->state & BG_BLOCK_ERROR_FLAG))
			continue;
#endif
		xassert(bg_record->bg_block_id != NULL);

		_pack_block(bg_record, buffer, SLURM_PROTOCOL_VERSION);
		blocks_packed++;
	}
	list_iterator_destroy(itr);
	slurm_mutex_unlock(&block_state_mutex);
	tmp_offset = get_buf_offset(buffer);
	set_buf_offset(buffer, block_offset);
	pack32(blocks_packed, buffer);
	set_buf_offset(buffer, tmp_offset);
	/* Maintain config read lock until we copy state_save_location *\
	   \* unlock_slurmctld(part_read_lock);          - see below      */

	/* write the buffer to file */
	slurm_conf_lock();
	old_file = xstrdup(slurmctld_conf.state_save_location);
	xstrcat(old_file, "/block_state.old");
	reg_file = xstrdup(slurmctld_conf.state_save_location);
	xstrcat(reg_file, "/block_state");
	new_file = xstrdup(slurmctld_conf.state_save_location);
	xstrcat(new_file, "/block_state.new");
	slurm_conf_unlock();

	log_fd = creat(new_file, 0600);
	if (log_fd < 0) {
		error("Can't save state, error creating file %s, %m",
		      new_file);
		error_code = errno;
	} else {
		int pos = 0, nwrite = get_buf_offset(buffer), amount;
		char *data = (char *)get_buf_data(buffer);

		while (nwrite > 0) {
			amount = write(log_fd, &data[pos], nwrite);
			if ((amount < 0) && (errno != EINTR)) {
				error("Error writing file %s, %m", new_file);
				error_code = errno;
				break;
			}
			nwrite -= amount;
			pos    += amount;
		}
		fsync(log_fd);
		close(log_fd);
	}
	if (error_code)
		(void) unlink(new_file);
	else {			/* file shuffle */
		(void) unlink(old_file);
		if (link(reg_file, old_file))
			debug4("unable to create link for %s -> %s: %m",
			       reg_file, old_file);
		(void) unlink(reg_file);
		if (link(new_file, reg_file))
			debug4("unable to create link for %s -> %s: %m",
			       new_file, reg_file);
		(void) unlink(new_file);
	}
	xfree(old_file);
	xfree(reg_file);
	xfree(new_file);

	free_buf(buffer);
	END_TIMER2("select_p_state_save");
	return SLURM_SUCCESS;
#else
	return SLURM_ERROR;
#endif
}

extern int select_p_state_restore(char *dir_name)
{
#ifdef HAVE_BG
	debug("bluegene: select_p_state_restore");

	/* found bg blocks already on system */
	List curr_block_list = NULL;
	List found_block_list = NULL;
	static time_t last_config_update = (time_t) 0;

	/* only run on startup */
	if (last_config_update)
		return SLURM_SUCCESS;

	last_config_update = time(NULL);
	curr_block_list = list_create(destroy_bg_record);
	found_block_list = list_create(NULL);
//#if 0
	/* Check to see if the configs we have are correct */
	if (_validate_config_blocks(curr_block_list, found_block_list, dir_name)
	    == SLURM_ERROR) {
		_delete_old_blocks(curr_block_list, found_block_list);
	}
//#endif
	/* looking for blocks only I created */
	if (bg_conf->layout_mode == LAYOUT_DYNAMIC) {
		info("No blocks created until jobs are submitted");
	} else {
		if (create_defined_blocks(bg_conf->layout_mode,
					  found_block_list)
		    == SLURM_ERROR) {
			/* error in creating the static blocks, so
			 * blocks referenced by submitted jobs won't
			 * correspond to actual slurm blocks.
			 */
			fatal("Error, could not create the static blocks");
			return SLURM_ERROR;
		}
	}

	list_destroy(curr_block_list);
	curr_block_list = NULL;
	list_destroy(found_block_list);
	found_block_list = NULL;

	slurm_mutex_lock(&block_state_mutex);
	last_bg_update = time(NULL);
	sort_bg_record_inc_size(bg_lists->main);
	slurm_mutex_unlock(&block_state_mutex);
	if (bg_conf->slurm_debug_flags & DEBUG_FLAG_SELECT_TYPE)
		info("Blocks have finished being created.");
	return SLURM_SUCCESS;
#else
	return SLURM_ERROR;
#endif
}

/* Sync BG blocks to currently active jobs */
extern int select_p_job_init(List job_list)
{
#ifdef HAVE_BG
	int rc = sync_jobs(job_list);

	/* after we have synced the blocks then we say they are
	   created. */
	blocks_are_created = 1;
	return rc;
#else
	return SLURM_ERROR;
#endif
}

extern bool select_p_node_ranking(struct node_record *node_ptr, int node_cnt)
{
	return false;
}

/* All initialization is performed by init() */
extern int select_p_node_init(struct node_record *node_ptr, int node_cnt)
{
#ifdef HAVE_BG
	if (node_cnt>0 && bg_conf)
		if (node_ptr->cpus >= bg_conf->mp_cnode_cnt)
			bg_conf->cpus_per_mp = node_ptr->cpus;

	return SLURM_SUCCESS;
#else
	return SLURM_ERROR;
#endif
}

/*
 * Called by slurmctld when a new configuration file is loaded
 * or scontrol is used to change block configuration
 */
extern int select_p_block_init(List part_list)
{
#ifdef HAVE_BG
	/* select_p_node_init needs to be called before this to set
	   this up correctly
	*/
	if (read_bg_conf() == SLURM_ERROR) {
		fatal("Error, could not read the file");
		return SLURM_ERROR;
	}

	if (part_list) {
		struct part_record *part_ptr = NULL;
		ListIterator itr = list_iterator_create(part_list);
		while ((part_ptr = list_next(itr))) {
			part_ptr->max_nodes = part_ptr->max_nodes_orig;
			part_ptr->min_nodes = part_ptr->min_nodes_orig;
			select_p_alter_node_cnt(SELECT_SET_MP_CNT,
						&part_ptr->max_nodes);
			select_p_alter_node_cnt(SELECT_SET_MP_CNT,
						&part_ptr->min_nodes);
		}
		list_iterator_destroy(itr);
	}
	return SLURM_SUCCESS;
#else
	return SLURM_ERROR;
#endif
}


/*
 * select_p_job_test - Given a specification of scheduling requirements,
 *	identify the nodes which "best" satify the request. The specified
 *	nodes may be DOWN or BUSY at the time of this test as may be used
 *	to deterime if a job could ever run.
 * IN/OUT job_ptr - pointer to job being scheduled start_time is set
 *	when we can possibly start job.
 * IN/OUT bitmap - usable nodes are set on input, nodes not required to
 *	satisfy the request are cleared, other left set
 * IN min_nodes - minimum count of nodes
 * IN max_nodes - maximum count of nodes (0==don't care)
 * IN req_nodes - requested (or desired) count of nodes
 * IN mode - SELECT_MODE_RUN_NOW: try to schedule job now
 *           SELECT_MODE_TEST_ONLY: test if job can ever run
 *           SELECT_MODE_WILL_RUN: determine when and where job can run
 * IN preemptee_candidates - List of pointers to jobs which can be preempted.
 * IN/OUT preemptee_job_list - Pointer to list of job pointers. These are the
 *		jobs to be preempted to initiate the pending job. Not set
 *		if mode=SELECT_MODE_TEST_ONLY or input pointer is NULL.
 * RET zero on success, EINVAL otherwise
 * NOTE: bitmap must be a superset of req_nodes at the time that
 *	select_p_job_test is called
 */
extern int select_p_job_test(struct job_record *job_ptr, bitstr_t *bitmap,
			     uint32_t min_nodes, uint32_t max_nodes,
			     uint32_t req_nodes, uint16_t mode,
			     List preemptee_candidates,
			     List *preemptee_job_list)
{
#ifdef HAVE_BG
	/* submit_job - is there a block where we have:
	 * 1) geometry requested
	 * 2) min/max nodes (MPs) requested
	 * 3) type: TORUS or MESH or NAV (torus else mesh)
	 *
	 * note: we don't have to worry about security at this level
	 * as the SLURM block logic will handle access rights.
	 */

	return submit_job(job_ptr, bitmap, min_nodes, max_nodes,
			  req_nodes, mode, preemptee_candidates,
			  preemptee_job_list);
#else
	return SLURM_ERROR;
#endif
}

extern int select_p_job_begin(struct job_record *job_ptr)
{
#ifdef HAVE_BG
	return start_job(job_ptr);
#else
	return SLURM_ERROR;
#endif
}

extern int select_p_job_ready(struct job_record *job_ptr)
{
#ifdef HAVE_BG
	int rc = 1;
	char *block_id = NULL;
	bg_record_t *bg_record = NULL;

	rc = get_select_jobinfo(job_ptr->select_jobinfo->data,
				SELECT_JOBDATA_BLOCK_ID, &block_id);
	if (rc == SLURM_SUCCESS) {
		slurm_mutex_lock(&block_state_mutex);
		bg_record = find_bg_record_in_list(bg_lists->main, block_id);

		if (bg_record) {
			uint32_t job_id = NO_JOB_RUNNING, uid = NO_VAL;
			if (bg_record->job_list) {
				block_job_info_t *job_info;
				ListIterator itr = list_iterator_create(
					bg_record->job_list);
				xassert(itr);
				while ((job_info = list_next(itr))) {
					if (job_info->job_id
					    == job_ptr->job_id) {
						job_id = job_info->job_id;
						uid = job_info->user_id;
						break;
					}
				}
				list_iterator_destroy(itr);
			} else {
				uid = bg_record->user_uid;
				job_id = bg_record->job_running;
			}

			if (job_id != job_ptr->job_id) {
				rc = 0;
			} else if ((uid == job_ptr->user_id)
				   && (bg_record->state == BG_BLOCK_INITED)) {
				/* Clear the state just incase we
				 * missed it somehow. */
				job_ptr->job_state &= (~JOB_CONFIGURING);
				last_job_update = time(NULL);
				rc = 1;
			} else if (uid != job_ptr->user_id)
				rc = 0;
			else
				rc = READY_JOB_ERROR;	/* try again */

		} else {
			/* This means the block has been removed and
			   is no longer valid.  This could happen
			   often during an epilog on a busy system.
			*/
			debug2("block_ready: block %s not in bg_lists->main.",
			       block_id);
			rc = READY_JOB_FATAL;	/* fatal error */
		}
		slurm_mutex_unlock(&block_state_mutex);
	} else
		rc = READY_JOB_ERROR;
	/* info("returning %d for job %u block %s %d %d", */
	/*      rc, job_ptr->job_id, block_id, */
	/*      READY_JOB_ERROR, READY_JOB_FATAL); */
	xfree(block_id);
	return rc;
#else
	return SLURM_ERROR;
#endif
}

extern int select_p_job_resized(struct job_record *job_ptr,
				struct node_record *node_ptr)
{
	return ESLURM_NOT_SUPPORTED;
}

extern bool select_p_job_expand_allow(void)
{
	return false;
}

extern int select_p_job_expand(struct job_record *from_job_ptr,
			       struct job_record *to_job_ptr)
{
	return ESLURM_NOT_SUPPORTED;
}

extern int select_p_job_signal(struct job_record *job_ptr, int signal)
{
	return SLURM_SUCCESS;
}

extern int select_p_job_fini(struct job_record *job_ptr)
{
#ifdef HAVE_BG
	return term_job(job_ptr);
#else
	return SLURM_ERROR;
#endif
}

extern int select_p_job_suspend(struct job_record *job_ptr)
{
	return ESLURM_NOT_SUPPORTED;
}

extern int select_p_job_resume(struct job_record *job_ptr)
{
	return ESLURM_NOT_SUPPORTED;
}

extern bitstr_t *select_p_step_pick_nodes(struct job_record *job_ptr,
					  select_jobinfo_t *step_jobinfo,
					  uint32_t node_count)
{
	bitstr_t *picked_mps = NULL;
	bitstr_t *avail_mps = NULL;
	bg_record_t *bg_record = NULL;
	char *tmp_char = NULL, *tmp_char2 = NULL;
	ba_mp_t *ba_mp = NULL;
	select_jobinfo_t *jobinfo = NULL;
	xassert(job_ptr);

	slurm_mutex_lock(&block_state_mutex);

	jobinfo = job_ptr->select_jobinfo->data;
	bg_record = jobinfo->bg_record;

	if (!bg_record)
		fatal("This job %u does not have a bg block "
		      "assigned to it, but for some reason we are "
		      "trying to start a step on it?",
		      job_ptr->job_id);

	xassert(bg_record->mp_used_bitmap);

	if (!(avail_mps = bit_copy(bg_record->mp_used_bitmap)))
		fatal("bit_copy malloc failure");

	if (bg_conf->slurm_debug_flags & DEBUG_FLAG_BG_PICK) {
		tmp_char = bitmap2node_name(avail_mps);
		info("select_p_step_pick_nodes: looking to run a new "
		     "step for job %u requesting %u nodes on block %s with "
		     "used midplanes %s", job_ptr->job_id, node_count,
		     bg_record->bg_block_id, tmp_char);
		xfree(tmp_char);
	}

	if (((cluster_flags & CLUSTER_FLAG_BGL)
	     || (cluster_flags & CLUSTER_FLAG_BGP))
	    || (node_count == bg_record->cnode_cnt)) {
		/* If we are using the whole block we need to verify
		   if anything else is used.  If anything else is used
		   return NULL, else return that we can use the entire
		   thing.
		   On BGL/P This is always the default, no matter how
		   big the step is since you can only run 1 step per block.
		*/
		if (bit_ffs(avail_mps) != -1) {
			if (bg_conf->slurm_debug_flags & DEBUG_FLAG_BG_PICK)
				info("select_p_step_pick_nodes: Looking "
				     "for the entire block %s for job %u, "
				     "but some of it is used.",
				     bg_record->bg_block_id, job_ptr->job_id);
			goto end_it;
		}
		if (!(picked_mps = bit_copy(job_ptr->node_bitmap)))
			fatal("bit_copy malloc failure");
		bit_or(bg_record->mp_used_bitmap, picked_mps);
		goto found_it;
	} else if ((ba_mp = ba_pick_sub_block_cnodes(
			    bg_record, node_count,
			    &step_jobinfo->units_used))) {
		if (!(picked_mps = bit_alloc(bit_size(job_ptr->node_bitmap))))
			fatal("bit_copy malloc failure");
		bit_set(bg_record->mp_used_bitmap, ba_mp->index);
		bit_set(picked_mps, ba_mp->index);
	}

found_it:
	if (picked_mps) {
		if (bg_conf->slurm_debug_flags & DEBUG_FLAG_BG_PICK) {
			tmp_char = bitmap2node_name(picked_mps);
			tmp_char2 = bitmap2node_name(bg_record->mp_used_bitmap);
			info("select_p_step_pick_nodes: picked %s mps on "
			     "block %s used is now %s",
			     tmp_char, bg_record->bg_block_id,
			     tmp_char2);
			xfree(tmp_char);
			xfree(tmp_char2);
		}
		step_jobinfo->cnode_cnt = node_count;
	}
end_it:

	FREE_NULL_BITMAP(avail_mps);

	slurm_mutex_unlock(&block_state_mutex);

	return picked_mps;
}

extern int select_p_step_finish(struct step_record *step_ptr)
{
	bg_record_t *bg_record = NULL;
	select_jobinfo_t *jobinfo = NULL;
	int rc = SLURM_SUCCESS;
	char *tmp_char = NULL, *tmp_char2 = NULL;

	xassert(step_ptr);

	slurm_mutex_lock(&block_state_mutex);

	jobinfo = step_ptr->job_ptr->select_jobinfo->data;
	bg_record = jobinfo->bg_record;

	if (!bg_record)
		fatal("This step %u.%u does not have a bg block "
		      "assigned to it, but for some reason we are "
		      "trying to end the step?",
		      step_ptr->job_ptr->job_id, step_ptr->step_id);
	/* At this moment the step_node_bitmap has already been
	   cleared and the step_node_bitmap has been set so use it
	   instead.
	*/
	bit_not(step_ptr->step_node_bitmap);
	bit_and(bg_record->mp_used_bitmap, step_ptr->step_node_bitmap);
	bit_not(step_ptr->step_node_bitmap);

	if (bg_conf->slurm_debug_flags & DEBUG_FLAG_BG_PICK) {
		tmp_char = bitmap2node_name(bg_record->mp_used_bitmap);
		tmp_char2 = bitmap2node_name(step_ptr->step_node_bitmap);
		info("select_p_step_finish: cleared %s "
		     "from job %u, now %s used",
		     tmp_char2, step_ptr->job_ptr->job_id, tmp_char);
		xfree(tmp_char);
		xfree(tmp_char2);
	}

	jobinfo = step_ptr->select_jobinfo->data;
#ifndef HAVE_BG_L_P
	if (jobinfo->cnode_cnt != bg_record->cnode_cnt) {
		/* This means we only used one midplane. */
		bitoff_t bit = bit_ffs(step_ptr->step_node_bitmap);
		ListIterator itr = NULL;
		ba_mp_t *ba_mp = NULL;

		if (bit == -1) {
			error("we couldn't find any bits set in "
			      "this step %u.%u",
			      step_ptr->job_ptr->job_id, step_ptr->step_id);
			rc = SLURM_ERROR;
		}

		itr = list_iterator_create(bg_record->ba_mp_list);
		while ((ba_mp = list_next(itr))) {
			char tmp_char3[BUF_SIZE];
			char tmp_char4[BUF_SIZE];

			if (ba_mp->index != bit)
				continue;
			xassert(jobinfo->units_used);

			bit_not(jobinfo->units_used);
			bit_and(ba_mp->cnode_bitmap, jobinfo->units_used);
			if (bg_conf->slurm_debug_flags & DEBUG_FLAG_BG_PICK) {
				bit_not(jobinfo->units_used);
				bit_fmt(tmp_char3, sizeof(tmp_char3),
					jobinfo->units_used);
				bit_fmt(tmp_char4, sizeof(tmp_char4),
					ba_mp->cnode_bitmap);
				info("select_p_step_finish: cleared %s bits "
				     "from %s, now %s used",
				     tmp_char3, ba_mp->coord_str, tmp_char4);
				xfree(tmp_char);
				xfree(tmp_char2);
			}
		}
		list_iterator_destroy(itr);

	}
#endif
	slurm_mutex_unlock(&block_state_mutex);

	return rc;
}

extern int select_p_pack_select_info(time_t last_query_time,
				     uint16_t show_flags, Buf *buffer_ptr,
				     uint16_t protocol_version)
{
#ifdef HAVE_BG
	ListIterator itr;
	bg_record_t *bg_record = NULL;
	uint32_t blocks_packed = 0, tmp_offset;
	Buf buffer;

	/* check to see if data has changed */
	if (last_query_time >= last_bg_update) {
		debug2("Node select info hasn't changed since %ld",
		       last_bg_update);
		return SLURM_NO_CHANGE_IN_DATA;
	} else if (blocks_are_created) {
		*buffer_ptr = NULL;
		buffer = init_buf(HUGE_BUF_SIZE);
		pack32(blocks_packed, buffer);
		pack_time(last_bg_update, buffer);

		if (protocol_version >= SLURM_2_1_PROTOCOL_VERSION) {
			if (bg_lists->main) {
				slurm_mutex_lock(&block_state_mutex);
				itr = list_iterator_create(bg_lists->main);
				while ((bg_record = list_next(itr))) {
					if (bg_record->magic != BLOCK_MAGIC)
						continue;
					_pack_block(bg_record, buffer,
						    protocol_version);
					blocks_packed++;
				}
				list_iterator_destroy(itr);
				slurm_mutex_unlock(&block_state_mutex);
			} else {
				error("select_p_pack_select_info: "
				      "no bg_lists->main");
				return SLURM_ERROR;
			}
		}
		tmp_offset = get_buf_offset(buffer);
		set_buf_offset(buffer, 0);
		pack32(blocks_packed, buffer);
		set_buf_offset(buffer, tmp_offset);

		*buffer_ptr = buffer;
	} else {
		error("select_p_pack_node_info: bg_lists->main not ready yet");
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
#else
	return SLURM_ERROR;
#endif
}

extern int select_p_select_nodeinfo_pack(select_nodeinfo_t *nodeinfo,
					 Buf buffer,
					 uint16_t protocol_version)
{
	return select_nodeinfo_pack(nodeinfo, buffer, protocol_version);
}

extern int select_p_select_nodeinfo_unpack(select_nodeinfo_t **nodeinfo,
					   Buf buffer,
					   uint16_t protocol_version)
{
	return select_nodeinfo_unpack(nodeinfo, buffer, protocol_version);
}

extern select_nodeinfo_t *select_p_select_nodeinfo_alloc(uint32_t size)
{
	return select_nodeinfo_alloc(size);
}

extern int select_p_select_nodeinfo_free(select_nodeinfo_t *nodeinfo)
{
	return select_nodeinfo_free(nodeinfo);
}

extern int select_p_select_nodeinfo_set_all(time_t last_query_time)
{
	return select_nodeinfo_set_all(last_query_time);
}

extern int select_p_select_nodeinfo_set(struct job_record *job_ptr)
{
	return SLURM_SUCCESS;
}

extern int select_p_select_nodeinfo_get(select_nodeinfo_t *nodeinfo,
					enum select_nodedata_type dinfo,
					enum node_states state,
					void *data)
{
	return select_nodeinfo_get(nodeinfo, dinfo, state, data);
}

extern select_jobinfo_t *select_p_select_jobinfo_alloc(void)
{
	return alloc_select_jobinfo();
}

extern int select_p_select_jobinfo_set(select_jobinfo_t *jobinfo,
				       enum select_jobdata_type data_type,
				       void *data)
{
	return set_select_jobinfo(jobinfo, data_type, data);
}

extern int select_p_select_jobinfo_get(select_jobinfo_t *jobinfo,
				       enum select_jobdata_type data_type,
				       void *data)
{
	return get_select_jobinfo(jobinfo, data_type, data);
}

extern select_jobinfo_t *select_p_select_jobinfo_copy(select_jobinfo_t *jobinfo)
{
	return copy_select_jobinfo(jobinfo);
}

extern int select_p_select_jobinfo_free(select_jobinfo_t *jobinfo)
{
	return free_select_jobinfo(jobinfo);
}

extern int  select_p_select_jobinfo_pack(select_jobinfo_t *jobinfo, Buf buffer,
					 uint16_t protocol_version)
{
	return pack_select_jobinfo(jobinfo, buffer, protocol_version);
}

extern int  select_p_select_jobinfo_unpack(select_jobinfo_t **jobinfo,
					   Buf buffer,
					   uint16_t protocol_version)
{
	return unpack_select_jobinfo(jobinfo, buffer, protocol_version);
}

extern char *select_p_select_jobinfo_sprint(select_jobinfo_t *jobinfo,
					    char *buf, size_t size, int mode)
{
	return sprint_select_jobinfo(jobinfo, buf, size, mode);
}

extern char *select_p_select_jobinfo_xstrdup(select_jobinfo_t *jobinfo,
					     int mode)
{
	return xstrdup_select_jobinfo(jobinfo, mode);
}

extern int select_p_update_block(update_block_msg_t *block_desc_ptr)
{
#ifdef HAVE_BG
	int rc = SLURM_SUCCESS;
	bg_record_t *bg_record = NULL;
	char reason[200];

	if (!block_desc_ptr->bg_block_id) {
		error("update_block: No name specified");
		return ESLURM_INVALID_BLOCK_NAME;
	}

	slurm_mutex_lock(&block_state_mutex);
	bg_record = find_bg_record_in_list(bg_lists->main,
					   block_desc_ptr->bg_block_id);
	if (!bg_record) {
		slurm_mutex_unlock(&block_state_mutex);
		return ESLURM_INVALID_BLOCK_NAME;
	}

	if (block_desc_ptr->reason)
		snprintf(reason, sizeof(reason), "%s", block_desc_ptr->reason);
	else if (block_desc_ptr->state == BG_BLOCK_BOOTING)
		snprintf(reason, sizeof(reason),
			 "update_block: "
			 "Admin recreated %s.", bg_record->bg_block_id);
	else if (block_desc_ptr->state == BG_BLOCK_NAV) {
		if (bg_record->conn_type[0] < SELECT_SMALL)
			snprintf(reason, sizeof(reason),
				 "update_block: "
				 "Admin removed block %s",
				 bg_record->bg_block_id);
		else
			snprintf(reason, sizeof(reason),
				 "update_block: "
				 "Removed all blocks on midplane %s",
				 bg_record->mp_str);

	} else
		snprintf(reason, sizeof(reason),
			 "update_block: "
			 "Admin set block %s state to %s",
			 bg_record->bg_block_id,
			 bg_block_state_string(block_desc_ptr->state));

	/* First fail any job running on this block */
	if (bg_record->job_running > NO_JOB_RUNNING) {
		slurm_mutex_unlock(&block_state_mutex);
		bg_requeue_job(bg_record->job_running, 0);
		slurm_mutex_lock(&block_state_mutex);
		if (!block_ptr_exist_in_list(bg_lists->main, bg_record)) {
			slurm_mutex_unlock(&block_state_mutex);
			error("while trying to put block in "
			      "error state it disappeared");
			return SLURM_ERROR;
		}
		/* need to set the job_ptr to NULL
		   here or we will get error message
		   about us trying to free this block
		   with a job in it.
		*/
		bg_record->job_ptr = NULL;
	}

	if (block_desc_ptr->state == BG_BLOCK_ERROR_FLAG) {
		bg_record_t *found_record = NULL;
		ListIterator itr;
		List delete_list = list_create(NULL);
		/* This loop shouldn't do much in regular Dynamic mode
		   since there shouldn't be overlapped blocks.  But if
		   there is a trouble block that isn't going away and
		   we need to mark it in an error state there could be
		   blocks overlapped where we need to requeue the jobs.
		*/
		itr = list_iterator_create(bg_lists->main);
		while ((found_record = list_next(itr))) {
			if (bg_record == found_record)
				continue;

			if (!blocks_overlap(bg_record, found_record)) {
				debug2("block %s isn't part of errored %s",
				       found_record->bg_block_id,
				       bg_record->bg_block_id);
				continue;
			}
			if (found_record->job_running > NO_JOB_RUNNING) {
				if (found_record->job_ptr
				    && IS_JOB_CONFIGURING(
					    found_record->job_ptr))
					info("Pending job %u on block %s "
					     "will try to be requeued "
					     "because overlapping block %s "
					     "is in an error state.",
					     found_record->job_running,
					     found_record->bg_block_id,
					     bg_record->bg_block_id);
				else
					info("Failing job %u on block %s "
					     "because overlapping block %s "
					     "is in an error state.",
					     found_record->job_running,
					     found_record->bg_block_id,
					     bg_record->bg_block_id);

				/* This job will be requeued in the
				   free_block_list code below, just
				   make note of it here.
				*/
			} else {
				debug2("block %s is part of errored %s "
				       "but no running job",
				       found_record->bg_block_id,
				       bg_record->bg_block_id);
			}
			list_push(delete_list, found_record);
		}
		list_iterator_destroy(itr);
		slurm_mutex_unlock(&block_state_mutex);
		free_block_list(NO_VAL, delete_list, 0, 0);
		list_destroy(delete_list);
		put_block_in_error_state(bg_record, reason);
	} else if (block_desc_ptr->state == BG_BLOCK_FREE) {
		/* Increment free_cnt to make sure we don't loose this
		 * block since bg_free_block will unlock block_state_mutex.
		 */
		bg_record->free_cnt++;
		bg_free_block(bg_record, 0, 1);
		bg_record->free_cnt--;
		resume_block(bg_record);
		slurm_mutex_unlock(&block_state_mutex);
	} else if (block_desc_ptr->state == BG_BLOCK_TERM) {
		/* This can't be RM_PARTITION_READY since the enum
		   changed from BGL to BGP and if we are running cross
		   cluster it just doesn't work.
		*/
		resume_block(bg_record);
		slurm_mutex_unlock(&block_state_mutex);
	} else if (bg_conf->layout_mode == LAYOUT_DYNAMIC
		   && (block_desc_ptr->state == BG_BLOCK_NAV)) {
		/* This means remove the block from the system.  If
		   the block is a small block we need to remove all the
		   blocks on that midplane.
		*/
		bg_record_t *found_record = NULL;
		ListIterator itr;
		List delete_list = list_create(NULL);

		list_push(delete_list, bg_record);
		/* only do the while loop if we are dealing with a
		   small block */
		if (bg_record->conn_type[0] < SELECT_SMALL)
			goto large_block;

		itr = list_iterator_create(bg_lists->main);
		while ((found_record = list_next(itr))) {
			if (bg_record == found_record)
				continue;

			if (!bit_equal(bg_record->mp_bitmap,
				       found_record->mp_bitmap)) {
				debug2("block %s isn't part of to be freed %s",
				       found_record->bg_block_id,
				       bg_record->bg_block_id);
				continue;
			}
			if (found_record->job_running > NO_JOB_RUNNING) {
				if (found_record->job_ptr
				    && IS_JOB_CONFIGURING(
					    found_record->job_ptr))
					info("Pending job %u on block %s "
					     "will try to be requeued "
					     "because overlapping block %s "
					     "is in an error state.",
					     found_record->job_running,
					     found_record->bg_block_id,
					     bg_record->bg_block_id);
				else
					info("Failing job %u on block %s "
					     "because overlapping block %s "
					     "is in an error state.",
					     found_record->job_running,
					     found_record->bg_block_id,
					     bg_record->bg_block_id);
				/* This job will be requeued in the
				   free_block_list code below, just
				   make note of it here.
				*/
			} else {
				debug2("block %s is part of to be freed %s "
				       "but no running job",
				       found_record->bg_block_id,
				       bg_record->bg_block_id);
			}
			list_push(delete_list, found_record);
		}
		list_iterator_destroy(itr);

	large_block:
		/* make sure if we are removing a block to put it back
		   to a normal state in accounting first */
		itr = list_iterator_create(delete_list);
		while ((found_record = list_next(itr))) {
			if (found_record->state & BG_BLOCK_ERROR_FLAG)
				resume_block(found_record);
		}
		list_iterator_destroy(itr);

		slurm_mutex_unlock(&block_state_mutex);
		free_block_list(NO_VAL, delete_list, 0, 0);
		list_destroy(delete_list);
	} else if (block_desc_ptr->state == BG_BLOCK_BOOTING) {
		/* This means recreate the block, remove it and then
		   recreate it.
		*/

		/* make sure if we are removing a block to put it back
		   to a normal state in accounting first */
		if (bg_record->state & BG_BLOCK_ERROR_FLAG)
			resume_block(bg_record);

		term_jobs_on_block(bg_record->bg_block_id);
		if (bg_conf->slurm_debug_flags & DEBUG_FLAG_SELECT_TYPE)
			info("select_p_update_block: "
			     "freeing the block %s.", bg_record->bg_block_id);
		/* Increment free_cnt to make sure we don't loose this
		 * block since bg_free_block will unlock block_state_mutex.
		 */
		bg_record->free_cnt++;
		bg_free_block(bg_record, 1, 1);
		bg_record->free_cnt--;
		if (bg_conf->slurm_debug_flags & DEBUG_FLAG_SELECT_TYPE)
			info("select_p_update_block: done");

		/* Now remove it from the main list since we are
		   looking for a state change and it won't be caught
		   unless it is in the main list until now.
		*/
		remove_from_bg_list(bg_lists->main, bg_record);

#if defined HAVE_BG_FILES
		if (bg_conf->slurm_debug_flags & DEBUG_FLAG_SELECT_TYPE)
			info("select_p_update_block: "
			     "removing %s from database",
			     bg_record->bg_block_id);

		rc = bridge_block_remove(bg_record);
		if (rc != SLURM_SUCCESS) {
			if (rc == BG_ERROR_BLOCK_NOT_FOUND) {
				debug("select_p_update_block: "
				      "block %s is not found",
				      bg_record->bg_block_id);
			} else {
				error("select_p_update_block: "
				      "rm_remove_partition(%s): %s",
				      bg_record->bg_block_id,
				      bg_err_str(rc));
			}
		} else
			if (bg_conf->slurm_debug_flags & DEBUG_FLAG_SELECT_TYPE)
				info("select_p_update_block: done %s",
				     (char *)bg_record->bg_block_id);
#endif
		xfree(bg_record->bg_block_id);
		if (bridge_block_create(bg_record) == SLURM_ERROR) {
			destroy_bg_record(bg_record);
			error("select_p_update_block: "
			      "unable to configure block in api");
		} else {
			print_bg_record(bg_record);
			list_append(bg_lists->main, bg_record);
			sort_bg_record_inc_size(bg_lists->main);
		}

		slurm_mutex_unlock(&block_state_mutex);
	} else {
		slurm_mutex_unlock(&block_state_mutex);
		error("state is ? %s",
		      bg_block_state_string(block_desc_ptr->state));
		return ESLURM_INVALID_NODE_STATE;
	}

	/* info("%s", reason); */
	last_bg_update = time(NULL);

	return rc;
#else
	return SLURM_ERROR;
#endif
}

extern int select_p_update_sub_node (update_block_msg_t *block_desc_ptr)
{
#ifdef HAVE_BG
	int rc = SLURM_SUCCESS;
	int i = 0, j = 0;
	char coord[SYSTEM_DIMENSIONS+1], *node_name = NULL;
	char ionodes[128];
	int set = 0;
	double nc_pos = 0, last_pos = -1;
	bitstr_t *ionode_bitmap = NULL;
	char *name = NULL;

	if (bg_conf->layout_mode != LAYOUT_DYNAMIC) {
		info("You can't use this call unless you are on a Dynamically "
		     "allocated system.  Please use update BlockName instead");
		rc = ESLURM_INVALID_BLOCK_LAYOUT;
		goto end_it;
	}

	memset(coord, 0, sizeof(coord));
	memset(ionodes, 0, 128);
	if (!block_desc_ptr->mp_str) {
		error("update_sub_node: No name specified");
		rc = ESLURM_INVALID_BLOCK_NAME;
		goto end_it;
	}
	name = block_desc_ptr->mp_str;

	while (name[j] != '\0') {
		if (name[j] == '[') {
			if (set<1) {
				rc = SLURM_ERROR;
				goto end_it;
			}
			i = j++;
			if ((name[j] < '0'
			     || name[j] > 'Z'
			     || (name[j] > '9'
				 && name[j] < 'A'))) {
				error("update_sub_node: sub block is empty");
				rc = SLURM_ERROR;
				goto end_it;
			}
			while (name[i] != '\0') {
				if (name[i] == ']')
					break;
				i++;
			}
			if (name[i] != ']') {
				error("update_sub_node: "
				      "No close (']') on sub block");
				rc = SLURM_ERROR;
				goto end_it;
			}

			strncpy(ionodes, name+j, i-j);
			set++;
			break;
		} else if ((name[j] >= '0'
			    && name[j] <= '9')
			   || (name[j] >= 'A'
			       && name[j] <= 'Z')) {
			if (set) {
				rc = SLURM_ERROR;
				goto end_it;
			}
			/* make sure we are asking for a correct name */
			for(i = 0; i < SYSTEM_DIMENSIONS; i++) {
				if ((name[j+i] >= '0'
				     && name[j+i] <= '9')
				    || (name[j+i] >= 'A'
					&& name[j+i] <= 'Z'))
					continue;

				error("update_sub_node: "
				      "misformatted name given %s",
				      name);
				rc = SLURM_ERROR;
				goto end_it;
			}

			strncpy(coord, name+j,
				SYSTEM_DIMENSIONS);
			j += SYSTEM_DIMENSIONS-1;
			set++;
		}
		j++;
	}

	if (set != 2) {
		error("update_sub_node: "
		      "I didn't get the base partition and the sub part.");
		rc = SLURM_ERROR;
		goto end_it;
	}
	ionode_bitmap = bit_alloc(bg_conf->ionodes_per_mp);
	bit_unfmt(ionode_bitmap, ionodes);
	if (bit_ffs(ionode_bitmap) == -1) {
		error("update_sub_node: Invalid ionode '%s' given.", ionodes);
		rc = SLURM_ERROR;
		FREE_NULL_BITMAP(ionode_bitmap);
		goto end_it;
	}
	node_name = xstrdup_printf("%s%s", bg_conf->slurm_node_prefix, coord);
	/* find out how many nodecards to get for each ionode */
	if (block_desc_ptr->state == BG_BLOCK_ERROR_FLAG) {
		info("Admin setting %s[%s] in an error state",
		     node_name, ionodes);
		for(i = 0; i<bg_conf->ionodes_per_mp; i++) {
			if (bit_test(ionode_bitmap, i)) {
				if ((int)nc_pos != (int)last_pos) {
					/* find first bit in nc */
					int start_io =
						(int)nc_pos * bg_conf->io_ratio;
					down_nodecard(node_name, start_io, 0);
					last_pos = nc_pos;
				}
			}
			nc_pos += bg_conf->nc_ratio;
		}
	} else if (block_desc_ptr->state == BG_BLOCK_FREE) {
		info("Admin setting %s[%s] in an free state",
		     node_name, ionodes);
		up_nodecard(node_name, ionode_bitmap);
	} else {
		error("update_sub_node: Unknown state %s",
		      bg_block_state_string(block_desc_ptr->state));
		rc = ESLURM_INVALID_BLOCK_STATE;
	}

	FREE_NULL_BITMAP(ionode_bitmap);
	xfree(node_name);

	last_bg_update = time(NULL);
end_it:
	return rc;
#else
	return SLURM_ERROR;
#endif
}

extern int select_p_get_info_from_plugin (enum select_plugindata_info dinfo,
					  struct job_record *job_ptr,
					  void *data)
{
#ifdef HAVE_BG
	uint16_t *tmp16 = (uint16_t *) data;
	uint32_t *tmp32 = (uint32_t *) data;
	List *tmp_list = (List *) data;
	int rc = SLURM_SUCCESS;

	switch(dinfo) {
	case SELECT_CR_PLUGIN:
		*tmp32 = 0;
		break;
	case SELECT_STATIC_PART:
		if (bg_conf->layout_mode == LAYOUT_STATIC)
			*tmp16 = 1;
		else
			*tmp16 = 0;
		break;

	case SELECT_CONFIG_INFO:
		*tmp_list = _get_config();
		break;
	default:
		error("select_p_get_info_from_plugin info %d invalid",
		      dinfo);
		rc = SLURM_ERROR;
		break;
	}

	return rc;
#else
	return SLURM_ERROR;
#endif
}

extern int select_p_update_node_config (int index)
{
#ifdef HAVE_BG
	return SLURM_SUCCESS;
#else
	return SLURM_ERROR;
#endif
}

extern int select_p_update_node_state(struct node_record *node_ptr)
{
#ifdef HAVE_BG
	ba_mp_t *curr_mp;

	xassert(node_ptr);

	if(!(curr_mp = str2ba_mp(node_ptr->name)))
		return SLURM_ERROR;

	ba_update_mp_state(curr_mp, node_ptr->node_state);
	return SLURM_SUCCESS;
#else
	return SLURM_ERROR;
#endif
}

extern int select_p_alter_node_cnt(enum select_node_cnt type, void *data)
{
#ifdef HAVE_BG
	job_desc_msg_t *job_desc = (job_desc_msg_t *)data;
	uint16_t *cpus = (uint16_t *)data;
	uint32_t *nodes = (uint32_t *)data, tmp = 0;
	int i;
	uint16_t req_geometry[SYSTEM_DIMENSIONS];

	if (!bg_conf->mp_cnode_cnt) {
		fatal("select_p_alter_node_cnt: This can't be called "
		      "before init");
	}

	switch (type) {
	case SELECT_GET_NODE_SCALING:
		if ((*nodes) != INFINITE)
			(*nodes) = bg_conf->mp_cnode_cnt;
		break;
	case SELECT_GET_NODE_CPU_CNT:
		if ((*cpus) != (uint16_t)INFINITE)
			(*cpus) = bg_conf->cpu_ratio;
		break;
	case SELECT_GET_MP_CPU_CNT:
		if ((*nodes) != INFINITE)
			(*nodes) = bg_conf->cpus_per_mp;
		break;
	case SELECT_SET_MP_CNT:
		if (((*nodes) == INFINITE) || ((*nodes) == NO_VAL))
			tmp = (*nodes);
		else if ((*nodes) > bg_conf->mp_cnode_cnt) {
			tmp = (*nodes);
			tmp /= bg_conf->mp_cnode_cnt;
			if (tmp < 1)
				tmp = 1;
		} else
			tmp = 1;
		(*nodes) = tmp;
		break;
	case SELECT_APPLY_NODE_MIN_OFFSET:
		if ((*nodes) == 1) {
			/* Job will actually get more than one c-node,
			 * but we can't be sure exactly how much so we
			 * don't scale up this value. */
			break;
		}
		(*nodes) *= bg_conf->mp_cnode_cnt;
		break;
	case SELECT_APPLY_NODE_MAX_OFFSET:
		if ((*nodes) != INFINITE)
			(*nodes) *= bg_conf->mp_cnode_cnt;
		break;
	case SELECT_SET_NODE_CNT:
		get_select_jobinfo(job_desc->select_jobinfo->data,
				   SELECT_JOBDATA_ALTERED, &tmp);
		if (tmp == 1) {
			return SLURM_SUCCESS;
		}
		tmp = 1;
		set_select_jobinfo(job_desc->select_jobinfo->data,
				   SELECT_JOBDATA_ALTERED, &tmp);

		if (job_desc->min_nodes == (uint32_t) NO_VAL)
			return SLURM_SUCCESS;

		get_select_jobinfo(job_desc->select_jobinfo->data,
				   SELECT_JOBDATA_GEOMETRY, &req_geometry);

		if (req_geometry[0] != 0
		    && req_geometry[0] != (uint16_t)NO_VAL) {
			job_desc->min_nodes = 1;
			for (i=0; i<SYSTEM_DIMENSIONS; i++)
				job_desc->min_nodes *=
					(uint16_t)req_geometry[i];
			job_desc->min_nodes *= bg_conf->mp_cnode_cnt;
			job_desc->max_nodes = job_desc->min_nodes;
		}

		/* make sure if the user only specified min_cpus to
		   set min_nodes correctly
		*/
		if ((job_desc->min_cpus != NO_VAL)
		    && (job_desc->min_cpus > job_desc->min_nodes))
			job_desc->min_nodes =
				job_desc->min_cpus / bg_conf->cpu_ratio;

		/* initialize min_cpus to the min_nodes */
		job_desc->min_cpus = job_desc->min_nodes * bg_conf->cpu_ratio;

		if ((job_desc->max_nodes == (uint32_t) NO_VAL)
		    || (job_desc->max_nodes < job_desc->min_nodes))
			job_desc->max_nodes = job_desc->min_nodes;

		/* See if min_nodes is greater than one base partition */
		if (job_desc->min_nodes > bg_conf->mp_cnode_cnt) {
			/*
			 * if it is make sure it is a factor of
			 * bg_conf->mp_cnode_cnt, if it isn't make it
			 * that way
			 */
			tmp = job_desc->min_nodes % bg_conf->mp_cnode_cnt;
			if (tmp > 0)
				job_desc->min_nodes +=
					(bg_conf->mp_cnode_cnt-tmp);
		}
		tmp = job_desc->min_nodes / bg_conf->mp_cnode_cnt;

		/* this means it is greater or equal to one mp */
		if (tmp > 0) {
			set_select_jobinfo(job_desc->select_jobinfo->data,
					   SELECT_JOBDATA_NODE_CNT,
					   &job_desc->min_nodes);
			job_desc->min_nodes = tmp;
			job_desc->min_cpus = bg_conf->cpus_per_mp * tmp;
		} else {
#ifdef HAVE_BGL
			if (job_desc->min_nodes <= bg_conf->nodecard_cnode_cnt
			    && bg_conf->nodecard_ionode_cnt)
				job_desc->min_nodes =
					bg_conf->nodecard_cnode_cnt;
			else if (job_desc->min_nodes
				 <= bg_conf->quarter_cnode_cnt)
				job_desc->min_nodes =
					bg_conf->quarter_cnode_cnt;
			else
				job_desc->min_nodes =
					bg_conf->mp_cnode_cnt;

			set_select_jobinfo(job_desc->select_jobinfo->data,
					   SELECT_JOBDATA_NODE_CNT,
					   &job_desc->min_nodes);

			tmp = bg_conf->mp_cnode_cnt/job_desc->min_nodes;

			job_desc->min_cpus = bg_conf->cpus_per_mp/tmp;
			job_desc->min_nodes = 1;
#else
			i = bg_conf->smallest_block;
			while (i <= bg_conf->mp_cnode_cnt) {
				if (job_desc->min_nodes <= i) {
					job_desc->min_nodes = i;
					break;
				}
				i *= 2;
			}

			set_select_jobinfo(job_desc->select_jobinfo->data,
					   SELECT_JOBDATA_NODE_CNT,
					   &job_desc->min_nodes);

			job_desc->min_cpus = job_desc->min_nodes
				* bg_conf->cpu_ratio;
			job_desc->min_nodes = 1;
#endif
		}

		if (job_desc->max_nodes > bg_conf->mp_cnode_cnt) {
			tmp = job_desc->max_nodes % bg_conf->mp_cnode_cnt;
			if (tmp > 0)
				job_desc->max_nodes +=
					(bg_conf->mp_cnode_cnt-tmp);
		}
		tmp = job_desc->max_nodes / bg_conf->mp_cnode_cnt;

		if (tmp > 0) {
			job_desc->max_nodes = tmp;
			job_desc->max_cpus =
				job_desc->max_nodes * bg_conf->cpus_per_mp;
			tmp = NO_VAL;
		} else {
#ifdef HAVE_BGL
			if (job_desc->max_nodes <= bg_conf->nodecard_cnode_cnt
			    && bg_conf->nodecard_ionode_cnt)
				job_desc->max_nodes =
					bg_conf->nodecard_cnode_cnt;
			else if (job_desc->max_nodes
				 <= bg_conf->quarter_cnode_cnt)
				job_desc->max_nodes =
					bg_conf->quarter_cnode_cnt;
			else
				job_desc->max_nodes =
					bg_conf->mp_cnode_cnt;

			tmp = bg_conf->mp_cnode_cnt/job_desc->max_nodes;
			job_desc->max_cpus = bg_conf->cpus_per_mp/tmp;
			job_desc->max_nodes = 1;
#else
			i = bg_conf->smallest_block;
			while (i <= bg_conf->mp_cnode_cnt) {
				if (job_desc->max_nodes <= i) {
					job_desc->max_nodes = i;
					break;
				}
				i *= 2;
			}
			job_desc->max_cpus =
				job_desc->max_nodes * bg_conf->cpu_ratio;

			job_desc->max_nodes = 1;
#endif
		}
		tmp = NO_VAL;

		break;
	default:
		error("unknown option %d for alter_node_cnt", type);
	}

	return SLURM_SUCCESS;
#else
	return SLURM_ERROR;
#endif
}

extern int select_p_reconfigure(void)
{
#ifdef HAVE_BG
	slurm_conf_lock();
	if (!slurmctld_conf.slurm_user_name
	    || strcmp(bg_conf->slurm_user_name, slurmctld_conf.slurm_user_name))
		error("The slurm user has changed from '%s' to '%s'.  "
		      "If this is really what you "
		      "want you will need to restart slurm for this "
		      "change to be enforced in the bluegene plugin.",
		      bg_conf->slurm_user_name, slurmctld_conf.slurm_user_name);
	if (!slurmctld_conf.node_prefix
	    || strcmp(bg_conf->slurm_node_prefix, slurmctld_conf.node_prefix))
		error("Node Prefix has changed from '%s' to '%s'.  "
		      "If this is really what you "
		      "want you will need to restart slurm for this "
		      "change to be enforced in the bluegene plugin.",
		      bg_conf->slurm_node_prefix, slurmctld_conf.node_prefix);
	bg_conf->slurm_debug_flags = slurmctld_conf.debug_flags;
	bg_conf->slurm_debug_level = slurmctld_conf.slurmctld_debug;
	set_ba_debug_flags(bg_conf->slurm_debug_flags);
	slurm_conf_unlock();

	return SLURM_SUCCESS;
#else
	return SLURM_ERROR;
#endif
}

extern bitstr_t *select_p_resv_test(bitstr_t *avail_bitmap, uint32_t node_cnt)
{
	return NULL;
}

extern void select_p_ba_init(node_info_msg_t *node_info_ptr, bool sanity_check)
{
	ba_init(node_info_ptr, sanity_check);
}

extern void select_p_ba_fini(void)
{
	ba_fini();
}

extern int *select_p_ba_get_dims(void)
{
#ifdef HAVE_BG
	return DIM_SIZE;
#else
	return NULL;
#endif
}

extern void select_p_ba_reset(bool track_down_nodes)
{
	reset_ba_system(track_down_nodes);
}

extern int select_p_ba_request_apply(select_ba_request_t *ba_request)
{
	return new_ba_request(ba_request);
}

extern int select_p_ba_remove_block(List mps, bool is_small)
{
	return remove_block(mps, is_small);
}

