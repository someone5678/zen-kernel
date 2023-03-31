/*
 * Copyright 2018 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: AMD
 *
 */
#include "amdgpu.h"
#include "amdgpu_mode.h"
#include "amdgpu_dm.h"
#include "dc.h"
#include "modules/color/color_gamma.h"
#include "basics/conversion.h"

/**
 * DOC: overview
 *
 * The DC interface to HW gives us the following color management blocks
 * per pipe (surface):
 *
 * - Input gamma LUT (de-normalized)
 * - Input CSC (normalized)
 * - Surface degamma LUT (normalized)
 * - Surface CSC (normalized)
 * - Surface regamma LUT (normalized)
 * - Output CSC (normalized)
 *
 * But these aren't a direct mapping to DRM color properties. The current DRM
 * interface exposes CRTC degamma, CRTC CTM and CRTC regamma while our hardware
 * is essentially giving:
 *
 * Plane CTM -> Plane degamma -> Plane CTM -> Plane regamma -> Plane CTM
 *
 * The input gamma LUT block isn't really applicable here since it operates
 * on the actual input data itself rather than the HW fp representation. The
 * input and output CSC blocks are technically available to use as part of
 * the DC interface but are typically used internally by DC for conversions
 * between color spaces. These could be blended together with user
 * adjustments in the future but for now these should remain untouched.
 *
 * The pipe blending also happens after these blocks so we don't actually
 * support any CRTC props with correct blending with multiple planes - but we
 * can still support CRTC color management properties in DM in most single
 * plane cases correctly with clever management of the DC interface in DM.
 *
 * As per DRM documentation, blocks should be in hardware bypass when their
 * respective property is set to NULL. A linear DGM/RGM LUT should also
 * considered as putting the respective block into bypass mode.
 *
 * This means that the following
 * configuration is assumed to be the default:
 *
 * Plane DGM Bypass -> Plane CTM Bypass -> Plane RGM Bypass -> ...
 * CRTC DGM Bypass -> CRTC CTM Bypass -> CRTC RGM Bypass
 */

#define MAX_DRM_LUT_VALUE 0xFFFF

static enum dc_transfer_func_predefined drm_tf_to_dc_tf(enum drm_transfer_function drm_tf);

/**
 * amdgpu_dm_init_color_mod - Initialize the color module.
 *
 * We're not using the full color module, only certain components.
 * Only call setup functions for components that we need.
 */
void amdgpu_dm_init_color_mod(void)
{
	setup_x_points_distribution();
}

/**
 * __extract_blob_lut - Extracts the DRM lut and lut size from a blob.
 * @blob: DRM color mgmt property blob
 * @size: lut size
 *
 * Returns:
 * DRM LUT or NULL
 */
static const struct drm_color_lut *
__extract_blob_lut(const struct drm_property_blob *blob, uint32_t *size)
{
	*size = blob ? drm_color_lut_size(blob) : 0;
	return blob ? (struct drm_color_lut *)blob->data : NULL;
}

/**
 * __is_lut_linear - check if the given lut is a linear mapping of values
 * @lut: given lut to check values
 * @size: lut size
 *
 * It is considered linear if the lut represents:
 * f(a) = (0xFF00/MAX_COLOR_LUT_ENTRIES-1)a; for integer a in [0,
 * MAX_COLOR_LUT_ENTRIES)
 *
 * Returns:
 * True if the given lut is a linear mapping of values, i.e. it acts like a
 * bypass LUT. Otherwise, false.
 */
static bool __is_lut_linear(const struct drm_color_lut *lut, uint32_t size)
{
	int i;
	uint32_t expected;
	int delta;

	for (i = 0; i < size; i++) {
		/* All color values should equal */
		if ((lut[i].red != lut[i].green) || (lut[i].green != lut[i].blue))
			return false;

		expected = i * MAX_DRM_LUT_VALUE / (size-1);

		/* Allow a +/-1 error. */
		delta = lut[i].red - expected;
		if (delta < -1 || 1 < delta)
			return false;
	}
	return true;
}

/**
 * __drm_lut_to_dc_gamma - convert the drm_color_lut to dc_gamma.
 * @lut: DRM lookup table for color conversion
 * @gamma: DC gamma to set entries
 * @is_legacy: legacy or atomic gamma
 *
 * The conversion depends on the size of the lut - whether or not it's legacy.
 */
static void __drm_lut_to_dc_gamma(const struct drm_color_lut *lut,
				  struct dc_gamma *gamma, bool is_legacy)
{
	uint32_t r, g, b;
	int i;

