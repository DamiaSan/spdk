/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "bdev_longhorn.h"
#include "bdev_longhorn_impl.h"
#include "bdev_longhorn_nvmf.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/json.h"
#include "spdk/string.h"

static bool g_shutdown_started = false;

/* longhorn bdev config as read from config file */
struct longhorn_config	g_longhorn_config = {
	.longhorn_bdev_config_head = TAILQ_HEAD_INITIALIZER(g_longhorn_config.longhorn_bdev_config_head),
};

/*
 * List of longhorn bdev in configured list, these longhorn bdevs are registered with
 * bdev layer
 */
struct longhorn_configured_tailq	g_longhorn_bdev_configured_list = TAILQ_HEAD_INITIALIZER(
			g_longhorn_bdev_configured_list);

/* List of longhorn bdev in configuring list */
struct longhorn_configuring_tailq	g_longhorn_bdev_configuring_list = TAILQ_HEAD_INITIALIZER(
			g_longhorn_bdev_configuring_list);

/* List of all longhorn bdevs */
struct longhorn_all_tailq		g_longhorn_bdev_list = TAILQ_HEAD_INITIALIZER(g_longhorn_bdev_list);

/* List of all longhorn bdevs that are offline */
struct longhorn_offline_tailq	g_longhorn_bdev_offline_list = TAILQ_HEAD_INITIALIZER(
			g_longhorn_bdev_offline_list);

/* Function declarations */
static void	longhorn_bdev_examine(struct spdk_bdev *bdev);
static int	longhorn_bdev_init(void);
static void	longhorn_bdev_deconfigure(struct longhorn_bdev *longhorn_bdev,
				      longhorn_bdev_destruct_cb cb_fn, void *cb_arg);
static void	longhorn_bdev_event_base_bdev(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
		void *event_ctx);

/*
 * brief:
 * longhorn_bdev_create_cb function is a cb function for longhorn bdev which creates the
 * hierarchy from longhorn bdev to base bdev io channels. It will be called per core
 * params:
 * io_device - pointer to longhorn bdev io device represented by longhorn_bdev
 * ctx_buf - pointer to context buffer for longhorn bdev io channel
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
longhorn_bdev_create_cb(void *io_device, void *ctx_buf)
{
	struct longhorn_bdev            *longhorn_bdev = io_device;
	struct longhorn_bdev_io_channel *longhorn_ch = ctx_buf;
	struct longhorn_base_bdev_info *base_info;
	struct spdk_thread *thread;
	struct longhorn_base_io_channel *base_channel;
	uint8_t i = 0;

	TAILQ_INIT(&longhorn_ch->base_channels);
	thread = spdk_get_thread();

	longhorn_ch->thread = thread;
	SPDK_DEBUGLOG(bdev_longhorn, "onghorn_bdev_create_cb, %p\n", longhorn_ch);
	SPDK_ERRLOG("longhorn_bdev_create_cb, %p, %p (%s)\n", longhorn_ch, thread, spdk_thread_get_name(thread));

	assert(longhorn_bdev != NULL);
	assert(longhorn_bdev->state == RAID_BDEV_STATE_ONLINE);

	longhorn_ch->num_channels = longhorn_bdev->num_base_bdevs;
	longhorn_ch->longhorn_bdev = longhorn_bdev;


#if 0
	// TODO linked list
	longhorn_ch->base_channel = calloc(longhorn_ch->num_channels,
				       sizeof(struct spdk_io_channel *));
	if (!longhorn_ch->base_channel) {
		SPDK_ERRLOG("Unable to allocate base bdevs io channel\n");
		return -ENOMEM;
	}
#endif

	TAILQ_FOREACH(base_info, &longhorn_bdev->base_bdevs_head, infos) {
		base_channel = calloc(1, sizeof(*base_channel));
		/*
		 * Get the spdk_io_channel for all the base bdevs. This is used during
		 * split logic to send the respective child bdev ios to respective base
		 * bdev io channel.
		 */
		base_channel->base_channel = spdk_bdev_get_io_channel(base_info->desc);
		//longhorn_ch->base_channel[i] = base_info->base_channel;

		//base_channel->base_channel = base_info->base_channel;
		SPDK_ERRLOG("base_info when creating io_channel %p\n", base_info);
		base_channel->base_info = base_info;

		if (!base_channel->base_channel) {
			SPDK_ERRLOG("Unable to create io channel for base bdev\n");
		}
#if 0
		if (!base_channel->base_channel[i]) {
			uint8_t j;

			for (j = 0; j < i; j++) {
				spdk_put_io_channel(longhorn_ch->base_channel[j]);
			}
			free(base_channel->base_channel);
			longhorn_ch->base_channel = NULL;
			SPDK_ERRLOG("Unable to create io channel for base bdev\n");
			return -ENOMEM;
		}
#endif

		TAILQ_INSERT_TAIL(&longhorn_ch->base_channels, 
				  base_channel, channels);


		++i;

	}

	TAILQ_INSERT_TAIL(&longhorn_bdev->io_channel_head, longhorn_ch, channels);
	longhorn_bdev->num_io_channels++;
		SPDK_ERRLOG("adding num io channels %u\n", longhorn_bdev->num_io_channels);

	return 0;
}

static void longhorn_check_pause_complete(struct longhorn_bdev *longhorn_bdev)
{
	struct longhorn_bdev_io_channel *io_channel;
	struct longhorn_pause_cb_entry *entry;
	struct longhorn_pause_cb_entry *next;

	TAILQ_FOREACH(io_channel, &longhorn_bdev->io_channel_head, channels) {
		if (!io_channel->pause_complete) {
			return;
		}
	}

	SPDK_ERRLOG("PAUSE COMPLETE \n");

	// Call pause callback(s).
	entry = TAILQ_FIRST(&longhorn_bdev->pause_cbs);

	while (entry != NULL) {

		if (entry->cb_fn != NULL) {
			entry->cb_fn(longhorn_bdev, entry->cb_arg);
		} else {
		SPDK_ERRLOG("PAUSE CB NULL \n");
		}
		next = TAILQ_NEXT(entry, link);

		free(entry);

		entry = next;
	}

	TAILQ_INIT(&longhorn_bdev->pause_cbs);
}

void bdev_longhorn_pause_io(void *cb_arg) {
	struct longhorn_bdev_io_channel *longhorn_ch = cb_arg;

	longhorn_ch->paused = true;

		SPDK_ERRLOG("PAUSE CB : %d \n", longhorn_ch->io_ops);


	if (longhorn_ch->io_ops == 0) {
		longhorn_ch->pause_complete = true;

		longhorn_check_pause_complete(longhorn_ch->longhorn_bdev);
	}

}

void bdev_longhorn_unpause_io(void *cb_arg) {
	struct longhorn_bdev_io_channel *longhorn_ch = cb_arg;

	longhorn_ch->paused = false;
	longhorn_ch->pause_complete = false;
}

void longhorn_volume_add_pause_cb(struct longhorn_bdev *longhorn_bdev,
				  longhorn_pause_cb cb_fn,
				  void *cb_arg) 
{
	struct longhorn_pause_cb_entry *entry;

	entry = calloc(1, sizeof(*entry));
	entry->cb_fn = cb_fn;
	entry->cb_arg = cb_arg;
		SPDK_ERRLOG("adding PAUSE CB  \n");

	TAILQ_INSERT_TAIL(&longhorn_bdev->pause_cbs, entry, link);
}

/*
 * brief:
 * longhorn_bdev_destroy_cb function is a cb function for longhorn bdev which deletes the
 * hierarchy from longhorn bdev to base bdev io channels. It will be called per core
 * params:
 * io_device - pointer to longhorn bdev io device represented by longhorn_bdev
 * ctx_buf - pointer to context buffer for longhorn bdev io channel
 * returns:
 * none
 */
