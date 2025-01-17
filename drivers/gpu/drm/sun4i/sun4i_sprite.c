// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2019 Luc Verhaegen <libv@skynet.be>
 */
#include <linux/regmap.h>

#include <drm/drmP.h>
#include <drm/drm_atomic.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_fb_cma_helper.h>

#include "sun4i_crtc.h"
#include "sunxi_engine.h"
#include "sun4i_backend_regs.h"
#include "sun4i_backend.h"
#include "sun4i_sprite.h"

#define SUN4I_BE_SPRITE_COUNT 32

struct sun4i_sprite {
	struct drm_plane plane;
	struct sun4i_backend *backend;
	struct drm_device *drm;

	int id;
};

struct sun4i_sprite_state {
	struct drm_plane_state state;

	/* position in our linked list, depending on zpos */
	int block_id;
};

static inline struct sun4i_sprite *
sun4i_sprite_from_drm_plane(struct drm_plane *plane)
{
	return container_of(plane, struct sun4i_sprite, plane);
}

static inline struct sun4i_sprite_state *
sun4i_sprite_state_from_drm_state(struct drm_plane_state *state)
{
	return container_of(state, struct sun4i_sprite_state, state);
}


static void
sun4i_sprite_plane_reset(struct drm_plane *plane)
{
	struct sun4i_sprite_state *state;

	if (plane->state) {
		state = sun4i_sprite_state_from_drm_state(plane->state);

		__drm_atomic_helper_plane_destroy_state(&state->state);

		kfree(state);
		plane->state = NULL;
	}

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (state)
		__drm_atomic_helper_plane_reset(plane, &state->state);
}

static struct drm_plane_state *
sun4i_sprite_atomic_state_duplicate(struct drm_plane *plane)
{
	struct sun4i_sprite_state *orig =
		sun4i_sprite_state_from_drm_state(plane->state);
	struct sun4i_sprite_state *copy;

	copy = kzalloc(sizeof(*copy), GFP_KERNEL);
	if (!copy)
		return NULL;

	__drm_atomic_helper_plane_duplicate_state(plane, &copy->state);

	copy->block_id = orig->block_id;

	return &copy->state;
}

static void
sun4i_sprite_atomic_state_destroy(struct drm_plane *plane,
				  struct drm_plane_state *plane_state)
{
	struct sun4i_sprite_state *state =
		sun4i_sprite_state_from_drm_state(plane_state);

	__drm_atomic_helper_plane_destroy_state(plane_state);

	kfree(state);
}

static int
sun4i_sprite_atomic_check(struct drm_plane *plane,
			  struct drm_plane_state *state)
{
	struct sun4i_sprite *sprite = sun4i_sprite_from_drm_plane(plane);

	DRM_DEBUG_DRIVER("%s(%d);\n", __func__, sprite->id);

	/* verify that we are not scaling */
	if (((state->crtc_w << 16) != state->src_w) ||
	    ((state->crtc_h << 16) != state->src_h)) {
		DRM_ERROR("%s(%d.%d): scaling is not allowed.\n",
			  __func__, sprite->backend->engine.id, sprite->id);
		return -EINVAL;
	}

	/* todo: pluck the physical limits from my 2013 code */

	return 0;
}

/*
 * Figures out the block ordering, as in KMS it apaprently is allowed to
 * set multiple planes to the same zpos.
 */
int
sun4i_sprites_crtc_atomic_check(struct sunxi_engine *engine,
				struct drm_crtc_state *crtc_state)
{
	struct sun4i_backend *backend = engine_to_sun4i_backend(engine);
	uint32_t sprites_mask = crtc_state->plane_mask & backend->sprites_mask;
	struct drm_atomic_state *atomic_state = crtc_state->state;
	struct drm_device *drm = atomic_state->dev;
	struct drm_plane *plane;
	struct sun4i_sprite_state *table[SUN4I_BE_SPRITE_COUNT] = { NULL };
	unsigned int lowest = 0xFF, highest = 0, zpos;
	int count, i, block;

	/*
	 * Somehow, this feels like it might not be as reliable as i think it
	 * should be. -- libv
	 */
	/* no zpos change, no need for us to get involved! */
	if (!crtc_state->zpos_changed)
		return 0;

	/* no sprites enabled, no need to calculate anything */
	if (!sprites_mask)
		return 0;

	/* gather a list of candidates */
	count = 0;
	drm_for_each_plane_mask(plane, drm, sprites_mask) {
		struct drm_plane_state *plane_state =
			drm_atomic_get_plane_state(atomic_state, plane);
		struct sun4i_sprite_state *state =
			sun4i_sprite_state_from_drm_state(plane_state);

		if (plane_state->zpos < lowest)
			lowest = plane_state->zpos;
		if (plane_state->zpos > highest)
			highest = plane_state->zpos;

		table[count] = state;
		count++;
	}

	/* zpos might repeat, so we need to count up separately */
	block = 0;
	for (zpos = lowest; zpos <= highest; zpos++) {
		for (i = 0; i < count; i++) {
			if (table[i]->state.zpos == zpos) {
				table[i]->block_id = block;
				block++;
			}
		}
	}

	return 0;
}

static void
sun4i_sprite_atomic_update(struct drm_plane *plane,
			   struct drm_plane_state *plane_state_old)
{
	struct sun4i_sprite *sprite = sun4i_sprite_from_drm_plane(plane);
	struct drm_plane_state *plane_state = plane->state;
	struct sun4i_sprite_state *state =
		sun4i_sprite_state_from_drm_state(plane_state);
	struct drm_framebuffer *fb = plane_state->fb;
	struct sunxi_engine *engine = &sprite->backend->engine;
	int i = state->block_id;
	dma_addr_t paddr;