	if (is_legacy) {
		for (i = 0; i < MAX_COLOR_LEGACY_LUT_ENTRIES; i++) {
			r = drm_color_lut_extract(lut[i].red, 16);
			g = drm_color_lut_extract(lut[i].green, 16);
			b = drm_color_lut_extract(lut[i].blue, 16);

			gamma->entries.red[i] = dc_fixpt_from_int(r);
			gamma->entries.green[i] = dc_fixpt_from_int(g);
			gamma->entries.blue[i] = dc_fixpt_from_int(b);
		}
		return;
	}

	/* else */
	for (i = 0; i < MAX_COLOR_LUT_ENTRIES; i++) {
		r = drm_color_lut_extract(lut[i].red, 16);
		g = drm_color_lut_extract(lut[i].green, 16);
		b = drm_color_lut_extract(lut[i].blue, 16);

		gamma->entries.red[i] = dc_fixpt_from_fraction(r, MAX_DRM_LUT_VALUE);
		gamma->entries.green[i] = dc_fixpt_from_fraction(g, MAX_DRM_LUT_VALUE);
		gamma->entries.blue[i] = dc_fixpt_from_fraction(b, MAX_DRM_LUT_VALUE);
	}
}

/**
 * __drm_ctm_to_dc_matrix - converts a DRM CTM to a DC CSC float matrix
 * @ctm: DRM color transformation matrix
 * @matrix: DC CSC float matrix
 *
 * The matrix needs to be a 3x4 (12 entry) matrix.
 */
static void __drm_ctm_to_dc_matrix(const struct drm_color_ctm *ctm,
				   struct fixed31_32 *matrix)
{
	int i;

	/*
	 * DRM gives a 3x3 matrix, but DC wants 3x4. Assuming we're operating
	 * with homogeneous coordinates, augment the matrix with 0's.
	 *
	 * The format provided is S31.32, using signed-magnitude representation.
	 * Our fixed31_32 is also S31.32, but is using 2's complement. We have
	 * to convert from signed-magnitude to 2's complement.
	 */
	for (i = 0; i < 12; i++) {
		/* Skip 4th element */
		if (i % 4 == 3) {
			matrix[i] = dc_fixpt_zero;
			continue;
		}

		/* gamut_remap_matrix[i] = ctm[i - floor(i/4)] */
		matrix[i] = dc_fixpt_from_s3132(ctm->matrix[i - (i / 4)]);
	}
}

/**
 * __set_legacy_tf - Calculates the legacy transfer function
 * @func: transfer function
 * @lut: lookup table that defines the color space
 * @lut_size: size of respective lut
 * @has_rom: if ROM can be used for hardcoded curve
 *
 * Only for sRGB input space
 *
 * Returns:
 * 0 in case of success, -ENOMEM if fails
 */
static int __set_legacy_tf(struct dc_transfer_func *func,
			   const struct drm_color_lut *lut, uint32_t lut_size,
			   bool has_rom)
{
	struct dc_gamma *gamma = NULL;
	struct calculate_buffer cal_buffer = {0};
	bool res;

	ASSERT(lut && lut_size == MAX_COLOR_LEGACY_LUT_ENTRIES);

	cal_buffer.buffer_index = -1;

	gamma = dc_create_gamma();
	if (!gamma)
		return -ENOMEM;

	gamma->type = GAMMA_RGB_256;
	gamma->num_entries = lut_size;
	__drm_lut_to_dc_gamma(lut, gamma, true);

	res = mod_color_calculate_regamma_params(func, gamma, true, has_rom,
						 NULL, &cal_buffer);

	dc_gamma_release(&gamma);

	return res ? 0 : -ENOMEM;
}

/**
 * __set_output_tf - calculates the output transfer function based on expected input space.
 * @func: transfer function
 * @lut: lookup table that defines the color space
 * @lut_size: size of respective lut
 * @has_rom: if ROM can be used for hardcoded curve
 *
 * Returns:
 * 0 in case of success. -ENOMEM if fails.
 */