static void
longhorn_bdev_destroy_cb(void *io_device, void *ctx_buf)
{
	struct longhorn_bdev            *longhorn_bdev = io_device;
	struct longhorn_bdev_io_channel *longhorn_ch = ctx_buf;
	struct longhorn_base_io_channel *base_channel;
	struct longhorn_base_io_channel *next;
	uint8_t i;

	SPDK_DEBUGLOG(bdev_longhorn, "longhorn_bdev_destroy_cb\n");
	SPDK_ERRLOG("longhorn_bdev_destroy_cb, %p\n", longhorn_ch);

	assert(longhorn_ch != NULL);


	base_channel = TAILQ_FIRST(&longhorn_ch->base_channels);

	while (base_channel != NULL) {
		next = TAILQ_NEXT(base_channel, channels);

		
		SPDK_ERRLOG("longhorn_bdev_destroy_cb, removing bdev %s\n", base_channel->base_info->bdev->name);
		spdk_put_io_channel(base_channel->base_channel);

		free(base_channel);

		base_channel = next;
	}


	longhorn_ch->deleted = true;

	longhorn_bdev->num_io_channels--;
		SPDK_ERRLOG("removing num io channels %u\n", longhorn_bdev->num_io_channels);
	TAILQ_REMOVE(&longhorn_bdev->io_channel_head, longhorn_ch, channels);

#if 0
	
	for (i = 0; i < longhorn_ch->num_channels; i++) {
		/* Free base bdev channels */
		assert(longhorn_ch->base_channel[i] != NULL);
		spdk_put_io_channel(longhorn_ch->base_channel[i]);
	}
	free(longhorn_ch->base_channel);
#endif
	//onghorn_ch->base_channel = NULL;
}

/*
 * brief:
 * longhorn_bdev_cleanup is used to cleanup and free longhorn_bdev related data
 * structures.
 * params:
 * longhorn_bdev - pointer to longhorn_bdev
 * returns:
 * none
 */
static void
longhorn_bdev_cleanup(struct longhorn_bdev *longhorn_bdev)
{
	SPDK_DEBUGLOG(bdev_longhorn, "longhorn_bdev_cleanup, %p name %s, state %u, config %p\n",
		      longhorn_bdev,
		      longhorn_bdev->bdev.name, longhorn_bdev->state, longhorn_bdev->config);
	if (longhorn_bdev->state == RAID_BDEV_STATE_CONFIGURING) {
		TAILQ_REMOVE(&g_longhorn_bdev_configuring_list, longhorn_bdev, state_link);
	} else if (longhorn_bdev->state == RAID_BDEV_STATE_OFFLINE) {
		TAILQ_REMOVE(&g_longhorn_bdev_offline_list, longhorn_bdev, state_link);
	} else {
		assert(0);
	}
	TAILQ_REMOVE(&g_longhorn_bdev_list, longhorn_bdev, global_link);
	free(longhorn_bdev->bdev.name);
	free(longhorn_bdev->base_bdev_info);
	if (longhorn_bdev->config) {
		longhorn_bdev->config->longhorn_bdev = NULL;
	}
	free(longhorn_bdev);
}

/*
 * brief:
 * wrapper for the bdev close operation
 * params:
 * base_info - longhorn base bdev info
 * returns:
 */
static void
_longhorn_bdev_free_base_bdev_resource(void *ctx)
{
	struct spdk_bdev_desc *desc = ctx;

	spdk_bdev_close(desc);
}


/*
 * brief:
 * free resource of base bdev for longhorn bdev
 * params:
 * longhorn_bdev - pointer to longhorn bdev
 * base_info - longhorn base bdev info
 * returns:
 * 0 - success
 * non zero - failure
 */
static void
longhorn_bdev_free_base_bdev_resource(struct longhorn_bdev *longhorn_bdev,
				  struct longhorn_base_bdev_info *base_info)
{
	spdk_bdev_module_release_bdev(base_info->bdev);
	if (base_info->thread && base_info->thread != spdk_get_thread()) {
		spdk_thread_send_msg(base_info->thread, _longhorn_bdev_free_base_bdev_resource, base_info->desc);
	} else {
		spdk_bdev_close(base_info->desc);
	}
	base_info->desc = NULL;
	base_info->bdev = NULL;

	assert(longhorn_bdev->num_base_bdevs_discovered);
	longhorn_bdev->num_base_bdevs_discovered--;
}

/*
 * brief:
 * longhorn_bdev_destruct is the destruct function table pointer for longhorn bdev
 * params:
 * ctxt - pointer to longhorn_bdev
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
longhorn_bdev_destruct(void *ctxt)
{
	struct longhorn_bdev *longhorn_bdev = ctxt;
	struct longhorn_base_bdev_info *base_info;

	SPDK_DEBUGLOG(bdev_longhorn, "longhorn_bdev_destruct\n");

	longhorn_bdev->destruct_called = true;
	//LONGHORN_FOR_EACH_BASE_BDEV(longhorn_bdev, base_info) {
	TAILQ_FOREACH(base_info, &longhorn_bdev->base_bdevs_head, infos) {
	
		/*
		 * Close all base bdev descriptors for which call has come from below
		 * layers.  Also close the descriptors if we have started shutdown.
		 */
		if (g_shutdown_started ||
		    ((base_info->remove_scheduled == true) &&
		     (base_info->bdev != NULL))) {
			longhorn_bdev_free_base_bdev_resource(longhorn_bdev, base_info);
		}
	}

	if (g_shutdown_started) {
		TAILQ_REMOVE(&g_longhorn_bdev_configured_list, longhorn_bdev, state_link);
		longhorn_bdev->state = RAID_BDEV_STATE_OFFLINE;
		TAILQ_INSERT_TAIL(&g_longhorn_bdev_offline_list, longhorn_bdev, state_link);
	}

	spdk_io_device_unregister(longhorn_bdev, NULL);

	if (longhorn_bdev->num_base_bdevs_discovered == 0) {
		/* Free longhorn_bdev when there are no base bdevs left */
		SPDK_DEBUGLOG(bdev_longhorn, "longhorn bdev base bdevs is 0, going to free all in destruct\n");
		longhorn_bdev_cleanup(longhorn_bdev);
	}

	return 0;
}

void
longhorn_bdev_io_complete(struct longhorn_bdev_io *longhorn_io, enum spdk_bdev_io_status status)
{
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(longhorn_io);

	if (longhorn_io->submitted) {
		if (atomic_load(&longhorn_io->longhorn_ch->io_ops) == 0) {
			if (longhorn_io->longhorn_ch->paused) {
				longhorn_io->longhorn_ch->pause_complete = true;

				longhorn_check_pause_complete(longhorn_io->longhorn_bdev);

			}

		}
	}


	spdk_bdev_io_complete(bdev_io, status);
}

/*
 * brief:
 * longhorn_bdev_io_complete_part - signal the completion of a part of the expected
 * base bdev IOs and complete the longhorn_io if this is the final expected IO.
 * The caller should first set longhorn_io->base_bdev_io_remaining. This function
 * will decrement this counter by the value of the 'completed' parameter and
 * complete the longhorn_io if the counter reaches 0. The caller is free to
 * interpret the 'base_bdev_io_remaining' and 'completed' values as needed,
 * it can represent e.g. blocks or IOs.
 * params:
 * longhorn_io - pointer to longhorn_bdev_io
 * completed - the part of the longhorn_io that has been completed
 * status - status of the base IO
 * returns:
 * true - if the longhorn_io is completed
 * false - otherwise
 */
bool
longhorn_bdev_io_complete_part(struct longhorn_bdev_io *longhorn_io, uint64_t completed,
			   enum spdk_bdev_io_status status)
{
	assert(longhorn_io->base_bdev_io_remaining >= completed);
	longhorn_io->base_bdev_io_remaining -= completed;

	atomic_fetch_sub(&longhorn_io->longhorn_bdev->io_ops, 1);
        atomic_fetch_sub(&longhorn_io->longhorn_ch->io_ops, 1);


	if (status != SPDK_BDEV_IO_STATUS_SUCCESS) {
		longhorn_io->base_bdev_io_status = status;
	}

	if (longhorn_io->base_bdev_io_remaining == 0) {
		longhorn_bdev_io_complete(longhorn_io, longhorn_io->base_bdev_io_status);
		return true;
	} else {
		return false;
	}
}

