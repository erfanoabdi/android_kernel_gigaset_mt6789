// SPDX-License-Identifier: GPL-2.0
//
// Copyright (c) 2019 MediaTek Inc.

#include <linux/clk.h>
#include <linux/component.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/vmalloc.h>
#include <linux/jiffies.h>

#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-subdev.h>

#include "mtk_cam.h"
#include "mtk_cam_pm.h"
#include "mtk_cam-pool.h"
#include "mtk_cam-mraw-regs.h"
#include "mtk_cam-mraw.h"
#include "mtk_cam-meta.h"
#include "mtk_camera-v4l2-controls.h"
#include "mtk_camera-videodev2.h"

#define MTK_MRAW_STOP_HW_TIMEOUT			(33 * USEC_PER_MSEC)


static const struct of_device_id mtk_mraw_of_ids[] = {
	{.compatible = "mediatek,mraw",},
	{}
};
MODULE_DEVICE_TABLE(of, mtk_mraw_of_ids);

#define MAX_NUM_MRAW_CLOCKS 5
#define LARB25_PORT_SIZE 12
#define LARB26_PORT_SIZE 13

static const struct v4l2_mbus_framefmt mraw_mfmt_default = {
	.code = MEDIA_BUS_FMT_SBGGR10_1X10,
	.width = DEFAULT_WIDTH,
	.height = DEFAULT_HEIGHT,
	.field = V4L2_FIELD_NONE,
	.colorspace = V4L2_COLORSPACE_SRGB,
	.xfer_func = V4L2_XFER_FUNC_DEFAULT,
	.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT,
	.quantization = V4L2_QUANTIZATION_DEFAULT,
};

static int mtk_mraw_sd_subscribe_event(struct v4l2_subdev *subdev,
				      struct v4l2_fh *fh,
				      struct v4l2_event_subscription *sub)
{
	switch (sub->type) {
	case V4L2_EVENT_FRAME_SYNC:
		return v4l2_event_subscribe(fh, sub, 0, NULL);
	case V4L2_EVENT_REQUEST_DRAINED:
		return v4l2_event_subscribe(fh, sub, 0, NULL);
	default:
		return -EINVAL;
	}
}

int mtk_cam_mraw_select(struct mtk_mraw_pipeline *pipe)
{
	pipe->enabled_mraw = 1 << (pipe->id - MTKCAM_SUBDEV_MRAW_START);

	return 0;
}

static int mtk_mraw_sd_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct mtk_mraw_pipeline *pipe =
		container_of(sd, struct mtk_mraw_pipeline, subdev);
	struct mtk_mraw *mraw = pipe->mraw;
	struct mtk_cam_device *cam = dev_get_drvdata(mraw->cam_dev);
	struct mtk_cam_ctx *ctx = mtk_cam_find_ctx(cam, &sd->entity);
	unsigned int i;

	if (WARN_ON(!ctx))
		return -EINVAL;

	if (enable) {
		pipe->enabled_dmas = 0;
		if (ctx->used_mraw_num < MAX_MRAW_PIPES_PER_STREAM)
			ctx->mraw_pipe[ctx->used_mraw_num++] = pipe;
		else
			dev_info(mraw->cam_dev,
				"un-expected used mraw number:%d\n", ctx->used_mraw_num);

		for (i = 0; i < ARRAY_SIZE(pipe->vdev_nodes); i++) {
			if (!pipe->vdev_nodes[i].enabled)
				continue;
			pipe->enabled_dmas |= (1ULL << pipe->vdev_nodes[i].desc.dma_port);
		}
	} else {
		pipe->enabled_mraw = 0;
		pipe->enabled_dmas = 0;
	}

	dev_info(mraw->cam_dev, "%s:mraw-%d: en %d, dev 0x%x dmas %llu\n",
		 __func__, pipe->id-MTKCAM_SUBDEV_MRAW_START, enable, pipe->enabled_mraw,
		 pipe->enabled_dmas);

	return 0;
}

static int mtk_mraw_init_cfg(struct v4l2_subdev *sd,
		struct v4l2_subdev_pad_config *cfg)
{
	struct v4l2_mbus_framefmt *mf;
	unsigned int i;
	struct mtk_mraw_pipeline *pipe =
		container_of(sd, struct mtk_mraw_pipeline, subdev);
	struct mtk_mraw *mraw = pipe->mraw;

	for (i = 0; i < sd->entity.num_pads; i++) {
		mf = v4l2_subdev_get_try_format(sd, cfg, i);
		*mf = mraw_mfmt_default;
		pipe->cfg[i].mbus_fmt = mraw_mfmt_default;

		dev_dbg(mraw->cam_dev, "%s init pad:%d format:0x%x\n",
			sd->name, i, mf->code);
	}

	return 0;
}

static int mtk_mraw_try_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_format *fmt)
{
	struct mtk_mraw_pipeline *pipe =
		container_of(sd, struct mtk_mraw_pipeline, subdev);
	struct mtk_mraw *mraw = pipe->mraw;
	unsigned int sensor_fmt = mtk_cam_get_sensor_fmt(fmt->format.code);

	dev_dbg(mraw->cam_dev, "%s try format 0x%x, w:%d, h:%d field:%d\n",
		sd->name, fmt->format.code, fmt->format.width,
		fmt->format.height, fmt->format.field);

	/* check sensor format */
	if (!sensor_fmt || fmt->pad == MTK_MRAW_SINK)
		return sensor_fmt;
	else if (fmt->pad < MTK_MRAW_PIPELINE_PADS_NUM) {
		/* check vdev node format */
		unsigned int img_fmt, i;
		struct mtk_cam_video_device *node =
			&pipe->vdev_nodes[fmt->pad - MTK_MRAW_SINK_NUM];

		dev_dbg(mraw->cam_dev, "node:%s num_fmts:%d",
				node->desc.name, node->desc.num_fmts);
		for (i = 0; i < node->desc.num_fmts; i++) {
			img_fmt = mtk_cam_get_img_fmt(
				node->desc.fmts[i].vfmt.fmt.pix_mp.pixelformat);
			dev_dbg(mraw->cam_dev,
				"try format sensor_fmt 0x%x img_fmt 0x%x",
				sensor_fmt, img_fmt);
			if (sensor_fmt == img_fmt)
				return img_fmt;
		}
	}

	return MTKCAM_IPI_IMG_FMT_UNKNOWN;
}

static struct v4l2_mbus_framefmt *get_mraw_fmt(struct mtk_mraw_pipeline *pipe,
					  struct v4l2_subdev_pad_config *cfg,
					  unsigned int padid, int which)
{
	/* format invalid and return default format */
	if (which == V4L2_SUBDEV_FORMAT_TRY)
		return v4l2_subdev_get_try_format(&pipe->subdev, cfg, padid);

	if (WARN_ON(padid >= pipe->subdev.entity.num_pads))
		return &pipe->cfg[0].mbus_fmt;

	return &pipe->cfg[padid].mbus_fmt;
}

static int mtk_mraw_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_pad_config *cfg,
			  struct v4l2_subdev_format *fmt)
{
	struct mtk_mraw_pipeline *pipe =
		container_of(sd, struct mtk_mraw_pipeline, subdev);
	struct mtk_mraw *mraw = pipe->mraw;
	struct v4l2_mbus_framefmt *mf;
	unsigned int ipi_fmt;

	if (!sd || !cfg) {
		dev_dbg(mraw->cam_dev, "%s: Required sd(%p), cfg(%p)\n",
			__func__, sd, cfg);
		return -EINVAL;
	}

	if (!mtk_mraw_try_fmt(sd, fmt)) {
		mf = get_mraw_fmt(pipe, cfg, fmt->pad, fmt->which);
		fmt->format = *mf;
	} else {
		mf = get_mraw_fmt(pipe, cfg, fmt->pad, fmt->which);
		*mf = fmt->format;
		dev_info(mraw->cam_dev,
			"sd:%s pad:%d set format w/h/code %d/%d/0x%x\n",
			sd->name, fmt->pad, mf->width, mf->height, mf->code);

		if (fmt->pad == MTK_MRAW_SINK) {
			dev_info(mraw->cam_dev,
				"%s:Set res_config tg param:%d\n", __func__);
			/* set cfg buffer for tg/crp info. */
			ipi_fmt = mtk_cam_get_sensor_fmt(fmt->format.code);
			if (ipi_fmt == MTKCAM_IPI_IMG_FMT_UNKNOWN) {
				dev_info(mraw->cam_dev, "%s:Unknown pixelfmt:%d\n"
					, __func__, ipi_fmt);
			}
			pipe->res_config.width = mf->width;
			pipe->res_config.height = mf->height;
			pipe->res_config.img_fmt = ipi_fmt;
		}
	}

	return 0;
}

static int mtk_mraw_get_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_pad_config *cfg,
			  struct v4l2_subdev_format *fmt)
{
	struct mtk_mraw_pipeline *pipe =
		container_of(sd, struct mtk_mraw_pipeline, subdev);
	struct mtk_mraw *mraw = pipe->mraw;
	struct v4l2_mbus_framefmt *mf;

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY)
		mf = v4l2_subdev_get_try_format(sd, cfg, fmt->pad);
	else {
		if (WARN_ON(fmt->pad >= sd->entity.num_pads))
			mf = &pipe->cfg[0].mbus_fmt;
		else
			mf = &pipe->cfg[fmt->pad].mbus_fmt;
	}

	fmt->format = *mf;
	dev_dbg(mraw->cam_dev, "sd:%s pad:%d get format 0x%x\n",
		sd->name, fmt->pad, fmt->format.code);

	return 0;
}

static int mtk_mraw_media_link_setup(struct media_entity *entity,
				    const struct media_pad *local,
				    const struct media_pad *remote, u32 flags)
{
	struct mtk_mraw_pipeline *pipe =
		container_of(entity, struct mtk_mraw_pipeline, subdev.entity);
	struct mtk_mraw *mraw = pipe->mraw;
	u32 pad = local->index;

	dev_info(mraw->cam_dev, "%s: mraw %d: %d->%d flags:0x%x\n",
		__func__, pipe->id, remote->index, local->index, flags);

	if (pad == MTK_MRAW_SINK)
		pipe->seninf_padidx = remote->index;

	if (pad < MTK_MRAW_PIPELINE_PADS_NUM && pad != MTK_MRAW_SINK)
		pipe->vdev_nodes[pad-MTK_MRAW_SINK_NUM].enabled =
			!!(flags & MEDIA_LNK_FL_ENABLED);

	if (!(flags & MEDIA_LNK_FL_ENABLED))
		memset(pipe->cfg, 0, sizeof(pipe->cfg));

	return 0;
}