static int __set_output_tf(struct dc_transfer_func *func,
			   const struct drm_color_lut *lut, uint32_t lut_size,
			   bool has_rom)
{
	struct dc_gamma *gamma = NULL;
	struct calculate_buffer cal_buffer = {0};
	bool res;

	ASSERT(lut && lut_size == MAX_COLOR_LUT_ENTRIES);

	cal_buffer.buffer_index = -1;

	if (lut_size) {
		gamma = dc_create_gamma();
		if (!gamma)
			return -ENOMEM;

		gamma->num_entries = lut_size;
		__drm_lut_to_dc_gamma(lut, gamma, false);
	}

	if (func->tf == TRANSFER_FUNCTION_LINEAR) {
		/*
		 * Color module doesn't like calculating regamma params
		 * on top of a linear input. But degamma params can be used
		 * instead to simulate this.
		 */
		if (gamma)
			gamma->type = GAMMA_CUSTOM;
		res = mod_color_calculate_degamma_params(NULL, func,
							gamma, gamma != NULL);
	} else {
		/*
		 * Assume sRGB. The actual mapping will depend on whether the
		 * input was legacy or not.
		 */
		if (gamma)
			gamma->type = GAMMA_CS_TFM_1D;
		res = mod_color_calculate_regamma_params(func, gamma, gamma != NULL,
							 has_rom, NULL, &cal_buffer);
	}

	if (gamma)
		dc_gamma_release(&gamma);

	return res ? 0 : -ENOMEM;
}

static int amdgpu_dm_set_atomic_regamma(struct dc_stream_state *stream,
					const struct drm_color_lut *regamma_lut,
					uint32_t regamma_size, bool has_rom,
					enum dc_transfer_func_predefined tf)
{
	int ret = 0;

	if (regamma_size || tf != TRANSFER_FUNCTION_LINEAR) {
		/* CRTC RGM goes into RGM LUT.
		 *
		 * Note: here there is no implicit sRGB regamma. We are using
		 * degamma calculation from color module to calculate the curve
		 * from a linear base.
		 */
		stream->out_transfer_func->type = TF_TYPE_DISTRIBUTED_POINTS;
		stream->out_transfer_func->tf = tf;
		stream->out_transfer_func->sdr_ref_white_level = 80; /* hardcoded for now */

		ret = __set_output_tf(stream->out_transfer_func, regamma_lut,
				      regamma_size, has_rom);
	} else {
		/*
		 * No CRTC RGM means we can just put the block into bypass
		 * since we don't have any plane level adjustments using it.
		 */
		stream->out_transfer_func->type = TF_TYPE_BYPASS;
		stream->out_transfer_func->tf = TRANSFER_FUNCTION_LINEAR;
	}

	return ret;
}

/**
 * __set_input_tf - calculates the input transfer function based on expected
 * input space.
 * @func: transfer function
 * @lut: lookup table that defines the color space
 * @lut_size: size of respective lut.
 *
 * Returns:
 * 0 in case of success. -ENOMEM if fails.
 */
static int __set_input_tf(struct dc_transfer_func *func,
			  const struct drm_color_lut *lut, uint32_t lut_size)
{
	struct dc_gamma *gamma = NULL;
	bool res;

	gamma = dc_create_gamma();
	if (!gamma)
		return -ENOMEM;

	gamma->type = GAMMA_CUSTOM;
	gamma->num_entries = lut_size;

	__drm_lut_to_dc_gamma(lut, gamma, false);

	res = mod_color_calculate_degamma_params(NULL, func, gamma, true);
	dc_gamma_release(&gamma);

	return res ? 0 : -ENOMEM;
}

static int __set_func_shaper(struct dc_transfer_func *shaper_func,
			     const struct drm_color_lut *lut, uint32_t lut_size)
{
	struct dc_gamma *gamma = NULL;
	struct calculate_buffer cal_buffer = {0};
	bool res;

	ASSERT(lut && lut_size == MAX_COLOR_LUT_ENTRIES);

	cal_buffer.buffer_index = -1;

	gamma = dc_create_gamma();
	if (!gamma)
		return -ENOMEM;

	gamma->num_entries = lut_size;
	__drm_lut_to_dc_gamma(lut, gamma, false);

	/*
	 * Color module doesn't like calculating gamma params
	 * on top of a linear input. But degamma params can be used
	 * instead to simulate this.
	 */
	gamma->type = GAMMA_CUSTOM;
	res = mod_color_calculate_degamma_params(NULL, shaper_func, gamma, true);

	dc_gamma_release(&gamma);

	return res ? 0 : -ENOMEM;
}

static void __to_dc_lut3d_color(struct dc_rgb *rgb,
				const struct drm_color_lut lut,
				int bit_precision)
{
	rgb->red = drm_color_lut_extract(lut.red, bit_precision);
	rgb->green = drm_color_lut_extract(lut.green, bit_precision);
	rgb->blue  = drm_color_lut_extract(lut.blue, bit_precision);
}

