// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2018-2020 Broadcom.
 */

#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/hash.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>
#include <linux/timer.h>

#include "bcm_vk.h"
#include "bcm_vk_msg.h"
#include "bcm_vk_sg.h"

/* macros to manipulate the transport id in msg block */
#define BCM_VK_MSG_Q_SHIFT	 4
#define BCM_VK_MSG_Q_MASK	 0xF
#define BCM_VK_MSG_ID_MASK	 0xFFF
#define BCM_VK_GET_Q(msg_p)			 \
	((msg_p)->trans_id & BCM_VK_MSG_Q_MASK)
#define BCM_VK_SET_Q(msg_p, val)		 \
{						 \
	(msg_p)->trans_id =			 \
		((msg_p)->trans_id & ~BCM_VK_MSG_Q_MASK) | (val); \
}
#define BCM_VK_GET_MSG_ID(msg_p)		 \
	(((msg_p)->trans_id >> BCM_VK_MSG_Q_SHIFT) & BCM_VK_MSG_ID_MASK)
#define BCM_VK_SET_MSG_ID(msg_p, val)		 \
{						 \
	(msg_p)->trans_id =			 \
		(val << BCM_VK_MSG_Q_SHIFT) | BCM_VK_GET_Q(msg_p);\
}

#if defined(CONFIG_BCM_VK_H2VK_VERIFY_AND_RETRY)
/*
 * Turn on the following to verify the data passed down to VK is good, and
 * if not, do retry.  This is a debug/workaround on FPGA PCIe timing issues
 * but may be found useful for debugging other PCIe hardware issues.
 */
static void bcm_vk_h2vk_verify_idx(struct device *dev,
				   const char *tag,
				   volatile uint32_t *idx,
				   uint32_t expected)
{
	unsigned int count = 0;

	while (*idx != expected) {
		count++;
		dev_err(dev, "[%d] %s exp %d idx %d\n",
			count, tag, expected, *idx);

		/* write again */
		*idx = expected;
	}
}

static void bcm_vk_h2vk_verify_blk(struct device *dev,
				   const struct vk_msg_blk *src,
				   volatile struct vk_msg_blk *dst)

{
	struct vk_msg_blk rd_bck;
	unsigned int count = 0;

	rd_bck = *dst;
	while (memcmp(&rd_bck, src, sizeof(rd_bck)) != 0) {
		count++;
		dev_err(dev,
			"[%d]Src Blk: [0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x]\n",
			count,
			src->function_id,
			src->size,
			BCM_VK_GET_Q(src),
			BCM_VK_GET_MSG_ID(src),
			src->context_id,
			src->args[0],
			src->args[1]);
		dev_err(dev,
			"[%d]Rdb Blk: [0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x]\n",
			count,
			rd_bck.function_id,
			rd_bck.size,
			BCM_VK_GET_Q(&rd_bck),
			BCM_VK_GET_MSG_ID(&rd_bck),
			rd_bck.context_id,
			rd_bck.args[0],
			rd_bck.args[1]);

		*dst = *src;
		rd_bck = *dst;
	}
}
#else
static void bcm_vk_h2vk_verify_idx(struct device __always_unused *dev,
				   const char __always_unused *tag,
				   volatile uint32_t __always_unused *idx,
				   uint32_t __always_unused expected)
{
}

static void bcm_vk_h2vk_verify_blk
		(struct device __always_unused *dev,
		 const struct vk_msg_blk __always_unused *src,
		 volatile struct vk_msg_blk __always_unused *dst)

{
}
#endif

#if defined(CONFIG_BCM_VK_QSTATS)

/* Use default value of 20000 rd/wr per update */
#if !defined(BCM_VK_QSTATS_ACC_CNT)
#define BCM_VK_QSTATS_ACC_CNT 20000
#endif

void bcm_vk_update_qstats(struct bcm_vk *vk, const char *tag,
			  struct bcm_vk_qstats *qstats, uint32_t occupancy)
{
	struct bcm_vk_qs_cnts *qcnts = &qstats->qcnts;

	if (occupancy > qcnts->max_occ) {
		qcnts->max_occ = occupancy;
		if (occupancy > qcnts->max_abs)
			qcnts->max_abs = occupancy;
	}

	qcnts->acc_sum += occupancy;
	if (++qcnts->cnt >= BCM_VK_QSTATS_ACC_CNT) {
		/* log average and clear counters */
		dev_info(&vk->pdev->dev,
			 "%s[%d]: Max: [%3d/%3d] Acc %d num %d, Aver %d\n",
			 tag, qstats->q_num,
			 qcnts->max_occ, qcnts->max_abs,
			 qcnts->acc_sum,
			 qcnts->cnt,
			 qcnts->acc_sum / qcnts->cnt);

		qcnts->cnt = 0;
		qcnts->max_occ = 0;
		qcnts->acc_sum = 0;
	}
}
#endif

/* number of retries when enqueue message fails before returning EAGAIN */
#define BCM_VK_H2VK_ENQ_RETRY 10
#define BCM_VK_H2VK_ENQ_RETRY_DELAY_MS 50

bool bcm_vk_drv_access_ok(struct bcm_vk *vk)
{
	return (!!atomic_read(&vk->msgq_inited));
}

static void bcm_vk_set_host_alert(struct bcm_vk *vk, uint32_t bit_mask)
{
	struct bcm_vk_alert *alert = &vk->host_alert;
	unsigned long flags;

	/* use irqsave version as this maybe called inside timer interrupt */
	spin_lock_irqsave(&vk->host_alert_lock, flags);
	alert->notfs |= bit_mask;
	spin_unlock_irqrestore(&vk->host_alert_lock, flags);

	if (test_and_set_bit(BCM_VK_WQ_NOTF_PEND, vk->wq_offload) == 0)
		queue_work(vk->wq_thread, &vk->wq_work);
}

#if defined(BCM_VK_LEGACY_API)
/*
 * legacy does not support heartbeat mechanism
 */
void bcm_vk_hb_init(struct bcm_vk *vk)
{
	dev_info(&vk->pdev->dev, "skipped\n");
}

void bcm_vk_hb_deinit(struct bcm_vk *vk)
{
	dev_info(&vk->pdev->dev, "skipped\n");
}
#else

/*
 * Heartbeat related defines
 * The heartbeat from host is a last resort.  If stuck condition happens
 * on the card, firmware is supposed to detect it.  Therefore, the heartbeat
 * values used will be more relaxed on the driver, which need to be bigger
 * than the watchdog timeout on the card.  The watchdog timeout on the card
 * is 20s, with a jitter of 2s => 22s.  We use a value of 27s here.
 */
