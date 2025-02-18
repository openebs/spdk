/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#ifndef SPDK_VBDEV_CRYPTO_H
#define SPDK_VBDEV_CRYPTO_H

#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk/accel.h"
#include "spdk/accel_module.h"

#include "spdk/bdev.h"

#define BDEV_CRYPTO_DEFAULT_CIPHER "AES_CBC" /* QAT and AESNI_MB */

/* Structure to hold crypto options */
struct vbdev_crypto_opts {
	char				*vbdev_name;	/* name of the vbdev to create */
	char				*bdev_name;	/* base bdev name */
	struct spdk_accel_crypto_key	*key;		/* crypto key */
	bool				key_owner;	/* If wet to true then the key was created by RPC and needs to be destroyed */
};

typedef void (*spdk_delete_crypto_complete)(void *cb_arg, int bdeverrno);

/**
 * Create new crypto bdev.
 *
 * \param opts Crypto options populated by create_crypto_opts()
 * \return 0 on success, other on failure.
 */
int create_crypto_disk(struct vbdev_crypto_opts *opts);

/**
 * Delete crypto bdev.
 *
 * \param bdev_name Crypto bdev name.
 * \param cb_fn Function to call after deletion.
 * \param cb_arg Argument to pass to cb_fn.
 */
void delete_crypto_disk(const char *bdev_name, spdk_delete_crypto_complete cb_fn,
			void *cb_arg);

/**
 * Create crypto opts for the given crypto vbdev name and base bdev name.
 *
 * \param name Name of crypto vbdev
 * \param base_bdev_name Name of base bdev for crypto vbdev
 * \param key crypto key for the vbdev
 * \param key_owner Is key created by application/RPC.
 * \return Handle to created vbdev_crypto_opts or NULL if failed ot create.
 */
struct vbdev_crypto_opts *
create_crypto_opts_by_name(char* name, char* base_bdev_name, struct spdk_accel_crypto_key *key,
		   bool key_owner);

/**
 * Release crypto opts created with create_crypto_opts()
 *
 * \param opts Crypto opts to release
 */
void free_crypto_opts(struct vbdev_crypto_opts *opts);

/**
 * Given a vbdev crypto name, get the handle for vbdev_crypto object.
 *
 * \param vbdev_name Name of crypto vbdev.
 * \return Handle to lvol_store_bdev or NULL if not found.
 */
struct spdk_bdev *
vbdev_crypto_disk_get_base_bdev(const char *vbdev_name);

#endif /* SPDK_VBDEV_CRYPTO_H */