static const struct v4l2_subdev_core_ops mtk_mraw_subdev_core_ops = {
	.subscribe_event = mtk_mraw_sd_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_video_ops mtk_mraw_subdev_video_ops = {
	.s_stream =  mtk_mraw_sd_s_stream,
};

static const struct v4l2_subdev_pad_ops mtk_mraw_subdev_pad_ops = {
	.link_validate = v4l2_subdev_link_validate_default,
	.init_cfg = mtk_mraw_init_cfg,
	.set_fmt = mtk_mraw_set_fmt,
	.get_fmt = mtk_mraw_get_fmt,
};

static const struct v4l2_subdev_ops mtk_mraw_subdev_ops = {
	.core = &mtk_mraw_subdev_core_ops,
	.video = &mtk_mraw_subdev_video_ops,
	.pad = &mtk_mraw_subdev_pad_ops,
};

static const struct media_entity_operations mtk_mraw_media_entity_ops = {
	.link_setup = mtk_mraw_media_link_setup,
	.link_validate = v4l2_subdev_link_validate,
};

static const struct v4l2_ioctl_ops mtk_mraw_v4l2_meta_cap_ioctl_ops = {
	.vidioc_querycap = mtk_cam_vidioc_querycap,
	.vidioc_enum_fmt_meta_cap = mtk_cam_vidioc_meta_enum_fmt,
	.vidioc_g_fmt_meta_cap = mtk_cam_vidioc_g_meta_fmt,
	.vidioc_s_fmt_meta_cap = mtk_cam_vidioc_g_meta_fmt,
	.vidioc_try_fmt_meta_cap = mtk_cam_vidioc_g_meta_fmt,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
	.vidioc_expbuf = vb2_ioctl_expbuf,
};

static const struct v4l2_ioctl_ops mtk_mraw_v4l2_meta_out_ioctl_ops = {
	.vidioc_querycap = mtk_cam_vidioc_querycap,
	.vidioc_enum_fmt_meta_out = mtk_cam_vidioc_meta_enum_fmt,
	.vidioc_g_fmt_meta_out = mtk_cam_vidioc_g_meta_fmt,
	.vidioc_s_fmt_meta_out = mtk_cam_vidioc_g_meta_fmt,
	.vidioc_try_fmt_meta_out = mtk_cam_vidioc_g_meta_fmt,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
	.vidioc_expbuf = vb2_ioctl_expbuf,
};

static const struct mtk_cam_format_desc meta_fmts[] = { /* FIXME for ISP6 meta format */
	{
		.vfmt.fmt.meta = {
			.dataformat = V4L2_META_FMT_MTISP_PARAMS,
			.buffersize = MRAW_STATS_CFG_SIZE,
		},
	},
	{
		.vfmt.fmt.meta = {
			.dataformat = V4L2_META_FMT_MTISP_PARAMS,
			.buffersize = MRAW_STATS_0_SIZE,
		},
	},
};

#define  MTK_MRAW_TOTAL_OUTPUT_QUEUES 1

static const struct
mtk_cam_dev_node_desc mraw_output_queues[] = {
	{
		.id = MTK_MRAW_META_IN,
		.name = "meta input",
		.cap = V4L2_CAP_META_OUTPUT,
		.buf_type = V4L2_BUF_TYPE_META_OUTPUT,
		.link_flags = MEDIA_LNK_FL_ENABLED |  MEDIA_LNK_FL_IMMUTABLE,
		.image = false,
#ifdef CONFIG_MTK_SCP
		.smem_alloc = true,
#else
		.smem_alloc = false,
#endif
		.dma_port = MTKCAM_IPI_MRAW_META_STATS_CFG,
		.fmts = meta_fmts,
		.default_fmt_idx = 0,
		.max_buf_count = 16,
		.ioctl_ops = &mtk_mraw_v4l2_meta_out_ioctl_ops,
	},
};

static const char *mraw_output_queue_names[MRAW_PIPELINE_NUM]
	[MTK_MRAW_TOTAL_OUTPUT_QUEUES] = {
	{"mtk-cam mraw-0 meta-input"},
	{"mtk-cam mraw-1 meta-input"},
	{"mtk-cam mraw-2 meta-input"},
	{"mtk-cam mraw-3 meta-input"}
};

#define MTK_MRAW_TOTAL_CAPTURE_QUEUES 1

static const struct
mtk_cam_dev_node_desc mraw_capture_queues[] = {
	{
		.id = MTK_MRAW_META_OUT,
		.name = "meta output",
		.cap = V4L2_CAP_META_CAPTURE,
		.buf_type = V4L2_BUF_TYPE_META_CAPTURE,
		.link_flags = MEDIA_LNK_FL_ENABLED |  MEDIA_LNK_FL_IMMUTABLE,
		.image = false,
		.smem_alloc = false,
		.dma_port = MTKCAM_IPI_MRAW_META_STATS_0,
		.fmts = meta_fmts,
		.default_fmt_idx = 1,
		.max_buf_count = 16,
		.ioctl_ops = &mtk_mraw_v4l2_meta_cap_ioctl_ops,
	}
};

static const char *mraw_capture_queue_names[MRAW_PIPELINE_NUM]
	[MTK_MRAW_TOTAL_CAPTURE_QUEUES] = {
	{"mtk-cam mraw-0 partial-meta-0"},
	{"mtk-cam mraw-1 partial-meta-0"},
	{"mtk-cam mraw-2 partial-meta-0"},
	{"mtk-cam mraw-3 partial-meta-0"}
};

void apply_mraw_cq(struct mtk_mraw_device *dev,
	      dma_addr_t cq_addr, unsigned int cq_size, unsigned int cq_offset,
	      int initial)
{
#define CQ_VADDR_MASK 0xffffffff
	u32 cq_addr_lsb = (cq_addr + cq_offset) & CQ_VADDR_MASK;
	u32 cq_addr_msb = ((cq_addr + cq_offset) >> 32);

	dev_info(dev->dev,
		"apply mraw%d cq - addr:0x%llx ,size:%d,offset:%d, REG_MRAW_CQ_THR0_CTL:0x%8x\n",
		dev->id, cq_addr, cq_size, cq_offset,
		readl_relaxed(dev->base + REG_MRAW_CQ_THR0_CTL));

	writel_relaxed(cq_addr_lsb, dev->base + REG_MRAW_CQ_THR0_BASEADDR);
	writel_relaxed(cq_addr_msb, dev->base + REG_MRAW_CQ_THR0_BASEADDR_MSB);
	writel_relaxed(cq_size, dev->base + REG_MRAW_CQ_THR0_DESC_SIZE);

	wmb(); /* TBC */
	if (initial) {
		writel_relaxed(MRAWCTL_CQ_THR0_DONE_ST,
			       dev->base + REG_MRAW_CTL_INT6_EN);
		writel_relaxed(MRAWCTL_CQ_THR0_START,
			       dev->base + REG_MRAW_CTL_START);
		wmb(); /* TBC */
	} else {
#if USING_MRAW_SCQ
		writel_relaxed(MRAWCTL_CQ_THR0_START, dev->base + REG_MRAW_CTL_START);
		wmb(); /* TBC */
#endif
	}
	dev_info(dev->dev,
		"apply mraw%d scq - addr/size = [main] 0x%llx/%d\n",
		dev->id, cq_addr, cq_size);
}

unsigned int mtk_cam_mraw_powi(unsigned int x, unsigned int n)
{
	unsigned int rst = 1.0;
	unsigned int m = (n >= 0) ? n : -n;

	while (m--)
		rst *= x;

	return (n >= 0) ? rst : 1.0 / rst;
}

unsigned int mtk_cam_mraw_xsize_cal(unsigned int length)
{
	return length * 16 / 8;
}

unsigned int mtk_cam_mraw_xsize_cal_cpio(unsigned int length)
{
	return (length + 7) / 8;
}

void mtk_cam_mraw_set_mraw_dmao_info(
	struct device *dev,
	struct mtk_cam_mraw_resource_config *res_config,
	struct mtk_cam_mraw_dmao_info *mraw_dmao_info,
	unsigned int tg_width, unsigned int tg_height)
{
	unsigned int tg_width_temp = tg_width;
	unsigned int tg_height_temp = tg_height;
	int i;
	/* cal. for imgo & imgbo */
	if (res_config->mqe_en) {
		switch (res_config->mqe_mode) {
		case UL_MODE:
		case UR_MODE:
		case DL_MODE:
		case DR_MODE:
			tg_width_temp /= 2;
			tg_height_temp /= 2;
			break;
		case PD_L_MODE:
		case PD_R_MODE:
		case PD_M_MODE:
		case PD_B01_MODE:
		case PD_B02_MODE:
			tg_width_temp /= 2;
			break;
		default:
			dev_info(dev, "imgo & imgbo MQE-Mode %d %s fail\n",
				res_config->mqe_mode, "unknown idx");
			return;
		}
	}

	if (res_config->mbn_pow < 2 || res_config->mbn_pow > 6) {
		dev_info(dev, "Invalid mbn_pow: %d", res_config->mbn_pow);
		return;
	}

	switch (res_config->mbn_dir) {
	case MBN_POW_VERTICAL:
		tg_height_temp /= mtk_cam_mraw_powi(2, res_config->mbn_pow);
		break;
	case MBN_POW_HORIZONTAL:
		tg_width_temp /= mtk_cam_mraw_powi(2, res_config->mbn_pow);
		break;
	default:
		dev_info(dev, "%s:imgo & imgbo MBN's dir %d %s fail",
			__func__, res_config->mbn_dir, "unknown idx");
		return;
	}

	tg_width_temp /= 2;  // divided for 2 path from MBN

	mraw_dmao_info->dmao_width[IMGO] = mtk_cam_mraw_xsize_cal(tg_width_temp);
	mraw_dmao_info->dmao_height[IMGO] = tg_height_temp;
	mraw_dmao_info->dmao_stride[IMGO] = mraw_dmao_info->dmao_width[IMGO];

	mraw_dmao_info->dmao_width[IMGBO] = mtk_cam_mraw_xsize_cal(tg_width_temp);
	mraw_dmao_info->dmao_height[IMGBO] = tg_height_temp;
	mraw_dmao_info->dmao_stride[IMGBO] = mraw_dmao_info->dmao_width[IMGBO];

	/* cal. for cpio */
	tg_width_temp = tg_width;
	tg_height_temp = tg_height;

	if (res_config->mqe_en) {
		switch (res_config->mqe_mode) {
		case UL_MODE:
		case UR_MODE:
		case DL_MODE:
		case DR_MODE:
			tg_width_temp /= 2;
			tg_height_temp /= 2;
			break;
		case PD_L_MODE:
		case PD_R_MODE:
		case PD_M_MODE:
		case PD_B01_MODE:
		case PD_B02_MODE:
			tg_width_temp /= 2;
			break;
		default:
			dev_info(dev, "%s:imgo & imgbo MQE-Mode %d %s fail\n",
				__func__, res_config->mqe_mode, "unknown idx");
			return;
		}
	}

	if (res_config->cpi_pow < 2 || res_config->cpi_pow > 6) {
		dev_info(dev, "Invalid cpi_pow: %d", res_config->cpi_pow);
		return;
	}


	switch (res_config->cpi_dir) {
	case CPI_POW_VERTICAL:
		tg_height_temp /= mtk_cam_mraw_powi(2, res_config->cpi_pow);
		break;
	case CPI_POW_HORIZONTAL:
		tg_width_temp /= mtk_cam_mraw_powi(2, res_config->cpi_pow);
		break;
	default:
		dev_info(dev, "cpio CPI's dir %d %s fail"
			, res_config->cpi_dir, "unknown idx");
		return;
	}

	mraw_dmao_info->dmao_width[CPIO] = mtk_cam_mraw_xsize_cal_cpio(tg_width_temp);
	mraw_dmao_info->dmao_height[CPIO] = tg_height_temp;
	mraw_dmao_info->dmao_stride[CPIO] = mraw_dmao_info->dmao_width[CPIO];

	for (i = DMAO_ID_BEGIN; i < DMAO_ID_MAX; i++) {
		dev_info(dev, "dma_id:%d, w:%d s:%d stride:%d\n",
			i,
			mraw_dmao_info->dmao_width[i],
			mraw_dmao_info->dmao_height[i],
			mraw_dmao_info->dmao_stride[i]
			);
	}
}

void mtk_cam_mraw_copy_user_input_param(
	struct mtk_cam_uapi_meta_mraw_stats_cfg *mraw_meta_in_buf,
	struct mtkcam_ipi_frame_param *frame_param,
	struct mtk_mraw_pipeline *mraw_pipline, int mraw_param_num)
{
	/* Set tg/crp param from kernel info. */
	frame_param->mraw_param[mraw_param_num].tg_pos_x = 0;
	frame_param->mraw_param[mraw_param_num].tg_pos_y = 0;
	frame_param->mraw_param[mraw_param_num].tg_size_w
		= mraw_pipline->res_config.width;
	frame_param->mraw_param[mraw_param_num].tg_size_h
		= mraw_pipline->res_config.height;
	frame_param->mraw_param[mraw_param_num].tg_fmt
		= mraw_pipline->res_config.img_fmt;
	frame_param->mraw_param[mraw_param_num].crop_pos_x = 0;
	frame_param->mraw_param[mraw_param_num].crop_pos_y = 0;
	frame_param->mraw_param[mraw_param_num].crop_size_w
		= mraw_pipline->res_config.width;
	frame_param->mraw_param[mraw_param_num].crop_size_h
		= mraw_pipline->res_config.height;
	/* Set mraw res_config */
	mraw_pipline->res_config.mqe_en = mraw_meta_in_buf->mqe_enable;
	mraw_pipline->res_config.mqe_mode = mraw_meta_in_buf->mqe_param.mqe_mode;
	mraw_pipline->res_config.mbn_dir = mraw_meta_in_buf->mbn_param.mbn_dir;
	mraw_pipline->res_config.mbn_pow = mraw_meta_in_buf->mbn_param.mbn_pow;
	mraw_pipline->res_config.cpi_pow = mraw_meta_in_buf->cpi_param.cpi_pow;
	mraw_pipline->res_config.cpi_dir = mraw_meta_in_buf->cpi_param.cpi_dir;
}

void mtk_cam_mraw_set_frame_param_dmao(
	struct device *dev,
	struct mtkcam_ipi_frame_param *frame_param,
	struct mtk_cam_mraw_dmao_info mraw_dmao_info, int pipe_id, int param_num,
	struct mtk_cam_buffer *buf)
{
	struct mtkcam_ipi_img_output *mraw_img_outputs, *last_mraw_img_outputs;
	int i;

	for (i = DMAO_ID_BEGIN; i < DMAO_ID_MAX; i++) {
		last_mraw_img_outputs = mraw_img_outputs;
		mraw_img_outputs = &frame_param->mraw_param[param_num].mraw_img_outputs[i];

		mraw_img_outputs->uid.id = MTKCAM_IPI_MRAW_META_STATS_0;
		mraw_img_outputs->uid.pipe_id = pipe_id;

		mraw_img_outputs->fmt.stride[0] = mraw_dmao_info.dmao_stride[i];
		mraw_img_outputs->fmt.s.w = mraw_dmao_info.dmao_width[i];
		mraw_img_outputs->fmt.s.h = mraw_dmao_info.dmao_height[i];

		mraw_img_outputs->crop.p.x = 0;
		mraw_img_outputs->crop.p.y = 0;
		mraw_img_outputs->crop.s.w = mraw_dmao_info.dmao_width[i];
		mraw_img_outputs->crop.s.h = mraw_dmao_info.dmao_height[i];

		if (i == 0)
			mraw_img_outputs->buf[0][0].iova
				= buf->daddr + sizeof(struct mtk_cam_uapi_meta_mraw_stats_0);
		else
			mraw_img_outputs->buf[0][0].iova = last_mraw_img_outputs->buf[0][0].iova
				+ last_mraw_img_outputs->fmt.stride[0] *
					last_mraw_img_outputs->fmt.s.h;

		mraw_img_outputs->buf[0][0].size = mraw_dmao_info.dmao_stride[i];

		dev_dbg(dev, "%s:dmao_id:%d iova:0x%08x\n",
			__func__, i, mraw_img_outputs->buf[0][0].iova);
	}
}

void mtk_cam_mraw_handle_enque(struct vb2_buffer *vb)
{
	struct mtk_cam_device *cam = vb2_get_drv_priv(vb->vb2_queue);
	struct mtk_cam_video_device *node = mtk_cam_vbq_to_vdev(vb->vb2_queue);
	struct mtk_cam_buffer *buf = mtk_cam_vb2_buf_to_dev_buf(vb);
	struct mtk_cam_request *req = to_mtk_cam_req(vb->request);
	void *vaddr = vb2_plane_vaddr(vb, 0);
	unsigned int pipe_id = node->uid.pipe_id;
	unsigned int dma_port = node->desc.dma_port;
	struct mtk_cam_request_stream_data *req_stream_data
		= mtk_cam_req_get_s_data(req, pipe_id, 0);
	struct device *dev = cam->dev;
	struct mtkcam_ipi_meta_input *meta_in;
	struct mtkcam_ipi_frame_param *frame_param;
	struct mtk_cam_uapi_meta_mraw_stats_cfg *mraw_meta_in_buf;
	struct mtk_mraw_pipeline *mraw_pipline;
	unsigned int width, height;
	unsigned int ipi_fmt;
	struct mtk_cam_mraw_dmao_info mraw_dmao_info;

	frame_param = &req_stream_data->frame_params;
	mraw_pipline = mtk_cam_dev_get_mraw_pipeline(cam, pipe_id);

	switch (dma_port) {
	case MTKCAM_IPI_MRAW_META_STATS_CFG:
		mraw_meta_in_buf = (struct mtk_cam_uapi_meta_mraw_stats_cfg *)vaddr;
		mraw_pipline->res_config.vaddr[MTKCAM_IPI_MRAW_META_STATS_CFG
			- MTKCAM_IPI_MRAW_ID_START] = vaddr;
		meta_in = &frame_param->mraw_param[0].mraw_meta_inputs;
		meta_in->buf.ccd_fd = vb->planes[0].m.fd;
		meta_in->buf.size = node->active_fmt.fmt.meta.buffersize;
		meta_in->buf.iova = buf->daddr;
		meta_in->uid.id = dma_port;
		meta_in->uid.pipe_id = node->uid.pipe_id;
		mtk_cam_mraw_copy_user_input_param(mraw_meta_in_buf,
			frame_param, mraw_pipline, 0);
		mraw_pipline->res_config.enque_num++;
		break;
	case MTKCAM_IPI_MRAW_META_STATS_0:
		mraw_pipline->res_config.vaddr[MTKCAM_IPI_MRAW_META_STATS_0
			- MTKCAM_IPI_MRAW_ID_START] = vaddr;
		mraw_pipline->res_config.enque_num++;
		break;
	default:
		dev_dbg(dev, "%s:pipe(%d):buffer with invalid port(%d)\n",
			__func__, pipe_id, dma_port);
		break;
	}

	if (mraw_pipline->res_config.enque_num == MAX_MRAW_VIDEO_DEV_NUM) {
		width = mraw_pipline->res_config.width;
		height = mraw_pipline->res_config.height;
		ipi_fmt = mraw_pipline->res_config.img_fmt;

		mtk_cam_mraw_set_mraw_dmao_info(cam->dev,
			&(mraw_pipline->res_config),
			&mraw_dmao_info, width, height);

		mtk_cam_mraw_set_frame_param_dmao(cam->dev, frame_param,
			mraw_dmao_info, pipe_id, 0, buf);

		mtk_cam_mraw_set_meta_stats_info(
			mraw_pipline->res_config.vaddr[MTKCAM_IPI_MRAW_META_STATS_0
				- MTKCAM_IPI_MRAW_ID_START], &mraw_dmao_info);

		mraw_pipline->res_config.enque_num = 0;
	}
}

int mtk_cam_mraw_apply_next_buffer(struct mtk_cam_ctx *ctx)
{
	struct mtk_mraw_working_buf_entry *mraw_buf_entry;
	dma_addr_t base_addr;
	struct mtk_mraw_device *mraw_dev;
	struct mtk_cam_device *cam = ctx->cam;
	struct device *dev_mraw;
	int i;

	spin_lock(&ctx->mraw_composed_buffer_list.lock);
	if (list_empty(&ctx->mraw_composed_buffer_list.list)) {
		spin_unlock(&ctx->mraw_composed_buffer_list.lock);
		return 0;
	}
	mraw_buf_entry = list_first_entry(&ctx->mraw_composed_buffer_list.list,
						struct mtk_mraw_working_buf_entry,
						list_entry);
	list_del(&mraw_buf_entry->list_entry);
	ctx->mraw_composed_buffer_list.cnt--;
	spin_unlock(&ctx->mraw_composed_buffer_list.lock);
	spin_lock(&ctx->mraw_processing_buffer_list.lock);
	list_add_tail(&mraw_buf_entry->list_entry,
				&ctx->mraw_processing_buffer_list.list);
	ctx->mraw_processing_buffer_list.cnt++;
	spin_unlock(&ctx->mraw_processing_buffer_list.lock);
	base_addr = mraw_buf_entry->buffer.iova;
	for (i = 0; i < ctx->used_mraw_num; i++) {
		dev_mraw = mtk_cam_find_mraw_dev(cam, ctx->used_mraw_dev[i]);
		mraw_dev = dev_get_drvdata(dev_mraw);
		apply_mraw_cq(mraw_dev,
			base_addr,
			mraw_buf_entry->mraw_cq_desc_size[i],
			mraw_buf_entry->mraw_cq_desc_offset[i], 1);
	}
	return 1;
}

void mraw_reset(struct mtk_mraw_device *dev)
{
	unsigned long end = jiffies + msecs_to_jiffies(100);

	dev_dbg(dev->dev, "%s\n", __func__);

	writel_relaxed(0, dev->base + REG_MRAW_CTL_SW_CTL);
	writel_relaxed(1, dev->base + REG_MRAW_CTL_SW_CTL);
	wmb(); /* TBC */

	while (time_before(jiffies, end)) {
		if (readl(dev->base + REG_MRAW_CTL_SW_CTL) & 0x2) {
			// do hw rst
			writel_relaxed(4, dev->base + REG_MRAW_CTL_SW_CTL);
			writel_relaxed(0, dev->base + REG_MRAW_CTL_SW_CTL);
			wmb(); /* TBC */
			return;
		}

		dev_info(dev->dev,
			"tg_sen_mode: 0x%x, ctl_en: 0x%x, ctl_sw_ctl:0x%x, frame_no:0x%x\n",
			readl(dev->base + REG_MRAW_TG_SEN_MODE),
			readl(dev->base + REG_MRAW_CTL_MOD_EN),
			readl(dev->base + REG_MRAW_CTL_SW_CTL),
			readl(dev->base + REG_MRAW_FRAME_SEQ_NUM)
			);
		usleep_range(10, 20);
	}

	dev_dbg(dev->dev, "reset hw timeout\n");
}

struct mtk_mraw_pipeline*
mtk_cam_dev_get_mraw_pipeline(struct mtk_cam_device *cam,
			     unsigned int pipe_id)
{
	if (pipe_id < MTKCAM_SUBDEV_MRAW_0 || pipe_id > MTKCAM_SUBDEV_MRAW_END)
		return NULL;
	else
		return &cam->mraw.pipelines[pipe_id - MTKCAM_SUBDEV_MRAW_0];
}


int mtk_cam_mraw_pipeline_config(
	struct mtk_cam_ctx *ctx, unsigned int idx)
{
	struct mtk_mraw_pipeline *mraw_pipe = ctx->mraw_pipe[idx];
	struct mtk_mraw *mraw = mraw_pipe->mraw;
	unsigned int i;
	int ret;

	/* reset pm_runtime during streaming dynamic change */
	if (ctx->streaming) {
		for (i = 0; i < ARRAY_SIZE(mraw->devs); i++)
			if (mraw_pipe->enabled_mraw & 1<<i)
				pm_runtime_put(mraw->devs[i]);
	}

	ret = mtk_cam_mraw_select(mraw_pipe);
	if (ret) {
		dev_dbg(mraw->cam_dev, "failed select raw: %d\n",
			ctx->stream_id);
		return ret;
	}

	for (i = 0; i < ARRAY_SIZE(mraw->devs); i++)
		if (mraw_pipe->enabled_mraw & 1<<i)
			ret = pm_runtime_get_sync(mraw->devs[i]);

	if (ret < 0) {
		dev_dbg(mraw->cam_dev,
			"failed at pm_runtime_get_sync: %s\n",
			dev_driver_string(mraw->devs[i]));
		for (i = i-1; i >= 0; i--)
			if (mraw_pipe->enabled_mraw & 1<<i)
				pm_runtime_put(mraw->devs[i]);
		return ret;
	}

	ctx->used_mraw_dev[idx] = mraw_pipe->enabled_mraw;
	dev_info(mraw->cam_dev, "ctx_id %d used_mraw_dev %d pipe_id %d\n",
		ctx->stream_id, ctx->used_mraw_dev[idx], mraw_pipe->id);
	return 0;
}


struct device *mtk_cam_find_mraw_dev(struct mtk_cam_device *cam
	, unsigned int mraw_mask)
{
	unsigned int i;

	for (i = 0; i < cam->num_mraw_drivers; i++) {
		if (mraw_mask & (1 << i))
			return cam->mraw.devs[i];
	}

	return NULL;
}

unsigned int mtk_cam_mraw_format_sel(unsigned int pixel_fmt)
{
	union MRAW_FMT_SEL fmt;

	fmt.Raw = 0;
	fmt.Bits.MRAWCTL_TG_SWAP = MRAW_TG_SW_UYVY;

	switch (pixel_fmt) {
	case V4L2_PIX_FMT_SBGGR8:
	case V4L2_PIX_FMT_SGBRG8:
	case V4L2_PIX_FMT_SGRBG8:
	case V4L2_PIX_FMT_SRGGB8:
		fmt.Bits.MRAWCTL_TG_FMT = MRAW_TG_FMT_RAW8;
		break;
	case V4L2_PIX_FMT_MTISP_SBGGR10:
	case V4L2_PIX_FMT_MTISP_SGBRG10:
	case V4L2_PIX_FMT_MTISP_SGRBG10:
	case V4L2_PIX_FMT_MTISP_SRGGB10:
		fmt.Bits.MRAWCTL_TG_FMT = MRAW_TG_FMT_RAW10;
		break;
	case V4L2_PIX_FMT_MTISP_SBGGR12:
	case V4L2_PIX_FMT_MTISP_SGBRG12:
	case V4L2_PIX_FMT_MTISP_SGRBG12:
	case V4L2_PIX_FMT_MTISP_SRGGB12:
		fmt.Bits.MRAWCTL_TG_FMT = MRAW_TG_FMT_RAW12;
		break;
	case V4L2_PIX_FMT_MTISP_SBGGR14:
	case V4L2_PIX_FMT_MTISP_SGBRG14:
	case V4L2_PIX_FMT_MTISP_SGRBG14:
	case V4L2_PIX_FMT_MTISP_SRGGB14:
		fmt.Bits.MRAWCTL_TG_FMT = MRAW_TG_FMT_RAW14;
		break;
	default:
		break;
	}

	return fmt.Raw;
}

static void set_payload(struct mtk_cam_uapi_meta_hw_buf *buf,
			unsigned int size, unsigned long *offset)
{
	buf->offset = *offset;
	buf->size = size;
	*offset += size;
}

void mtk_cam_mraw_set_meta_stats_info(
	void *vaddr, struct mtk_cam_mraw_dmao_info *mraw_dmao_info)
{
	struct mtk_cam_uapi_meta_mraw_stats_0 *mraw_stats0;
	unsigned long offset;
	unsigned int size;

	mraw_stats0 = (struct mtk_cam_uapi_meta_mraw_stats_0 *)vaddr;
	offset = sizeof(*mraw_stats0);
	/* imgo */
	size = mraw_dmao_info->dmao_stride[IMGO] * mraw_dmao_info->dmao_height[IMGO];
	set_payload(&mraw_stats0->pdp_0_stats.pdo_buf, size, &offset);
	mraw_stats0->pdp_0_stats_enabled = 1;
	mraw_stats0->pdp_0_stats.stats_src.width = mraw_dmao_info->dmao_width[IMGO];
	mraw_stats0->pdp_0_stats.stats_src.height = mraw_dmao_info->dmao_height[IMGO];
	mraw_stats0->pdp_0_stats.stride = mraw_dmao_info->dmao_stride[IMGO];
	/* imgbo */
	size = mraw_dmao_info->dmao_stride[IMGBO] * mraw_dmao_info->dmao_height[IMGBO];
	set_payload(&mraw_stats0->pdp_1_stats.pdo_buf, size, &offset);
	mraw_stats0->pdp_1_stats_enabled = 1;
	mraw_stats0->pdp_1_stats.stats_src.width = mraw_dmao_info->dmao_width[IMGBO];
	mraw_stats0->pdp_1_stats.stats_src.height = mraw_dmao_info->dmao_height[IMGBO];
	mraw_stats0->pdp_1_stats.stride = mraw_dmao_info->dmao_stride[IMGBO];
	/* cpio */
	size = mraw_dmao_info->dmao_stride[CPIO] * mraw_dmao_info->dmao_height[CPIO];
	set_payload(&mraw_stats0->cpi_stats.cpio_buf, size, &offset);
	mraw_stats0->cpi_stats_enabled = 1;
	mraw_stats0->cpi_stats.stats_src.width = mraw_dmao_info->dmao_width[CPIO];
	mraw_stats0->cpi_stats.stats_src.height = mraw_dmao_info->dmao_height[CPIO];
	mraw_stats0->cpi_stats.stride = mraw_dmao_info->dmao_stride[CPIO];
}

int mtk_cam_mraw_top_config(struct mtk_mraw_device *dev)
{
	int ret = 0;

	unsigned int int_en1 = (
							MRAW_INT_EN1_VS_INT_EN |
							MRAW_INT_EN1_TG_ERR_EN |
							MRAW_INT_EN1_TG_GBERR_EN |
							MRAW_INT_EN1_TG_SOF_INT_EN |
							MRAW_INT_EN1_CQ_CODE_ERR_EN |
							MRAW_INT_EN1_CQ_DB_LOAD_ERR_EN |
							MRAW_INT_EN1_CQ_VS_ERR_EN |
							MRAW_INT_EN1_CQ_TRIG_DLY_INT_EN |
							MRAW_INT_EN1_SW_PASS1_DONE_EN |
							MRAW_INT_EN1_DMA_ERR_EN
							);

	unsigned int int_en5 = (
							MRAW_INT_EN5_IMGO_M1_ERR_EN |
							MRAW_INT_EN5_IMGBO_M1_ERR_EN |
							MRAW_INT_EN5_CPIO_M1_ERR_EN
							);

	/* int en */
	MRAW_WRITE_REG(dev->base + REG_MRAW_CTL_INT_EN, int_en1);
	MRAW_WRITE_REG(dev->base + REG_MRAW_CTL_INT5_EN, int_en5);

	MRAW_WRITE_BITS(dev->base + REG_MRAW_M_MRAWCTL_MISC,
		MRAW_CTL_MISC, MRAWCTL_DB_EN, 0);

	MRAW_WRITE_BITS(dev->base + REG_MRAW_M_MRAWCTL_MISC,
		MRAW_CTL_MISC, MRAWCTL_DB_LOAD_SRC, MRAW_DB_SRC_SOF);

	return ret;
}

int mtk_cam_mraw_dma_config(struct mtk_mraw_device *dev)
{
	int ret = 0;

	/* imgo con */
	MRAW_WRITE_REG(dev->base + REG_MRAW_M_IMGO_ORIWDMA_CON0,
		0x10000280);  // BURST_LEN and FIFO_SIZE
	MRAW_WRITE_REG(dev->base + REG_MRAW_M_IMGO_ORIWDMA_CON1,
		0x00A00150);  // Threshold for pre-ultra
	MRAW_WRITE_REG(dev->base + REG_MRAW_M_IMGO_ORIWDMA_CON2,
		0x014000F0);  // Threshold for ultra
	MRAW_WRITE_REG(dev->base + REG_MRAW_M_IMGO_ORIWDMA_CON3,
		0x01AB049E);  // Threshold for urgent
	MRAW_WRITE_REG(dev->base + REG_MRAW_M_IMGO_ORIWDMA_CON4,
		0x00500000);  // Threshold for DVFS

	/* imgbo con */
	MRAW_WRITE_REG(dev->base + REG_MRAW_M_IMGBO_ORIWDMA_CON0,
		0x10000280);  // BURST_LEN and FIFO_SIZE
	MRAW_WRITE_REG(dev->base + REG_MRAW_M_IMGBO_ORIWDMA_CON1,
		0x00A00150);  // Threshold for pre-ultra
	MRAW_WRITE_REG(dev->base + REG_MRAW_M_IMGBO_ORIWDMA_CON2,
		0x014000F0);  // Threshold for ultra
	MRAW_WRITE_REG(dev->base + REG_MRAW_M_IMGBO_ORIWDMA_CON3,
		0x01AB049E);  // Threshold for urgent
	MRAW_WRITE_REG(dev->base + REG_MRAW_M_IMGBO_ORIWDMA_CON4,
		0x00500000);  // Threshold for DVFS

	/* cpio con */
	MRAW_WRITE_REG(dev->base + REG_MRAW_M_CPIO_ORIWDMA_CON0,
		0x10000100);  // BURST_LEN and FIFO_SIZE
	MRAW_WRITE_REG(dev->base + REG_MRAW_M_CPIO_ORIWDMA_CON1,
		0x00400020);  // Threshold for pre-ultra
	MRAW_WRITE_REG(dev->base + REG_MRAW_M_CPIO_ORIWDMA_CON2,
		0x00800060);  // Threshold for ultra
	MRAW_WRITE_REG(dev->base + REG_MRAW_M_CPIO_ORIWDMA_CON3,
		0x018001D9);  // Threshold for urgent
	MRAW_WRITE_REG(dev->base + REG_MRAW_M_CPIO_ORIWDMA_CON4,
		0x00200000);  // Threshold for DVFS

	/* lsci con */
	MRAW_WRITE_REG(dev->base + REG_MRAW_M_LSCI_ORIRDMA_CON0,
		0x10000080);  // BURST_LEN and FIFO_SIZE
	MRAW_WRITE_REG(dev->base + REG_MRAW_M_LSCI_ORIRDMA_CON1,
		0x00200010);  // Threshold for pre-ultra
	MRAW_WRITE_REG(dev->base + REG_MRAW_M_LSCI_ORIRDMA_CON2,
		0x00400030);  // Threshold for ultra
	MRAW_WRITE_REG(dev->base + REG_MRAW_M_LSCI_ORIRDMA_CON3,
		0x00560046);  // Threshold for urgent
	MRAW_WRITE_REG(dev->base + REG_MRAW_M_LSCI_ORIRDMA_CON4,
		0x00100000);  // Threshold for DVFS

	/* cqi con */
	MRAW_WRITE_REG(dev->base + REG_MRAW_M1_CQI_ORIRDMA_CON0,
		0x10000040);  // BURST_LEN and FIFO_SIZE
	MRAW_WRITE_REG(dev->base + REG_MRAW_M1_CQI_ORIRDMA_CON1,
		0x00100008);  // Threshold for pre-ultra
	MRAW_WRITE_REG(dev->base + REG_MRAW_M1_CQI_ORIRDMA_CON2,
		0x00200018);  // Threshold for ultra
	MRAW_WRITE_REG(dev->base + REG_MRAW_M1_CQI_ORIRDMA_CON3,
		0x002B0023);  // Threshold for urgent
	MRAW_WRITE_REG(dev->base + REG_MRAW_M1_CQI_ORIRDMA_CON4,
		0x00080000);  // Threshold for DVFS
	return ret;
}

int mtk_cam_mraw_fbc_config(
	struct mtk_mraw_device *dev)
{
	int ret = 0;

	MRAW_WRITE_REG(dev->base + REG_MRAW_FBC_IMGO_CTL1, 0);
	MRAW_WRITE_REG(dev->base + REG_MRAW_FBC_IMGBO_CTL1, 0);
	MRAW_WRITE_REG(dev->base + REG_MRAW_FBC_CPIO_CTL1, 0);

	MRAW_WRITE_BITS(dev->base + REG_MRAW_MRAWCTL_FBC_GROUP,
		MRAW_MRAWCTL_FBC_GROUP, MRAWCTL_IMGO_M1_FBC_SEL, 1);
	MRAW_WRITE_BITS(dev->base + REG_MRAW_MRAWCTL_FBC_GROUP,
		MRAW_MRAWCTL_FBC_GROUP, MRAWCTL_IMGBO_M1_FBC_SEL, 1);
	MRAW_WRITE_BITS(dev->base + REG_MRAW_MRAWCTL_FBC_GROUP,
		MRAW_MRAWCTL_FBC_GROUP, MRAWCTL_CPIO_M1_FBC_SEL, 1);
	return ret;
}


int mtk_cam_mraw_cq_disable(struct mtk_mraw_device *dev)
{
	int ret = 0;

	writel_relaxed(~CQ_THR0_EN, dev->base + REG_MRAW_CQ_THR0_CTL);
	wmb(); /* TBC */

	return ret;
}

int mtk_cam_mraw_top_enable(struct mtk_mraw_device *dev)
{
	int ret = 0;

	//  toggle db
	MRAW_WRITE_BITS(dev->base + REG_MRAW_M_MRAWCTL_MISC,
		MRAW_CTL_MISC, MRAWCTL_DB_EN, 0);
	MRAW_WRITE_BITS(dev->base + REG_MRAW_M_MRAWCTL_MISC,
		MRAW_CTL_MISC, MRAWCTL_DB_EN, 1);

	//  toggle mraw vf
	MRAW_WRITE_BITS(dev->base + REG_MRAW_TG_PATH_CFG,
		MRAW_TG_PATH_CFG, TG_DB_LOAD_DIS, 1);
	MRAW_WRITE_BITS(dev->base + REG_MRAW_TG_PATH_CFG,
		MRAW_TG_PATH_CFG, TG_DB_LOAD_DIS, 0);

	/* Enable CMOS */
	dev_info(dev->dev, "%s: enable CMOS and VF\n", __func__);
	MRAW_WRITE_BITS(dev->base + REG_MRAW_TG_SEN_MODE,
		MRAW_TG_SEN_MODE, TG_CMOS_EN, 1);

	/* Enable VF */
	if (MRAW_READ_BITS(dev->base + REG_MRAW_TG_SEN_MODE,
		MRAW_TG_SEN_MODE, TG_CMOS_EN))
		MRAW_WRITE_BITS(dev->base + REG_MRAW_TG_VF_CON,
			MRAW_TG_VF_CON, TG_VFDATA_EN, 1);
	else
		dev_info(dev->dev, "%s, CMOS is off\n", __func__);

	return ret;
}

int mtk_cam_mraw_fbc_enable(struct mtk_mraw_device *dev)
{
	int ret = 0;

	if (MRAW_READ_BITS(dev->base + REG_MRAW_TG_VF_CON,
			MRAW_TG_VF_CON, TG_VFDATA_EN) == 1) {
		ret = -1;
		dev_dbg(dev->dev, "cannot enable fbc when streaming");
		goto EXIT;
	}
	MRAW_WRITE_BITS(dev->base + REG_MRAW_FBC_IMGO_CTL1,
		MRAW_FBC_IMGO_CTL1, FBC_IMGO_FBC_EN, 1);

	MRAW_WRITE_BITS(dev->base + REG_MRAW_FBC_IMGBO_CTL1,
		MRAW_FBC_IMGBO_CTL1, FBC_IMGBO_FBC_EN, 1);

	MRAW_WRITE_BITS(dev->base + REG_MRAW_FBC_CPIO_CTL1,
		MRAW_FBC_CPIO_CTL1, FBC_CPIO_FBC_EN, 1);

	MRAW_WRITE_BITS(dev->base + REG_MRAW_FBC_IMGO_CTL1,
		MRAW_FBC_IMGO_CTL1, FBC_IMGO_FBC_DB_EN, 0);
	MRAW_WRITE_BITS(dev->base + REG_MRAW_FBC_IMGBO_CTL1,
		MRAW_FBC_IMGBO_CTL1, FBC_IMGBO_FBC_DB_EN, 0);
	MRAW_WRITE_BITS(dev->base + REG_MRAW_FBC_CPIO_CTL1,
		MRAW_FBC_CPIO_CTL1, FBC_CPIO_FBC_DB_EN, 0);


EXIT:
	return ret;
}

int mtk_cam_mraw_cq_config(struct mtk_mraw_device *dev)
{
	int ret = 0;
#if USING_MRAW_SCQ
	u32 val;

	val = readl_relaxed(dev->base + REG_MRAW_CQ_EN);
	writel_relaxed(val | SCQ_EN, dev->base + REG_MRAW_CQ_EN);

	writel_relaxed(0xffffffff, dev->base + REG_MRAW_SCQ_START_PERIOD);
	wmb(); /* TBC */
#endif
	writel_relaxed(CQ_THR0_MODE_IMMEDIATE | CQ_THR0_EN,
		       dev->base + REG_MRAW_CQ_THR0_CTL);
	writel_relaxed(MRAWCTL_CQ_THR0_DONE_ST,
		       dev->base + REG_MRAW_CTL_INT6_EN);
	wmb(); /* TBC */

	dev->sof_count = 0;

	dev_info(dev->dev, "%s - REG_CQ_EN:0x%x ,REG_CQ_THR0_CTL:0x%8x\n",
		__func__,
			readl_relaxed(dev->base + REG_MRAW_CQ_EN),
			readl_relaxed(dev->base + REG_MRAW_CQ_THR0_CTL));

	return ret;
}

int mtk_cam_mraw_cq_enable(struct mtk_cam_ctx *ctx,
	struct mtk_mraw_device *dev)
{
	int ret = 0;
	u32 val;
#if USING_MRAW_SCQ
	val = readl_relaxed(dev->base + REG_MRAW_TG_TIME_STAMP_CNT);

	//[todo]: implement/check the start period
	writel_relaxed(SCQ_DEADLINE_MS * 1000 * SCQ_DEFAULT_CLK_RATE
		, dev->base + REG_MRAW_SCQ_START_PERIOD);
#else
	writel_relaxed(CQ_THR0_MODE_CONTINUOUS | CQ_THR0_EN,
				dev->base + REG_MRAW_CQ_THR0_CTL);

	writel_relaxed(CQ_DB_EN | CQ_DB_LOAD_MODE,
				dev->base + REG_MRAW_CQ_EN);
	wmb(); /* TBC */

	dev_info(dev->dev, "%s - REG_CQ_EN:0x%x ,REG_CQ_THR0_CTL:0x%8x\n",
		__func__,
			readl_relaxed(dev->base + REG_MRAW_CQ_EN),
			readl_relaxed(dev->base + REG_MRAW_CQ_THR0_CTL));
#endif
	return ret;
}

int mtk_cam_mraw_tg_disable(struct mtk_mraw_device *dev)
{
	int ret = 0;
	u32 val;

	dev_dbg(dev->dev, "stream off, disable CMOS\n");
	val = readl(dev->base + REG_MRAW_TG_SEN_MODE);
	writel(val & (~MRAW_TG_SEN_MODE_CMOS_EN),
		dev->base + REG_MRAW_TG_SEN_MODE);

	return ret;
}

int mtk_cam_mraw_top_disable(struct mtk_mraw_device *dev)
{
	int ret = 0;

	if (MRAW_READ_BITS(dev->base + REG_MRAW_TG_VF_CON,
			MRAW_TG_VF_CON, TG_VFDATA_EN)) {
		MRAW_WRITE_BITS(dev->base + REG_MRAW_TG_VF_CON,
			MRAW_TG_VF_CON, TG_VFDATA_EN, 0);
		mraw_reset(dev);
	}

	MRAW_WRITE_BITS(dev->base + REG_MRAW_M_MRAWCTL_MISC,
		MRAW_CTL_MISC, MRAWCTL_DB_EN, 0);
	MRAW_WRITE_REG(dev->base + REG_MRAW_M_MRAWCTL_MISC, 0);
	MRAW_WRITE_REG(dev->base + REG_MRAW_MRAWCTL_FMT_SEL, 0);
	MRAW_WRITE_REG(dev->base + REG_MRAW_CTL_INT_EN, 0);
	MRAW_WRITE_REG(dev->base + REG_MRAW_CTL_INT5_EN, 0);
	MRAW_WRITE_REG(dev->base + REG_MRAW_FBC_IMGO_CTL1, 0);
	MRAW_WRITE_REG(dev->base + REG_MRAW_FBC_IMGBO_CTL1, 0);
	MRAW_WRITE_REG(dev->base + REG_MRAW_FBC_CPIO_CTL1, 0);

	MRAW_WRITE_BITS(dev->base + REG_MRAW_M_MRAWCTL_MISC,
		MRAW_CTL_MISC, MRAWCTL_DB_EN, 1);
	return ret;
}

int mtk_cam_mraw_dma_disable(struct mtk_mraw_device *dev)
{
	int ret = 0;

	MRAW_WRITE_BITS(dev->base + REG_MRAW_CTL_MOD2_EN,
		MRAW_CTL_MOD2_EN, MRAWCTL_IMGO_M1_EN, 0);
	MRAW_WRITE_BITS(dev->base + REG_MRAW_CTL_MOD2_EN,
		MRAW_CTL_MOD2_EN, MRAWCTL_IMGBO_M1_EN, 0);
	MRAW_WRITE_BITS(dev->base + REG_MRAW_CTL_MOD2_EN,
		MRAW_CTL_MOD2_EN, MRAWCTL_CPIO_M1_EN, 0);
	MRAW_WRITE_BITS(dev->base + REG_MRAW_CTL_MOD2_EN,
		MRAW_CTL_MOD2_EN, MRAWCTL_LSCI_M1_EN, 0);

	return ret;
}

int mtk_cam_mraw_fbc_disable(struct mtk_mraw_device *dev)
{
	int ret = 0;

	MRAW_WRITE_REG(dev->base + REG_MRAW_FBC_IMGO_CTL1, 0);
	MRAW_WRITE_REG(dev->base + REG_MRAW_FBC_IMGBO_CTL1, 0);
	MRAW_WRITE_REG(dev->base + REG_MRAW_FBC_CPIO_CTL1, 0);

	MRAW_WRITE_BITS(dev->base + REG_MRAW_MRAWCTL_FBC_GROUP,
		MRAW_MRAWCTL_FBC_GROUP, MRAWCTL_IMGO_M1_FBC_SEL, 0);
	MRAW_WRITE_BITS(dev->base + REG_MRAW_MRAWCTL_FBC_GROUP,
		MRAW_MRAWCTL_FBC_GROUP, MRAWCTL_IMGBO_M1_FBC_SEL, 0);
	MRAW_WRITE_BITS(dev->base + REG_MRAW_MRAWCTL_FBC_GROUP,
		MRAW_MRAWCTL_FBC_GROUP, MRAWCTL_CPIO_M1_FBC_SEL, 0);

	return ret;
}

int mtk_cam_find_mraw_dev_index(
	struct mtk_cam_ctx *ctx,
	unsigned int idx)
{
	int i;

	for (i = 0 ; i < MAX_MRAW_PIPES_PER_STREAM ; i++) {
		if (ctx->used_mraw_dev[i] == (1 << idx))
			return i;
	}

	return -1;
}

int mtk_cam_mraw_dev_config(
	struct mtk_cam_ctx *ctx,
	unsigned int idx,
	unsigned int stag_en)
{
	struct mtk_cam_device *cam = ctx->cam;
	struct device *dev = cam->dev;
	struct v4l2_mbus_framefmt *mf;
	struct device *dev_mraw;
	struct mtk_mraw_device *mraw_dev;
	struct v4l2_format *img_fmt;
	unsigned int i;
	int ret, pad_idx = 0;

	dev_dbg(dev, "%s\n", __func__);

	img_fmt = &ctx->mraw_pipe[idx]
		->vdev_nodes[MTK_MRAW_META_OUT-MTK_MRAW_SINK_NUM].active_fmt;
	pad_idx = ctx->mraw_pipe[idx]->seninf_padidx;
	mf = &ctx->mraw_pipe[idx]->cfg[MTK_MRAW_SINK].mbus_fmt;

	ret = mtk_cam_mraw_pipeline_config(ctx, idx);
	if (ret)
		return ret;

	dev_mraw = mtk_cam_find_mraw_dev(cam, ctx->used_mraw_dev[idx]);
	if (dev_mraw == NULL) {
		dev_dbg(dev, "config mraw device not found\n");
		return -EINVAL;
	}
	mraw_dev = dev_get_drvdata(dev_mraw);
	for (i = 0; i < MRAW_PIPELINE_NUM; i++)
		if (cam->mraw.pipelines[i].enabled_mraw & 1<<mraw_dev->id) {
			mraw_dev->pipeline = &cam->mraw.pipelines[i];
			break;
		}

	mtk_cam_mraw_top_config(mraw_dev);
	mtk_cam_mraw_dma_config(mraw_dev);
	mtk_cam_mraw_fbc_config(mraw_dev);
	mtk_cam_mraw_fbc_enable(mraw_dev);
	mtk_cam_mraw_cq_config(mraw_dev);

	dev_info(dev, "mraw %d %s done\n", mraw_dev->id, __func__);

	return 0;
}

int mtk_cam_mraw_dev_stream_on(
	struct mtk_cam_ctx *ctx,
	unsigned int idx,
	unsigned int streaming,
	unsigned int stag_en)
{
	struct mtk_cam_device *cam = ctx->cam;
	struct device *dev = cam->dev;
	struct device *dev_mraw;
	struct mtk_mraw_device *mraw_dev;
	unsigned int i;
	int ret = 0;

	dev_info(dev, "%s\n", __func__);

	dev_mraw = mtk_cam_find_mraw_dev(cam, ctx->used_mraw_dev[idx]);
	if (dev_mraw == NULL) {
		dev_dbg(dev, "stream on mraw device not found\n");
		return -EINVAL;
	}
	mraw_dev = dev_get_drvdata(dev_mraw);
	for (i = 0; i < MRAW_PIPELINE_NUM; i++)
		if (cam->mraw.pipelines[i].enabled_mraw & 1<<mraw_dev->id) {
			mraw_dev->pipeline = &cam->mraw.pipelines[i];
			break;
		}

	if (streaming)
		ret = mtk_cam_mraw_cq_enable(ctx, mraw_dev) ||
			mtk_cam_mraw_top_enable(mraw_dev);
	else {
		ret = mtk_cam_mraw_cq_disable(mraw_dev) ||
			mtk_cam_mraw_top_disable(mraw_dev) ||
			mtk_cam_mraw_fbc_disable(mraw_dev) ||
			mtk_cam_mraw_dma_disable(mraw_dev) ||
			mtk_cam_mraw_tg_disable(mraw_dev);

		pm_runtime_put(mraw_dev->dev);
	}

	dev_info(dev, "mraw %d %s en(%d)\n", mraw_dev->id, __func__, streaming);

	return ret;
}

static void mtk_mraw_pipeline_queue_setup(
	struct mtk_mraw_pipeline *pipe)
{
	unsigned int node_idx, i;

	node_idx = 0;

	/* Setup the output queue */
	for (i = 0; i < MTK_MRAW_TOTAL_OUTPUT_QUEUES; i++) {
		pipe->vdev_nodes[node_idx].desc = mraw_output_queues[i];
		pipe->vdev_nodes[node_idx++].desc.name =
			mraw_output_queue_names[pipe->id-MTKCAM_SUBDEV_MRAW_START][i];
	}
	/* Setup the capture queue */
	for (i = 0; i < MTK_MRAW_TOTAL_CAPTURE_QUEUES; i++) {
		pipe->vdev_nodes[node_idx].desc = mraw_capture_queues[i];
		pipe->vdev_nodes[node_idx++].desc.name =
			mraw_capture_queue_names[pipe->id-MTKCAM_SUBDEV_MRAW_START][i];
	}
}

static int  mtk_mraw_pipeline_register(
	unsigned int id, struct device *dev,
	struct mtk_mraw_pipeline *pipe,
	struct v4l2_device *v4l2_dev)
{
	struct mtk_cam_device *cam = dev_get_drvdata(pipe->mraw->cam_dev);
	struct mtk_mraw_device *mraw_dev = dev_get_drvdata(dev);
	struct v4l2_subdev *sd = &pipe->subdev;
	struct mtk_cam_video_device *video;
	unsigned int i;
	int ret;

	pipe->id = id;
	pipe->cammux_id = mraw_dev->cammux_id;

	/* Initialize subdev */
	v4l2_subdev_init(sd, &mtk_mraw_subdev_ops);
	sd->entity.function = MEDIA_ENT_F_PROC_VIDEO_PIXEL_FORMATTER;
	sd->entity.ops = &mtk_mraw_media_entity_ops;
	sd->flags = V4L2_SUBDEV_FL_HAS_DEVNODE | V4L2_SUBDEV_FL_HAS_EVENTS;
	ret = snprintf(sd->name, sizeof(sd->name),
		 "%s-%d", dev_driver_string(dev), (pipe->id-MTKCAM_SUBDEV_MRAW_START));
	if (ret < 0) {
		dev_info(dev, "Failed to compose device name: %d\n", ret);
		return ret;
	}
	v4l2_set_subdevdata(sd, pipe);

	dev_info(dev, "%s: %s\n", __func__, sd->name);

	ret = v4l2_device_register_subdev(v4l2_dev, sd);
	if (ret < 0) {
		dev_info(dev, "Failed to register subdev: %d\n", ret);
		return ret;
	}

	mtk_mraw_pipeline_queue_setup(pipe);

	//setup pads of mraw pipeline
	for (i = 0; i < ARRAY_SIZE(pipe->pads); i++) {
		pipe->pads[i].flags = i < MTK_MRAW_SOURCE_BEGIN ?
			MEDIA_PAD_FL_SINK : MEDIA_PAD_FL_SOURCE;
	}

	media_entity_pads_init(&sd->entity, ARRAY_SIZE(pipe->pads), pipe->pads);

	/* setup video node */
	for (i = 0; i < ARRAY_SIZE(pipe->vdev_nodes); i++) {
		video = pipe->vdev_nodes + i;

		switch (pipe->id) {
		case MTKCAM_SUBDEV_MRAW_0:
		case MTKCAM_SUBDEV_MRAW_1:
		case MTKCAM_SUBDEV_MRAW_2:
		case MTKCAM_SUBDEV_MRAW_3:
			video->uid.pipe_id = pipe->id;
			break;
		default:
			dev_dbg(dev, "invalid pipe id\n");
			return -EINVAL;
		}

		video->uid.id = video->desc.dma_port;
		video->ctx = &cam->ctxs[id];
		ret = mtk_cam_video_register(video, v4l2_dev);
		if (ret)
			goto fail_unregister_video;

		if (V4L2_TYPE_IS_OUTPUT(video->desc.buf_type))
			ret = media_create_pad_link(&video->vdev.entity, 0,
						    &sd->entity,
						    video->desc.id,
						    video->desc.link_flags);
		else
			ret = media_create_pad_link(&sd->entity,
						    video->desc.id,
						    &video->vdev.entity, 0,
						    video->desc.link_flags);

		if (ret)
			goto fail_unregister_video;
	}

	return 0;

fail_unregister_video:
	for (i = i-1; i >= 0; i--)
		mtk_cam_video_unregister(pipe->vdev_nodes + i);

	return ret;
}

static void mtk_mraw_pipeline_unregister(
	struct mtk_mraw_pipeline *pipe)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(pipe->vdev_nodes); i++)
		mtk_cam_video_unregister(pipe->vdev_nodes + i);

	v4l2_device_unregister_subdev(&pipe->subdev);
	media_entity_cleanup(&pipe->subdev.entity);
}