#define BCM_VK_HB_TIMER_S 3
#define BCM_VK_HB_TIMER_VALUE (BCM_VK_HB_TIMER_S * HZ)
#define BCM_VK_HB_LOST_MAX (27 / BCM_VK_HB_TIMER_S)

static void bcm_vk_hb_poll(struct timer_list *t)
{
	uint32_t uptime_s;
	struct bcm_vk_hb_ctrl *hb = container_of(t, struct bcm_vk_hb_ctrl,
						 timer);
	struct bcm_vk *vk = container_of(hb, struct bcm_vk, hb_ctrl);

	if (bcm_vk_drv_access_ok(vk)) {
		/* read uptime from register and compare */
		uptime_s = vkread32(vk, BAR_0, BAR_OS_UPTIME);

		if (uptime_s == hb->last_uptime)
			hb->lost_cnt++;
		else /* reset to avoid accumulation */
			hb->lost_cnt = 0;

		dev_dbg(&vk->pdev->dev, "Last uptime %d current %d, lost %d\n",
			hb->last_uptime, uptime_s, hb->lost_cnt);

		/*
		 * if the interface goes down without any activity, a value
		 * of 0xFFFFFFFF will be continuously read, and the detection
		 * will be happened eventually.
		 */
		hb->last_uptime = uptime_s;
	} else {
		/* reset heart beat lost cnt */
		hb->lost_cnt = 0;
	}

	/* next, check if heartbeat exceeds limit */
	if (hb->lost_cnt > BCM_VK_HB_LOST_MAX) {
		dev_err(&vk->pdev->dev, "Heartbeat Misses %d times, %d s!\n",
			BCM_VK_HB_LOST_MAX,
			BCM_VK_HB_LOST_MAX * BCM_VK_HB_TIMER_S);

		bcm_vk_blk_drv_access(vk);
		bcm_vk_set_host_alert(vk, ERR_LOG_HOST_HB_FAIL);
	}
	/* re-arm timer */
	mod_timer(&hb->timer, jiffies + BCM_VK_HB_TIMER_VALUE);
}

void bcm_vk_hb_init(struct bcm_vk *vk)
{
	struct bcm_vk_hb_ctrl *hb = &vk->hb_ctrl;

	timer_setup(&hb->timer, bcm_vk_hb_poll, 0);
	mod_timer(&hb->timer, jiffies + BCM_VK_HB_TIMER_VALUE);
}

void bcm_vk_hb_deinit(struct bcm_vk *vk)
{
	struct bcm_vk_hb_ctrl *hb = &vk->hb_ctrl;

	del_timer(&hb->timer);
}
#endif

static void bcm_vk_msgid_bitmap_clear(struct bcm_vk *vk,
				      unsigned int start,
				      unsigned int nbits)
{
	spin_lock(&vk->msg_id_lock);
	bitmap_clear(vk->bmap, start, nbits);
	spin_unlock(&vk->msg_id_lock);
}

/*
 * allocate a ctx per file struct
 */
static struct bcm_vk_ctx *bcm_vk_get_ctx(struct bcm_vk *vk, const pid_t pid)
{
	uint32_t i;
	struct bcm_vk_ctx *ctx = NULL;
	uint32_t hash_idx = hash_32(pid, VK_PID_HT_SHIFT_BIT);

	spin_lock(&vk->ctx_lock);

	/* check if it is in reset, if so, don't allow */
	if (vk->reset_pid) {
		dev_err(&vk->pdev->dev,
			"No context allowed during reset by pid %d\n",
			vk->reset_pid);

		goto in_reset_exit;
	}

	for (i = 0; i < ARRAY_SIZE(vk->ctx); i++) {
		if (!vk->ctx[i].in_use) {
			vk->ctx[i].in_use = true;
			ctx = &vk->ctx[i];
			break;
		}
	}

	if (!ctx) {
		dev_err(&vk->pdev->dev, "All context in use\n");

		goto all_in_use_exit;
	}

	/* set the pid and insert it to hash table */
	ctx->pid = pid;
	ctx->hash_idx = hash_idx;
	list_add_tail(&ctx->node, &vk->pid_ht[hash_idx].head);

	/* increase kref */
	kref_get(&vk->kref);

	/* clear counter */
	ctx->pend_cnt = 0;
all_in_use_exit:
in_reset_exit:
	spin_unlock(&vk->ctx_lock);

	return ctx;
}

static uint16_t bcm_vk_get_msg_id(struct bcm_vk *vk)
{
	uint16_t rc = VK_MSG_ID_OVERFLOW;
	uint16_t test_bit_count = 0;

	spin_lock(&vk->msg_id_lock);
	while (test_bit_count < (VK_MSG_ID_BITMAP_SIZE - 1)) {
		/*
		 * first time come in this loop, msg_id will be 0
		 * and the first one tested will be 1.  We skip
		 * VK_SIMPLEX_MSG_ID (0) for one way host2vk
		 * communication
		 */
		vk->msg_id++;
		if (vk->msg_id == VK_MSG_ID_BITMAP_SIZE)
			vk->msg_id = 1;

		if (test_bit(vk->msg_id, vk->bmap)) {
			test_bit_count++;
			continue;
		}
		rc = vk->msg_id;
		bitmap_set(vk->bmap, vk->msg_id, 1);
		break;
	}
	spin_unlock(&vk->msg_id_lock);

	return rc;
}

static int bcm_vk_free_ctx(struct bcm_vk *vk, struct bcm_vk_ctx *ctx)
{
	uint32_t idx;
	uint32_t hash_idx;
	pid_t pid;
	struct bcm_vk_ctx *entry;
	int count = 0;

	if (ctx == NULL) {
		dev_err(&vk->pdev->dev, "NULL context detected\n");
		return -EINVAL;
	}
	idx = ctx->idx;
	pid = ctx->pid;

	spin_lock(&vk->ctx_lock);

	if (!vk->ctx[idx].in_use) {
		dev_err(&vk->pdev->dev, "context[%d] not in use!\n", idx);
	} else {
		vk->ctx[idx].in_use = false;
		vk->ctx[idx].miscdev = NULL;

		/* Remove it from hash list and see if it is the last one. */
		list_del(&ctx->node);
		hash_idx = ctx->hash_idx;
		list_for_each_entry(entry, &vk->pid_ht[hash_idx].head, node) {
			if (entry->pid == pid)
				count++;
		}
	}

	spin_unlock(&vk->ctx_lock);

	return count;
}

static void bcm_vk_free_wkent(struct device *dev, struct bcm_vk_wkent *entry)
{
	bcm_vk_sg_free(dev, entry->dma, VK_DMA_MAX_ADDRS);

	kfree(entry->vk2h_msg);
	kfree(entry);
}

