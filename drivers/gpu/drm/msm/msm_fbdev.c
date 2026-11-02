// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2013 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 */

#include <linux/fb.h>

#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_prime.h>
#include <drm/drm_probe_helper.h>

#include "msm_drv.h"
#include "msm_gem.h"
#include "msm_kms.h"

static bool fbdev = true;
MODULE_PARM_DESC(fbdev, "Enable fbdev compat layer");
module_param(fbdev, bool, 0600);

/*
 * fbdev funcs, to implement legacy fbdev interface on top of drm driver
 */

FB_GEN_DEFAULT_DEFERRED_SYSMEM_OPS(msm_fbdev,
				   drm_fb_helper_damage_range,
				   drm_fb_helper_damage_area)

static int msm_fbdev_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
	struct drm_fb_helper *helper = (struct drm_fb_helper *)info->par;
	struct drm_gem_object *bo = msm_framebuffer_bo(helper->fb, 0);

	return drm_gem_prime_mmap(bo, vma);
}

static void msm_fbdev_fb_destroy(struct fb_info *info)
{
	struct drm_fb_helper *helper = (struct drm_fb_helper *)info->par;
	struct drm_framebuffer *fb = helper->fb;
	struct drm_gem_object *bo = msm_framebuffer_bo(fb, 0);

	DBG();

	drm_fb_helper_fini(helper);

	/* this will free the backing object */
	msm_gem_put_vaddr(bo);
	drm_framebuffer_remove(fb);

	drm_client_release(&helper->client);
	drm_fb_helper_unprepare(helper);
	kfree(helper);
}

static const struct fb_ops msm_fb_ops = {
	.owner = THIS_MODULE,
	__FB_DEFAULT_DEFERRED_OPS_RDWR(msm_fbdev),
	DRM_FB_HELPER_DEFAULT_OPS,
	__FB_DEFAULT_DEFERRED_OPS_DRAW(msm_fbdev),
	.fb_mmap = msm_fbdev_mmap,
	.fb_destroy = msm_fbdev_fb_destroy,
};

static int msm_fbdev_fb_dirty(struct drm_fb_helper *helper,
			      struct drm_clip_rect *clip)
{
	struct drm_device *dev = helper->dev;
	int ret;

	/* Call damage handlers only if necessary */
	if (!(clip->x1 < clip->x2 && clip->y1 < clip->y2))
		return 0;

	if (helper->fb->funcs->dirty) {
		ret = helper->fb->funcs->dirty(helper->fb, NULL, 0, 0, clip, 1);
		if (drm_WARN_ONCE(dev, ret, "Dirty helper failed: ret=%d\n", ret))
			return ret;
	}

	return 0;
}

static const struct drm_fb_helper_funcs msm_fbdev_helper_funcs = {
	.fb_dirty = msm_fbdev_fb_dirty,
};

/*
 * msm_fbdev_preallocate - allocate the fbdev scanout buffer at KMS init
 *
 * DRM fbdev emulation only allocates its scanout buffer once a connector
 * reports a mode, so no buffer exists when the driver comes up without a
 * display.  Allocate it unconditionally here instead, sized from the first
 * connector mode, and hand it over to msm_fbdev_driver_fbdev_probe() once
 * fbdev emulation is set up (which can also happen later, on hotplug).
 */
int msm_fbdev_preallocate(struct drm_device *dev)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct drm_connector_list_iter conn_iter;
	struct drm_connector *connector;
	struct drm_display_mode *mode = NULL;
	struct drm_framebuffer *fb;
	struct drm_gem_object *bo;
	uint64_t paddr;
	uint32_t format;
	int ret, pitch;

	if (!priv->kms || !priv->kms->vm || priv->fbdev_fb)
		return 0;

	mutex_lock(&dev->mode_config.mutex);
	drm_connector_list_iter_begin(dev, &conn_iter);
	drm_for_each_connector_iter(connector, &conn_iter) {
		drm_helper_probe_single_connector_modes(connector, 0, 0);
		if (!list_empty(&connector->modes)) {
			mode = list_first_entry(&connector->modes,
						struct drm_display_mode, head);
			break;
		}
	}
	drm_connector_list_iter_end(&conn_iter);
	mutex_unlock(&dev->mode_config.mutex);

	if (!mode) {
		drm_dbg_kms(dev, "fbdev: no connector mode, deferring allocation\n");
		return 0;
	}

	format = drm_mode_legacy_fb_format(32, 24);
	pitch = align_pitch(mode->hdisplay, 32);

	fb = msm_alloc_stolen_fb(dev, mode->hdisplay, mode->vdisplay,
				 pitch, format);
	if (IS_ERR(fb)) {
		drm_err(dev, "fbdev: failed to allocate %dx%d framebuffer: %pe\n",
			mode->hdisplay, mode->vdisplay, fb);
		return PTR_ERR(fb);
	}

	/*
	 * Back the buffer with pages now: the framebuffer memory is allocated at
	 * init instead of on the first fbdev setup.  Drop the temporary pin
	 * again, so that fbdev emulation can pin it the usual way when it takes
	 * the buffer over; the pages stay in the willneed LRU in the meantime.
	 */
	bo = msm_framebuffer_bo(fb, 0);
	ret = msm_gem_get_and_pin_iova(bo, priv->kms->vm, &paddr);
	if (ret) {
		drm_err(dev, "fbdev: failed to allocate framebuffer memory: %d\n",
			ret);
		drm_framebuffer_put(fb);
		return ret;
	}
	msm_gem_unpin_iova(bo, priv->kms->vm);

	priv->fbdev_fb = fb;

	drm_dbg_kms(dev, "fbdev: preallocated %dx%d framebuffer\n",
		    mode->hdisplay, mode->vdisplay);

	return 0;
}

