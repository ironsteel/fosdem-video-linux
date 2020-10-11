// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2015 Free Electrons
 * Copyright (C) 2015 NextThing Co
 *
 * Maxime Ripard <maxime.ripard@free-electrons.com>
 *
 * Copyright (c) 2019 Luc Verhaegen <libv@skynet.be>
 */

#include <drm/drm_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_print.h>

#include "sun4i_backend.h"
#include "sun4i_frontend.h"
#include "sun4i_layer.h"
#include "sun4i_sprite.h"
#include "sunxi_engine.h"

#define SUN4I_LAYER_FORMATS_RGB \
	DRM_FORMAT_XRGB8888, \
	DRM_FORMAT_BGRX8888, \
	DRM_FORMAT_RGB888, \
	DRM_FORMAT_BGR888, \
	DRM_FORMAT_RGB565

#define SUN4I_LAYER_FORMATS_RGBA \
	DRM_FORMAT_ARGB8888, \
	DRM_FORMAT_BGRA8888, \
	DRM_FORMAT_ARGB1555, \
	DRM_FORMAT_RGBA5551, \
	DRM_FORMAT_ARGB4444, \
	DRM_FORMAT_RGBA4444

#define SUN4I_LAYER_FORMATS_YUV \
	DRM_FORMAT_UYVY, \
	DRM_FORMAT_VYUY, \
	DRM_FORMAT_YUYV, \
	DRM_FORMAT_YVYU, \
	DRM_FORMAT_R8_G8_B8

static const uint32_t sun4i_layer_formats_rgb[] = {
	SUN4I_LAYER_FORMATS_RGB,
};

static const uint32_t sun4i_layer_formats_rgba[] = {
	SUN4I_LAYER_FORMATS_RGBA,
	SUN4I_LAYER_FORMATS_RGB,
};

static const uint32_t sun4i_layer_formats_yuv[] = {
	SUN4I_LAYER_FORMATS_RGB,
	SUN4I_LAYER_FORMATS_RGBA,
	SUN4I_LAYER_FORMATS_YUV,
};

/*
 * TODO: fully test all formats. --libv
 * Alpha channel likely gets set to 0xFF when using the frontend.
  */
static const uint32_t sun4i_layer_formats_frontend[] = {
	SUN4I_LAYER_FORMATS_RGB,
	SUN4I_LAYER_FORMATS_RGBA,
	SUN4I_LAYER_FORMATS_YUV,
	DRM_FORMAT_NV12,
	DRM_FORMAT_NV16,
	DRM_FORMAT_NV21,
	DRM_FORMAT_NV61,
	DRM_FORMAT_YUV411,
	DRM_FORMAT_YUV420,
	DRM_FORMAT_YUV422,
	DRM_FORMAT_YUV444,
	DRM_FORMAT_YVU411,
	DRM_FORMAT_YVU420,
	DRM_FORMAT_YVU422,
	DRM_FORMAT_YVU444,
};

static const uint64_t sun4i_layer_format_modifiers_frontend[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_ALLWINNER_TILED,
	DRM_FORMAT_MOD_INVALID
};

static void sun4i_backend_layer_reset(struct drm_plane *plane)
{
	struct sun4i_layer *layer = plane_to_sun4i_layer(plane);
	struct sun4i_layer_state *state;

	if (plane->state) {
		state = state_to_sun4i_layer_state(plane->state);

		__drm_atomic_helper_plane_destroy_state(&state->state);

		kfree(state);
		plane->state = NULL;
	}

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (state) {
		__drm_atomic_helper_plane_reset(plane, &state->state);
		plane->state->zpos = layer->id;
	}
}

static struct drm_plane_state *
sun4i_backend_layer_duplicate_state(struct drm_plane *plane)
{
	struct sun4i_layer_state *orig = state_to_sun4i_layer_state(plane->state);
	struct sun4i_layer_state *copy;

	copy = kzalloc(sizeof(*copy), GFP_KERNEL);
	if (!copy)
		return NULL;

	__drm_atomic_helper_plane_duplicate_state(plane, &copy->state);
	copy->uses_frontend = orig->uses_frontend;

	return &copy->state;
}

static void sun4i_backend_layer_destroy_state(struct drm_plane *plane,
					      struct drm_plane_state *state)
{
	struct sun4i_layer_state *s_state = state_to_sun4i_layer_state(state);

	__drm_atomic_helper_plane_destroy_state(state);

	kfree(s_state);
}

static void sun4i_backend_layer_atomic_disable(struct drm_plane *plane,
					       struct drm_plane_state *old_state)
{
	struct sun4i_layer_state *layer_state = state_to_sun4i_layer_state(old_state);
	struct sun4i_layer *layer = plane_to_sun4i_layer(plane);
	struct sun4i_backend *backend = layer->backend;

	sun4i_backend_layer_enable(backend, layer->id, false);

	if (layer_state->uses_frontend) {
		unsigned long flags;

		spin_lock_irqsave(&backend->frontend_lock, flags);
		backend->frontend_teardown = true;
		spin_unlock_irqrestore(&backend->frontend_lock, flags);
	}
}