static void __drm_3dlut_to_dc_3dlut(const struct drm_color_lut *lut,
				    uint32_t lut_size,
				    struct dc_3dlut *lut3d)
{
	int lut_i, i;

	for (lut_i = 0, i = 0; i < lut_size - 4; lut_i++, i += 4) {
		/* We should consider the 3dlut RGB values are distributed
		 * along four arrays lut0-3 where the first sizes 1229 and the
		 * other 1228. The bit depth supported for 3dlut channel is
		 * 12-bit, but DC also supports 10-bit.
		 *
		 * TODO: improve color pipeline API to enable the userspace set
		 * bit depth and 3D LUT size/stride, as specified by VA-API.
		 */
		__to_dc_lut3d_color(&lut3d->lut_3d.tetrahedral_17.lut0[lut_i], lut[i], 12);
		__to_dc_lut3d_color(&lut3d->lut_3d.tetrahedral_17.lut1[lut_i], lut[i + 1], 12);
		__to_dc_lut3d_color(&lut3d->lut_3d.tetrahedral_17.lut2[lut_i], lut[i + 2], 12);
		__to_dc_lut3d_color(&lut3d->lut_3d.tetrahedral_17.lut3[lut_i], lut[i + 3], 12);
	}
	/* lut0 has 1229 points (lut_size/4 + 1) */
	__to_dc_lut3d_color(&lut3d->lut_3d.tetrahedral_17.lut0[lut_i], lut[i], 12);
}

/* amdgpu_dm_atomic_lut3d - set DRM 3D LUT to DC stream
 * @stream: DC stream state to set shaper LUT and 3D LUT
 * @drm_lut3d: DRM CRTC (user) 3D LUT
 * @drm_lut3d_size: size of 3D LUT
 * @lut3d: DC 3D LUT
 *
 * Map DRM CRTC 3D LUT to DC 3D LUT and all necessary bits to program it
 * on DCN MPC accordingly.
 */
static void amdgpu_dm_atomic_lut3d(struct dc_stream_state *stream,
				   const struct drm_color_lut *lut,
				   uint32_t lut_size,
				   struct dc_3dlut *lut3d)
{
	ASSERT(lut3d && lut_size == MAX_COLOR_3DLUT_ENTRIES);

	__drm_3dlut_to_dc_3dlut(lut, lut_size, lut3d);

	/* Stride and bit depth is not programmable by API so far. Therefore,
	 * only supports 17x17x17 3D LUT with 12-bit.
	 */
	lut3d->lut_3d.use_tetrahedral_9 = false;
	lut3d->lut_3d.use_12bits = true;
	lut3d->state.bits.initialized = 1;

	stream->lut3d_func = lut3d;
}

static int amdgpu_dm_atomic_shaper_lut(struct dc_stream_state *stream,
				       const struct drm_color_lut *shaper_lut,
				       uint32_t shaper_size,
				       struct dc_transfer_func *func_shaper_new)
{
	/* If no DRM shaper LUT, we assume the input color space is already
	 * delinearized, so we don't need a shaper LUT and we can just BYPASS
	 */
	if (!shaper_size) {
		func_shaper_new->type = TF_TYPE_BYPASS;
		func_shaper_new->tf = TRANSFER_FUNCTION_LINEAR;
	} else {
		int r;

		/* If DRM shaper LUT is set, we assume a linear color space
		 * (linearized by DRM degamma 1D LUT or not)
		 */
		func_shaper_new->type = TF_TYPE_DISTRIBUTED_POINTS;
		func_shaper_new->tf = TRANSFER_FUNCTION_LINEAR;

		r = __set_func_shaper(func_shaper_new, shaper_lut,
				shaper_size);
		if (r)
			return r;
	}

	stream->func_shaper = func_shaper_new;

	return 0;
}

/* amdgpu_dm_atomic_shaper_lut3d - set DRM CRTC shaper LUT and 3D LUT to DC
 * interface
 * @dc: Display Core control structure
 * @stream: DC stream state to set shaper LUT and 3D LUT
 * @drm_shaper_lut: DRM CRTC (user) shaper LUT
 * @drm_shaper_size: size of shaper LUT
 * @drm_lut3d: DRM CRTC (user) 3D LUT
 * @drm_lut3d_size: size of 3D LUT
 *
 * Returns:
 * 0 on success.
 */
