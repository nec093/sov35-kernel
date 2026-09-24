/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Legacy VIDIOC_{G,S}_CROP / VIDIOC_CROPCAP driver callbacks on top of the
 * selection API. The ->vidioc_{g,s}_crop and ->vidioc_cropcap ops were
 * removed from struct v4l2_ioctl_ops (the core now translates the old
 * ioctls into {g,s}_selection with non-MPLANE types and CROP/COMPOSE
 * targets); this lets the CAF msm drivers keep their crop handlers.
 */
#ifndef _MEDIA_V4L2_CROP_COMPAT_H
#define _MEDIA_V4L2_CROP_COMPAT_H

#include <linux/videodev2.h>
#include <linux/fs.h>

typedef int (*v4l2_legacy_g_crop_t)(struct file *, void *, struct v4l2_crop *);
typedef int (*v4l2_legacy_s_crop_t)(struct file *, void *,
				    const struct v4l2_crop *);
typedef int (*v4l2_legacy_cropcap_t)(struct file *, void *,
				     struct v4l2_cropcap *);

static inline u32 v4l2_legacy_crop_type(u32 type, bool mplane)
{
	if (!mplane)
		return type;
	if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT)
		return V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	return type;
}

static inline int v4l2_legacy_g_selection(struct file *file, void *fh,
					  struct v4l2_selection *s,
					  v4l2_legacy_g_crop_t g_crop,
					  v4l2_legacy_cropcap_t cropcap,
					  bool mplane)
{
	u32 type = v4l2_legacy_crop_type(s->type, mplane);
	int ret;

	switch (s->target) {
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_COMPOSE: {
		struct v4l2_crop c = { .type = type };

		if (!g_crop)
			return -ENOTTY;
		ret = g_crop(file, fh, &c);
		if (!ret)
			s->r = c.c;
		return ret;
	}
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_COMPOSE_BOUNDS:
	case V4L2_SEL_TGT_COMPOSE_DEFAULT: {
		struct v4l2_cropcap cc = { .type = type };

		if (!cropcap)
			return -EINVAL;
		ret = cropcap(file, fh, &cc);
		if (ret)
			return ret;
		if (s->target == V4L2_SEL_TGT_CROP_BOUNDS ||
		    s->target == V4L2_SEL_TGT_COMPOSE_BOUNDS)
			s->r = cc.bounds;
		else
			s->r = cc.defrect;
		return 0;
	}
	default:
		return -EINVAL;
	}
}

static inline int v4l2_legacy_s_selection(struct file *file, void *fh,
					  struct v4l2_selection *s,
					  v4l2_legacy_s_crop_t s_crop,
					  bool mplane)
{
	struct v4l2_crop c = {
		.type = v4l2_legacy_crop_type(s->type, mplane),
		.c = s->r,
	};

	if (s->target != V4L2_SEL_TGT_CROP && s->target != V4L2_SEL_TGT_COMPOSE)
		return -EINVAL;
	if (!s_crop)
		return -ENOTTY;
	return s_crop(file, fh, &c);
}

#endif /* _MEDIA_V4L2_CROP_COMPAT_H */