static void sun4i_backend_layer_atomic_update(struct drm_plane *plane,
					      struct drm_plane_state *old_state)
{
	struct sun4i_layer_state *layer_state = state_to_sun4i_layer_state(plane->state);
	struct sun4i_layer *layer = plane_to_sun4i_layer(plane);
	struct sun4i_backend *backend = layer->backend;
	struct sun4i_frontend *frontend = backend->frontend;

	if (layer_state->uses_frontend) {
		const struct drm_format_info *format =
			plane->state->fb->format;
		uint32_t format_backend;

		if (format->has_alpha)
			format_backend = DRM_FORMAT_ARGB8888;
		else
			format_backend = DRM_FORMAT_XRGB8888;

		sun4i_frontend_init(frontend, backend->engine.id);
		sun4i_frontend_update_coord(frontend, plane);
		sun4i_frontend_update_buffer(frontend, plane);
		sun4i_frontend_format_set(frontend, plane, format_backend);
		sun4i_backend_frontend_set(backend, layer->id, format_backend);
		sun4i_frontend_enable(frontend);
	} else {
		sun4i_backend_update_layer_formats(backend, layer->id, plane);
		sun4i_backend_update_layer_buffer(backend, layer->id, plane);
	}

	sun4i_backend_update_layer_coord(backend, layer->id, plane);
	sun4i_backend_update_layer_zpos(backend, layer->id, plane);
	sun4i_backend_update_layer_alpha(backend, layer->id, plane);
	sun4i_backend_layer_enable(backend, layer->id, true);
}

static bool sun4i_layer_format_mod_supported(struct drm_plane *plane,
					     uint32_t format, uint64_t modifier)
{
	struct sun4i_layer *layer = plane_to_sun4i_layer(plane);
	bool supported;

	supported = sun4i_backend_format_is_supported(format, modifier);
	if (!supported && !IS_ERR_OR_NULL(layer->backend->frontend))
		supported =
			sun4i_frontend_format_is_supported(format, modifier);

	DRM_DEBUG_DRIVER("%s(%d): is format 0x%08X supported: %s.\n",
			 __func__, layer->id,
			 format, supported ? "Yes" : "No");

	return supported;
}

/*
 * Only scale when we have the frontend.
 */
static int sun4i_backend_layer_atomic_check(struct drm_plane *plane,
					    struct drm_plane_state *state)
{
	struct sun4i_layer *layer = plane_to_sun4i_layer(plane);
	struct sun4i_layer_state *layer_state =
		state_to_sun4i_layer_state(state);

	DRM_DEBUG("%s(%d.%d);\n", __func__,
		  layer->backend->engine.id, layer->id);

	/* Are we scaling? */
	if (((state->crtc_w << 16) != state->src_w) ||
	    ((state->crtc_h << 16) != state->src_h)) {

		if (!layer->frontend) {
			DRM_ERROR("%s(%d.%d): this layer does not support "
				  "scaling.\n", __func__,
				  layer->backend->engine.id, layer->id);
			return -EINVAL;
		}

		layer_state->uses_frontend = true;
	} else if (layer->frontend)
		layer_state->uses_frontend = false;

	/* check whether we have a frontend specific format */
	if (layer->frontend && !layer_state->uses_frontend) {
		/* yes, atomic_check gets called for unused planes */
		if (state->fb) {
			const struct drm_format_info *format =
				state->fb->format;

			if (format->is_yuv)
				layer_state->uses_frontend = true;
		}
	}

	/* todo: test physical limits */

	return 0;
}

static const struct drm_plane_helper_funcs sun4i_backend_layer_helper_funcs = {
	.prepare_fb	= drm_gem_fb_prepare_fb,
	.atomic_check = sun4i_backend_layer_atomic_check,
	.atomic_disable	= sun4i_backend_layer_atomic_disable,
	.atomic_update	= sun4i_backend_layer_atomic_update,
};

static const struct drm_plane_funcs sun4i_backend_layer_funcs = {
	.atomic_destroy_state	= sun4i_backend_layer_destroy_state,
	.atomic_duplicate_state	= sun4i_backend_layer_duplicate_state,
	.destroy		= drm_plane_cleanup,
	.disable_plane		= drm_atomic_helper_disable_plane,
	.reset			= sun4i_backend_layer_reset,
	.update_plane		= drm_atomic_helper_update_plane,
	.format_mod_supported	= sun4i_layer_format_mod_supported,
};