/*
 * brief:
 * longhorn_bdev_queue_io_wait function processes the IO which failed to submit.
 * It will try to queue the IOs after storing the context to bdev wait queue logic.
 * params:
 * longhorn_io - pointer to longhorn_bdev_io
 * bdev - the block device that the IO is submitted to
 * ch - io channel
 * cb_fn - callback when the spdk_bdev_io for bdev becomes available
 * returns:
 * none
 */
void
longhorn_bdev_queue_io_wait(struct longhorn_bdev_io *longhorn_io, struct spdk_bdev *bdev,
			struct spdk_io_channel *ch, spdk_bdev_io_wait_cb cb_fn)
{
	longhorn_io->waitq_entry.bdev = bdev;
	longhorn_io->waitq_entry.cb_fn = cb_fn;
	longhorn_io->waitq_entry.cb_arg = longhorn_io;
	spdk_bdev_queue_io_wait(bdev, ch, &longhorn_io->waitq_entry);
}

static void
longhorn_base_bdev_reset_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct longhorn_bdev_io *longhorn_io = cb_arg;

	spdk_bdev_free_io(bdev_io);

	longhorn_bdev_io_complete_part(longhorn_io, 1, success ?
				   SPDK_BDEV_IO_STATUS_SUCCESS :
				   SPDK_BDEV_IO_STATUS_FAILED);
}

static void
longhorn_bdev_submit_reset_request(struct longhorn_bdev_io *longhorn_io);

static void
_longhorn_bdev_submit_reset_request(void *_longhorn_io)
{
	struct longhorn_bdev_io *longhorn_io = _longhorn_io;

	longhorn_bdev_submit_reset_request(longhorn_io);
}

/*
 * brief:
 * longhorn_bdev_submit_reset_request function submits reset requests
 * to member disks; it will submit as many as possible unless a reset fails with -ENOMEM, in
 * which case it will queue it for later submission
 * params:
 * longhorn_io
 * returns:
 * none
 */
static void
longhorn_bdev_submit_reset_request(struct longhorn_bdev_io *longhorn_io)
{
	struct longhorn_bdev_io_channel *longhorn_ch = longhorn_io->longhorn_ch;
        struct longhorn_bdev            *longhorn_bdev = longhorn_io->longhorn_bdev;
	int				ret;
	struct longhorn_base_bdev_info	*base_info;
	struct spdk_io_channel		*base_ch;
	struct longhorn_base_io_channel *base_channel;


	if (longhorn_io->base_bdev_io_remaining == 0) {
		longhorn_io->base_bdev_io_remaining = longhorn_bdev->num_base_bdevs;
	}

	TAILQ_FOREACH(base_channel, &longhorn_ch->base_channels, channels) {
	//while (longhorn_io->base_bdev_io_submitted < longhorn_bdev->num_base_bdevs) {
		base_ch = base_channel->base_channel;
                base_info = base_channel->base_info;

		ret = spdk_bdev_reset(base_info->desc, base_ch,
				      longhorn_base_bdev_reset_complete, longhorn_io);
		if (ret == 0) {
			longhorn_io->base_bdev_io_submitted++;
		} else if (ret == -ENOMEM) {
			longhorn_bdev_queue_io_wait(longhorn_io, base_info->bdev, base_ch,
						_longhorn_bdev_submit_reset_request);
			return;
		} else {
			SPDK_ERRLOG("bdev io submit error not due to ENOMEM, it should not happen\n");
			assert(false);
			longhorn_bdev_io_complete(longhorn_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}
	}
}

/*
 * brief:
 * Callback function to spdk_bdev_io_get_buf.
 * params:
 * ch - pointer to longhorn bdev io channel
 * bdev_io - pointer to parent bdev_io on longhorn bdev device
 * success - True if buffer is allocated or false otherwise.
 * returns:
 * none
 */
static void
longhorn_bdev_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
		     bool success)
{
	struct longhorn_bdev_io *longhorn_io = (struct longhorn_bdev_io *)bdev_io->driver_ctx;

	if (!success) {
		longhorn_bdev_io_complete(longhorn_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	longhorn_submit_rw_request(longhorn_io);
}

/*
 * brief:
 * longhorn_bdev_submit_request function is the submit_request function pointer of
 * longhorn bdev function table. This is used to submit the io on longhorn_bdev to below
 * layers.
 * params:
 * ch - pointer to longhorn bdev io channel
 * bdev_io - pointer to parent bdev_io on longhorn bdev device
 * returns:
 * none
 */
static void
longhorn_bdev_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct longhorn_bdev_io *longhorn_io = (struct longhorn_bdev_io *)bdev_io->driver_ctx;

	longhorn_io->longhorn_bdev = bdev_io->bdev->ctxt;
	longhorn_io->longhorn_ch = spdk_io_channel_get_ctx(ch);
	longhorn_io->base_bdev_io_remaining = 0;
	longhorn_io->base_bdev_io_submitted = 0;
	longhorn_io->base_bdev_io_status = SPDK_BDEV_IO_STATUS_SUCCESS;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, longhorn_bdev_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		longhorn_submit_rw_request(longhorn_io);
		break;

	case SPDK_BDEV_IO_TYPE_RESET:
		longhorn_bdev_submit_reset_request(longhorn_io);
		break;

	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_UNMAP:
		longhorn_submit_null_payload_request(longhorn_io);
		break;

	default:
		SPDK_ERRLOG("submit request, invalid io type %u\n", bdev_io->type);
		longhorn_bdev_io_complete(longhorn_io, SPDK_BDEV_IO_STATUS_FAILED);
		break;
	}
}

/*
 * brief:
 * _longhorn_bdev_io_type_supported checks whether io_type is supported in
 * all base bdev modules of longhorn bdev module. If anyone among the base_bdevs
 * doesn't support, the longhorn device doesn't supports
 * params:
 * longhorn_bdev - pointer to longhorn bdev context
 * io_type - io type
 * returns:
 * true - io_type is supported
 * false - io_type is not supported
 */
inline static bool
_longhorn_bdev_io_type_supported(struct longhorn_bdev *longhorn_bdev, enum spdk_bdev_io_type io_type)
{
	struct longhorn_base_bdev_info *base_info;

#if 0
	if (io_type == SPDK_BDEV_IO_TYPE_FLUSH ||
	    io_type == SPDK_BDEV_IO_TYPE_UNMAP) {
		return false;
	}

#endif 
	//LONGHORN_FOR_EACH_BASE_BDEV(longhorn_bdev, base_info) {
	TAILQ_FOREACH(base_info, &longhorn_bdev->base_bdevs_head, infos) {
		if (base_info->bdev == NULL) {
			assert(false);
			continue;
		}

		if (spdk_bdev_io_type_supported(base_info->bdev, io_type) == false) {
			return false;
		}
	}

	return true;
}

/*
 * brief:
 * longhorn_bdev_io_type_supported is the io_supported function for bdev function
 * table which returns whether the particular io type is supported or not by
 * longhorn bdev module
 * params:
 * ctx - pointer to longhorn bdev context
 * type - io type
 * returns:
 * true - io_type is supported
 * false - io_type is not supported
 */
static bool
longhorn_bdev_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		return true;

	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_UNMAP:
		return _longhorn_bdev_io_type_supported(ctx, io_type);

	default:
		return false;
	}

	return false;
}

/*
 * brief:
 * longhorn_bdev_get_io_channel is the get_io_channel function table pointer for
 * longhorn bdev. This is used to return the io channel for this longhorn bdev
 * params:
 * ctxt - pointer to longhorn_bdev
 * returns:
 * pointer to io channel for longhorn bdev
 */
static struct spdk_io_channel *
longhorn_bdev_get_io_channel(void *ctxt)
{
	struct longhorn_bdev *longhorn_bdev = ctxt;

	return spdk_get_io_channel(longhorn_bdev);
}

