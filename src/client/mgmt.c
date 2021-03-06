/**
 * (C) Copyright 2015, 2016 Intel Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
 * The Government's rights to use, modify, reproduce, release, perform, display,
 * or disclose this software are subject to the terms of the Apache License as
 * provided in Contract No. B609815.
 * Any reproduction of computer software, computer software documentation, or
 * portions thereof marked with this legend must also reproduce the markings.
 */
#define DD_SUBSYS	DD_FAC(client)

#include <daos/mgmt.h>
#include <daos/pool.h>
#include <client_internal.h>

int
daos_mgmt_svc_rip(const char *grp, daos_rank_t rank, bool force,
		  daos_event_t *ev)
{
	struct daos_task	*task;
	int			rc;

	rc = daos_client_task_prep(daos_event_comp_cb, NULL, 0, &task, &ev);
	if (rc != 0)
		return rc;

	dc_mgmt_svc_rip(grp, rank, force, task);

	return daos_client_result_wait(ev);
}

int
daos_pool_create(unsigned int mode, unsigned int uid, unsigned int gid,
		 const char *grp, const daos_rank_list_t *tgts, const char *dev,
		 daos_size_t size, daos_rank_list_t *svc, uuid_t uuid,
		 daos_event_t *ev)
{
	struct daos_task	*task;
	int			rc;

	rc = daos_client_task_prep(daos_event_comp_cb, NULL, 0, &task, &ev);
	if (rc != 0)
		return rc;

	dc_pool_create(mode, uid, gid, grp, tgts, dev, size, svc, uuid, task);

	return daos_client_result_wait(ev);
}

int
daos_pool_destroy(const uuid_t uuid, const char *grp, int force,
		  daos_event_t *ev)
{
	struct daos_task	*task;
	int			rc;

	rc = daos_client_task_prep(daos_event_comp_cb, NULL, 0, &task, &ev);
	if (rc != 0)
		return rc;

	dc_pool_destroy(uuid, grp, force, task);

	return daos_client_result_wait(ev);
}

int
daos_pool_evict(const uuid_t uuid, const char *grp, daos_event_t *ev)
{
	struct daos_task	*task;
	int			rc;

	rc = daos_client_task_prep(daos_event_comp_cb, NULL, 0, &task, &ev);
	if (rc != 0)
		return rc;

	dc_pool_evict(uuid, grp, task);

	return daos_client_result_wait(ev);
}