static struct drm_plane *sun4i_layer_init(struct drm_device *drm,
					  struct sun4i_backend *backend,
					  enum drm_plane_type type,
					  int id, bool frontend, bool yuv,
					  bool alpha)
{
	struct sun4i_layer *layer;
	const uint64_t *modifiers;
	const uint32_t *formats;
	unsigned int formats_len;
	int ret;

	layer = devm_kzalloc(drm->dev, sizeof(*layer), GFP_KERNEL);
	if (!layer)
		return ERR_PTR(-ENOMEM);

	layer->id = id;
	layer->backend = backend;
	layer->frontend = frontend;

	if (layer->frontend) {
		formats = sun4i_layer_formats_frontend;
		formats_len = ARRAY_SIZE(sun4i_layer_formats_frontend);
		modifiers = sun4i_layer_format_modifiers_frontend;
	} else if (yuv) {
		formats = sun4i_layer_formats_yuv;
		formats_len = ARRAY_SIZE(sun4i_layer_formats_yuv);
		modifiers = NULL;
	} else if (alpha) {
		formats = sun4i_layer_formats_rgba;
		formats_len = ARRAY_SIZE(sun4i_layer_formats_rgba);
		modifiers = NULL;
	} else {
		formats = sun4i_layer_formats_rgb;
		formats_len = ARRAY_SIZE(sun4i_layer_formats_rgb);
		modifiers = NULL;
	}

	/* possible crtcs are set later */
	ret = drm_universal_plane_init(drm, &layer->plane, 0,
				       &sun4i_backend_layer_funcs,
				       formats, formats_len,
				       modifiers, type, "Backend-%d", id);
	if (ret) {
		DRM_DEV_ERROR(drm->dev, "%s(): Couldn't initialize layer\n",
			      __func__);
		return ERR_PTR(ret);
	}

	drm_plane_helper_add(&layer->plane,
			     &sun4i_backend_layer_helper_funcs);

	if (alpha)
		drm_plane_create_alpha_property(&layer->plane);
	drm_plane_create_zpos_immutable_property(&layer->plane, layer->id);

	return &layer->plane;
}

/*
 * This is a limit of the kms infrastructure. Only a uint32_t is
 * used in crtc_state as an active plane mask, for _all_ planes.
 *
 * We have up to 37 planes per CRTC... And yet KMS only has a 32 total.
 * So give each of our CRTCs half of the KMS "space".
 */
#define KMS_PER_CRTC_PLANES_MAX 16

struct drm_plane **
sun4i_layers_init(struct drm_device *drm, struct sunxi_engine *engine,
		  int *plane_count)
{
	struct drm_plane **planes;
	struct sun4i_backend *backend = engine_to_sun4i_backend(engine);
	struct drm_plane *plane;
	int i, j = 0, sprite_start;

	planes = kzalloc(KMS_PER_CRTC_PLANES_MAX * sizeof(*planes),
			 GFP_KERNEL);
	if (!planes)
		return ERR_PTR(-ENOMEM);

	/*
	 * Our first layer, primary, rgb, no scaling, no alpha.
	 * This one is critical for kms, error out if it fails
	 */
	plane = sun4i_layer_init(drm, backend, DRM_PLANE_TYPE_PRIMARY,
				 0, false, false, false);
	if (IS_ERR(plane)) {
		DRM_DEV_ERROR(drm->dev, "%s(): primary layer init failed.\n",
			      __func__);
		return ERR_CAST(plane);
	}
	planes[j] = plane;
	j++;

	/*
	 * From now on, we only make some noise when something fails.
	 * Working display is more important than working overlays.
	 */

	/* Our second layer, scaling + yuv (frontend) and alpha */
	if (IS_ERR_OR_NULL(backend->frontend))
		plane = sun4i_layer_init(drm, backend, DRM_PLANE_TYPE_OVERLAY,
					 1, false, false, true);
	else
		plane = sun4i_layer_init(drm, backend, DRM_PLANE_TYPE_OVERLAY,
					 1, true, false, true);
	if (IS_ERR(plane)) {
		DRM_DEV_ERROR(drm->dev, "%s() layer 1 init failed.\n",
			      __func__);
		/* if it fails, continue anyway */
	} else {
		planes[j] = plane;
		j++;
	}

	/* Our third layer, rgb only, no alpha */
	plane = sun4i_layer_init(drm, backend, DRM_PLANE_TYPE_OVERLAY,
				 2, false, false, false);
	if (IS_ERR(plane)) {
		DRM_DEV_ERROR(drm->dev, "%s() layer 2 init failed.\n",
			      __func__);
	} else {
		planes[j] = plane;
		j++;
	}

	/* final layer, yuv, alpha */
	plane = sun4i_layer_init(drm, backend, DRM_PLANE_TYPE_OVERLAY,
				 3, false, true, true);
	if (IS_ERR(plane)) {
		DRM_DEV_ERROR(drm->dev, "%s() layer 3 init failed.\n",
			      __func__);
	} else {
		planes[j] = plane;
		j++;
	}

	/* fill up the rest with sprites */
	sprite_start = j;
	for (i = 0; j < KMS_PER_CRTC_PLANES_MAX; i++) {
		plane = sun4i_sprite_plane_init(drm, backend, i, sprite_start);
		if (IS_ERR(plane)) {
			/* we might have simply run out of sprites */
			if (PTR_ERR(plane) != -ENODEV)
				DRM_DEV_ERROR(drm->dev,
					      "%s(): sprite %d init failed.\n",
					      __func__, i);
			break;
		}

		planes[j] = plane;
		j++;
	}

	*plane_count = j;
	return planes;
}