	DRM_DEBUG_DRIVER("%s(%d);\n", __func__, sprite->id);

	if (!i) {
		/* select ARGB8888 */
		regmap_write(engine->regs, SUN4I_BACKEND_SPRFMTCTL_REG, 0);
		/* disable alpha */
		regmap_write(engine->regs, SUN4I_BACKEND_SPRALPHACTL_REG, 0);
	}

	regmap_write(engine->regs, SUN4I_BACKEND_SPRCOORCTL_REG(i),
		     ((plane_state->crtc_y & 0xFFFF) << 16) |
		     (plane_state->crtc_x & 0xFFFF));

	/* Empty next block id, we set that later */
	regmap_write(engine->regs, SUN4I_BACKEND_SPRATTCTL_REG(i),
		     (((plane_state->crtc_h - 1) & 0xFFF) << 20) |
		     (((plane_state->crtc_w - 1) & 0xFFF) << 8));

	/*
	 * Note, sprites need to live below 256MiB, so make sure that CMA
	 * lives there.
	 */
	paddr = drm_fb_cma_get_gem_addr(fb, plane_state, 0);

	regmap_write(engine->regs, SUN4I_BACKEND_SPRADD_REG(i), paddr);

	regmap_write(engine->regs, SUN4I_BACKEND_SPRLINEWIDTH_REG(i),
		     fb->pitches[0] << 3);

	if (!i) /* enable */
		regmap_write(engine->regs, SUN4I_BACKEND_SPREN_REG, 0x01);
	else /* link previous block to this block */
		regmap_update_bits(engine->regs,
				   SUN4I_BACKEND_SPRATTCTL_REG(i - 1),
				   0x3F, i);
}

void
sun4i_sprites_crtc_commit(struct drm_crtc *drm_crtc,
			  struct drm_crtc_state *state_old)
{
	struct sun4i_crtc *crtc = drm_crtc_to_sun4i_crtc(drm_crtc);
	struct sunxi_engine *engine = crtc->engine;
	struct sun4i_backend *backend = engine_to_sun4i_backend(engine);
	struct drm_crtc_state *state = drm_crtc->state;
	uint32_t sprites_mask_new, sprites_mask_old;

	sprites_mask_new = state->plane_mask & backend->sprites_mask;
	sprites_mask_old = state_old->plane_mask & backend->sprites_mask;

	if (!sprites_mask_new && sprites_mask_old) /* disable */
		regmap_write(engine->regs, SUN4I_BACKEND_SPREN_REG, 0);
}

/*
 * Nothing needs to happen here. We either have constructed a new list
 * from active sprites, or, we have disabled the sprite block from the
 * crtc atomic_flush hook.
 */
static void
sun4i_sprite_atomic_disable(struct drm_plane *plane,
			    struct drm_plane_state *old_state)
{
	struct sun4i_sprite *sprite = sun4i_sprite_from_drm_plane(plane);

	DRM_DEBUG_DRIVER("%s(%d);\n", __func__, sprite->id);
}

static const struct drm_plane_helper_funcs sun4i_sprite_helper_funcs = {
	.prepare_fb = drm_gem_fb_prepare_fb,
	.atomic_check = sun4i_sprite_atomic_check,
	.atomic_disable = sun4i_sprite_atomic_disable,
	.atomic_update = sun4i_sprite_atomic_update,
};

static const struct drm_plane_funcs sun4i_sprite_drm_plane_funcs = {
	.atomic_destroy_state = sun4i_sprite_atomic_state_destroy,
	.atomic_duplicate_state	= sun4i_sprite_atomic_state_duplicate,

	.destroy = drm_plane_cleanup,

	.disable_plane = drm_atomic_helper_disable_plane,
	.reset = sun4i_sprite_plane_reset,
	.update_plane = drm_atomic_helper_update_plane,
};

/*
 * Lock down format for now, to ease multi-sprite logic, as this one
 * format must fit all.
 */
static const uint32_t sun4i_sprite_drm_formats[] = {
	DRM_FORMAT_ARGB8888,
	//DRM_FORMAT_BGRA8888,
};

struct drm_plane *
sun4i_sprite_plane_init(struct drm_device *drm,
			struct sun4i_backend *backend, int id,
			int zpos_start)
{
	struct sun4i_sprite *sprite;
	int zpos_end = zpos_start + SUN4I_BE_SPRITE_COUNT - 1;
	int ret;

	if (id >= SUN4I_BE_SPRITE_COUNT)
		return ERR_PTR(-ENODEV);

	sprite = devm_kzalloc(drm->dev, sizeof(struct sun4i_sprite),
			      GFP_KERNEL);
	if (!sprite)
		return ERR_PTR(-ENOMEM);

	sprite->backend = backend;
	sprite->drm = drm;
	sprite->id = id;

	ret = drm_universal_plane_init(drm, &sprite->plane, 0,
				       &sun4i_sprite_drm_plane_funcs,
				       sun4i_sprite_drm_formats,
				       ARRAY_SIZE(sun4i_sprite_drm_formats),
				       NULL, DRM_PLANE_TYPE_OVERLAY,
				       "Sprite-%d", id);
	if (ret) {
		DRM_DEV_ERROR(drm->dev, "%s(%d,%d): drm_universal_plane_init()"
			      " failed\n", __func__, backend->engine.id, id);
		return ERR_PTR(ret);
	}

	drm_plane_helper_add(&sprite->plane, &sun4i_sprite_helper_funcs);

	/* disable global alpha for now */
	//drm_plane_create_alpha_property(&sprite->plane);
	drm_plane_create_zpos_property(&sprite->plane, zpos_start + id,
				       zpos_start, zpos_end);

	backend->sprites_mask |= 1 << sprite->plane.index;

	return &sprite->plane;
}