int mtk_mraw_setup_dependencies(struct mtk_mraw *mraw, struct mtk_larb *larb)
{
	struct device *dev = mraw->cam_dev;
	struct device *consumer, *supplier;
	struct device_link *link;
	int i;

	for (i = 0; i < MRAW_PIPELINE_NUM; i++) {
		consumer = mraw->devs[i];
		if (!consumer) {
			dev_info(dev, "failed to get dev for id %d\n", i);
			continue;
		}

		switch (i) {
		case 0:
		case 2:
			supplier = find_larb(larb, 25);
			break;
		default:
			supplier = find_larb(larb, 26);
			break;
		}

		if (!supplier) {
			dev_info(dev, "failed to get supplier for id %d\n", i);
			return -ENODEV;
		}

		link = device_link_add(consumer, supplier,
				       DL_FLAG_AUTOREMOVE_CONSUMER |
				       DL_FLAG_PM_RUNTIME);
		if (!link) {
			dev_info(dev, "Unable to create link between %s and %s\n",
				dev_name(consumer), dev_name(supplier));
			return -ENODEV;
		}
	}

	return 0;
}

int mtk_mraw_register_entities(
	struct mtk_mraw *mraw,
	struct v4l2_device *v4l2_dev)
{
	unsigned int i;
	int ret;