static int amdgpu_dm_atomic_shaper_lut3d(struct dc *dc,
					 struct dc_state *ctx,
					 struct dc_stream_state *stream,
					 const struct drm_color_lut *drm_shaper_lut,
					 uint32_t drm_shaper_size,
					 const struct drm_color_lut *drm_lut3d,
					 uint32_t drm_lut3d_size)
{
	struct dc_3dlut *lut3d_func;
	struct dc_transfer_func *func_shaper;
	bool acquire = drm_shaper_size && drm_lut3d_size;

	lut3d_func = (struct dc_3dlut *)stream->lut3d_func;
	func_shaper = (struct dc_transfer_func *)stream->func_shaper;

	ASSERT((lut3d_func && func_shaper) || (!lut3d_func && !func_shaper));
	if ((acquire && !lut3d_func && !func_shaper) ||
		(!acquire && lut3d_func && func_shaper))
	{
		if (!dc_acquire_release_mpc_3dlut_for_ctx(dc, acquire, ctx, stream,
							&lut3d_func, &func_shaper))
			return DC_ERROR_UNEXPECTED;
	}

	stream->lut3d_func = lut3d_func;
	stream->func_shaper = func_shaper;

	if (!acquire)
		return 0;

	amdgpu_dm_atomic_lut3d(stream, drm_lut3d, drm_lut3d_size, lut3d_func);

	return amdgpu_dm_atomic_shaper_lut(stream, drm_shaper_lut,
					   drm_shaper_size, func_shaper);
}

/**
 * amdgpu_dm_lut3d_size - get expected size according to hw color caps
 * @adev: amdgpu device
 * @lut_size: default size
 *
 * Return:
 * lut_size if DC 3D LUT is supported, zero otherwise.
 */
static uint32_t amdgpu_dm_get_lut3d_size(struct amdgpu_device *adev,
					 uint32_t lut_size)
{
	return adev->dm.dc->caps.color.mpc.num_3dluts ? lut_size : 0;
}

/**
 * amdgpu_dm_verify_lut3d_size - verifies if 3D LUT is supported and if DRM 3D
 * LUT matches the hw supported size
 * @adev: amdgpu device
 * @crtc_state: the DRM CRTC state
 *
 * Verifies if post-blending (MPC) 3D LUT is supported by the HW (DCN 3.0 or
 * newer) and if the DRM 3D LUT matches the supported size.
 *
 * Returns:
 * 0 on success. -EINVAL if lut size are invalid.
 */
int amdgpu_dm_verify_lut3d_size(struct amdgpu_device *adev,
				const struct drm_crtc_state *crtc_state)
{
	const struct drm_color_lut *shaper = NULL, *lut3d = NULL;
	uint32_t exp_size, size;

	/* shaper LUT is only available if 3D LUT color caps*/
	exp_size = amdgpu_dm_get_lut3d_size(adev, MAX_COLOR_LUT_ENTRIES);
	shaper = __extract_blob_lut(crtc_state->shaper_lut, &size);

	if (shaper && size != exp_size) {
		DRM_DEBUG_DRIVER(
			"Invalid Shaper LUT size. Should be %u but got %u.\n",
			exp_size, size);
		return -EINVAL;
	}

	exp_size = amdgpu_dm_get_lut3d_size(adev, MAX_COLOR_3DLUT_ENTRIES);
	lut3d = __extract_blob_lut(crtc_state->lut3d, &size);

	if (lut3d && size != exp_size) {
		DRM_DEBUG_DRIVER("Invalid Gamma 3D LUT size. Should be %u but got %u.\n",
				 exp_size, size);
		return -EINVAL;
	}

	return 0;
}

/**
 * amdgpu_dm_verify_lut_sizes - verifies if DRM luts match the hw supported sizes
 * @crtc_state: the DRM CRTC state
 *
 * Verifies that the Degamma and Gamma LUTs attached to the &crtc_state
 * are of the expected size.
 *
 * Returns:
 * 0 on success. -EINVAL if any lut sizes are invalid.
 */
int amdgpu_dm_verify_lut_sizes(const struct drm_crtc_state *crtc_state)
{
	const struct drm_color_lut *lut = NULL;
	uint32_t size = 0;

	lut = __extract_blob_lut(crtc_state->degamma_lut, &size);
	if (lut && size != MAX_COLOR_LUT_ENTRIES) {
		DRM_DEBUG_DRIVER(
			"Invalid Degamma LUT size. Should be %u but got %u.\n",
			MAX_COLOR_LUT_ENTRIES, size);
		return -EINVAL;
	}

	lut = __extract_blob_lut(crtc_state->gamma_lut, &size);
	if (lut && size != MAX_COLOR_LUT_ENTRIES &&
	    size != MAX_COLOR_LEGACY_LUT_ENTRIES) {
		DRM_DEBUG_DRIVER(
			"Invalid Gamma LUT size. Should be %u (or %u for legacy) but got %u.\n",
			MAX_COLOR_LUT_ENTRIES, MAX_COLOR_LEGACY_LUT_ENTRIES,
			size);
		return -EINVAL;
	}

	return 0;
}

