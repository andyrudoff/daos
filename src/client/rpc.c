/**
 * (C) Copyright 2016 Intel Corporation.
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

#include <daos/rpc.h>
#include <daos/scheduler.h>
#include <daos/event.h>

static int
daos_rpc_cb(const struct crt_cb_info *cb_info)
{
	struct daos_task	*task = cb_info->cci_arg;
	int			rc = cb_info->cci_rc;

	if (cb_info->cci_rc == -DER_TIMEDOUT)
		/** TODO */
		;

	daos_task_complete(task, rc);

	return 0;
}

int
daos_rpc_send(crt_rpc_t *rpc, struct daos_task *task)
{
	int rc;

	rc = crt_req_send(rpc, daos_rpc_cb, task);
	if (rc != 0) {
		daos_task_complete(task, rc);
		rc = 0;
	}

	return rc;
}