	for (i = 0; i < MRAW_PIPELINE_NUM; i++) {
		struct mtk_mraw_pipeline *pipe = mraw->pipelines + i;

		pipe->mraw = mraw;
		memset(pipe->cfg, 0, sizeof(*pipe->cfg));
		ret = mtk_mraw_pipeline_register(MTKCAM_SUBDEV_MRAW_START + i,
						mraw->devs[i],
						mraw->pipelines + i, v4l2_dev);
		if (ret)
			return ret;
	}
	return 0;
}

void mtk_mraw_unregister_entities(struct mtk_mraw *mraw)
{
	unsigned int i;

	for (i = 0; i < MRAW_PIPELINE_NUM; i++)
		mtk_mraw_pipeline_unregister(mraw->pipelines + i);
}

void mraw_irq_handle_tg_grab_err(
	struct mtk_mraw_device *mraw_dev)
{
	int val, val2;

	val = readl_relaxed(mraw_dev->base + REG_MRAW_TG_PATH_CFG);
	val = val|MRAW_TG_PATH_TG_FULL_SEL;
	writel_relaxed(val, mraw_dev->base + REG_MRAW_TG_PATH_CFG);
	wmb(); /* TBC */
	val2 = readl_relaxed(mraw_dev->base + REG_MRAW_TG_SEN_MODE);
	val2 = val2|MRAW_TG_CMOS_RDY_SEL;
	writel_relaxed(val2, mraw_dev->base + REG_MRAW_TG_SEN_MODE);
	wmb(); /* TBC */
	dev_dbg_ratelimited(mraw_dev->dev,
		"TG PATHCFG/SENMODE/FRMSIZE/RGRABPXL/LIN:%x/%x/%x/%x/%x/%x\n",
		readl_relaxed(mraw_dev->base + REG_MRAW_TG_PATH_CFG),
		readl_relaxed(mraw_dev->base + REG_MRAW_TG_SEN_MODE),
		readl_relaxed(mraw_dev->base + REG_MRAW_TG_FRMSIZE_ST),
		readl_relaxed(mraw_dev->base + REG_MRAW_TG_FRMSIZE_ST_R),
		readl_relaxed(mraw_dev->base + REG_MRAW_TG_SEN_GRAB_PXL),
		readl_relaxed(mraw_dev->base + REG_MRAW_TG_SEN_GRAB_LIN));
}

