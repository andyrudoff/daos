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
/*
 * Target Methods
 */
#define DD_SUBSYS	DD_FAC(mgmt)

#include <sys/stat.h>
#include <fcntl.h>
#include <sys/sysinfo.h>
#include <ftw.h>

#include "srv_internal.h"

#include <daos_srv/vos.h>
#include <daos_srv/pool.h>
#include <daos_srv/daos_mgmt_srv.h>

/** directory for newly created pool, reclaimed on restart */
static char		*newborns_path;
static size_t		 newborns_path_size;
/** directory for destroyed pool */
static char		*zombies_path;
static size_t		 zombies_path_size;

static inline int
dir_fsync(const char *path)
{
	int	fd;
	int	rc;

	fd = open(path, O_RDONLY|O_DIRECTORY);
	if (fd < 0) {
		D_ERROR("failed to open %s for sync: %d\n", path, errno);
		return daos_errno2der(errno);
	}

	rc = fsync(fd);
	if (rc < 0) {
		D_ERROR("failed to fync %s: %d\n", path, errno);
		rc = daos_errno2der(errno);
	}

	(void)close(fd);

	return rc;
}

static int
destroy_cb(const char *path, const struct stat *sb, int flag,
	   struct FTW *ftwbuf)
{
	int rc;

	if (ftwbuf->level == 0)
		return 0;

	if (flag == FTW_DP || flag == FTW_D)
		rc = rmdir(path);
	else
		rc = unlink(path);
	if (rc)
		D_ERROR("failed to remove %s\n", path);
	return rc;
}

static int
subtree_destroy(const char *path)
{
	int rc;

	rc = nftw(path, destroy_cb, 32, FTW_DEPTH | FTW_PHYS | FTW_MOUNT);
	if (rc)
		rc = daos_errno2der(errno);

	return rc;
}

/* Allocate a new string that is the concatenation of "s1" and "s2". */
char *
stracat(const char *s1, const char *s2, size_t *size)
{
	char   *s3;
	size_t	l1;
	size_t	l2;
	size_t	l3;

	D_ASSERT(s1 != NULL && s2 != NULL);

	l1 = strlen(s1);
	l2 = strlen(s2);
	l3 = l1 + l2;

	D_ALLOC(s3, l3 + 1);
	if (s3 == NULL)
		return NULL;

	memcpy(s3, s1, l1);		/* copy s1 */
	memcpy(s3 + l1, s2, l2);	/* copy s2 */
	s3[l3] = '\0';			/* terminate */

	*size = l3 + 1;
	return s3;
}

int
ds_mgmt_tgt_init(void)
{
	mode_t	stored_mode, mode;
	int	rc;

	/** create the path string */
	newborns_path = stracat(storage_path, "/NEWBORNS", &newborns_path_size);
	if (newborns_path == NULL)
		D_GOTO(err, rc = -DER_NOMEM);
	zombies_path = stracat(storage_path, "/ZOMBIES", &zombies_path_size);
	if (zombies_path == NULL)
		D_GOTO(err_newborns, rc = -DER_NOMEM);

	stored_mode = umask(0);
	mode = S_IRWXU | S_IRWXG | S_IRWXO;
	/** create NEWBORNS directory if it does not exist already */
	rc = mkdir(newborns_path, mode);
	if (rc < 0 && errno != EEXIST) {
		D_ERROR("failed to create NEWBORNS dir: %d\n", errno);
		umask(stored_mode);
		D_GOTO(err_zombies, rc = daos_errno2der(errno));
	}

	/** create ZOMBIES directory if it does not exist already */
	rc = mkdir(zombies_path, mode);
	if (rc < 0 && errno != EEXIST) {
		D_ERROR("failed to create ZOMBIES dir: %d\n", errno);
		umask(stored_mode);
		D_GOTO(err_zombies, rc = daos_errno2der(errno));
	}
	umask(stored_mode);

	/** remove leftover from previous runs */
	rc = subtree_destroy(newborns_path);
	if (rc)
		/** only log error, will try again next time */
		D_ERROR("failed to cleanup NEWBORNS dir: %d, will try again\n",
			rc);

	rc = subtree_destroy(zombies_path);
	if (rc)
		/** only log error, will try again next time */
		D_ERROR("failed to cleanup ZOMBIES dir: %d, will try again\n",
			rc);
	return 0;

err_zombies:
	D_FREE(zombies_path, zombies_path_size);
err_newborns:
	D_FREE(newborns_path, newborns_path_size);
err:
	return rc;
}

void
ds_mgmt_tgt_fini(void)
{
	D_FREE(zombies_path, zombies_path_size);
	D_FREE(newborns_path, newborns_path_size);
}