void msm_fbdev_release(struct drm_device *dev)
{
	struct msm_drm_private *priv = dev->dev_private;

	if (!priv->fbdev_fb)
		return;

	drm_framebuffer_put(priv->fbdev_fb);
	priv->fbdev_fb = NULL;
}

int msm_fbdev_driver_fbdev_probe(struct drm_fb_helper *helper,
				 struct drm_fb_helper_surface_size *sizes)
{
	struct drm_device *dev = helper->dev;
	struct msm_drm_private *priv = dev->dev_private;
	struct drm_framebuffer *fb = NULL;
	struct drm_gem_object *bo;
	struct fb_info *fbi = NULL;
	uint64_t paddr;
	uint32_t format;
	int ret, pitch;

	format = drm_mode_legacy_fb_format(sizes->surface_bpp, sizes->surface_depth);

	DBG("create fbdev: %dx%d@%d (%dx%d)", sizes->surface_width,
			sizes->surface_height, sizes->surface_bpp,
			sizes->fb_width, sizes->fb_height);

	pitch = align_pitch(sizes->surface_width, sizes->surface_bpp);

	/*
	 * Take over the buffer allocated at KMS init, if it fits the mode fbdev
	 * picked; otherwise fall back to allocating one for this mode.
	 */
	fb = priv->fbdev_fb;
	priv->fbdev_fb = NULL;
	if (fb && (fb->width != sizes->surface_width ||
		   fb->height != sizes->surface_height ||
		   fb->pitches[0] != pitch ||
		   fb->format->format != format)) {
		drm_framebuffer_put(fb);
		fb = NULL;
	}

	if (!fb)
		fb = msm_alloc_stolen_fb(dev, sizes->surface_width,
				sizes->surface_height, pitch, format);

	if (IS_ERR(fb)) {
		DRM_DEV_ERROR(dev->dev, "failed to allocate fb\n");
		return PTR_ERR(fb);
	}

	bo = msm_framebuffer_bo(fb, 0);

	/*
	 * NOTE: if we can be guaranteed to be able to map buffer
	 * in panic (ie. lock-safe, etc) we could avoid pinning the
	 * buffer now:
	 */
	ret = msm_gem_get_and_pin_iova(bo, priv->kms->vm, &paddr);
	if (ret) {
		DRM_DEV_ERROR(dev->dev, "failed to get buffer obj iova: %d\n", ret);
		goto fail;
	}

	fbi = drm_fb_helper_alloc_info(helper);
	if (IS_ERR(fbi)) {
		DRM_DEV_ERROR(dev->dev, "failed to allocate fb info\n");
		ret = PTR_ERR(fbi);
		goto fail;
	}

	DBG("fbi=%p, dev=%p", fbi, dev);

	helper->funcs = &msm_fbdev_helper_funcs;
	helper->fb = fb;

	fbi->fbops = &msm_fb_ops;

	drm_fb_helper_fill_info(fbi, helper, sizes);

	fbi->screen_buffer = msm_gem_get_vaddr(bo);
	if (IS_ERR(fbi->screen_buffer)) {
		ret = PTR_ERR(fbi->screen_buffer);
		goto fail;
	}
	fbi->screen_size = bo->size;
	fbi->fix.smem_start = paddr;
	fbi->fix.smem_len = bo->size;

	DBG("par=%p, %dx%d", fbi->par, fbi->var.xres, fbi->var.yres);
	DBG("allocated %dx%d fb", fb->width, fb->height);

	return 0;

fail:
	drm_framebuffer_remove(fb);
	return ret;
}
