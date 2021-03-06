/*****************************************************************************\
 *  groups.c - Functions to gather group membership information
 *             These functions utilize a cache for performance reasons
 *****************************************************************************
 *  Copyright (C) 2010 Lawrence Livermore National Security.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Morris Jette <jette1@llnl.gov> et. al.
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

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif
/* needed for getgrent_r */
#ifndef   _GNU_SOURCE
#  define _GNU_SOURCE
#endif
#ifndef   __USE_GNU
#  define   __USE_GNU
#endif

#include <grp.h>
#include <pthread.h>
#include <pwd.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#include "src/common/list.h"
#include "src/common/log.h"
#include "src/common/uid.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

#define _DEBUG 0

static void   _cache_del_func(void *x);
static uid_t *_get_group_cache(char *group_name);
static void   _log_group_members(char *group_name, uid_t *group_uids);
static void   _put_group_cache(char *group_name, void *group_uids, int uid_cnt);

static List group_cache_list = NULL;
static pthread_mutex_t group_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
struct group_cache_rec {
	char *group_name;
	int uid_cnt;
	uid_t *group_uids;
};

/*
 * get_group_members - identify the users in a given group name
 * IN group_name - a single group name
 * RET a zero terminated list of its UIDs or NULL on error
 * NOTE: User root has implicitly access to every group
 * NOTE: The caller must xfree non-NULL return values
 */