void mraw_irq_handle_dma_err(struct mtk_mraw_device *mraw_dev)
{
	dev_dbg_ratelimited(mraw_dev->dev,
		"IMGO:0x%x\n",
		readl_relaxed(mraw_dev->base + REG_MRAW_IMGO_ERR_STAT));

	dev_dbg_ratelimited(mraw_dev->dev,
		"IMGBO:0x%x\n",
		readl_relaxed(mraw_dev->base + REG_MRAW_IMGBO_ERR_STAT));

	dev_dbg_ratelimited(mraw_dev->dev,
		"CPIO:0x%x\n",
		readl_relaxed(mraw_dev->base + REG_MRAW_CPIO_ERR_STAT));
}

static void mraw_irq_handle_tg_overrun_err(struct mtk_mraw_device *mraw_dev)
{
	dev_info(mraw_dev->dev,
			 "TG PATHCFG/SENMODE FRMSIZE/R GRABPXL/LIN:%x/%x %x/%x %x/%x\n",
			 readl_relaxed(mraw_dev->base + REG_MRAW_TG_PATH_CFG),
			 readl_relaxed(mraw_dev->base + REG_MRAW_TG_SEN_MODE),
			 readl_relaxed(mraw_dev->base + REG_MRAW_TG_FRMSIZE_ST),
			 readl_relaxed(mraw_dev->base + REG_MRAW_TG_FRMSIZE_ST_R),
			 readl_relaxed(mraw_dev->base + REG_MRAW_TG_SEN_GRAB_PXL),
			 readl_relaxed(mraw_dev->base + REG_MRAW_TG_SEN_GRAB_LIN));
}