/*
 * brief:
 * longhorn_bdev_dump_info_json is the function table pointer for longhorn bdev
 * params:
 * ctx - pointer to longhorn_bdev
 * w - pointer to json context
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
longhorn_bdev_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct longhorn_bdev *longhorn_bdev = ctx;
	struct longhorn_base_bdev_info *base_info;

	SPDK_DEBUGLOG(bdev_longhorn, "longhorn_bdev_dump_config_json\n");
	assert(longhorn_bdev != NULL);

	/* Dump the longhorn bdev configuration related information */
	spdk_json_write_named_object_begin(w, "longhorn");
	spdk_json_write_named_uint32(w, "state", longhorn_bdev->state);
	spdk_json_write_named_uint32(w, "destruct_called", longhorn_bdev->destruct_called);
	spdk_json_write_named_uint32(w, "num_base_bdevs", longhorn_bdev->num_base_bdevs);
	spdk_json_write_named_uint32(w, "num_base_bdevs_discovered", longhorn_bdev->num_base_bdevs_discovered);
	spdk_json_write_name(w, "base_bdevs_list");
	spdk_json_write_array_begin(w);
	//LONGHORN_FOR_EACH_BASE_BDEV(longhorn_bdev, base_info) {
	TAILQ_FOREACH(base_info, &longhorn_bdev->base_bdevs_head, infos) {
		if (base_info->bdev) {
			spdk_json_write_string(w, base_info->bdev->name);
		} else {
			spdk_json_write_null(w);
		}
	}
	spdk_json_write_array_end(w);
	spdk_json_write_object_end(w);

	return 0;
}

/*
 * brief:
 * longhorn_bdev_write_config_json is the function table pointer for longhorn bdev
 * params:
 * bdev - pointer to spdk_bdev
 * w - pointer to json context
 * returns:
 * none
 */
static void
longhorn_bdev_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	struct longhorn_bdev *longhorn_bdev = bdev->ctxt;
	struct longhorn_base_bdev_info *base_info;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_longhorn_create");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", bdev->name);

	spdk_json_write_named_array_begin(w, "base_bdevs");
	//LONGHORN_FOR_EACH_BASE_BDEV(longhorn_bdev, base_info) {
	TAILQ_FOREACH(base_info, &longhorn_bdev->base_bdevs_head, infos) {
		if (base_info->bdev) {
			spdk_json_write_string(w, base_info->bdev->name);
		}
	}
	spdk_json_write_array_end(w);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

/* g_longhorn_bdev_fn_table is the function table for longhorn bdev */
static const struct spdk_bdev_fn_table g_longhorn_bdev_fn_table = {
	.destruct		= longhorn_bdev_destruct,
	.submit_request		= longhorn_bdev_submit_request,
	.io_type_supported	= longhorn_bdev_io_type_supported,
	.get_io_channel		= longhorn_bdev_get_io_channel,
	.dump_info_json		= longhorn_bdev_dump_info_json,
	.write_config_json	= longhorn_bdev_write_config_json,
};

/*
 * brief:
 * longhorn_bdev_config_cleanup function is used to free memory for one longhorn_bdev in configuration
 * params:
 * longhorn_cfg - pointer to longhorn_bdev_config structure
 * returns:
 * none
 */
void
longhorn_bdev_config_cleanup(struct longhorn_bdev_config *longhorn_cfg)
{
	uint8_t i;

	TAILQ_REMOVE(&g_longhorn_config.longhorn_bdev_config_head, longhorn_cfg, link);
	g_longhorn_config.total_longhorn_bdev--;

	if (longhorn_cfg->base_bdev) {
		for (i = 0; i < longhorn_cfg->num_base_bdevs; i++) {
			free(longhorn_cfg->base_bdev[i].name);
		}
		free(longhorn_cfg->base_bdev);
	}
	free(longhorn_cfg->name);
	free(longhorn_cfg);
}

/*
 * brief:
 * longhorn_bdev_free is the longhorn bdev function table function pointer. This is
 * called on bdev free path
 * params:
 * none
 * returns:
 * none
 */
static void
longhorn_bdev_free(void)
{
	struct longhorn_bdev_config *longhorn_cfg, *tmp;

	SPDK_DEBUGLOG(bdev_longhorn, "longhorn_bdev_free\n");
	TAILQ_FOREACH_SAFE(longhorn_cfg, &g_longhorn_config.longhorn_bdev_config_head, link, tmp) {
		longhorn_bdev_config_cleanup(longhorn_cfg);
	}
}

/* brief
 * longhorn_bdev_config_find_by_name is a helper function to find longhorn bdev config
 * by name as key.
 *
 * params:
 * longhorn_name - name for longhorn bdev.
 */
struct longhorn_bdev_config *
longhorn_bdev_config_find_by_name(const char *longhorn_name)
{
	struct longhorn_bdev_config *longhorn_cfg;

	TAILQ_FOREACH(longhorn_cfg, &g_longhorn_config.longhorn_bdev_config_head, link) {
		if (!strcmp(longhorn_cfg->name, longhorn_name)) {
			return longhorn_cfg;
		}
	}

	return longhorn_cfg;
}

/* brief
 * longhorn_bdev_find_by_name is a helper function to find longhorn bdev 
 * by name as key.
 *
 * params:
 * longhorn_name - name for longhorn bdev.
 */
struct longhorn_bdev *
longhorn_bdev_find_by_name(const char *longhorn_name)
{
	struct longhorn_bdev *longhorn_bdev;

	TAILQ_FOREACH(longhorn_bdev, &g_longhorn_bdev_list, global_link) {
		if (!strcmp(longhorn_bdev->bdev.name, longhorn_name)) {
			return longhorn_bdev;
		}
	}

	return NULL;
}


/*
 * brief
 * longhorn_bdev_config_add function adds config for newly created longhorn bdev.
 *
 * params:
 * longhorn_name - name for longhorn bdev.
 * strip_size - strip size in KB
 * num_base_bdevs - number of base bdevs.
 * _longhorn_cfg - Pointer to newly added configuration
 */
int
longhorn_bdev_config_add(const char *longhorn_name, uint8_t num_base_bdevs,
		     struct longhorn_bdev_config **_longhorn_cfg)
{
	struct longhorn_bdev_config *longhorn_cfg;

	longhorn_cfg = longhorn_bdev_config_find_by_name(longhorn_name);
	if (longhorn_cfg != NULL) {
		SPDK_ERRLOG("Duplicate longhorn bdev name found in config file %s\n",
			    longhorn_name);
		return -EEXIST;
	}


	if (num_base_bdevs == 0) {
		SPDK_ERRLOG("Invalid base device count %u\n", num_base_bdevs);
		return -EINVAL;
	}

	longhorn_cfg = calloc(1, sizeof(*longhorn_cfg));
	if (longhorn_cfg == NULL) {
		SPDK_ERRLOG("unable to allocate memory\n");
		return -ENOMEM;
	}

	longhorn_cfg->name = strdup(longhorn_name);
	if (!longhorn_cfg->name) {
		free(longhorn_cfg);
		SPDK_ERRLOG("unable to allocate memory\n");
		return -ENOMEM;
	}
	longhorn_cfg->num_base_bdevs = num_base_bdevs;

	longhorn_cfg->base_bdev = calloc(num_base_bdevs, sizeof(*longhorn_cfg->base_bdev));
	if (longhorn_cfg->base_bdev == NULL) {
		free(longhorn_cfg->name);
		free(longhorn_cfg);
		SPDK_ERRLOG("unable to allocate memory\n");
		return -ENOMEM;
	}

	TAILQ_INSERT_TAIL(&g_longhorn_config.longhorn_bdev_config_head, longhorn_cfg, link);
	g_longhorn_config.total_longhorn_bdev++;

	*_longhorn_cfg = longhorn_cfg;
	return 0;
}

/*
 * brief:
 * longhorn_bdev_config_add_base_bdev function add base bdev to longhorn bdev config.
 *
 * params:
 * longhorn_cfg - pointer to longhorn bdev configuration
 * base_bdev_name - name of base bdev
 * slot - Position to add base bdev
 */