static void bcm_vk_drain_all_pend(struct device *dev,
				  struct bcm_vk_msg_chan *chan,
				  struct bcm_vk_ctx *ctx)
{
	uint32_t num;
	struct bcm_vk_wkent *entry, *tmp;
	struct bcm_vk *vk;
	struct list_head del_q;

	if (ctx)
		vk = container_of(ctx->miscdev, struct bcm_vk, miscdev);

	INIT_LIST_HEAD(&del_q);
	spin_lock(&chan->pendq_lock);
	for (num = 0; num < chan->q_nr; num++) {
		list_for_each_entry_safe(entry, tmp, &chan->pendq[num], node) {
			if (ctx == NULL) {
				list_del(&entry->node);
				list_add_tail(&entry->node, &del_q);
			} else if (entry->ctx->idx == ctx->idx) {
				struct vk_msg_blk *msg;
				int bit_set;
				bool responded;
				uint32_t msg_id;

				/* if it is specific ctx, log for any stuck */
				msg = entry->h2vk_msg;
				msg_id = BCM_VK_GET_MSG_ID(msg);
				bit_set = test_bit(msg_id, vk->bmap);
				responded = entry->vk2h_msg ? true : false;
				dev_info(dev,
					 "Drained: fid %u size %u msg 0x%x(seq-%x) ctx 0x%x[fd-%d] args:[0x%x 0x%x] resp %s, bmap %d\n",
					 msg->function_id, msg->size,
					 msg_id, entry->seq_num,
					 msg->context_id, entry->ctx->idx,
					 msg->args[0], msg->args[1],
					 responded ? "T" : "F", bit_set);
				list_del(&entry->node);
				list_add_tail(&entry->node, &del_q);
				if (responded)
					ctx->pend_cnt--;
				else if (bit_set)
					bcm_vk_msgid_bitmap_clear(vk,
								  msg_id,
								  1);
			}
		}
	}
	spin_unlock(&chan->pendq_lock);

	/* batch clean up */
	num = 0;
	list_for_each_entry_safe(entry, tmp, &del_q, node) {
		list_del(&entry->node);
		bcm_vk_free_wkent(dev, entry);
		num++;
	}
	if (num)
		dev_info(dev, "Total drained items %d\n", num);
}

bool bcm_vk_msgq_marker_valid(struct bcm_vk *vk)
{
	uint32_t rdy_marker = 0;
	uint32_t fw_status;

	fw_status = vkread32(vk, BAR_0, VK_BAR_FWSTS);

	if ((fw_status & VK_FWSTS_READY) == VK_FWSTS_READY)
		rdy_marker = vkread32(vk, BAR_1, VK_BAR1_MSGQ_DEF_RDY);

	return (rdy_marker == VK_BAR1_MSGQ_RDY_MARKER);
}

/*
 * Function to sync up the messages queue info that is provided by BAR1
 */
int bcm_vk_sync_msgq(struct bcm_vk *vk, bool force_sync)
{
	struct bcm_vk_msgq *msgq = NULL;
	struct device *dev = &vk->pdev->dev;
	uint32_t msgq_off;
	uint32_t num_q;
	struct bcm_vk_msg_chan *chan_list[] = {&vk->h2vk_msg_chan,
					       &vk->vk2h_msg_chan};
	struct bcm_vk_msg_chan *chan = NULL;
	int i, j;
	int ret = 0;

	/*
	 * If the driver is loaded at startup where vk OS is not up yet,
	 * the msgq-info may not be available until a later time.  In
	 * this case, we skip and the sync function is supposed to be
	 * called again.
	 */
	if (!bcm_vk_msgq_marker_valid(vk)) {
		dev_info(dev, "BAR1 msgq marker not initialized.\n");
		return ret;
	}

	msgq_off = vkread32(vk, BAR_1, VK_BAR1_MSGQ_CTRL_OFF);

	/* each side is always half the total  */
	num_q = vk->h2vk_msg_chan.q_nr = vk->vk2h_msg_chan.q_nr =
		vkread32(vk, BAR_1, VK_BAR1_MSGQ_NR) / 2;

	/* first msgq location */
	msgq = (struct bcm_vk_msgq *)(vk->bar[BAR_1] + msgq_off);

	/*
	 * if this function is called when it is already inited,
	 * something is wrong
	 */
	if (bcm_vk_drv_access_ok(vk) && (!force_sync)) {
		dev_err(dev, "Msgq info already in sync\n");
		ret = -EPERM;
		goto already_inited;
	}

	for (i = 0; i < ARRAY_SIZE(chan_list); i++) {
		chan = chan_list[i];
		memset(chan->sync_qinfo, 0, sizeof(chan->sync_qinfo));

		for (j = 0; j < num_q; j++) {
			chan->msgq[j] = msgq;

			dev_info(dev,
				 "MsgQ[%d] type %d num %d, @ 0x%x, rd_idx %d wr_idx %d, size %d, nxt 0x%x\n",
				 j,
				 chan->msgq[j]->type,
				 chan->msgq[j]->num,
				 chan->msgq[j]->start,
				 chan->msgq[j]->rd_idx,
				 chan->msgq[j]->wr_idx,
				 chan->msgq[j]->size,
				 chan->msgq[j]->nxt);

			/* formulate and record static info */
			chan->sync_qinfo[j].q_start =
				vk->bar[BAR_1] + chan->msgq[j]->start;
			chan->sync_qinfo[j].q_size = chan->msgq[j]->size;
			/* set low threshold as 50% or 1/2 */
			chan->sync_qinfo[j].q_low =
				chan->sync_qinfo[j].q_size >> 1;
			chan->sync_qinfo[j].q_mask =
				chan->sync_qinfo[j].q_size - 1;

			msgq = (struct bcm_vk_msgq *)
				((char *)msgq + sizeof(*msgq) + msgq->nxt);

			rmb(); /* do a read mb to guarantee */
		}
	}
	atomic_set(&vk->msgq_inited, 1);

already_inited:
	return ret;
}

static int bcm_vk_msg_chan_init(struct bcm_vk_msg_chan *chan)
{
	int rc = 0;
	uint32_t i;

	mutex_init(&chan->msgq_mutex);
	spin_lock_init(&chan->pendq_lock);
	for (i = 0; i < VK_MSGQ_MAX_NR; i++) {
		INIT_LIST_HEAD(&chan->pendq[i]);
#if defined(CONFIG_BCM_VK_QSTATS)
		chan->qstats[i].q_num = i;
#endif
	}

	return rc;
}