static irqreturn_t mtk_irq_mraw(int irq, void *data)
{
	struct mtk_mraw_device *mraw_dev = (struct mtk_mraw_device *)data;
	struct device *dev = mraw_dev->dev;
	struct mtk_camsys_irq_info irq_info;
	unsigned int dequeued_imgo_seq_no, dequeued_imgo_seq_no_inner;
	unsigned int tg_timestamp;
	unsigned int irq_status, irq_status5, irq_status6, err_status;
	unsigned int irq_status2, irq_status3, irq_status4;
	unsigned int dma_err_status;
	unsigned int imgo_overr_status, imgbo_overr_status, cpio_overr_status;
	unsigned int fbc_imgo_status, imgo_addr;
	unsigned int fbc_imgbo_status, imgbo_addr;
	unsigned int fbc_cpio_status, cpio_addr;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&mraw_dev->spinlock_irq, flags);
	irq_status	= readl_relaxed(mraw_dev->base + REG_MRAW_CTL_INT_STATUS);

	/*
	 * [ISP 7.0/7.1] HW Bug Workaround: read MRAWCTL_INT2_STATUS every irq
	 * Because MRAWCTL_INT2_EN is attach to OTF_OVER_FLOW ENABLE incorrectly
	 */
	irq_status2	= readl_relaxed(mraw_dev->base + REG_MRAW_CTL_INT2_STATUS);
	irq_status3	= readl_relaxed(mraw_dev->base + REG_MRAW_CTL_INT3_STATUS);
	irq_status4	= readl_relaxed(mraw_dev->base + REG_MRAW_CTL_INT4_STATUS);

	irq_status5 = readl_relaxed(mraw_dev->base + REG_MRAW_CTL_INT5_STATUS);
	irq_status6	= readl_relaxed(mraw_dev->base + REG_MRAW_CTL_INT6_STATUS);
	tg_timestamp = readl_relaxed(mraw_dev->base + REG_MRAW_TG_TIME_STAMP);
	dequeued_imgo_seq_no =
		readl_relaxed(mraw_dev->base + REG_MRAW_FRAME_SEQ_NUM);
	dequeued_imgo_seq_no_inner =
		readl_relaxed(mraw_dev->base_inner + REG_MRAW_FRAME_SEQ_NUM);

	fbc_imgo_status =
		readl_relaxed(mraw_dev->base + REG_MRAW_FBC_IMGO_CTL2);
	imgo_addr =
		readl_relaxed(mraw_dev->base + REG_MRAW_IMGO_BASE_ADDR);

	fbc_imgbo_status =
		readl_relaxed(mraw_dev->base + REG_MRAW_FBC_IMGBO_CTL2);
	imgbo_addr =
		readl_relaxed(mraw_dev->base + REG_MRAW_IMGBO_BASE_ADDR);

	fbc_cpio_status =
		readl_relaxed(mraw_dev->base + REG_MRAW_FBC_CPIO_CTL2);
	cpio_addr =
		readl_relaxed(mraw_dev->base + REG_MRAW_CPIO_BASE_ADDR);
	spin_unlock_irqrestore(&mraw_dev->spinlock_irq, flags);

	err_status = irq_status & INT_ST_MASK_MRAW_ERR;
	dma_err_status = irq_status & MRAWCTL_DMA_ERR_ST;
	imgo_overr_status = irq_status5 & MRAWCTL_IMGO_M1_OTF_OVERFLOW_ST;
	imgbo_overr_status = irq_status5 & MRAWCTL_IMGBO_M1_OTF_OVERFLOW_ST;
	cpio_overr_status = irq_status5 & MRAWCTL_CPIO_M1_OTF_OVERFLOW_ST;

	dev_info(dev,
		"%i status:0x%x(err:0x%x) dma_err:0x%x fbc_status(imgo:0x%x, imgbo:0x%x, cpio:0x%x) dma_addr(imgo:0x%x, imgbo:0x%x, cpio:0x%x)\n",
		mraw_dev->id, irq_status, err_status, dma_err_status,
		fbc_imgo_status, fbc_imgbo_status, fbc_cpio_status,
		imgo_addr, imgbo_addr, cpio_addr);

	dev_dbg(dev,
		"%i imgo_overr_status:0x%x, imgbo_overr_status:0x%x, cpio_overr_status:0x%x\n",
		mraw_dev->id, imgo_overr_status, imgbo_overr_status, cpio_overr_status);

	/*
	 * In normal case, the next SOF ISR should come after HW PASS1 DONE ISR.
	 * If these two ISRs come together, print warning msg to hint.
	 */
	irq_info.engine_id = CAMSYS_ENGINE_MRAW_BEGIN + mraw_dev->id;
	irq_info.frame_idx = dequeued_imgo_seq_no;
	irq_info.frame_inner_idx = dequeued_imgo_seq_no_inner;
	irq_info.irq_type = 0;
	irq_info.slave_engine = 0;
	if ((irq_status & MRAWCTL_SOF_INT_ST) &&
		(irq_status & MRAWCTL_PASS1_DONE_ST))
		dev_dbg(dev, "sof_done block cnt:%d\n", mraw_dev->sof_count);
	/* Frame done */
	if (irq_status & MRAWCTL_SW_PASS1_DONE_ST) {
		irq_info.irq_type |= 1<<CAMSYS_IRQ_FRAME_DONE;
		dev_dbg(dev, "p1_done sof_cnt:%d\n", mraw_dev->sof_count);
	}
	/* Frame start */
	if (irq_status & MRAWCTL_SOF_INT_ST) {
		irq_info.irq_type |= 1<<CAMSYS_IRQ_FRAME_START;
		mraw_dev->sof_count++;
		dev_dbg(dev, "sof block cnt:%d\n", mraw_dev->sof_count);
	}
	/* CQ done */
	if (irq_status6 & MRAWCTL_CQ_THR0_DONE_ST) {
		irq_info.irq_type |= 1<<CAMSYS_IRQ_SETTING_DONE;
		dev_dbg(dev, "CQ done:%d\n", mraw_dev->sof_count);
	}
	/* inform interrupt information to camsys controller */
	if ((irq_status & MRAWCTL_SOF_INT_ST) ||
		(irq_status & MRAWCTL_SW_PASS1_DONE_ST) ||
		(irq_status6 & MRAWCTL_CQ_THR0_DONE_ST)) {
		ret = mtk_camsys_isr_event(mraw_dev->cam, &irq_info);
		if (ret)
			goto ctx_not_found;
	}

	/* Check ISP error status */
	if (err_status) {
		dev_info(dev, "int_err:0x%x 0x%x\n", irq_status, err_status);
		/* Show DMA errors in detail */
		if (err_status & DMA_ST_MASK_MRAW_ERR)
			mraw_irq_handle_dma_err(mraw_dev);
		/* Show TG register for more error detail*/
		if (err_status & MRAWCTL_TG_GBERR_ST)
			mraw_irq_handle_tg_grab_err(mraw_dev);
		if (err_status & MRAWCTL_TG_ERR_ST)
			mraw_irq_handle_tg_overrun_err(mraw_dev);
	}