int
longhorn_bdev_config_add_base_bdev(struct longhorn_bdev_config *longhorn_cfg, const char *base_bdev_name,
			       uint8_t slot)
{
	uint8_t i;
	struct longhorn_bdev_config *tmp;
	char *bdev_name;

	if (slot >= longhorn_cfg->num_base_bdevs) {
		return -EINVAL;
	}

	bdev_name = spdk_sprintf_alloc("%s/%s", base_bdev_name, longhorn_cfg->name);

#if 0
	TAILQ_FOREACH(tmp, &g_longhorn_config.longhorn_bdev_config_head, link) {
		for (i = 0; i < tmp->num_base_bdevs; i++) {
			if (tmp->base_bdev[i].name != NULL) {
				if (!strcmp(tmp->base_bdev[i].name, bdev_name)) {
					SPDK_ERRLOG("duplicate base bdev name %s mentioned\n",
						    base_bdev_name);
					return -EEXIST;
				}
			}
		}
	}
#endif

	longhorn_cfg->base_bdev[slot].name = bdev_name;
	if (longhorn_cfg->base_bdev[slot].name == NULL) {
		SPDK_ERRLOG("unable to allocate memory\n");
		return -ENOMEM;
	}

	return 0;
}

/*
 * brief:
 * longhorn_bdev_fini_start is called when bdev layer is starting the
 * shutdown process
 * params:
 * none
 * returns:
 * none
 */
static void
longhorn_bdev_fini_start(void)
{
	SPDK_DEBUGLOG(bdev_longhorn, "longhorn_bdev_fini_start\n");
	g_shutdown_started = true;
}

/*
 * brief:
 * longhorn_bdev_exit is called on longhorn bdev module exit time by bdev layer
 * params:
 * none
 * returns:
 * none
 */
static void
longhorn_bdev_exit(void)
{
	SPDK_DEBUGLOG(bdev_longhorn, "longhorn_bdev_exit\n");
	longhorn_bdev_free();
}

/*
 * brief:
 * longhorn_bdev_get_ctx_size is used to return the context size of bdev_io for longhorn
 * module
 * params:
 * none
 * returns:
 * size of spdk_bdev_io context for longhorn
 */
static int
longhorn_bdev_get_ctx_size(void)
{
	SPDK_DEBUGLOG(bdev_longhorn, "longhorn_bdev_get_ctx_size\n");
	return sizeof(struct longhorn_bdev_io);
}

/*
 * brief:
 * longhorn_bdev_can_claim_bdev is the function to check if this base_bdev can be
 * claimed by longhorn bdev or not.
 * params:
 * bdev_name - represents base bdev name
 * _longhorn_cfg - pointer to longhorn bdev config parsed from config file
 * base_bdev_slot - if bdev can be claimed, it represents the base_bdev correct
 * slot. This field is only valid if return value of this function is true
 * returns:
 * true - if bdev can be claimed
 * false - if bdev can't be claimed
 */
static bool
longhorn_bdev_can_claim_bdev(const char *bdev_name, struct longhorn_bdev_config **_longhorn_cfg,
			 uint8_t *base_bdev_slot)
{
	struct longhorn_bdev_config *longhorn_cfg;
	uint8_t i;

	TAILQ_FOREACH(longhorn_cfg, &g_longhorn_config.longhorn_bdev_config_head, link) {
		for (i = 0; i < longhorn_cfg->num_base_bdevs; i++) {
			/*
			 * Check if the base bdev name is part of longhorn bdev configuration.
			 * If match is found then return true and the slot information where
			 * this base bdev should be inserted in longhorn bdev
			 */
			if (!strcmp(bdev_name, longhorn_cfg->base_bdev[i].name)) {
				*_longhorn_cfg = longhorn_cfg;
				*base_bdev_slot = i;
				return true;
			}
		}
	}

	return false;
}


static struct spdk_bdev_module g_longhorn_if = {
	.name = "longhorn",
	.module_init = longhorn_bdev_init,
	.fini_start = longhorn_bdev_fini_start,
	.module_fini = longhorn_bdev_exit,
	.get_ctx_size = longhorn_bdev_get_ctx_size,
	.examine_config = longhorn_bdev_examine,
	.async_init = false,
	.async_fini = false,
};
SPDK_BDEV_MODULE_REGISTER(longhorn, &g_longhorn_if)

/*
 * brief:
 * longhorn_bdev_init is the initialization function for longhorn bdev module
 * params:
 * none
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
longhorn_bdev_init(void)
{
	return 0;
}

/*
 * brief:
 * longhorn_bdev_create allocates longhorn bdev based on passed configuration
 * params:
 * longhorn_cfg - configuration of longhorn bdev
 * returns:
 * 0 - success
 * non zero - failure
 */
int
//longhorn_bdev_create(struct longhorn_bdev_config *longhorn_cfg)
longhorn_bdev_create(const char *name, uint8_t num_base_bdevs)
{
	struct longhorn_bdev *longhorn_bdev;
	struct spdk_bdev *longhorn_bdev_gen;


	longhorn_bdev = calloc(1, sizeof(*longhorn_bdev));
	if (!longhorn_bdev) {
		SPDK_ERRLOG("Unable to allocate memory for longhorn bdev\n");
		return -ENOMEM;
	}

	longhorn_bdev->io_ops = 0;
	longhorn_bdev->num_base_bdevs = num_base_bdevs;
	//longhorn_bdev->base_bdev_info = calloc(longhorn_bdev->num_base_bdevs,
	// sizeof(struct longhorn_base_bdev_info));
	//if (!longhorn_bdev->base_bdev_info) {
//		SPDK_ERRLOG("Unable able to allocate base bdev info\n");
//		free(longhorn_bdev);
//		return -ENOMEM;
//	}
//

	pthread_mutex_init(&longhorn_bdev->base_bdevs_mutex, NULL);


	TAILQ_INIT(&longhorn_bdev->pause_cbs);
	TAILQ_INIT(&longhorn_bdev->base_bdevs_head);
	TAILQ_INIT(&longhorn_bdev->io_channel_head);

        longhorn_bdev->state = RAID_BDEV_STATE_CONFIGURING;

        longhorn_bdev_gen = &longhorn_bdev->bdev;

        longhorn_bdev_gen->name = strdup(name);
        if (!longhorn_bdev_gen->name) {
                SPDK_ERRLOG("Unable to allocate name for longhorn\n");
                //free(longhorn_bdev->base_bdev_info);
                free(longhorn_bdev);
                return -ENOMEM;
        }

        longhorn_bdev_gen->product_name = "Longhorn Volume";
        longhorn_bdev_gen->ctxt = longhorn_bdev;
        longhorn_bdev_gen->fn_table = &g_longhorn_bdev_fn_table;
        longhorn_bdev_gen->module = &g_longhorn_if;
        longhorn_bdev_gen->write_cache = 0;

        TAILQ_INSERT_TAIL(&g_longhorn_bdev_configuring_list, longhorn_bdev, state_link);
        TAILQ_INSERT_TAIL(&g_longhorn_bdev_list, longhorn_bdev, global_link);


        return 0;

}