extern uid_t *get_group_members(char *group_name)
{
	char grp_buffer[PW_BUF_SIZE];
  	struct group grp,  *grp_result = NULL;
	struct passwd *pwd_result = NULL;
	uid_t *group_uids = NULL, my_uid;
	gid_t my_gid;
	int i, j, uid_cnt;
#ifdef HAVE_AIX
	FILE *fp = NULL;
#elif defined (__APPLE__) || defined (__CYGWIN__)
#else
	char pw_buffer[PW_BUF_SIZE];
	struct passwd pw;
#endif

	group_uids = _get_group_cache(group_name);
	if (group_uids)	{	/* We found in cache */
		_log_group_members(group_name, group_uids);
		return group_uids;
	}

	/* We need to check for !grp_result, since it appears some
	 * versions of this function do not return an error on failure.
	 */
	if (getgrnam_r(group_name, &grp, grp_buffer, PW_BUF_SIZE,
		       &grp_result) || (grp_result == NULL)) {
		error("Could not find configured group %s", group_name);
		return NULL;
	}
	my_gid = grp_result->gr_gid;

	j = 0;
	uid_cnt = 0;
#ifdef HAVE_AIX
	setgrent_r(&fp);
	while (!getgrent_r(&grp, grp_buffer, PW_BUF_SIZE, &fp)) {
		grp_result = &grp;
#elif defined (__APPLE__) || defined (__CYGWIN__)
	setgrent();
	while ((grp_result = getgrent()) != NULL) {
#else
	setgrent();
	while (getgrent_r(&grp, grp_buffer, PW_BUF_SIZE,
			  &grp_result) == 0 && grp_result != NULL) {
#endif
	        if (grp_result->gr_gid == my_gid) {
			if (strcmp(grp_result->gr_name, group_name)) {
				debug("including members of group '%s' as it "
				      "corresponds to the same gid as group"
				      " '%s'",grp_result->gr_name,group_name);
			}

		        for (i=0; grp_result->gr_mem[i]; i++) {
				if (uid_from_string(grp_result->gr_mem[i],
						    &my_uid) < 0) {
					/* Group member without valid login */
					continue;
				}
				if (my_uid == 0)
					continue;
				if (j >= uid_cnt) {
					uid_cnt += 100;
					xrealloc(group_uids, 
						 (sizeof(uid_t) * uid_cnt));
				}
				group_uids[j++] = my_uid;
			}
		}
	}
#ifdef HAVE_AIX
	endgrent_r(&fp);
	setpwent_r(&fp);
	while (!getpwent_r(&pw, pw_buffer, PW_BUF_SIZE, &fp)) {
		pwd_result = &pw;
#else
	endgrent();
	setpwent();
#if defined (__sun)
	while ((pwd_result = getpwent_r(&pw, pw_buffer, PW_BUF_SIZE)) != NULL) {
#elif defined (__APPLE__) || defined (__CYGWIN__)
	while ((pwd_result = getpwent()) != NULL) {
#else
	while (!getpwent_r(&pw, pw_buffer, PW_BUF_SIZE, &pwd_result)) {
#endif
#endif
 		if (pwd_result->pw_gid != my_gid)
			continue;
		if (j >= uid_cnt) {
			uid_cnt += 100;
			xrealloc(group_uids, (sizeof(uid_t) * uid_cnt));
		}
		group_uids[j++] = pwd_result->pw_uid;
	}
#ifdef HAVE_AIX
	endpwent_r(&fp);
#else
	endpwent();
#endif

	_put_group_cache(group_name, group_uids, uid_cnt);
	_log_group_members(group_name, group_uids);
	return group_uids;
}

/* Delete our group/uid cache */
extern void clear_group_cache(void)
{
	pthread_mutex_lock(&group_cache_mutex);
	if (group_cache_list) {
		list_destroy(group_cache_list);
		group_cache_list = NULL;
	}
	pthread_mutex_unlock(&group_cache_mutex);
}

/* Get a record from our group/uid cache. 
 * Return NULL if not found. */
static uid_t *_get_group_cache(char *group_name)
{
	ListIterator iter;
	struct group_cache_rec *cache_rec;
	uid_t *group_uids = NULL;
	int sz;

	pthread_mutex_lock(&group_cache_mutex);
	if (!group_cache_list) {
		pthread_mutex_unlock(&group_cache_mutex);
		return NULL;
	}

	iter = list_iterator_create(group_cache_list);
	if (!iter)
		fatal("list_iterator_create: malloc failure");
	while ((cache_rec = (struct group_cache_rec *) list_next(iter))) {
		if (strcmp(group_name, cache_rec->group_name))
			continue;
		sz = sizeof(uid_t) * (cache_rec->uid_cnt + 1);
		group_uids = (uid_t *) xmalloc(sz);
		memcpy(group_uids, cache_rec->group_uids, sz);
		break;
	}
	list_iterator_destroy(iter);
	pthread_mutex_unlock(&group_cache_mutex);
	return group_uids;
}

/* Delete a record from the group/uid cache, used by list functions */
static void _cache_del_func(void *x)
{
	struct group_cache_rec *cache_rec;

	cache_rec = (struct group_cache_rec *) x;
	xfree(cache_rec->group_name);
	xfree(cache_rec->group_uids);
	xfree(cache_rec);
}

/* Put a record on our group/uid cache */
static void _put_group_cache(char *group_name, void *group_uids, int uid_cnt)
{
	struct group_cache_rec *cache_rec;
	int sz;

	pthread_mutex_lock(&group_cache_mutex);
	if (!group_cache_list) {
		group_cache_list = list_create(_cache_del_func);
		if (!group_cache_list)
			fatal("list_create: malloc failure:");
	}

	sz = sizeof(uid_t) * (uid_cnt + 1);
	cache_rec = xmalloc(sizeof(struct group_cache_rec));
	cache_rec->group_name = xstrdup(group_name);
	cache_rec->uid_cnt    = uid_cnt;
	cache_rec->group_uids = (uid_t *) xmalloc(sz);
	if (uid_cnt > 0)
		memcpy(cache_rec->group_uids, group_uids, sz);
	list_append(group_cache_list, cache_rec);
	pthread_mutex_unlock(&group_cache_mutex);
}

static void _log_group_members(char *group_name, uid_t *group_uids)
{
#if _DEBUG
	int i;

	if ((group_uids == NULL) || (group_uids[0] == 0)) {
		info("Group %s has no users", group_name);
		return;
	}

	info("Group %s contains uids:", group_name);
	for (i=0; group_uids && group_uids[i]; i++)
		info("  %u", group_uids[i]);
#endif
}