ctx_not_found:

	return IRQ_HANDLED;
}

bool
mtk_cam_mraw_finish_buf(struct mtk_cam_request_stream_data *req_stream_data)
{
	bool result = false;
	struct mtk_mraw_working_buf_entry *mraw_buf_entry, *mraw_buf_entry_prev;
	struct mtk_cam_ctx *ctx = req_stream_data->ctx;

	if (!ctx->used_mraw_num)
		return false;

	spin_lock(&ctx->mraw_processing_buffer_list.lock);

	list_for_each_entry_safe(mraw_buf_entry, mraw_buf_entry_prev,
				 &ctx->mraw_processing_buffer_list.list,
				 list_entry) {
		if (mraw_buf_entry->s_data->frame_seq_no
			== req_stream_data->frame_seq_no) {
			list_del(&mraw_buf_entry->list_entry);
			mtk_cam_mraw_wbuf_set_s_data(mraw_buf_entry, NULL);
			mtk_cam_mraw_working_buf_put(ctx, mraw_buf_entry);
			ctx->mraw_processing_buffer_list.cnt--;
			result = true;
			break;
		}
	}
	spin_unlock(&ctx->mraw_processing_buffer_list.lock);

	dev_dbg(ctx->cam->dev, "put mraw bufs, %s\n",
		req_stream_data->req->req.debug_str);

	return result;
}