/*
 * brief
 * longhorn_bdev_alloc_base_bdev_resource allocates resource of base bdev.
 * params:
 * longhorn_bdev - pointer to longhorn bdev
 * bdev_name - base bdev name
 * base_bdev_slot - position to add base bdev
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
longhorn_bdev_alloc_base_bdev_resource(struct longhorn_bdev *longhorn_bdev, const char *bdev_name)
{
	struct spdk_bdev_desc *desc;
	struct spdk_bdev *bdev;
	struct longhorn_base_bdev_info *base_info;
	int rc;

	rc = spdk_bdev_open_ext(bdev_name, true, longhorn_bdev_event_base_bdev, NULL, &desc);
	if (rc != 0) {
		if (rc != -ENODEV) {
			SPDK_ERRLOG("Unable to create desc on bdev '%s'\n", bdev_name);
		}
		return rc;
	}

	bdev = spdk_bdev_desc_get_bdev(desc);

	rc = spdk_bdev_module_claim_bdev(bdev, NULL, &g_longhorn_if);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to claim this bdev as it is already claimed\n");
		spdk_bdev_close(desc);
		return rc;
	}

	SPDK_DEBUGLOG(bdev_longhorn, "bdev %s is claimed\n", bdev_name);

	assert(longhorn_bdev->state != RAID_BDEV_STATE_ONLINE);
	//assert(base_bdev_slot < longhorn_bdev->num_base_bdevs);


	base_info = calloc(sizeof (struct longhorn_base_bdev_info), 1);

	
	base_info->thread = spdk_get_thread();
	base_info->bdev = bdev;
	base_info->desc = desc;

	longhorn_bdev->num_base_bdevs_discovered++;
	assert(longhorn_bdev->num_base_bdevs_discovered <= longhorn_bdev->num_base_bdevs);

	TAILQ_INSERT_TAIL(&longhorn_bdev->base_bdevs_head, base_info, infos);

	return 0;
}

static int
longhorn_bdev_configure_base_info(struct longhorn_bdev *longhorn_bdev, 
				  struct longhorn_base_bdev_info *base_info) 
{
	struct spdk_bdev_desc *desc;
	struct spdk_bdev *bdev;
	int rc;

	rc = spdk_bdev_open_ext(base_info->bdev_name, true, longhorn_bdev_event_base_bdev, NULL, &desc);
	if (rc != 0) {
		if (rc != -ENODEV) {
			SPDK_ERRLOG("Unable to create desc on bdev '%s'\n", base_info->bdev_name);
		}
		return rc;
	}

	bdev = spdk_bdev_desc_get_bdev(desc);

	rc = spdk_bdev_module_claim_bdev(bdev, NULL, &g_longhorn_if);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to claim this bdev as it is already claimed\n");
		spdk_bdev_close(desc);
		return rc;
	}

	SPDK_DEBUGLOG(bdev_longhorn, "bdev %s is claimed\n", base_info->bdev_name);

	assert(longhorn_bdev->state != RAID_BDEV_STATE_ONLINE);
	//assert(base_bdev_slot < longhorn_bdev->num_base_bdevs);


	//base_info = calloc(sizeof (struct longhorn_base_bdev_info), 1);

	
	base_info->thread = spdk_get_thread();
	base_info->bdev = bdev;
	base_info->desc = desc;

	longhorn_bdev->num_base_bdevs_discovered++;
	assert(longhorn_bdev->num_base_bdevs_discovered <= longhorn_bdev->num_base_bdevs);

	TAILQ_INSERT_TAIL(&longhorn_bdev->base_bdevs_head, base_info, infos);

	return 0;
}

static void longhorn_bdev_nvmf_cb(void *cb) {
}

/*
 * brief:
 * If longhorn bdev config is complete, then only register the longhorn bdev to
 * bdev layer and remove this longhorn bdev from configuring list and
 * insert the longhorn bdev to configured list
 * params:
 * longhorn_bdev - pointer to longhorn bdev
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
longhorn_bdev_configure(struct longhorn_bdev *longhorn_bdev)
{
	uint32_t blocklen = 0;
	struct spdk_bdev *longhorn_bdev_gen;
	struct longhorn_base_bdev_info *base_info;
	int rc = 0;
	char *nqn;

	assert(longhorn_bdev->state == RAID_BDEV_STATE_CONFIGURING);
	assert(longhorn_bdev->num_base_bdevs_discovered == longhorn_bdev->num_base_bdevs);

	//LONGHORN_FOR_EACH_BASE_BDEV(longhorn_bdev, base_info) {
	TAILQ_FOREACH(base_info, &longhorn_bdev->base_bdevs_head, infos) {

		/* Check blocklen for all base bdevs that it should be same */
		if (blocklen == 0) {
			blocklen = base_info->bdev->blocklen;
		} else if (blocklen != base_info->bdev->blocklen) {
			/*
			 * Assumption is that all the base bdevs for any longhorn bdev should
			 * have same blocklen
			 */
			SPDK_ERRLOG("Blocklen of various bdevs not matching\n");
			return -EINVAL;
		}
	}
	assert(blocklen > 0);

	/* The strip_size_kb is read in from user in KB. Convert to blocks here for
	 * internal use.
	 */
	longhorn_bdev->blocklen_shift = spdk_u32log2(blocklen);

	longhorn_bdev_gen = &longhorn_bdev->bdev;
	longhorn_bdev_gen->blocklen = blocklen;

	rc = longhorn_start(longhorn_bdev);
	if (rc != 0) {
		SPDK_ERRLOG("longhorn module startup callback failed\n");
		return rc;
	}
	longhorn_bdev->state = RAID_BDEV_STATE_ONLINE;
	SPDK_DEBUGLOG(bdev_longhorn, "io device register %p\n", longhorn_bdev);
	SPDK_DEBUGLOG(bdev_longhorn, "blockcnt %" PRIu64 ", blocklen %u\n",
		      longhorn_bdev_gen->blockcnt, longhorn_bdev_gen->blocklen);
	spdk_io_device_register(longhorn_bdev, longhorn_bdev_create_cb, longhorn_bdev_destroy_cb,
				sizeof(struct longhorn_bdev_io_channel),
				longhorn_bdev->bdev.name);
	rc = spdk_bdev_register(longhorn_bdev_gen);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to register longhorn bdev and stay at configuring state\n");
		spdk_io_device_unregister(longhorn_bdev, NULL);
		longhorn_bdev->state = RAID_BDEV_STATE_CONFIGURING;
		return rc;
	}
	SPDK_DEBUGLOG(bdev_longhorn, "longhorn bdev generic %p\n", longhorn_bdev_gen);
	TAILQ_REMOVE(&g_longhorn_bdev_configuring_list, longhorn_bdev, state_link);
	TAILQ_INSERT_TAIL(&g_longhorn_bdev_configured_list, longhorn_bdev, state_link);
	SPDK_DEBUGLOG(bdev_longhorn, "longhorn bdev is created with name %s, longhorn_bdev %p\n",
		      longhorn_bdev_gen->name, longhorn_bdev);

	nqn = spdk_sprintf_alloc(VOLUME_FORMAT, longhorn_bdev_gen->name);
	longhorn_publish_nvmf(longhorn_bdev_gen->name, nqn, "127.0.0.1", 
			      4420, longhorn_bdev_nvmf_cb, NULL);


	return 0;
}

/*
 * brief:
 * If longhorn bdev is online and registered, change the bdev state to
 * configuring and unregister this longhorn device. Queue this longhorn device
 * in configuring list
 * params:
 * longhorn_bdev - pointer to longhorn bdev
 * cb_fn - callback function
 * cb_arg - argument to callback function
 * returns:
 * none
 */
static void
longhorn_bdev_deconfigure(struct longhorn_bdev *longhorn_bdev, longhorn_bdev_destruct_cb cb_fn,
		      void *cb_arg)
{
	if (longhorn_bdev->state != RAID_BDEV_STATE_ONLINE) {
		if (cb_fn) {
			cb_fn(cb_arg, 0);
		}
		return;
	}

	assert(longhorn_bdev->num_base_bdevs == longhorn_bdev->num_base_bdevs_discovered);
	TAILQ_REMOVE(&g_longhorn_bdev_configured_list, longhorn_bdev, state_link);
	longhorn_bdev->state = RAID_BDEV_STATE_OFFLINE;
	assert(longhorn_bdev->num_base_bdevs_discovered);
	TAILQ_INSERT_TAIL(&g_longhorn_bdev_offline_list, longhorn_bdev, state_link);
	SPDK_DEBUGLOG(bdev_longhorn, "longhorn bdev state chaning from online to offline\n");

	spdk_bdev_unregister(&longhorn_bdev->bdev, cb_fn, cb_arg);
}

/*
 * brief:
 * longhorn_bdev_find_by_base_bdev function finds the longhorn bdev which has
 *  claimed the base bdev.
 * params:
 * base_bdev - pointer to base bdev pointer
 * _longhorn_bdev - Reference to pointer to longhorn bdev
 * _base_info - Reference to the longhorn base bdev info.
 * returns:
 * true - if the longhorn bdev is found.
 * false - if the longhorn bdev is not found.
 */
static bool
longhorn_bdev_find_by_base_bdev(struct spdk_bdev *base_bdev, struct longhorn_bdev **_longhorn_bdev,
			    struct longhorn_base_bdev_info **_base_info)
{
	struct longhorn_bdev *longhorn_bdev;
	struct longhorn_base_bdev_info *base_info;