/**
 * amdgpu_dm_update_crtc_color_mgmt: Maps DRM color management to DC stream.
 * @crtc: amdgpu_dm crtc state
 *
 * With no plane level color management properties we're free to use any
 * of the HW blocks as long as the CRTC CTM always comes before the
 * CRTC RGM and after the CRTC DGM.
 *
 * - The CRTC RGM block will be placed in the RGM LUT block if it is non-linear.
 * - The CRTC DGM block will be placed in the DGM LUT block if it is non-linear.
 * - The CRTC CTM will be placed in the gamut remap block if it is non-linear.
 *
 * The RGM block is typically more fully featured and accurate across
 * all ASICs - DCE can't support a custom non-linear CRTC DGM.
 *
 * For supporting both plane level color management and CRTC level color
 * management at once we have to either restrict the usage of CRTC properties
 * or blend adjustments together.
 *
 * Returns:
 * 0 on success. Error code if setup fails.
 */
int amdgpu_dm_update_crtc_color_mgmt(struct dc_state *ctx, struct dm_crtc_state *crtc)
{
	struct dc_stream_state *stream = crtc->stream;
	struct amdgpu_device *adev = drm_to_adev(crtc->base.state->dev);
	bool has_rom = adev->asic_type <= CHIP_RAVEN;
	struct drm_color_ctm *ctm = NULL;
	const struct drm_color_lut *degamma_lut, *regamma_lut;
	const struct drm_color_lut *shaper_lut, *lut3d;
	uint32_t degamma_size, regamma_size;
	uint32_t lut3d_size, shaper_size;
	bool has_regamma, has_degamma;
	enum dc_transfer_func_predefined tf;
	bool has_lut3d, has_shaper_lut;
	bool is_legacy;
	int r;

	r = amdgpu_dm_verify_lut_sizes(&crtc->base);
	if (r)
		return r;

	r =  amdgpu_dm_verify_lut3d_size(adev, &crtc->base);
	if (r)
		return r;

	degamma_lut = __extract_blob_lut(crtc->base.degamma_lut, &degamma_size);
	shaper_lut = __extract_blob_lut(crtc->base.shaper_lut, &shaper_size);
	lut3d = __extract_blob_lut(crtc->base.lut3d, &lut3d_size);
	regamma_lut = __extract_blob_lut(crtc->base.gamma_lut, &regamma_size);

	has_degamma =
		degamma_lut && !__is_lut_linear(degamma_lut, degamma_size);

	has_shaper_lut = shaper_lut != NULL;

	has_lut3d = lut3d != NULL;

	has_regamma =
		regamma_lut && !__is_lut_linear(regamma_lut, regamma_size);

	is_legacy = regamma_size == MAX_COLOR_LEGACY_LUT_ENTRIES;

	tf = drm_tf_to_dc_tf(crtc->base.regamma_tf);

	/* Reset all adjustments. */
	crtc->cm_has_degamma = false;
	crtc->cm_is_degamma_srgb = false;

	/* Setup regamma and degamma. */
	if (is_legacy) {
		/*
		 * Legacy regamma forces us to use the sRGB RGM as a base.
		 * This also means we can't use linear DGM since DGM needs
		 * to use sRGB as a base as well, resulting in incorrect CRTC
		 * DGM and CRTC CTM.
		 *
		 * TODO: Just map this to the standard regamma interface
		 * instead since this isn't really right. One of the cases
		 * where this setup currently fails is trying to do an
		 * inverse color ramp in legacy userspace.
		 */
		crtc->cm_is_degamma_srgb = true;
		stream->out_transfer_func->type = TF_TYPE_DISTRIBUTED_POINTS;
		stream->out_transfer_func->tf = TRANSFER_FUNCTION_SRGB;

		/* Note: even if we pass has_rom as parameter here, we never
		 * actually use ROM because the color module only takes the ROM
		 * path if transfer_func->type == PREDEFINED.
		 *
		 * See more in mod_color_calculate_regamma_params()
		 */
		r = __set_legacy_tf(stream->out_transfer_func, regamma_lut,
				    regamma_size, has_rom);
		if (r)
			return r;
	} else {
		if (has_lut3d) {
			/* enable 3D LUT only for DRM atomic regamma */
			shaper_size = has_shaper_lut ? shaper_size : 0;

			r = amdgpu_dm_atomic_shaper_lut3d(adev->dm.dc, ctx, stream,
							  shaper_lut, shaper_size,
							  lut3d, lut3d_size);

			if (r) {
				DRM_DEBUG_DRIVER("Failed to set shaper and 3D LUT\n");
				return r;
			}
		} else {
			r = amdgpu_dm_atomic_shaper_lut3d(adev->dm.dc, ctx, stream,
							  NULL, 0,
							  NULL, 0);

			if (r) {
				DRM_DEBUG_DRIVER("Failed to unset shaper and 3D LUT\n");
				return r;
			}
		}
		/* Note: OGAM is disabled if 3D LUT is successfully programmed.
		 * See params and set_output_gamma in
		 * dcn30_set_output_transfer_func()
		 */
		regamma_size = has_regamma ? regamma_size : 0;
		r = amdgpu_dm_set_atomic_regamma(stream, regamma_lut, regamma_size, has_rom, tf);
		if (r)
			return r;
	}

	/*
	 * CRTC DGM goes into DGM LUT. It would be nice to place it
	 * into the RGM since it's a more featured block but we'd
	 * have to place the CTM in the OCSC in that case.
	 */
	crtc->cm_has_degamma = has_degamma;

	/* Setup CRTC CTM. */
	if (crtc->base.ctm) {
		ctm = (struct drm_color_ctm *)crtc->base.ctm->data;

		/*
		 * Gamut remapping must be used for gamma correction
		 * since it comes before the regamma correction.
		 *
		 * OCSC could be used for gamma correction, but we'd need to
		 * blend the adjustments together with the required output
		 * conversion matrix - so just use the gamut remap block
		 * for now.
		 */
		__drm_ctm_to_dc_matrix(ctm, stream->gamut_remap_matrix.matrix);

		stream->gamut_remap_matrix.enable_remap = true;
		stream->csc_color_matrix.enable_adjustment = false;
	} else {
		/* Bypass CTM. */
		stream->gamut_remap_matrix.enable_remap = false;
		stream->csc_color_matrix.enable_adjustment = false;
	}

	return 0;
}