static int mtk_mraw_of_probe(struct platform_device *pdev,
			    struct mtk_mraw_device *mraw)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	int irq, ret;
	int i;

	ret = of_property_read_u32(dev->of_node, "mediatek,mraw-id",
						       &mraw->id);
	if (ret) {
		dev_dbg(dev, "missing camid property\n");
		return ret;
	}

	ret = of_property_read_u32(dev->of_node, "mediatek,cammux-id",
						       &mraw->cammux_id);
	if (ret) {
		dev_dbg(dev, "missing cammux id property\n");
		return ret;
	}

	/* base outer register */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "base");
	if (!res) {
		dev_info(dev, "failed to get mem\n");
		return -ENODEV;
	}

	mraw->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(mraw->base)) {
		dev_dbg(dev, "failed to map register base\n");
		return PTR_ERR(mraw->base);
	}
	dev_dbg(dev, "mraw, map_addr=0x%pK\n", mraw->base);

	/* base inner register */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "inner_base");
	if (!res) {
		dev_dbg(dev, "failed to get mem\n");
		return -ENODEV;
	}

	mraw->base_inner = devm_ioremap_resource(dev, res);
	if (IS_ERR(mraw->base_inner)) {
		dev_dbg(dev, "failed to map register inner base\n");
		return PTR_ERR(mraw->base_inner);
	}
	dev_dbg(dev, "mraw, map_addr(inner)=0x%pK\n", mraw->base_inner);


	irq = platform_get_irq(pdev, 0);
	if (!irq) {
		dev_dbg(dev, "failed to get irq\n");
		return -ENODEV;
	}

	ret = devm_request_irq(dev, irq, mtk_irq_mraw, 0,
				dev_name(dev), mraw);
	if (ret) {
		dev_dbg(dev, "failed to request irq=%d\n", irq);
		return ret;
	}
	dev_dbg(dev, "registered irq=%d\n", irq);

	mraw->num_clks = 0;
	mraw->num_clks = of_count_phandle_with_args(pdev->dev.of_node, "clocks",
			"#clock-cells");
	dev_info(dev, "clk_num:%d\n", mraw->num_clks);
	if (!mraw->num_clks) {
		dev_dbg(dev, "no clock\n");
		return -ENODEV;
	}

	mraw->clks = devm_kcalloc(dev, mraw->num_clks, sizeof(*mraw->clks),
				 GFP_KERNEL);
	if (!mraw->clks)
		return -ENOMEM;

	for (i = 0; i < mraw->num_clks; i++) {
		mraw->clks[i] = of_clk_get(pdev->dev.of_node, i);
		if (IS_ERR(mraw->clks[i])) {
			dev_info(dev, "failed to get clk %d\n", i);
			return -ENODEV;
		}
	}

	return 0;
}

static int mtk_mraw_component_bind(
	struct device *dev,
	struct device *master,
	void *data)
{
	struct mtk_mraw_device *mraw_dev = dev_get_drvdata(dev);
	struct mtk_cam_device *cam_dev = data;
	struct mtk_mraw *mraw = &cam_dev->mraw;

	dev_info(dev, "%s: id=%d\n", __func__, mraw_dev->id);

	mraw_dev->cam = cam_dev;
	mraw->devs[mraw_dev->id] = dev;
	mraw->cam_dev = cam_dev->dev;

	return 0;
}

static void mtk_mraw_component_unbind(
	struct device *dev,
	struct device *master,
	void *data)
{
	struct mtk_mraw_device *mraw_dev = dev_get_drvdata(dev);
	struct mtk_cam_device *cam_dev = data;
	struct mtk_mraw *mraw = &cam_dev->mraw;

	dev_info(dev, "%s\n", __func__);

	mraw_dev->cam = NULL;
	mraw_dev->pipeline = NULL;
	mraw->devs[mraw_dev->id] = NULL;
}


static const struct component_ops mtk_mraw_component_ops = {
	.bind = mtk_mraw_component_bind,
	.unbind = mtk_mraw_component_unbind,
};

static int mtk_mraw_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_mraw_device *mraw_dev;
	int ret;

	dev_info(dev, "%s\n", __func__);

	mraw_dev = devm_kzalloc(dev, sizeof(*mraw_dev), GFP_KERNEL);
	if (!mraw_dev)
		return -ENOMEM;

	mraw_dev->dev = dev;
	dev_set_drvdata(dev, mraw_dev);

	spin_lock_init(&mraw_dev->spinlock_irq);

	ret = mtk_mraw_of_probe(pdev, mraw_dev);
	if (ret)
		return ret;

	pm_runtime_set_autosuspend_delay(dev, 2 * MTK_MRAW_STOP_HW_TIMEOUT);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_enable(dev);

	return component_add(dev, &mtk_mraw_component_ops);
}

static int mtk_mraw_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	dev_info(dev, "%s\n", __func__);

	pm_runtime_disable(dev);

	component_del(dev, &mtk_mraw_component_ops);
	return 0;
}

static int mtk_mraw_pm_suspend(struct device *dev)
{
	struct mtk_mraw_device *mraw_dev = dev_get_drvdata(dev);
	u32 val;
	int ret;

	dev_dbg(dev, "- %s\n", __func__);

	if (pm_runtime_suspended(dev))
		return 0;

	/* Disable ISP's view finder and wait for TG idle */
	dev_dbg(dev, "mraw suspend, disable VF\n");
	val = readl(mraw_dev->base + REG_MRAW_TG_VF_CON);
	writel(val & (~MRAW_TG_VF_CON_VFDATA_EN),
		mraw_dev->base + REG_MRAW_TG_VF_CON);
	ret = readl_poll_timeout_atomic(
					mraw_dev->base + REG_MRAW_TG_INTER_ST, val,
					(val & MRAW_TG_CS_MASK) == MRAW_TG_IDLE_ST,
					USEC_PER_MSEC, MTK_MRAW_STOP_HW_TIMEOUT);
	if (ret)
		dev_dbg(dev, "can't stop HW:%d:0x%x\n", ret, val);

	/* Disable CMOS */
	val = readl(mraw_dev->base + REG_MRAW_TG_SEN_MODE);
	writel(val & (~MRAW_TG_SEN_MODE_CMOS_EN),
		mraw_dev->base + REG_MRAW_TG_SEN_MODE);

	/* Force ISP HW to idle */
	ret = pm_runtime_force_suspend(dev);
	return ret;
}

static int mtk_mraw_pm_resume(struct device *dev)
{
	struct mtk_mraw_device *mraw_dev = dev_get_drvdata(dev);
	u32 val;
	int ret;

	dev_dbg(dev, "- %s\n", __func__);

	if (pm_runtime_suspended(dev))
		return 0;

	/* Force ISP HW to resume */
	ret = pm_runtime_force_resume(dev);
	if (ret)
		return ret;

	/* Enable CMOS */
	dev_dbg(dev, "mraw resume, enable CMOS/VF\n");
	val = readl(mraw_dev->base + REG_MRAW_TG_SEN_MODE);
	writel(val | MRAW_TG_SEN_MODE_CMOS_EN,
		mraw_dev->base + REG_MRAW_TG_SEN_MODE);

	/* Enable VF */
	val = readl(mraw_dev->base + REG_MRAW_TG_VF_CON);
	writel(val | MRAW_TG_VF_CON_VFDATA_EN,
		mraw_dev->base + REG_MRAW_TG_VF_CON);

	return 0;
}

static int mtk_mraw_runtime_suspend(struct device *dev)
{
	struct mtk_mraw_device *mraw_dev = dev_get_drvdata(dev);
	int i;

	dev_dbg(dev, "%s:disable clock\n", __func__);
	for (i = 0; i < mraw_dev->num_clks; i++)
		clk_disable_unprepare(mraw_dev->clks[i]);

	return 0;
}

static int mtk_mraw_runtime_resume(struct device *dev)
{
	struct mtk_mraw_device *mraw_dev = dev_get_drvdata(dev);
	int i, ret;

	dev_dbg(dev, "%s:enable clock\n", __func__);
	for (i = 0; i < mraw_dev->num_clks; i++) {
		ret = clk_prepare_enable(mraw_dev->clks[i]);
		if (ret) {
			dev_info(dev, "enable failed at clk #%d, ret = %d\n",
				 i, ret);
			i--;
			while (i >= 0)
				clk_disable_unprepare(mraw_dev->clks[i--]);

			return ret;
		}
	}
	mraw_reset(mraw_dev);

	return 0;
}

static const struct dev_pm_ops mtk_mraw_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(mtk_mraw_pm_suspend, mtk_mraw_pm_resume)
	SET_RUNTIME_PM_OPS(mtk_mraw_runtime_suspend, mtk_mraw_runtime_resume,
			   NULL)
};

struct platform_driver mtk_cam_mraw_driver = {
	.probe   = mtk_mraw_probe,
	.remove  = mtk_mraw_remove,
	.driver  = {
		.name  = "mtk-cam mraw",
		.of_match_table = of_match_ptr(mtk_mraw_of_ids),
		.pm     = &mtk_mraw_pm_ops,
	}
};