static void bcm_vk_append_pendq(struct bcm_vk_msg_chan *chan, uint16_t q_num,
				struct bcm_vk_wkent *entry)
{
	spin_lock(&chan->pendq_lock);
	list_add_tail(&entry->node, &chan->pendq[q_num]);
	if (entry->vk2h_msg)
		entry->ctx->pend_cnt++;
	spin_unlock(&chan->pendq_lock);
}

static uint32_t bcm_vk_append_ib_sgl(struct bcm_vk *vk,
				     struct bcm_vk_wkent *entry,
				     struct _vk_data *data,
				     unsigned int num_planes)
{
	unsigned int i;
	unsigned int item_cnt = 0;
	struct device *dev = &vk->pdev->dev;
	struct bcm_vk_msg_chan *chan = &vk->h2vk_msg_chan;
	struct vk_msg_blk *msg = &entry->h2vk_msg[0];
	struct bcm_vk_msgq *msgq;
	struct bcm_vk_sync_qinfo *qinfo;
	uint32_t ib_sgl_size = 0;
	uint8_t *buf = (uint8_t *)&entry->h2vk_msg[entry->h2vk_blks];
	uint32_t avail;
	uint32_t q_num;

	/* check if high watermark is hit, and if so, skip */
	q_num = BCM_VK_GET_Q(msg);
	msgq = chan->msgq[q_num];
	qinfo = &chan->sync_qinfo[q_num];
	avail = VK_MSGQ_AVAIL_SPACE(msgq, qinfo);
	if (avail < qinfo->q_low) {
		dev_dbg(dev, "Skip inserting inband SGL, [0x%x/0x%x]\n",
			avail, qinfo->q_size);
		return ib_sgl_size;
	}

	for (i = 0; i < num_planes; i++) {
		if (data[i].address &&
		    (ib_sgl_size + data[i].size) <= vk->ib_sgl_size) {

			item_cnt++;
			memcpy(buf, entry->dma[i].sglist, data[i].size);
			ib_sgl_size += data[i].size;
			buf += data[i].size;
		}
	}

	dev_dbg(dev, "Num %u sgl items appended, size 0x%x, room 0x%x\n",
		item_cnt, ib_sgl_size, vk->ib_sgl_size);

	/* round up size */
	ib_sgl_size = (ib_sgl_size + VK_MSGQ_BLK_SIZE - 1)
		       >> VK_MSGQ_BLK_SZ_SHIFT;

	return ib_sgl_size;
}

void bcm_h2vk_doorbell(struct bcm_vk *vk, uint32_t q_num,
			      uint32_t db_val)
{
	/* press door bell based on q_num */
	vkwrite32(vk,
		  db_val,
		  BAR_0,
		  VK_BAR0_REGSEG_DB_BASE + q_num * VK_BAR0_REGSEG_DB_REG_GAP);
}

static int bcm_h2vk_msg_enqueue(struct bcm_vk *vk, struct bcm_vk_wkent *entry)
{
	static uint32_t seq_num;
	struct bcm_vk_msg_chan *chan = &vk->h2vk_msg_chan;
	struct device *dev = &vk->pdev->dev;
	struct vk_msg_blk *src = &entry->h2vk_msg[0];

	volatile struct vk_msg_blk *dst;
	struct bcm_vk_msgq *msgq;
	struct bcm_vk_sync_qinfo *qinfo;
	uint32_t q_num = BCM_VK_GET_Q(src);
	uint32_t wr_idx; /* local copy */
	uint32_t i;
	uint32_t avail;
	uint32_t retry;

	if (entry->h2vk_blks != src->size + 1) {
		dev_err(dev, "number of blks %d not matching %d MsgId[0x%x]: func %d ctx 0x%x\n",
			entry->h2vk_blks,
			src->size + 1,
			BCM_VK_GET_MSG_ID(src),
			src->function_id,
			src->context_id);
		return -EMSGSIZE;
	}

	msgq = chan->msgq[q_num];
	qinfo = &chan->sync_qinfo[q_num];

	rmb(); /* start with a read barrier */
	mutex_lock(&chan->msgq_mutex);

	avail = VK_MSGQ_AVAIL_SPACE(msgq, qinfo);

#if defined(CONFIG_BCM_VK_QSTATS)
	bcm_vk_update_qstats(vk, "h2vk", &chan->qstats[q_num],
			     qinfo->q_size - avail);
#endif
	/* if not enough space, return EAGAIN and let app handles it */
	retry = 0;
	while ((avail < entry->h2vk_blks)
	       && (retry++ < BCM_VK_H2VK_ENQ_RETRY)) {
		mutex_unlock(&chan->msgq_mutex);

		msleep(BCM_VK_H2VK_ENQ_RETRY_DELAY_MS);
		mutex_lock(&chan->msgq_mutex);
		avail = VK_MSGQ_AVAIL_SPACE(msgq, qinfo);
	}
	if (retry > BCM_VK_H2VK_ENQ_RETRY) {
		mutex_unlock(&chan->msgq_mutex);
		return -EAGAIN;
	}

	/* at this point, mutex is taken and there is enough space */
	entry->seq_num = seq_num++; /* update debug seq number */
	wr_idx = msgq->wr_idx;

	if (wr_idx >= qinfo->q_size) {
		dev_crit(dev, "Invalid wr_idx 0x%x => max 0x%x!",
			 wr_idx, qinfo->q_size);
		bcm_vk_blk_drv_access(vk);
		bcm_vk_set_host_alert(vk, ERR_LOG_HOST_PCIE_DWN);
		goto idx_err;
	}

	dst = VK_MSGQ_BLK_ADDR(qinfo, wr_idx);
	for (i = 0; i < entry->h2vk_blks; i++) {
		*dst = *src;

		bcm_vk_h2vk_verify_blk(dev, src, dst);

		src++;
		wr_idx = VK_MSGQ_INC(qinfo, wr_idx, 1);
		dst = VK_MSGQ_BLK_ADDR(qinfo, wr_idx);
	}

	/* flush the write pointer */
	msgq->wr_idx = wr_idx;
	wmb(); /* flush */

	bcm_vk_h2vk_verify_idx(dev, "wr_idx", &msgq->wr_idx, wr_idx);

	/* log new info for debugging */
	dev_dbg(dev,
		"MsgQ[%d] [Rd Wr] = [%d %d] blks inserted %d - Q = [u-%d a-%d]/%d\n",
		msgq->num,
		msgq->rd_idx, msgq->wr_idx, entry->h2vk_blks,
		VK_MSGQ_OCCUPIED(msgq, qinfo),
		VK_MSGQ_AVAIL_SPACE(msgq, qinfo),
		msgq->size);
	/*
	 * press door bell based on queue number. 1 is added to the wr_idx
	 * to avoid the value of 0 appearing on the VK side to distinguish
	 * from initial value.
	 */
	bcm_h2vk_doorbell(vk, q_num, wr_idx + 1);
idx_err:
	mutex_unlock(&chan->msgq_mutex);
	return 0;
}