	TAILQ_FOREACH(longhorn_bdev, &g_longhorn_bdev_list, global_link) {
		TAILQ_FOREACH(base_info, &longhorn_bdev->base_bdevs_head, infos) {
			if (base_info->bdev == base_bdev) {
				*_longhorn_bdev = longhorn_bdev;
				*_base_info = base_info;
				return true;
			}
		}
	}

	return false;
}

/*
 * brief:
 * longhorn_bdev_remove_base_bdev function is called by below layers when base_bdev
 * is removed. This function checks if this base bdev is part of any longhorn bdev
 * or not. If yes, it takes necessary action on that particular longhorn bdev.
 * params:
 * base_bdev - pointer to base bdev pointer which got removed
 * returns:
 * none
 */
static void
longhorn_bdev_remove_base_bdev(struct spdk_bdev *base_bdev)
{
	struct longhorn_bdev	*longhorn_bdev = NULL;
	struct longhorn_base_bdev_info *base_info;

	SPDK_DEBUGLOG(bdev_longhorn, "longhorn_bdev_remove_base_bdev\n");

	/* Find the longhorn_bdev which has claimed this base_bdev */
	if (!longhorn_bdev_find_by_base_bdev(base_bdev, &longhorn_bdev, &base_info)) {
		SPDK_ERRLOG("bdev to remove '%s' not found\n", base_bdev->name);
		return;
	}

	assert(base_info->desc);
	base_info->remove_scheduled = true;

	if (longhorn_bdev->destruct_called == true ||
	    longhorn_bdev->state == RAID_BDEV_STATE_CONFIGURING) {
		/*
		 * As longhorn bdev is not registered yet or already unregistered,
		 * so cleanup should be done here itself.
		 */
		longhorn_bdev_free_base_bdev_resource(longhorn_bdev, base_info);
		if (longhorn_bdev->num_base_bdevs_discovered == 0) {
			/* There is no base bdev for this longhorn, so free the longhorn device. */
			longhorn_bdev_cleanup(longhorn_bdev);
			return;
		}
	}

	longhorn_bdev_deconfigure(longhorn_bdev, NULL, NULL);
}

/*
 * brief:
 * longhorn_bdev_event_base_bdev function is called by below layers when base_bdev
 * triggers asynchronous event.
 * params:
 * type - event details.
 * bdev - bdev that triggered event.
 * event_ctx - context for event.
 * returns:
 * none
 */
static void
longhorn_bdev_event_base_bdev(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
			  void *event_ctx)
{
	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		longhorn_bdev_remove_base_bdev(bdev);
		break;
	default:
		SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
		break;
	}
}

/*
 * brief:
 * Remove base bdevs from the longhorn bdev one by one.  Skip any base bdev which
 *  doesn't exist.
 * params:
 * longhorn_cfg - pointer to longhorn bdev config.
 * cb_fn - callback function
 * cb_ctx - argument to callback function
 */
void
longhorn_bdev_remove_base_devices(struct longhorn_bdev_config *longhorn_cfg,
			      longhorn_bdev_destruct_cb cb_fn, void *cb_arg)
{
	struct longhorn_bdev		*longhorn_bdev;
	struct longhorn_base_bdev_info	*base_info;

	SPDK_DEBUGLOG(bdev_longhorn, "longhorn_bdev_remove_base_devices\n");

	longhorn_bdev = longhorn_cfg->longhorn_bdev;
	if (longhorn_bdev == NULL) {
		SPDK_DEBUGLOG(bdev_longhorn, "longhorn bdev %s doesn't exist now\n", longhorn_cfg->name);
		if (cb_fn) {
			cb_fn(cb_arg, 0);
		}
		return;
	}

	if (longhorn_bdev->destroy_started) {
		SPDK_DEBUGLOG(bdev_longhorn, "destroying longhorn bdev %s is already started\n",
			      longhorn_cfg->name);
		if (cb_fn) {
			cb_fn(cb_arg, -EALREADY);
		}
		return;
	}

	longhorn_bdev->destroy_started = true;

	//LONGHORN_FOR_EACH_BASE_BDEV(longhorn_bdev, base_info) {
	TAILQ_FOREACH(base_info, &longhorn_bdev->base_bdevs_head, infos) {
		if (base_info->bdev == NULL) {
			continue;
		}

		assert(base_info->desc);
		base_info->remove_scheduled = true;

		if (longhorn_bdev->destruct_called == true ||
		    longhorn_bdev->state == RAID_BDEV_STATE_CONFIGURING) {
			/*
			 * As longhorn bdev is not registered yet or already unregistered,
			 * so cleanup should be done here itself.
			 */
			longhorn_bdev_free_base_bdev_resource(longhorn_bdev, base_info);
			if (longhorn_bdev->num_base_bdevs_discovered == 0) {
				/* There is no base bdev for this longhorn, so free the longhorn device. */
				longhorn_bdev_cleanup(longhorn_bdev);
				if (cb_fn) {
					cb_fn(cb_arg, 0);
				}
				return;
			}
		}
	}

	longhorn_bdev_deconfigure(longhorn_bdev, cb_fn, cb_arg);
}

/*
 * brief:
 * longhorn_bdev_add_base_device function is the actual function which either adds
 * the nvme base device to existing longhorn bdev or create a new longhorn bdev. It also claims
 * the base device and keep the open descriptor.
 * params:
 * longhorn_cfg - pointer to longhorn bdev config
 * bdev - pointer to base bdev
 * base_bdev_slot - position to add base bdev
 * returns:
 * 0 - success
 * non zero - failure
 */
int
//longhorn_bdev_add_base_device(struct longhorn_bdev_config *longhorn_cfg, const char *bdev_name,
//longhorn_bdev_add_base_device(const char *name, const char *bdev_name)
longhorn_bdev_add_base_device(struct longhorn_bdev *longhorn_bdev, 
			      struct longhorn_base_bdev_info *base_info) 
{
	int			rc;


	rc = longhorn_bdev_configure_base_info(longhorn_bdev, base_info);
	if (rc != 0) {
		if (rc != -ENODEV) {
			SPDK_ERRLOG("Failed to allocate resource for bdev '%s'\n", base_info->bdev_name);
		}
		return rc;
	}

	assert(longhorn_bdev->num_base_bdevs_discovered <= longhorn_bdev->num_base_bdevs);

	if (longhorn_bdev->num_base_bdevs_discovered == longhorn_bdev->num_base_bdevs) {
		rc = longhorn_bdev_configure(longhorn_bdev);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to configure longhorn bdev\n");
			return rc;
		}
	}

	return 0;
}

struct replica_add_ctx {
	struct longhorn_base_bdev_info *base_info;
};

int 
longhorn_bdev_add_replica(const char *name, char *lvs, char *addr, uint16_t nvmf_port, uint16_t comm_port) {
	struct longhorn_bdev	*longhorn_bdev;
	struct longhorn_base_bdev_info *base_info;

	longhorn_bdev = longhorn_bdev_find_by_name(name);
	if (!longhorn_bdev) {
		SPDK_ERRLOG("Longhorn bdev '%s' is not created yet\n", name);
		return -ENODEV;
	}
	

	base_info = calloc(1, sizeof(*base_info));
	base_info->lvs = strdup(lvs);

	if ((!addr || addr[0] == '\0')) {
		base_info->is_local = true;
		base_info->bdev_name = spdk_sprintf_alloc("%s/%s", lvs, name);

		longhorn_bdev_add_base_device(longhorn_bdev, base_info);
	} else {
		base_info->remote_addr = strdup(addr);
		base_info->nvmf_port = nvmf_port;
		base_info->comm_port = comm_port;
	}
}
	


/*
 * brief:
 * Add base bdevs to the longhorn bdev one by one.  Skip any base bdev which doesn't
 *  exist or fails to add. If all base bdevs are successfully added, the longhorn bdev
 *  moves to the configured state and becomes available. Otherwise, the longhorn bdev
 *  stays at the configuring state with added base bdevs.
 * params:
 * longhorn_cfg - pointer to longhorn bdev config
 * returns:
 * 0 - The longhorn bdev moves to the configured state or stays at the configuring
 *     state with added base bdevs due to any nonexistent base bdev.
 * non zero - Failed to add any base bdev and stays at the configuring state with
 *            added base bdevs.
 */