static enum dc_transfer_func_predefined drm_tf_to_dc_tf(enum drm_transfer_function drm_tf)
{
	switch (drm_tf)
	{
	default:
	case DRM_TRANSFER_FUNCTION_DEFAULT: return TRANSFER_FUNCTION_LINEAR;
	case DRM_TRANSFER_FUNCTION_SRGB:	return TRANSFER_FUNCTION_SRGB;

	case DRM_TRANSFER_FUNCTION_BT709:	return TRANSFER_FUNCTION_BT709;
	case DRM_TRANSFER_FUNCTION_PQ:		return TRANSFER_FUNCTION_PQ;
	case DRM_TRANSFER_FUNCTION_LINEAR:	return TRANSFER_FUNCTION_LINEAR;
	case DRM_TRANSFER_FUNCTION_UNITY:	return TRANSFER_FUNCTION_UNITY;
	case DRM_TRANSFER_FUNCTION_HLG:		return TRANSFER_FUNCTION_HLG;
	case DRM_TRANSFER_FUNCTION_GAMMA22:	return TRANSFER_FUNCTION_GAMMA22;
	case DRM_TRANSFER_FUNCTION_GAMMA24:	return TRANSFER_FUNCTION_GAMMA24;
	case DRM_TRANSFER_FUNCTION_GAMMA26:	return TRANSFER_FUNCTION_GAMMA26;
	}
}

/**
 * amdgpu_dm_update_plane_color_mgmt: Maps DRM color management to DC plane.
 * @crtc: amdgpu_dm crtc state
 * @plane_state: DRM plane
 * @dc_plane_state: target DC surface
 *
 * Update the underlying dc_stream_state's input transfer function (ITF) in
 * preparation for hardware commit. The transfer function used depends on
 * the preparation done on the stream for color management.
 *
 * Returns:
 * 0 on success. -ENOMEM if mem allocation fails.
 */