int bcm_vk_send_shutdown_msg(struct bcm_vk *vk, uint32_t shut_type,
			     pid_t pid)
{
	int rc = 0;
	struct bcm_vk_wkent *entry;
	struct device *dev = &vk->pdev->dev;

	/*
	 * check if the marker is still good.  Sometimes, the PCIe interface may
	 * have gone done, and if so and we ship down thing based on broken
	 * values, kernel may panic.
	 */
	if (!bcm_vk_msgq_marker_valid(vk)) {
		dev_info(dev, "PCIe comm chan - invalid marker (0x%x)!\n",
			 vkread32(vk, BAR_1, VK_BAR1_MSGQ_DEF_RDY));
		return -EINVAL;
	}

	entry = kzalloc(sizeof(struct bcm_vk_wkent) +
			sizeof(struct vk_msg_blk), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	/* fill up necessary data */
	entry->h2vk_msg[0].function_id = VK_FID_SHUTDOWN;
	BCM_VK_SET_Q(&entry->h2vk_msg[0], 0); /* use highest queue */
	BCM_VK_SET_MSG_ID(&entry->h2vk_msg[0], VK_SIMPLEX_MSG_ID);
	entry->h2vk_blks = 1; /* always 1 block */

	entry->h2vk_msg[0].args[0] = shut_type;
	entry->h2vk_msg[0].args[1] = pid;

	rc = bcm_h2vk_msg_enqueue(vk, entry);
	if (rc)
		dev_err(dev,
			"Sending shutdown message to q %d for pid %d fails.\n",
			BCM_VK_GET_Q(&entry->h2vk_msg[0]), pid);

	kfree(entry);

	return rc;
}

int bcm_vk_handle_last_sess(struct bcm_vk *vk, const pid_t pid)
{
	int rc = 0;
	struct device *dev = &vk->pdev->dev;

	/*
	 * don't send down or do anything if message queue is not initialized
	 * and if it is the reset session, clear it.
	 */
	if (!bcm_vk_drv_access_ok(vk)) {
		if (vk->reset_pid == pid)
			vk->reset_pid = 0;
		return -EPERM;
	}

	dev_dbg(dev, "No more sessions, shut down pid %d\n", pid);

	/* only need to do it if it is not the reset process */
	if (vk->reset_pid != pid)
		rc = bcm_vk_send_shutdown_msg(vk, VK_SHUTDOWN_PID, pid);
	else
		/* put reset_pid to 0 if it is exiting last session */
		vk->reset_pid = 0;

	return rc;
}

static struct bcm_vk_wkent *bcm_vk_find_pending(struct bcm_vk *vk,
						struct bcm_vk_msg_chan *chan,
						uint16_t q_num,
						uint16_t msg_id)
{
	bool found = false;
	struct bcm_vk_wkent *entry;

	spin_lock(&chan->pendq_lock);
	list_for_each_entry(entry, &chan->pendq[q_num], node) {

		if (BCM_VK_GET_MSG_ID(&entry->h2vk_msg[0]) == msg_id) {
			list_del(&entry->node);
			found = true;
			bcm_vk_msgid_bitmap_clear(vk, msg_id, 1);
			break;
		}
	}
	spin_unlock(&chan->pendq_lock);
	return ((found) ? entry : NULL);
}

static int32_t bcm_vk2h_msg_dequeue(struct bcm_vk *vk)
{
	struct device *dev = &vk->pdev->dev;
	struct bcm_vk_msg_chan *chan = &vk->vk2h_msg_chan;
	struct vk_msg_blk *data;
	volatile struct vk_msg_blk *src;
	struct vk_msg_blk *dst;
	struct bcm_vk_msgq *msgq;
	struct bcm_vk_sync_qinfo *qinfo;
	struct bcm_vk_wkent *entry;
	uint32_t rd_idx;
	uint32_t q_num, msg_id, j;
	uint32_t num_blks;
	int32_t total = 0;

	/*
	 * drain all the messages from the queues, and find its pending
	 * entry in the h2vk queue, based on msg_id & q_num, and move the
	 * entry to the vk2h pending queue, waiting for user space
	 * program to extract
	 */
	mutex_lock(&chan->msgq_mutex);
	rmb(); /* start with a read barrier */
	for (q_num = 0; q_num < chan->q_nr; q_num++) {
		msgq = chan->msgq[q_num];
		qinfo = &chan->sync_qinfo[q_num];

		while (!VK_MSGQ_EMPTY(msgq)) {

			/*
			 * Make a local copy and get pointer to src blk
			 * The rd_idx is masked before getting the pointer to
			 * avoid out of bound access in case the interface goes
			 * down.  It will end up pointing to the last block in
			 * the buffer, but subsequent src->size check would be
			 * able to catch this.
			 */
			rd_idx = msgq->rd_idx;
			src = VK_MSGQ_BLK_ADDR(qinfo,
					     rd_idx & VK_MSGQ_SIZE_MASK(qinfo));

			if ((rd_idx >= qinfo->q_size)
			    || (src->size > (qinfo->q_size - 1))) {
				dev_crit(dev,
					 "Invalid rd_idx 0x%x or size 0x%x => max 0x%x!",
					 rd_idx, src->size, qinfo->q_size);
				bcm_vk_blk_drv_access(vk);
				bcm_vk_set_host_alert(
					vk, ERR_LOG_HOST_PCIE_DWN);
				goto idx_err;
			}

#if defined(CONFIG_BCM_VK_QSTATS)
			bcm_vk_update_qstats(vk, "vk2h", &chan->qstats[q_num],
					     VK_MSGQ_OCCUPIED(msgq, qinfo));
#endif
			num_blks = src->size + 1;
			data = kzalloc(num_blks * VK_MSGQ_BLK_SIZE, GFP_KERNEL);
			if (data) {
				/* copy messages and linearize it */
				dst = data;
				for (j = 0; j < num_blks; j++) {
					*dst = *src;

					dst++;
					rd_idx = VK_MSGQ_INC(qinfo, rd_idx, 1);
					src = VK_MSGQ_BLK_ADDR(qinfo, rd_idx);
				}
				total++;
			} else {
				/*
				 * if we could not allocate memory in kernel,
				 * that is fatal.
				 */
				dev_crit(dev, "Kernel mem allocation failure.\n");
				return -ENOMEM;
			}

			/* flush rd pointer after a message is dequeued */
			msgq->rd_idx = rd_idx;
			mb(); /* do both rd/wr as we are extracting data out */

			bcm_vk_h2vk_verify_idx(dev, "rd_idx",
					       &msgq->rd_idx, rd_idx);

			/* log new info for debugging */
			dev_dbg(dev,
				"MsgQ[%d] [Rd Wr] = [%d %d] blks extracted %d - Q = [u-%d a-%d]/%d\n",
				msgq->num,
				msgq->rd_idx, msgq->wr_idx, num_blks,
				VK_MSGQ_OCCUPIED(msgq, qinfo),
				VK_MSGQ_AVAIL_SPACE(msgq, qinfo),
				msgq->size);

			/*
			 * No need to search if it is an autonomous one-way
			 * message from driver, as these messages do not bear
			 * a h2vk pending item. Currently, only the shutdown
			 * message falls into this category.
			 */
			if (data->function_id == VK_FID_SHUTDOWN) {
				kfree(data);
				continue;
			}

			msg_id = BCM_VK_GET_MSG_ID(data);
			/* lookup original message in h2vk direction */
			entry = bcm_vk_find_pending(vk,
						    &vk->h2vk_msg_chan,
						    q_num,
						    msg_id);

			/*
			 * if there is message to does not have prior send,
			 * this is the location to add here
			 */
			if (entry) {
				entry->vk2h_blks = num_blks;
				entry->vk2h_msg = data;
				bcm_vk_append_pendq(&vk->vk2h_msg_chan,
						    q_num, entry);

			} else {
				dev_crit(dev,
					 "Could not find MsgId[0x%x] for resp func %d bmap %d\n",
					 msg_id, data->function_id,
					 test_bit(msg_id, vk->bmap));
				kfree(data);
			}

		}
	}
idx_err:
	mutex_unlock(&chan->msgq_mutex);
	dev_dbg(dev, "total %d drained from queues\n", total);

	return total;
}

/*
 * deferred work queue for draining and auto download.
 */
static void bcm_vk_wq_handler(struct work_struct *work)
{
	struct bcm_vk *vk = container_of(work, struct bcm_vk, wq_work);
	struct device *dev = &vk->pdev->dev;
	int32_t ret;

	/* check wq offload bit map to perform various operations */
	if (test_bit(BCM_VK_WQ_NOTF_PEND, vk->wq_offload)) {
		/* clear bit right the way for notification */
		clear_bit(BCM_VK_WQ_NOTF_PEND, vk->wq_offload);
		bcm_vk_handle_notf(vk);
	}
	if (test_bit(BCM_VK_WQ_DWNLD_AUTO, vk->wq_offload)) {
		bcm_vk_auto_load_all_images(vk);

		/*
		 * at the end of operation, clear AUTO bit and pending
		 * bit
		 */
		clear_bit(BCM_VK_WQ_DWNLD_AUTO, vk->wq_offload);
		clear_bit(BCM_VK_WQ_DWNLD_PEND, vk->wq_offload);
	}

	/* next, try to drain */
	ret = bcm_vk2h_msg_dequeue(vk);

	if (ret == 0)
		dev_dbg(dev, "Spurious trigger for workqueue\n");
	else if (ret < 0)
		bcm_vk_blk_drv_access(vk);
}

/*
 * init routine for all required data structures
 */
static int bcm_vk_data_init(struct bcm_vk *vk)
{
	int rc = 0;
	int i;

	spin_lock_init(&vk->ctx_lock);
	for (i = 0; i < ARRAY_SIZE(vk->ctx); i++) {
		vk->ctx[i].in_use = false;
		vk->ctx[i].idx = i;	/* self identity */
		vk->ctx[i].miscdev = NULL;
	}
	spin_lock_init(&vk->msg_id_lock);
	spin_lock_init(&vk->host_alert_lock);
	vk->msg_id = 0;

	/* initialize hash table */
	for (i = 0; i < VK_PID_HT_SZ; i++)
		INIT_LIST_HEAD(&vk->pid_ht[i].head);

	INIT_WORK(&vk->wq_work, bcm_vk_wq_handler);
	return rc;
}

irqreturn_t bcm_vk_msgq_irqhandler(int irq, void *dev_id)
{
	struct bcm_vk *vk = dev_id;

	if (!bcm_vk_drv_access_ok(vk)) {
		dev_err(&vk->pdev->dev,
			"Interrupt %d received when msgq not inited\n", irq);
		goto skip_schedule_work;
	}

	queue_work(vk->wq_thread, &vk->wq_work);

skip_schedule_work:
	return IRQ_HANDLED;
}

int bcm_vk_open(struct inode *inode, struct file *p_file)
{
	struct bcm_vk_ctx *ctx;
	struct miscdevice *miscdev = (struct miscdevice *)p_file->private_data;
	struct bcm_vk *vk = container_of(miscdev, struct bcm_vk, miscdev);
	struct device *dev = &vk->pdev->dev;
	int    rc = 0;

	/* get a context and set it up for file */
	ctx = bcm_vk_get_ctx(vk, task_pid_nr(current));
	if (!ctx) {
		dev_err(dev, "Error allocating context\n");
		rc = -ENOMEM;
	} else {

		/*
		 * set up context and replace private data with context for
		 * other methods to use.  Reason for the context is because
		 * it is allowed for multiple sessions to open the sysfs, and
		 * for each file open, when upper layer query the response,
		 * only those that are tied to a specific open should be
		 * returned.  The context->idx will be used for such binding
		 */
		ctx->miscdev = miscdev;
		p_file->private_data = ctx;
		dev_dbg(dev, "ctx_returned with idx %d, pid %d\n",
			ctx->idx, ctx->pid);
	}
	return rc;
}

ssize_t bcm_vk_read(struct file *p_file, char __user *buf, size_t count,
			   loff_t *f_pos)
{
	ssize_t rc = -ENOMSG;
	struct bcm_vk_ctx *ctx = p_file->private_data;
	struct bcm_vk *vk = container_of(ctx->miscdev, struct bcm_vk,
					 miscdev);
	struct device *dev = &vk->pdev->dev;
	struct bcm_vk_msg_chan *chan = &vk->vk2h_msg_chan;
	struct bcm_vk_wkent *entry = NULL;
	uint32_t q_num;
	uint32_t rsp_length;
	bool found = false;

	if (!bcm_vk_drv_access_ok(vk))
		return -EPERM;

	dev_dbg(dev, "Buf count %ld\n", count);
	found = false;

	/*
	 * search through the pendq on the vk2h chan, and return only those
	 * that belongs to the same context.  Search is always from the high to
	 * the low priority queues
	 */
	spin_lock(&chan->pendq_lock);
	for (q_num = 0; q_num < chan->q_nr; q_num++) {
		list_for_each_entry(entry, &chan->pendq[q_num], node) {
			if (entry->ctx->idx == ctx->idx) {
				if (count >=
				    (entry->vk2h_blks * VK_MSGQ_BLK_SIZE)) {
					list_del(&entry->node);
					ctx->pend_cnt--;
					found = true;
				} else {
					/* buffer not big enough */
					rc = -EMSGSIZE;
				}
				goto bcm_vk_read_loop_exit;
			}
		}
	}
 bcm_vk_read_loop_exit:
	spin_unlock(&chan->pendq_lock);

	if (found) {
		/* retrieve the passed down msg_id */
		BCM_VK_SET_MSG_ID(&entry->vk2h_msg[0], entry->usr_msg_id);
		rsp_length = entry->vk2h_blks * VK_MSGQ_BLK_SIZE;
		if (copy_to_user(buf, entry->vk2h_msg, rsp_length) == 0)
			rc = rsp_length;

		bcm_vk_free_wkent(dev, entry);
	} else if (rc == -EMSGSIZE) {
		struct vk_msg_blk tmp_msg = entry->vk2h_msg[0];

		/*
		 * in this case, return just the first block, so
		 * that app knows what size it is looking for.
		 */
		BCM_VK_SET_MSG_ID(&tmp_msg, entry->usr_msg_id);
		tmp_msg.size = entry->vk2h_blks - 1;
		if (copy_to_user(buf, &tmp_msg, VK_MSGQ_BLK_SIZE) != 0) {
			dev_err(dev, "Error return 1st block in -EMSGSIZE\n");
			rc = -EFAULT;
		}
	}
	return rc;
}

ssize_t bcm_vk_write(struct file *p_file, const char __user *buf,
			    size_t count, loff_t *f_pos)
{
	ssize_t rc = -EPERM;
	struct bcm_vk_ctx *ctx = p_file->private_data;
	struct bcm_vk *vk = container_of(ctx->miscdev, struct bcm_vk,
					 miscdev);
	struct bcm_vk_msgq *msgq;
	struct device *dev = &vk->pdev->dev;
	struct bcm_vk_wkent *entry;
	uint32_t sgl_extra_blks;
	uint32_t q_num;
	uint32_t msg_size;

	if (!bcm_vk_drv_access_ok(vk))
		return -EPERM;

	dev_dbg(dev, "Msg count %ld\n", count);

	/* first, do sanity check where count should be multiple of basic blk */
	if (count & (VK_MSGQ_BLK_SIZE - 1)) {
		dev_err(dev, "Failure with size %ld not multiple of %ld\n",
			count, VK_MSGQ_BLK_SIZE);
		rc = -EBADR;
		goto bcm_vk_write_err;
	}

	/* allocate the work entry + buffer for size count and inband sgl */
	entry = kzalloc(sizeof(struct bcm_vk_wkent) + count + vk->ib_sgl_size,
			GFP_KERNEL);
	if (!entry) {
		rc = -ENOMEM;
		goto bcm_vk_write_err;
	}

	/* now copy msg from user space, and then formulate the wk ent */
	if (copy_from_user(&entry->h2vk_msg[0], buf, count))
		goto bcm_vk_write_free_ent;

	entry->h2vk_blks = count >> VK_MSGQ_BLK_SZ_SHIFT;
	entry->ctx = ctx;

	/* do a check on the blk size which could not exceed queue space */
	q_num = BCM_VK_GET_Q(&entry->h2vk_msg[0]);
	msgq = vk->h2vk_msg_chan.msgq[q_num];
	if (entry->h2vk_blks + (vk->ib_sgl_size >> VK_MSGQ_BLK_SZ_SHIFT)
	    > (msgq->size - 1)) {
		dev_err(dev, "Blk size %d exceed max queue size allowed %d\n",
			entry->h2vk_blks, msgq->size - 1);
		rc = -EOVERFLOW;
		goto bcm_vk_write_free_ent;
	}

	/* Use internal message id */
	entry->usr_msg_id = BCM_VK_GET_MSG_ID(&entry->h2vk_msg[0]);
	rc = bcm_vk_get_msg_id(vk);
	if (rc == VK_MSG_ID_OVERFLOW) {
		dev_err(dev, "msg_id overflow\n");
		rc = -EOVERFLOW;
		goto bcm_vk_write_free_ent;
	}
	BCM_VK_SET_MSG_ID(&entry->h2vk_msg[0], rc);

	dev_dbg(dev,
		"Message ctx id %d, usr_msg_id 0x%x sent msg_id 0x%x\n",
		ctx->idx, entry->usr_msg_id,
		BCM_VK_GET_MSG_ID(&entry->h2vk_msg[0]));

	/* Convert any pointers to sg list */
	if (entry->h2vk_msg[0].function_id == VK_FID_TRANS_BUF) {
		unsigned int num_planes;
		int dir;
		struct _vk_data *data;

		/*
		 * check if we are in reset, if so, no buffer transfer is
		 * allowed and return error.
		 */
		if (vk->reset_pid) {
			dev_dbg(dev, "No Transfer allowed during reset, pid %d.\n",
				ctx->pid);
			rc = -EACCES;
			goto bcm_vk_write_free_msgid;
		}

		num_planes = entry->h2vk_msg[0].args[0] & VK_CMD_PLANES_MASK;
		if ((entry->h2vk_msg[0].args[0] & VK_CMD_MASK)
		    == VK_CMD_DOWNLOAD) {
			/* Memory transfer from vk device */
			dir = DMA_FROM_DEVICE;
		} else {
			/* Memory transfer to vk device */
			dir = DMA_TO_DEVICE;
		}

		/* Calculate vk_data location */
		/* Go to end of the message */
		msg_size = entry->h2vk_msg[0].size;
		if (msg_size > entry->h2vk_blks) {
			rc = -EMSGSIZE;
			goto bcm_vk_write_free_msgid;
		}

		data = (struct _vk_data *)
			&(entry->h2vk_msg[msg_size + 1]);
		/* Now back up to the start of the pointers */
		data -= num_planes;

		/* Convert user addresses to DMA SG List */
		rc = bcm_vk_sg_alloc(dev, entry->dma, dir, data, num_planes);
		if (rc)
			goto bcm_vk_write_free_msgid;

		/* try to embed inband sgl */
		sgl_extra_blks = bcm_vk_append_ib_sgl(vk, entry, data,
						      num_planes);
		entry->h2vk_blks += sgl_extra_blks;
		entry->h2vk_msg[0].size += sgl_extra_blks;
	}

	/*
	 * store wk ent to pending queue until a response is got. This needs to
	 * be done before enqueuing the message
	 */
	bcm_vk_append_pendq(&vk->h2vk_msg_chan, q_num, entry);

	rc = bcm_h2vk_msg_enqueue(vk, entry);
	if (rc) {
		dev_err(dev, "Fail to enqueue msg to h2vk queue\n");

		/* remove message from pending list */
		entry = bcm_vk_find_pending(
				vk, &vk->h2vk_msg_chan, q_num,
				BCM_VK_GET_MSG_ID(&entry->h2vk_msg[0]));
		goto bcm_vk_write_free_ent;
	}

	return count;

bcm_vk_write_free_msgid:
	bcm_vk_msgid_bitmap_clear(vk,
				  BCM_VK_GET_MSG_ID(&entry->h2vk_msg[0]), 1);
bcm_vk_write_free_ent:
	kfree(entry);
bcm_vk_write_err:
	return rc;
}

int bcm_vk_release(struct inode *inode, struct file *p_file)
{
	int ret;
	struct bcm_vk_ctx *ctx = p_file->private_data;
	struct bcm_vk *vk = container_of(ctx->miscdev, struct bcm_vk, miscdev);
	struct device *dev = &vk->pdev->dev;
	pid_t pid = ctx->pid;

	dev_dbg(dev, "Draining with context idx %d pid %d\n",
		ctx->idx, pid);

	bcm_vk_drain_all_pend(&vk->pdev->dev, &vk->h2vk_msg_chan, ctx);
	bcm_vk_drain_all_pend(&vk->pdev->dev, &vk->vk2h_msg_chan, ctx);

	ret = bcm_vk_free_ctx(vk, ctx);
	if (ret == 0)
		ret = bcm_vk_handle_last_sess(vk, pid);

	/* free memory if it is the last reference */
	kref_put(&vk->kref, bcm_vk_release_data);

	return ret;
}

int bcm_vk_msg_init(struct bcm_vk *vk)
{
	struct device *dev = &vk->pdev->dev;
	int err = 0;

	if (bcm_vk_data_init(vk)) {
		dev_err(dev, "Error initializing internal data structures\n");
		err = -EINVAL;
		goto err_out;
	}

	if (bcm_vk_msg_chan_init(&vk->h2vk_msg_chan) ||
	    bcm_vk_msg_chan_init(&vk->vk2h_msg_chan)) {
		dev_err(dev, "Error initializing communication channel\n");
		err = -EIO;
		goto err_out;
	}

	/* create dedicated workqueue */
	vk->wq_thread = create_singlethread_workqueue(vk->miscdev.name);
	if (!vk->wq_thread) {
		dev_err(dev, "Fail to create workqueue thread\n");
		err = -ENOMEM;
		goto err_out;
	}

	/* read msgq info */
	if (bcm_vk_sync_msgq(vk, false)) {
		dev_err(dev, "Error reading comm msg Q info\n");
		err = -EIO;
		goto err_out;
	}

err_out:
	return err;
}

void bcm_vk_msg_remove(struct bcm_vk *vk)
{
	bcm_vk_blk_drv_access(vk);

	/* drain all pending items */
	bcm_vk_drain_all_pend(&vk->pdev->dev, &vk->h2vk_msg_chan, NULL);
	bcm_vk_drain_all_pend(&vk->pdev->dev, &vk->vk2h_msg_chan, NULL);
}

void bcm_vk_trigger_reset(struct bcm_vk *vk)
{
	uint32_t i;
	u32 value;

	/* clean up before pressing the door bell */
	bcm_vk_drain_all_pend(&vk->pdev->dev, &vk->h2vk_msg_chan, NULL);
	bcm_vk_drain_all_pend(&vk->pdev->dev, &vk->vk2h_msg_chan, NULL);
	vkwrite32(vk, 0, BAR_1, VK_BAR1_MSGQ_DEF_RDY);
	/* make tag '\0' terminated */
	vkwrite32(vk, 0, BAR_1, VK_BAR1_BOOT1_VER_TAG);

	for (i = 0; i < VK_BAR1_DAUTH_MAX; i++) {
		vkwrite32(vk, 0, BAR_1, VK_BAR1_DAUTH_STORE_ADDR(i));
		vkwrite32(vk, 0, BAR_1, VK_BAR1_DAUTH_VALID_ADDR(i));
	}
	for (i = 0; i < VK_BAR1_SOTP_REVID_MAX; i++)
		vkwrite32(vk, 0, BAR_1, VK_BAR1_SOTP_REVID_ADDR(i));

	memset(&vk->card_info, 0, sizeof(vk->card_info));
	memset(&vk->alert_cnts, 0, sizeof(vk->alert_cnts));

	/*
	 * When boot request fails, the CODE_PUSH_OFFSET stays persistent.
	 * Allowing us to debug the failure. When we call reset,
	 * we should clear CODE_PUSH_OFFSET so ROM does not execute
	 * boot again (and fails again) and instead waits for a new
	 * codepush.
	 */
	value = vkread32(vk, BAR_0, BAR_CODEPUSH_SBL);
	value &= ~CODEPUSH_MASK;
	vkwrite32(vk, value, BAR_0, BAR_CODEPUSH_SBL);

	/* reset fw_status with proper reason, and press db */
	vkwrite32(vk, VK_FWSTS_RESET_MBOX_DB, BAR_0, VK_BAR_FWSTS);
	bcm_h2vk_doorbell(vk, VK_BAR0_RESET_DB_NUM, VK_BAR0_RESET_DB_SOFT);

	/* clear the uptime register after reset pressed and alert record */
	vkwrite32(vk, 0, BAR_0, BAR_OS_UPTIME);
	memset(&vk->host_alert, 0, sizeof(vk->host_alert));
	memset(&vk->peer_alert, 0, sizeof(vk->peer_alert));
#if defined(CONFIG_BCM_VK_QSTATS)
	/* clear qstats */
	for (i = 0; i < VK_MSGQ_MAX_NR; i++) {
		memset(&vk->h2vk_msg_chan.qstats[i].qcnts, 0,
		       sizeof(vk->h2vk_msg_chan.qstats[i].qcnts));
		memset(&vk->vk2h_msg_chan.qstats[i].qcnts, 0,
		       sizeof(vk->vk2h_msg_chan.qstats[i].qcnts));
	}
#endif
	/* clear 4096 bits of bitmap */
	bitmap_clear(vk->bmap, 0, VK_MSG_ID_BITMAP_SIZE);
}