static int
path_gen(const uuid_t pool_uuid, const char *dir, const char *fname, int *idx,
	 char **fpath)
{
	int	 size;
	int	 off;

	/** *fpath = dir + "/" + pool_uuid + "/" + fname + idx */

	/** DAOS_UUID_STR_SIZE includes the trailing '\0' */
	size = strlen(dir) + 1 /* "/" */ + DAOS_UUID_STR_SIZE;
	if (fname != NULL || idx != NULL)
		size += 1 /* "/" */;
	if (fname)
		size += strlen(fname);
	if (idx)
		size += snprintf(NULL, 0, "%d", *idx);

	D_ALLOC(*fpath, size);
	if (*fpath == NULL)
		return -DER_NOMEM;

	off = sprintf(*fpath, "%s", dir);
	off += sprintf(*fpath + off, "/");
	uuid_unparse_lower(pool_uuid, *fpath + off);
	off += DAOS_UUID_STR_SIZE - 1;
	if (fname != NULL || idx != NULL)
		off += sprintf(*fpath + off, "/");
	if (fname)
		off += sprintf(*fpath + off, fname);
	if (idx)
		sprintf(*fpath + off, "%d", *idx);

	return 0;
}

/**
 * Generate path to a target file for pool \a pool_uuid with a filename set to
 * \a fname and suffixed by \a idx. \a idx can be NULL.
 */
int
ds_mgmt_tgt_file(const uuid_t pool_uuid, const char *fname, int *idx,
		 char **fpath)
{
	return path_gen(pool_uuid, storage_path, fname, idx, fpath);
}

static int
tgt_vos_create(uuid_t uuid, daos_size_t tgt_size)
{
	daos_size_t	 size;
	int		 i;
	char		*path = NULL;
	int		 fd = -1;
	int		 rc = 0;

	/**
	 * Create one VOS file per execution stream
	 * 16MB minimum per file
	 */
	size = max(tgt_size / dss_nxstreams, 1 << 24);
	/** tc_in->tc_tgt_dev is assumed to point at PMEM for now */

	for (i = 1; i <= dss_nxstreams; i++) {

		rc = path_gen(uuid, newborns_path, VOS_FILE, &i, &path);
		if (rc)
			break;

		D_DEBUG(DB_MGMT, DF_UUID": creating vos file %s\n",
			DP_UUID(uuid), path);

		fd = open(path, O_CREAT|O_RDWR, 0600);
		if (fd < 0) {
			D_ERROR(DF_UUID": failed to create vos file %s: %d\n",
				DP_UUID(uuid), path, rc);
			rc = daos_errno2der(errno);
			break;
		}

		rc = posix_fallocate(fd, 0, size);
		if (rc) {
			D_ERROR(DF_UUID": failed to allocate vos file %s with "
				"size: "DF_U64", rc: %d.\n",
				DP_UUID(uuid), path, size, rc);
			rc = daos_errno2der(rc);
			break;
		}

		/* A zero size accommodates the existing file */
		rc = vos_pool_create(path, (unsigned char *)uuid, 0 /* size */);
		if (rc) {
			D_ERROR(DF_UUID": failed to init vos pool %s: %d\n",
				DP_UUID(uuid), path, rc);
			break;
		}

		rc = fsync(fd);
		(void)close(fd);
		fd = -1;
		if (rc) {
			D_ERROR(DF_UUID": failed to sync vos pool %s: %d\n",
				DP_UUID(uuid), path, rc);
			rc = daos_errno2der(errno);
			break;
		}
	}
	if (path)
		free(path);
	if (fd >= 0)
		(void)close(fd);

	/** brute force cleanup to be done by the caller */
	return rc;
}

static int
tgt_create(uuid_t pool_uuid, uuid_t tgt_uuid, daos_size_t size, char *path)
{
	char	*newborn = NULL;
	int	 rc;

	/** XXX: many synchronous/blocking operations below */

	/** create the pool directory under NEWBORNS */
	rc = path_gen(pool_uuid, newborns_path, NULL, NULL, &newborn);
	if (rc)
		return rc;

	rc = mkdir(newborn, 0700);
	if (rc < 0 && errno != EEXIST) {
		D_ERROR("failed to created pool directory: %d\n", rc);
		D_GOTO(out, rc = daos_errno2der(errno));
	}

	/** create VOS files */
	rc = tgt_vos_create(pool_uuid, size);
	if (rc)
		D_GOTO(out_tree, rc);

	/** initialize DAOS-M target and fetch uuid */
	rc = ds_pool_create(pool_uuid, newborn, tgt_uuid);
	if (rc) {
		D_ERROR("ds_pool_create failed, rc: %d.\n", rc);
		D_GOTO(out_tree, rc);
	}

	/** ready for prime time, move away from NEWBORNS dir */
	rc = rename(newborn, path);
	if (rc < 0) {
		D_ERROR("failed to rename pool directory: %d\n", rc);
		D_GOTO(out_tree, rc = daos_errno2der(errno));
	}

	/** make sure the rename is persistent */
	rc = dir_fsync(path);

	D_GOTO(out, rc);

out_tree:
	/** cleanup will be re-executed on several occasions */
	(void)subtree_destroy(newborn);
	(void)rmdir(newborn);
out:
	free(newborn);
	return rc;
}