int amdgpu_dm_update_plane_color_mgmt(struct dm_crtc_state *crtc,
					  struct drm_plane_state *plane_state,
				      struct dc_plane_state *dc_plane_state)
{
	const struct drm_color_lut *degamma_lut;
	enum dc_transfer_func_predefined tf = TRANSFER_FUNCTION_SRGB;
	enum drm_transfer_function drm_tf = DRM_TRANSFER_FUNCTION_DEFAULT;
	uint32_t degamma_size;
	bool has_degamma;
	int r;

	degamma_lut = __extract_blob_lut(plane_state->degamma_lut, &degamma_size);

	has_degamma =
		degamma_lut && !__is_lut_linear(degamma_lut, degamma_size);

	drm_tf = plane_state->degamma_tf;
	dc_plane_state->hdr_mult = dc_fixpt_from_s3132(plane_state->hdr_mult);

	/* Get the correct base transfer function for implicit degamma. */
	switch (dc_plane_state->format) {
	case SURFACE_PIXEL_FORMAT_VIDEO_420_YCbCr:
	case SURFACE_PIXEL_FORMAT_VIDEO_420_YCrCb:
		/* DC doesn't have a transfer function for BT601 specifically. */
		tf = TRANSFER_FUNCTION_BT709;
		break;
	default:
		break;
	}

	if (has_degamma) {
		ASSERT(degamma_size == MAX_COLOR_LUT_ENTRIES);

		dc_plane_state->in_transfer_func->type =
			TF_TYPE_DISTRIBUTED_POINTS;

		dc_plane_state->in_transfer_func->tf =
			drm_tf_to_dc_tf(drm_tf);

		r = __set_input_tf(dc_plane_state->in_transfer_func,
				   degamma_lut, degamma_size);
		if (r)
			return r;
	} else if (drm_tf != DRM_TRANSFER_FUNCTION_DEFAULT) {
		dc_plane_state->in_transfer_func->type =
			TF_TYPE_PREDEFINED;

		dc_plane_state->in_transfer_func->tf =
			drm_tf_to_dc_tf(drm_tf);

		if (!mod_color_calculate_degamma_params(NULL,
			    dc_plane_state->in_transfer_func, NULL, false))
			return -ENOMEM;
	} else if (crtc->cm_has_degamma) {
		degamma_lut = __extract_blob_lut(crtc->base.degamma_lut,
						 &degamma_size);
		ASSERT(degamma_size == MAX_COLOR_LUT_ENTRIES);

		dc_plane_state->in_transfer_func->type =
			TF_TYPE_DISTRIBUTED_POINTS;

		/*
		 * This case isn't fully correct, but also fairly
		 * uncommon. This is userspace trying to use a
		 * legacy gamma LUT + atomic degamma LUT
		 * at the same time.
		 *
		 * Legacy gamma requires the input to be in linear
		 * space, so that means we need to apply an sRGB
		 * degamma. But color module also doesn't support
		 * a user ramp in this case so the degamma will
		 * be lost.
		 *
		 * Even if we did support it, it's still not right:
		 *
		 * Input -> CRTC DGM -> sRGB DGM -> CRTC CTM ->
		 * sRGB RGM -> CRTC RGM -> Output
		 *
		 * The CSC will be done in the wrong space since
		 * we're applying an sRGB DGM on top of the CRTC
		 * DGM.
		 *
		 * TODO: Don't use the legacy gamma interface and just
		 * map these to the atomic one instead.
		 */
		if (crtc->cm_is_degamma_srgb)
			dc_plane_state->in_transfer_func->tf = tf;
		else
			dc_plane_state->in_transfer_func->tf =
				TRANSFER_FUNCTION_LINEAR;

		r = __set_input_tf(dc_plane_state->in_transfer_func,
				   degamma_lut, degamma_size);
		if (r)
			return r;
	} else if (crtc->cm_is_degamma_srgb) {
		/*
		 * For legacy gamma support we need the regamma input
		 * in linear space. Assume that the input is sRGB.
		 */
		dc_plane_state->in_transfer_func->type = TF_TYPE_PREDEFINED;
		dc_plane_state->in_transfer_func->tf = tf;

		if (tf != TRANSFER_FUNCTION_SRGB &&
		    !mod_color_calculate_degamma_params(NULL,
			    dc_plane_state->in_transfer_func, NULL, false))
			return -ENOMEM;
	} else {
		/* ...Otherwise we can just bypass the DGM block. */
		dc_plane_state->in_transfer_func->type = TF_TYPE_BYPASS;
		dc_plane_state->in_transfer_func->tf = TRANSFER_FUNCTION_LINEAR;
	}

	return 0;
}