#if 0
int
longhorn_bdev_add_base_devices(struct longhorn_bdev_config *longhorn_cfg)
{
	uint8_t	i;
	int	rc = 0, _rc;

	for (i = 0; i < longhorn_cfg->num_base_bdevs; i++) {
		_rc = longhorn_bdev_add_base_device(longhorn_cfg, longhorn_cfg->base_bdev[i].name, i);
		if (_rc == -ENODEV) {
			SPDK_DEBUGLOG(bdev_longhorn, "base bdev %s doesn't exist now\n",
				      longhorn_cfg->base_bdev[i].name);
		} else if (_rc != 0) {
			SPDK_ERRLOG("Failed to add base bdev %s to RAID bdev %s: %s\n",
				    longhorn_cfg->base_bdev[i].name, longhorn_cfg->name,
				    spdk_strerror(-_rc));
			if (rc == 0) {
				rc = _rc;
			}
		}
	}

	return rc;
}
#endif

/*
 * brief:
 * longhorn_bdev_examine function is the examine function call by the below layers
 * like bdev_nvme layer. This function will check if this base bdev can be
 * claimed by this longhorn bdev or not.
 * params:
 * bdev - pointer to base bdev
 * returns:
 * none
 */
static void
longhorn_bdev_examine(struct spdk_bdev *bdev)
{
	struct longhorn_bdev_config	*longhorn_cfg;
	uint8_t			base_bdev_slot;

#if 0
	if (longhorn_bdev_can_claim_bdev(bdev->name, &longhorn_cfg, &base_bdev_slot)) {
		longhorn_bdev_add_base_device(longhorn_cfg, bdev->name, base_bdev_slot);
	} else {
		SPDK_DEBUGLOG(bdev_longhorn, "bdev %s can't be claimed\n",
			      bdev->name);
	}
#endif

	spdk_bdev_module_examine_done(&g_longhorn_if);
}



static struct longhorn_base_bdev_info *
longhorn_bdev_find_base_bdev(struct longhorn_bdev *longhorn_bdev, char *lvs, char *addr, uint16_t nvmf_port, uint16_t comm_port) {
	struct longhorn_base_bdev_info	*base_info;

	TAILQ_FOREACH(base_info, &longhorn_bdev->base_bdevs_head, infos) {
		if (!base_info->lvs) {
			SPDK_ERRLOG("base bdev lvs is null\n");
			continue;
		}

		if (strcmp(base_info->lvs, lvs) == 0) {
			return base_info;
		}
	}

	return NULL;
}

struct io_channel_remove_ctx {
	struct longhorn_base_bdev_info  *base_info;
	struct longhorn_bdev_io_channel *io_channel;
};

static struct longhorn_base_io_channel *
longhorn_io_find_channel(struct longhorn_base_bdev_info  *base_info,
			 struct longhorn_bdev_io_channel *io_channel)
{
	struct longhorn_base_io_channel *base_channel;

	TAILQ_FOREACH(base_channel, &io_channel->base_channels, channels) {
		if (base_channel->base_info == base_info) {
			return base_channel;
		}
	}

	return NULL;
}

static void longhorn_io_channel_remove_bdev(void *arg) {
	struct io_channel_remove_ctx *ctx = arg;
	struct longhorn_base_io_channel *base_channel;

	base_channel = longhorn_io_find_channel(ctx->base_info, ctx->io_channel);

	if (base_channel != NULL) {
		
		SPDK_ERRLOG("removing %p\n", base_channel);
		TAILQ_REMOVE(&ctx->io_channel->base_channels, base_channel, channels);
		//1TAILQ_INSERT_TAIL(&longhorn_ch->base_channels, base_channel, channels);
		free(base_channel);

	}

	ctx->io_channel->last_read_io_ch = NULL;
	
	TAILQ_FOREACH(base_channel, &ctx->io_channel->base_channels, channels) {
		SPDK_ERRLOG("Longhorn base bdev '%s' remaining %p\n", base_channel->base_info->lvs, base_channel);
	}

	/* TODO If this is the last io_channel to remove,
 	 * * unclaim the bdev 
 	 * * free the base_info */
	
	
	free(ctx);
}
	


int longhorn_bdev_remove_replica(char *name, char *lvs, char *addr, uint16_t nvmf_port, uint16_t comm_port) {
	struct longhorn_bdev	*longhorn_bdev;
	struct longhorn_base_bdev_info	*base_info;
	struct longhorn_bdev_io_channel *io_channel;
	struct io_channel_remove_ctx *ctx;
	int			rc;

	longhorn_bdev = longhorn_bdev_find_by_name(name);
	if (!longhorn_bdev) {
		SPDK_ERRLOG("Longhorn bdev '%s' is not created yet\n", name);
		return -ENODEV;
	}

	rc = pthread_mutex_trylock(&longhorn_bdev->base_bdevs_mutex);

	if (rc != 0) {
		if (errno == EBUSY) {
			SPDK_ERRLOG("Longhorn bdev '%s' is busy\n", name);
		}


		return -errno;
	}

	base_info = longhorn_bdev_find_base_bdev(longhorn_bdev, lvs, addr, nvmf_port, comm_port);
	 
	if (base_info == NULL) {
		SPDK_ERRLOG("replica in longhorn bdev '%s' is not found\n", name);
		pthread_mutex_unlock(&longhorn_bdev->base_bdevs_mutex);
		
		return -ENODEV;

	}


	longhorn_bdev->num_base_bdevs_discovered--;
	longhorn_bdev->num_base_bdevs--;
	TAILQ_REMOVE(&longhorn_bdev->base_bdevs_head, base_info, infos);

	/* signal each longhorn_io to stop using the bdev */
		SPDK_ERRLOG("num io channels %u\n", longhorn_bdev->num_io_channels);

	TAILQ_FOREACH(io_channel, &longhorn_bdev->io_channel_head, channels) {
		ctx = calloc(1, sizeof (*ctx));

		ctx->base_info = base_info;
		ctx->io_channel = io_channel;
		
		if (!io_channel->deleted) {
			spdk_thread_send_msg(io_channel->thread, longhorn_io_channel_remove_bdev, ctx);
		}


	}
	
	pthread_mutex_unlock(&longhorn_bdev->base_bdevs_mutex);



	return 0;
}


int longhorn_volume_add_replica(char *name, char *lvs, char *addr, uint16_t nvmf_port, uint16_t comm_port) {
	struct longhorn_bdev	*longhorn_bdev;
	struct longhorn_base_bdev_info	*base_info;
	struct longhorn_bdev_io_channel *io_channel;
	struct io_channel_remove_ctx *ctx;
	int			rc;

	longhorn_bdev = longhorn_bdev_find_by_name(name);
	if (!longhorn_bdev) {
		SPDK_ERRLOG("Longhorn bdev '%s' is not created yet\n", name);
		return -ENODEV;
	}

	rc = pthread_mutex_trylock(&longhorn_bdev->base_bdevs_mutex);

	if (rc != 0) {
		if (errno == EBUSY) {
			SPDK_ERRLOG("Longhorn bdev '%s' is busy\n", name);
		}


		return -errno;
	}
}

void longhorn_unpause(struct longhorn_bdev *longhorn_bdev)
{
	int			rc;
	struct longhorn_bdev_io_channel *io_channel;


	rc = pthread_mutex_trylock(&longhorn_bdev->base_bdevs_mutex);

	if (rc != 0) {
		return -errno;
	}

	TAILQ_FOREACH(io_channel, &longhorn_bdev->io_channel_head, channels) {
		spdk_thread_send_msg(io_channel->thread, bdev_longhorn_unpause_io, io_channel);

	}
	
	pthread_mutex_unlock(&longhorn_bdev->base_bdevs_mutex);
	SPDK_ERRLOG("UNPAUSE COMPLETE \n");

	return 0;
}

/* Log component for bdev longhorn bdev module */
SPDK_LOG_REGISTER_COMPONENT(bdev_longhorn)