/**
 * RPC handler for target creation
 */
int
ds_mgmt_hdlr_tgt_create(crt_rpc_t *tc_req)
{
	struct mgmt_tgt_create_in	*tc_in;
	struct mgmt_tgt_create_out	*tc_out;
	char				*path = NULL;
	int				 rc = 0;

	/** incoming request buffer */
	tc_in = crt_req_get(tc_req);
	/** reply buffer */
	tc_out = crt_reply_get(tc_req);
	D_ASSERT(tc_in != NULL && tc_out != NULL);

	/** generate path to the target directory */
	rc = ds_mgmt_tgt_file(tc_in->tc_pool_uuid, NULL, NULL, &path);
	if (rc)
		D_GOTO(out, rc);

	/** check whether the target already exists */
	rc = access(path, F_OK);
	if (rc >= 0) {
		/** target already exists, let's reuse it for idempotence */
		/** TODO: fetch tgt uuid from existing DSM pool */
		uuid_generate(tc_out->tc_tgt_uuid);

		/**
		 * flush again in case the previous one in tgt_create()
		 * failed
		 */
		rc = dir_fsync(path);
	} else if (errno == ENOENT) {
		/** target doesn't exist, create one */
		rc = tgt_create(tc_in->tc_pool_uuid, tc_out->tc_tgt_uuid,
				tc_in->tc_tgt_size, path);
	} else {
		rc = daos_errno2der(errno);
	}

	free(path);
out:
	tc_out->tc_rc = rc;
	return crt_reply_send(tc_req);
}

static int
tgt_destroy(uuid_t pool_uuid, char *path)
{
	char	*zombie = NULL;
	int	 rc;


	/** XXX: many synchronous/blocking operations below */

	/** move target directory to ZOMBIES */
	rc = path_gen(pool_uuid, zombies_path, NULL, NULL, &zombie);
	if (rc)
		return rc;

	rc = rename(path, zombie);
	if (rc < 0)
		D_GOTO(out, rc = daos_errno2der(errno));

	/** make sure the rename is persistent */
	rc = dir_fsync(zombie);
	if (rc < 0)
		D_GOTO(out, rc);

	/**
	 * once successfully moved to the ZOMBIES directory, the target will
	 * take care of retrying on failure and thus always report success to
	 * the caller.
	 */
	(void)subtree_destroy(zombie);
	(void)rmdir(zombie);
out:
	free(zombie);
	return rc;
}

/**
 * RPC handler for target destroy
 */
int
ds_mgmt_hdlr_tgt_destroy(crt_rpc_t *td_req)
{
	struct mgmt_tgt_destroy_in	*td_in;
	struct mgmt_tgt_destroy_out	*td_out;
	char				*path;
	int				  rc;

	/** incoming request buffer */
	td_in = crt_req_get(td_req);
	/** reply buffer */
	td_out = crt_reply_get(td_req);
	D_ASSERT(td_in != NULL && td_out != NULL);

	/** generate path to the target directory */
	rc = ds_mgmt_tgt_file(td_in->td_pool_uuid, NULL, NULL, &path);
	if (rc)
		D_GOTO(out, rc);

	/** check whether the target exists */
	rc = access(path, F_OK);
	if (rc >= 0) {
		/** target is still there, destroy it */
		rc = tgt_destroy(td_req->cr_input, path);
	} else if (errno == ENOENT) {
		char	*zombie;

		/**
		 * target is gone already, report success for idempotence
		 * that said, the previous flush in tgt_destroy() might have
		 * failed, so flush again.
		 */
		rc = path_gen(td_in->td_pool_uuid, zombies_path, NULL, NULL,
			      &zombie);
		if (rc)
			D_GOTO(out, rc);
		rc = dir_fsync(path);
		if (rc == -DER_NONEXIST)
			rc = 0;
		free(zombie);
	} else {
		rc = daos_errno2der(errno);
	}

	free(path);
out:
	td_out->td_rc = rc;
	return crt_reply_send(td_req);
}
