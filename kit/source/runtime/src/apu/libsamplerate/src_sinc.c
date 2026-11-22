/*
** Copyright (c) 2002-2021, Erik de Castro Lopo <erikd@mega-nerd.com>
** All rights reserved.
**
** This code is released under 2-clause BSD license. Please see the
** file at : https://github.com/libsndfile/libsamplerate/blob/master/COPYING
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "common.h"

#define	SINC_MAGIC_MARKER	MAKE_MAGIC (' ', 's', 'i', 'n', 'c', ' ')

/*========================================================================================
*/

#define MAKE_INCREMENT_T(x) 	((increment_t) (x))

#define	SHIFT_BITS				12
#define	FP_ONE					((double) (((increment_t) 1) << SHIFT_BITS))
#define	INV_FP_ONE				(1.0 / FP_ONE)

/* Customixe max channls from Kconfig. */
#ifndef CONFIG_CHAN_NR
#define MAX_CHANNELS			128
#else
#define MAX_CHANNELS 			CONFIG_CHAN_NR
#endif

/*========================================================================================
*/

typedef int32_t increment_t ;
typedef float	coeff_t ;
typedef int _CHECK_SHIFT_BITS[2 * (SHIFT_BITS < sizeof (increment_t) * 8 - 1) - 1]; /* sanity check. */

#ifdef ENABLE_SINC_FAST_CONVERTER
  #include "fastest_coeffs.h"
#endif
#ifdef ENABLE_SINC_MEDIUM_CONVERTER
  #include "mid_qual_coeffs.h"
#endif
#ifdef ENABLE_SINC_BEST_CONVERTER
  #include "high_qual_coeffs.h"
#endif

typedef struct
{	int		sinc_magic_marker ;

	long	in_count, in_used ;
	long	out_count, out_gen ;

	int		coeff_half_len, index_inc ;

	double	src_ratio, input_index ;

	coeff_t const	*coeffs ;

	int		b_current, b_end, b_real_end, b_len ;

	/* Sure hope noone does more than 128 channels at once. */
	double left_calc [MAX_CHANNELS], right_calc [MAX_CHANNELS] ;

	float	*buffer ;
} SINC_FILTER ;

static SRC_ERROR sinc_multichan_vari_process (SRC_STATE *state, SRC_DATA *data) ;
static SRC_ERROR sinc_hex_vari_process (SRC_STATE *state, SRC_DATA *data) ;
static SRC_ERROR sinc_quad_vari_process (SRC_STATE *state, SRC_DATA *data) ;
static SRC_ERROR sinc_stereo_vari_process (SRC_STATE *state, SRC_DATA *data) ;
static SRC_ERROR sinc_mono_vari_process (SRC_STATE *state, SRC_DATA *data) ;

static SRC_ERROR prepare_data (SINC_FILTER *filter, int channels, SRC_DATA *data, int half_filter_chan_len) WARN_UNUSED ;

static void sinc_reset (SRC_STATE *state) ;
static SRC_STATE *sinc_copy (SRC_STATE *state) ;
static void sinc_close (SRC_STATE *state) ;

static SRC_STATE_VT sinc_multichan_state_vt =
{
	sinc_multichan_vari_process,
	sinc_multichan_vari_process,
	sinc_reset,
	sinc_copy,
	sinc_close
} ;

static SRC_STATE_VT sinc_hex_state_vt =
{
	sinc_hex_vari_process,
	sinc_hex_vari_process,
	sinc_reset,
	sinc_copy,
	sinc_close
} ;

static SRC_STATE_VT sinc_quad_state_vt =
{
	sinc_quad_vari_process,
	sinc_quad_vari_process,
	sinc_reset,
	sinc_copy,
	sinc_close
} ;

static SRC_STATE_VT sinc_stereo_state_vt =
{
	sinc_stereo_vari_process,
	sinc_stereo_vari_process,
	sinc_reset,
	sinc_copy,
	sinc_close
} ;

static SRC_STATE_VT sinc_mono_state_vt =
{
	sinc_mono_vari_process,
	sinc_mono_vari_process,
	sinc_reset,
	sinc_copy,
	sinc_close
} ;

static inline increment_t
double_to_fp (double x)
{	return (increment_t) (lrint ((x) * FP_ONE)) ;
} /* double_to_fp */

static inline increment_t
int_to_fp (int x)
{	return (((increment_t) (x)) << SHIFT_BITS) ;
} /* int_to_fp */

static inline int
fp_to_int (increment_t x)
{	return (((x) >> SHIFT_BITS)) ;
} /* fp_to_int */

static inline increment_t
fp_fraction_part (increment_t x)
{	return ((x) & ((((increment_t) 1) << SHIFT_BITS) - 1)) ;
} /* fp_fraction_part */

static inline double
fp_to_double (increment_t x)
{	return fp_fraction_part (x) * INV_FP_ONE ;
} /* fp_to_double */

static inline int
int_div_ceil (int divident, int divisor) /* == (int) ceil ((float) divident / divisor) */
{	assert (divident >= 0 && divisor > 0) ; /* For positive numbers only */
	return (divident + (divisor - 1)) / divisor ;
}

/*----------------------------------------------------------------------------------------
*/

LIBSAMPLERATE_DLL_PRIVATE const char*
sinc_get_name (int src_enum)
{
	switch (src_enum)
	{	case SRC_SINC_BEST_QUALITY :
			return "Best Sinc Interpolator" ;

		case SRC_SINC_MEDIUM_QUALITY :
			return "Medium Sinc Interpolator" ;

		case SRC_SINC_FASTEST :
			return "Fastest Sinc Interpolator" ;

		default: break ;
		} ;

	return NULL ;
} /* sinc_get_descrition */

LIBSAMPLERATE_DLL_PRIVATE const char*
sinc_get_description (int src_enum)
{
	switch (src_enum)
	{	case SRC_SINC_FASTEST :
			return "Band limited sinc interpolation, fastest, 97dB SNR, 80% BW." ;

		case SRC_SINC_MEDIUM_QUALITY :
			return "Band limited sinc interpolation, medium quality, 121dB SNR, 90% BW." ;

		case SRC_SINC_BEST_QUALITY :
			return "Band limited sinc interpolation, best quality, 144dB SNR, 96% BW." ;

		default :
			break ;
		} ;

	return NULL ;
} /* sinc_get_descrition */

static SINC_FILTER *
sinc_filter_new (int converter_type, int channels)
{
	assert (converter_type == SRC_SINC_FASTEST ||
		converter_type == SRC_SINC_MEDIUM_QUALITY ||
		converter_type == SRC_SINC_BEST_QUALITY) ;
	assert (channels > 0 && channels <= MAX_CHANNELS) ;

	SINC_FILTER *priv = (SINC_FILTER *) calloc (1, sizeof (SINC_FILTER)) ;
	if (priv)
	{
		priv->sinc_magic_marker = SINC_MAGIC_MARKER ;
		switch (converter_type)
		{
#ifdef ENABLE_SINC_FAST_CONVERTER
		case SRC_SINC_FASTEST :
			priv->coeffs = fastest_coeffs.coeffs ;
			priv->coeff_half_len = ARRAY_LEN (fastest_coeffs.coeffs) - 2 ;
			priv->index_inc = fastest_coeffs.increment ;
			break ;
#endif
#ifdef ENABLE_SINC_MEDIUM_CONVERTER
		case SRC_SINC_MEDIUM_QUALITY :
			priv->coeffs = slow_mid_qual_coeffs.coeffs ;
			priv->coeff_half_len = ARRAY_LEN (slow_mid_qual_coeffs.coeffs) - 2 ;
			priv->index_inc = slow_mid_qual_coeffs.increment ;
			break ;
#endif
#ifdef ENABLE_SINC_BEST_CONVERTER
		case SRC_SINC_BEST_QUALITY :
			priv->coeffs = slow_high_qual_coeffs.coeffs ;
			priv->coeff_half_len = ARRAY_LEN (slow_high_qual_coeffs.coeffs) - 2 ;
			priv->index_inc = slow_high_qual_coeffs.increment ;
			break ;
#endif
		}

		priv->b_len = 3 * (int) lrint ((priv->coeff_half_len + 2.0) / priv->index_inc * SRC_MAX_RATIO + 1) ;
		priv->b_len = MAX (priv->b_len, 4096) ;
		priv->b_len *= channels ;
		priv->b_len += 1 ; // There is a <= check against samples_in_hand requiring a buffer bigger than the calculation above


		priv->buffer = (float *) calloc (priv->b_len + channels, sizeof (float)) ;
		if (!priv->buffer)
		{
			free (priv) ;
			priv = NULL ;
		}
	}

	return priv ;
}

LIBSAMPLERATE_DLL_PRIVATE SRC_STATE *
sinc_state_new (int converter_type, int channels, SRC_ERROR *error)
{
	assert (converter_type == SRC_SINC_FASTEST ||
		converter_type == SRC_SINC_MEDIUM_QUALITY ||
		converter_type == SRC_SINC_BEST_QUALITY) ;
	assert (channels > 0) ;
	assert (error != NULL) ;

	if (channels > MAX_CHANNELS)
	{
		*error = SRC_ERR_BAD_CHANNEL_COUNT ;
		return NULL ;
	}

	SRC_STATE *state = (SRC_STATE *) calloc (1, sizeof (SRC_STATE)) ;
	if (!state)
	{
		*error = SRC_ERR_MALLOC_FAILED ;
		return NULL ;
	}

	state->channels = channels ;
	state->mode = SRC_MODE_PROCESS ;

	if (state->channels == 1)
		state->vt = &sinc_mono_state_vt ;
	else if (state->channels == 2)
		state->vt = &sinc_stereo_state_vt ;
	else if (state->channels == 4)
		state->vt = &sinc_quad_state_vt ;
	else if (state->channels == 6)
		state->vt = &sinc_hex_state_vt ;
	else
		state->vt = &sinc_multichan_state_vt ;

	state->private_data = sinc_filter_new (converter_type, state->channels) ;
	if (!state->private_data)
	{
		free (state) ;
		*error = SRC_ERR_MALLOC_FAILED ;
		return NULL ;
	}

	sinc_reset (state) ;

	*error = SRC_ERR_NO_ERROR ;

	return state ;
}

static void
sinc_reset (SRC_STATE *state)
{	SINC_FILTER *filter ;

	filter = (SINC_FILTER*) state->private_data ;
	if (filter == NULL)
		return ;

	filter->b_current = filter->b_end = 0 ;
	filter->b_real_end = -1 ;

	filter->src_ratio = filter->input_index = 0.0 ;

	memset (filter->buffer, 0, filter->b_len * sizeof (filter->buffer [0])) ;

	/* Set this for a sanity check */
	memset (filter->buffer + filter->b_len, 0xAA, state->channels * sizeof (filter->buffer [0])) ;
} /* sinc_reset */

static SRC_STATE *
sinc_copy (SRC_STATE *state)
{
	assert (state != NULL) ;

	if (state->private_data == NULL)
		return NULL ;

	SRC_STATE *to = (SRC_STATE *) calloc (1, sizeof (SRC_STATE)) ;
	if (!to)
		return NULL ;
	memcpy (to, state, sizeof (SRC_STATE)) ;


	SINC_FILTER* from_filter = (SINC_FILTER*) state->private_data ;
	SINC_FILTER *to_filter = (SINC_FILTER *) calloc (1, sizeof (SINC_FILTER)) ;
	if (!to_filter)
	{
		free (to) ;
		return NULL ;
	}
	memcpy (to_filter, from_filter, sizeof (SINC_FILTER)) ;
	to_filter->buffer = (float *) malloc (sizeof (float) * (from_filter->b_len + state->channels)) ;
	if (!to_filter->buffer)
	{
		free (to) ;
		free (to_filter) ;
		return NULL ;
	}
	memcpy (to_filter->buffer, from_filter->buffer, sizeof (float) * (from_filter->b_len + state->channels)) ;

	to->private_data = to_filter ;

	return to ;
} /* sinc_copy */

/*========================================================================================
**	Beware all ye who dare pass this point. There be dragons here.
*/

static inline double
calc_output_single (SINC_FILTER *filter, increment_t increment, increment_t start_filter_index)
{	double		fraction, left, right, icoeff ;
	increment_t	filter_index, max_filter_index ;
	int			data_index, coeff_count, indx ;

	/* Convert input parameters into fixed point. */
	max_filter_index = int_to_fp (filter->coeff_half_len) ;

	/* First apply the left half of the filter. */
	filter_index = start_filter_index ;
	coeff_count = (max_filter_index - filter_index) / increment ;
	filter_index = filter_index + coeff_count * increment ;
	data_index = filter->b_current - coeff_count ;

	if (data_index < 0) /* Avoid underflow access to filter->buffer. */
	{	int steps = -data_index ;
		/* If the assert triggers we would have to take care not to underflow/overflow */
		assert (steps <= int_div_ceil (filter_index, increment)) ;
		filter_index -= increment * steps ;
		data_index += steps ;
	}
	left = 0.0 ;
	while (filter_index >= MAKE_INCREMENT_T (0))
	{	fraction = fp_to_double (filter_index) ;
		indx = fp_to_int (filter_index) ;
		assert (indx >= 0 && indx + 1 < filter->coeff_half_len + 2) ;
		icoeff = filter->coeffs [indx] + fraction * (filter->coeffs [indx + 1] - filter->coeffs [indx]) ;
		assert (data_index >= 0 && data_index < filter->b_len) ;
		assert (data_index < filter->b_end) ;
		left += icoeff * filter->buffer [data_index] ;

		filter_index -= increment ;
		data_index = data_index + 1 ;
		} ;

	/* Now apply the right half of the filter. */
	filter_index = increment - start_filter_index ;
	coeff_count = (max_filter_index - filter_index) / increment ;
	filter_index = filter_index + coeff_count * increment ;
	data_index = filter->b_current + 1 + coeff_count ;

	right = 0.0 ;
	do
	{	fraction = fp_to_double (filter_index) ;
		indx = fp_to_int (filter_index) ;
		assert (indx < filter->coeff_half_len + 2) ;
		icoeff = filter->coeffs [indx] + fraction * (filter->coeffs [indx + 1] - filter->coeffs [indx]) ;
		assert (data_index >= 0 && data_index < filter->b_len) ;
		assert (data_index < filter->b_end) ;
		right += icoeff * filter->buffer [data_index] ;

		filter_index -= increment ;
		data_index = data_index - 1 ;
		}
	while (filter_index > MAKE_INCREMENT_T (0)) ;

	return (left + right) ;
} /* calc_output_single */

static SRC_ERROR
sinc_mono_vari_process (SRC_STATE *state, SRC_DATA *data)
{	SINC_FILTER *filter ;
	double		input_index, src_ratio, count, float_increment, terminate, rem ;
	increment_t	increment, start_filter_index ;
	int			half_filter_chan_len, samples_in_hand ;

	if (state->private_data == NULL)
		return SRC_ERR_NO_PRIVATE ;

	filter = (SINC_FILTER*) state->private_data ;

	/* If there is not a problem, this will be optimised out. */
	if (sizeof (filter->buffer [0]) != sizeof (data->data_in [0]))
		return SRC_ERR_SIZE_INCOMPATIBILITY ;

	filter->in_count = data->input_frames * state->channels ;
	filter->out_count = data->output_frames * state->channels ;
	filter->in_used = filter->out_gen = 0 ;

	src_ratio = state->last_ratio ;

	if (is_bad_src_ratio (src_ratio))
		return SRC_ERR_BAD_INTERNAL_STATE ;

	/* Check the sample rate ratio wrt the buffer len. */
	count = (filter->coeff_half_len + 2.0) / filter->index_inc ;
	if (MIN (state->last_ratio, data->src_ratio) < 1.0)
		count /= MIN (state->last_ratio, data->src_ratio) ;

	/* Maximum coefficientson either side of center point. */
	half_filter_chan_len = state->channels * (int) (lrint (count) + 1) ;

	input_index = state->last_position ;

	rem = fmod_one (input_index) ;
	filter->b_current = (filter->b_current + state->channels * lrint (input_index - rem)) % filter->b_len ;
	input_index = rem ;

	terminate = 1.0 / src_ratio + 1e-20 ;

	/* Main processing loop. */
	while (filter->out_gen < filter->out_count)
	{
		/* Need to reload buffer? */
		samples_in_hand = (filter->b_end - filter->b_current + filter->b_len) % filter->b_len ;

		if (samples_in_hand <= half_filter_chan_len)
		{	if ((state->error = prepare_data (filter, state->channels, data, half_filter_chan_len)) != 0)
				return state->error ;

			samples_in_hand = (filter->b_end - filter->b_current + filter->b_len) % filter->b_len ;
			if (samples_in_hand <= half_filter_chan_len)
				break ;
			} ;

		/* This is the termination condition. */
		if (filter->b_real_end >= 0)
		{	if (filter->b_current + input_index + terminate > filter->b_real_end)
				break ;
			} ;

		if (filter->out_count > 0 && fabs (state->last_ratio - data->src_ratio) > 1e-10)
			src_ratio = state->last_ratio + filter->out_gen * (data->src_ratio - state->last_ratio) / filter->out_count ;

		float_increment = filter->index_inc * (src_ratio < 1.0 ? src_ratio : 1.0) ;
		increment = double_to_fp (float_increment) ;

		start_filter_index = double_to_fp (input_index * float_increment) ;

		data->data_out [filter->out_gen] = (float) ((float_increment / filter->index_inc) *
										calc_output_single (filter, increment, start_filter_index)) ;
		filter->out_gen ++ ;

		/* Figure out the next index. */
		input_index += 1.0 / src_ratio ;
		rem = fmod_one (input_index) ;

		filter->b_current = (filter->b_current + state->channels * lrint (input_index - rem)) % filter->b_len ;
		input_index = rem ;
		} ;

	state->last_position = input_index ;

	/* Save current ratio rather then target ratio. */
	state->last_ratio = src_ratio ;

	data->input_frames_used = filter->in_used / state->channels ;
	data->output_frames_gen = filter->out_gen / state->channels ;

	return SRC_ERR_NO_ERROR ;
} /* sinc_mono_vari_process */

static inline void
calc_output_stereo (SINC_FILTER *filter, int channels, increment_t increment, increment_t start_filter_index, double scale, float * output)
{	double		fraction, left [2], right [2], icoeff ;
	increment_t	filter_index, max_filter_index ;
	int			data_index, coeff_count, indx ;

	/* Convert input parameters into fixed point. */
	max_filter_index = int_to_fp (filter->coeff_half_len) ;

	/* First apply the left half of the filter. */
	filter_index = start_filter_index ;
	coeff_count = (max_filter_index - filter_index) / increment ;
	filter_index = filter_index + coeff_count * increment ;
	data_index = filter->b_current - channels * coeff_count ;

	if (data_index < 0) /* Avoid underflow access to filter->buffer. */
	{	int steps = int_div_ceil (-data_index, 2) ;
		/* If the assert triggers we would have to take care not to underflow/overflow */
		assert (steps <= int_div_ceil (filter_index, increment)) ;
		filter_index -= increment * steps ;
		data_index += steps * 2;
	}
	left [0] = left [1] = 0.0 ;
	while (filter_index >= MAKE_INCREMENT_T (0))
	{	fraction = fp_to_double (filter_index) ;
		indx = fp_to_int (filter_index) ;
		assert (indx >= 0 && indx + 1 < filter->coeff_half_len + 2) ;
		icoeff = filter->coeffs [indx] + fraction * (filter->coeffs [indx + 1] - filter->coeffs [indx]) ;
		assert (data_index >= 0 && data_index + 1 < filter->b_len) ;
		assert (data_index + 1 < filter->b_end) ;
		for (int ch = 0; ch < 2; ch++)
			left [ch] += icoeff * filter->buffer [data_index + ch] ;

		filter_index -= increment ;
		data_index = data_index + 2 ;
		} ;

	/* Now apply the right half of the filter. */
	filter_index = increment - start_filter_index ;
	coeff_count = (max_filter_index - filter_index) / increment ;
	filter_index = filter_index + coeff_count * increment ;
	data_index = filter->b_current + channels * (1 + coeff_count) ;

	right [0] = right [1] = 0.0 ;
	do
	{	fraction = fp_to_double (filter_index) ;
		indx = fp_to_int (filter_index) ;
		assert (indx >= 0 && indx + 1 < filter->coeff_half_len + 2) ;
		icoeff = filter->coeffs [indx] + fraction * (filter->coeffs [indx + 1] - filter->coeffs [indx]) ;
		assert (data_index >= 0 && data_index + 1 < filter->b_len) ;
		assert (data_index + 1 < filter->b_end) ;
		for (int ch = 0; ch < 2; ch++)
			right [ch] += icoeff * filter->buffer [data_index + ch] ;

		filter_index -= increment ;
		data_index = data_index - 2 ;
		}
	while (filter_index > MAKE_INCREMENT_T (0)) ;

	for (int ch = 0; ch < 2; ch++)
		output [ch] = (float) (scale * (left [ch] + right [ch])) ;
} /* calc_output_stereo */

/* PL-T2-6 (MA2 recomp): bit-exact fast stereo kernel, RECOMP_APU_SRC=ref|simd|auto|verify.
 * Same taps, same per-accumulator operation order as calc_output_stereo above:
 *  - the two channels of one half are one packed double pair (SSE2 mulpd/addpd are the same IEEE
 *    operations as the scalar mulsd/addsd per lane);
 *  - the left and right halves run in one loop, but each keeps its own accumulator and its own
 *    tap sequence (left: far tap -> centre, while index >= 0; right: do-while index > 0), so
 *    every sum sees exactly the scalar sequence of additions; only independent chains overlap;
 *  - icoeff is the same C expression (float subtract, then double); no FMA (no /arch, /fp:precise).
 * The final (float)(scale * (left + right)) is the same per channel. */
#if defined(_WIN32) && (defined(_M_X64) || defined(_M_IX86))
#define MA2_SINC_SSE2 1
#include <emmintrin.h>
#include <intrin.h>

/* x / inc for x >= 0 and inc a power of two is x >> log2(inc) exactly; anything else divides. */
static inline int
sinc_div_inc (increment_t x, increment_t inc, int sh)
{	return (sh >= 0 && x >= 0) ? (int) (x >> sh) : (int) (x / inc) ;
}

static inline __m128d
sinc_tap_pair (const SINC_FILTER *filter, increment_t filter_index, int data_index)
{	double fraction = fp_to_double (filter_index) ;
	int indx = fp_to_int (filter_index) ;
	double icoeff = filter->coeffs [indx] + fraction * (filter->coeffs [indx + 1] - filter->coeffs [indx]) ;
	__m128d b = _mm_cvtps_pd (_mm_castpd_ps (_mm_load_sd ((const double *) (filter->buffer + data_index)))) ;
	return _mm_mul_pd (_mm_set1_pd (icoeff), b) ;
}

static inline void
calc_output_stereo_fast (SINC_FILTER *filter, increment_t increment, increment_t start_filter_index, double scale, float * output)
{	increment_t	fl, fr, max_filter_index ;
	int			dl, dr, coeff_count, la, ra ;
	__m128d		accl, accr, out ;

	int		sh = -1 ;
	unsigned long bit ;
	if (increment > 0 && (increment & (increment - 1)) == 0 && _BitScanForward (&bit, (unsigned long) increment)) sh = (int) bit ;

	max_filter_index = int_to_fp (filter->coeff_half_len) ;

	fl = start_filter_index ;
	coeff_count = sinc_div_inc (max_filter_index - fl, increment, sh) ;
	fl = fl + coeff_count * increment ;
	dl = filter->b_current - 2 * coeff_count ;
	if (dl < 0)
	{	int steps = int_div_ceil (-dl, 2) ;
		fl -= increment * steps ;
		dl += steps * 2 ;
		} ;

	fr = increment - start_filter_index ;
	coeff_count = sinc_div_inc (max_filter_index - fr, increment, sh) ;
	fr = fr + coeff_count * increment ;
	dr = filter->b_current + 2 * (1 + coeff_count) ;

	accl = _mm_setzero_pd () ;
	accr = _mm_setzero_pd () ;
	la = fl >= MAKE_INCREMENT_T (0) ;
	ra = 1 ;
	if (fp_fraction_part (increment) == 0)
	{	/* Ratio >= 1 (increment = index_inc exactly): every tap of a half has the same fraction
		 * bits, so fp_to_double gives the same double for each, and the integer index steps by
		 * increment >> SHIFT_BITS. Same values, same operations, fewer of them. */
		const coeff_t *c = filter->coeffs ;
		const float *buf = filter->buffer ;
		int step = fp_to_int (increment) ;
		double fracl = fp_to_double (fl), fracr = fp_to_double (fr) ;
		int il = fp_to_int (fl), ir = fp_to_int (fr) ;
		/* Tap counts: left while fl - k*inc >= 0, right do-while fr - k*inc > 0. */
		int nl = la ? sinc_div_inc (fl, increment, sh) + 1 : 0 ;
		int nr = fr > 0 ? sinc_div_inc (fr - 1, increment, sh) + 1 : 1 ;
		int k, both = nl < nr ? nl : nr ;
		/* Both halves' icoeff in one packed pair: lane 0 = left, lane 1 = right. Per lane it is
		 * the scalar's float subtract c[i+1]-c[i], then (double)c[i] + fraction * (double)diff. */
		__m128d frac2 = _mm_set_pd (fracr, fracl) ;
		for (k = 0 ; k < both ; k++)
		{	__m128 pl = _mm_castpd_ps (_mm_load_sd ((const double *) (c + il))) ;	/* c[il], c[il+1] */
			__m128 pr = _mm_castpd_ps (_mm_load_sd ((const double *) (c + ir))) ;	/* c[ir], c[ir+1] */
			__m128 q = _mm_unpacklo_ps (pl, pr) ;								/* c0l, c0r, c1l, c1r */
			__m128 diff = _mm_sub_ps (_mm_movehl_ps (q, q), q) ;				/* c1l-c0l, c1r-c0r */
			__m128d ic = _mm_add_pd (_mm_cvtps_pd (q), _mm_mul_pd (frac2, _mm_cvtps_pd (diff))) ;
			__m128d bl = _mm_cvtps_pd (_mm_castpd_ps (_mm_load_sd ((const double *) (buf + dl)))) ;
			__m128d br = _mm_cvtps_pd (_mm_castpd_ps (_mm_load_sd ((const double *) (buf + dr)))) ;
			accl = _mm_add_pd (accl, _mm_mul_pd (_mm_unpacklo_pd (ic, ic), bl)) ;
			accr = _mm_add_pd (accr, _mm_mul_pd (_mm_unpackhi_pd (ic, ic), br)) ;
			il -= step ; dl += 2 ; ir -= step ; dr -= 2 ;
			} ;
		for ( ; k < nl ; k++)
		{	double icl = c [il] + fracl * (c [il + 1] - c [il]) ;
			__m128d bl = _mm_cvtps_pd (_mm_castpd_ps (_mm_load_sd ((const double *) (buf + dl)))) ;
			accl = _mm_add_pd (accl, _mm_mul_pd (_mm_set1_pd (icl), bl)) ;
			il -= step ; dl += 2 ;
			} ;
		for (k = both ; k < nr ; k++)
		{	double icr = c [ir] + fracr * (c [ir + 1] - c [ir]) ;
			__m128d br = _mm_cvtps_pd (_mm_castpd_ps (_mm_load_sd ((const double *) (buf + dr)))) ;
			accr = _mm_add_pd (accr, _mm_mul_pd (_mm_set1_pd (icr), br)) ;
			ir -= step ; dr -= 2 ;
			} ;
		la = ra = 0 ;
		} ;
	while (la && ra)
	{	accl = _mm_add_pd (accl, sinc_tap_pair (filter, fl, dl)) ;
		accr = _mm_add_pd (accr, sinc_tap_pair (filter, fr, dr)) ;
		fl -= increment ; dl += 2 ; la = fl >= MAKE_INCREMENT_T (0) ;
		fr -= increment ; dr -= 2 ; ra = fr > MAKE_INCREMENT_T (0) ;
		} ;
	while (la)
	{	accl = _mm_add_pd (accl, sinc_tap_pair (filter, fl, dl)) ;
		fl -= increment ; dl += 2 ; la = fl >= MAKE_INCREMENT_T (0) ;
		} ;
	while (ra)
	{	accr = _mm_add_pd (accr, sinc_tap_pair (filter, fr, dr)) ;
		fr -= increment ; dr -= 2 ; ra = fr > MAKE_INCREMENT_T (0) ;
		} ;

	out = _mm_mul_pd (_mm_set1_pd (scale), _mm_add_pd (accl, accr)) ;
	_mm_storel_pi ((__m64 *) output, _mm_cvtpd_ps (out)) ;
} /* calc_output_stereo_fast */

/* 0 = ref (the scalar kernel), 1 = fast, 2 = verify (both, compare bits, ship ref). SSE2 is the x64
 * baseline, so auto = fast on every CPU this port runs on. */
static volatile long sinc_src_mode = -1 ;
static volatile long long sinc_verify_n, sinc_verify_bad ;
static long sinc_verify_every = 16, sinc_verify_tick ;

static int
sinc_src_mode_get (void)
{	long m = sinc_src_mode ;
	if (m < 0)
	{	const char *e = getenv ("RECOMP_APU_SRC") ;
		m = 1 ; /* default: auto = fast (T2-6 flip; bit-exact, proved by the harness and t2s1) */
		if (e && *e)
		{	if (!strcmp (e, "ref") || !strcmp (e, "0")) m = 0 ;
			else if (!strcmp (e, "verify") || !strcmp (e, "2")) m = 2 ;
			else m = 1 ;
			} ;
		if (m == 2)
		{	const char *n = getenv ("RECOMP_APU_SRC_VERIFY_EVERY") ;
			if (n && atol (n) > 0) sinc_verify_every = atol (n) ;
			} ;
		sinc_src_mode = m ;
		fprintf (stderr, "[APU-SRC] kernel=%s (RECOMP_APU_SRC=%s)\n", m == 1 ? "simd" : m == 2 ? "verify" : "ref", e ? e : "") ;
		} ;
	return (int) m ;
}

/* Called by apu_vp.c's 5 s report; prints only in verify mode. */
void
apu_sinc_verify_report (void)
{	if (sinc_src_mode != 2) return ;
	fprintf (stderr, "[APU-SRC-VERIFY] calls=%lld mismatches=%lld\n", sinc_verify_n, sinc_verify_bad) ;
}

/* Exact replacements used only by the fast loop (proved by the bit-exact harness):
 * lrint -> cvtsd2si (both round by MXCSR; every value here is far inside int range);
 * x % len -> subtract (every dividend here is in [0, 2*len) or a small positive step). */
static inline long
sinc_lrint (double x)
{	return _mm_cvtsd_si32 (_mm_set_sd (x)) ;
}

static inline double
sinc_fmod_one (double x)
{	double res = x - sinc_lrint (x) ;
	if (res < 0.0)
		return res + 1.0 ;
	return res ;
}

static inline increment_t
sinc_double_to_fp (double x)
{	return (increment_t) (sinc_lrint ((x) * FP_ONE)) ;
}

static inline int
sinc_wrap (long x, int len)
{	while (x >= len) x -= len ;
	return (int) x ;
}
#endif /* MA2_SINC_SSE2 */

static SRC_ERROR
sinc_stereo_vari_process_ref (SRC_STATE *state, SRC_DATA *data)
{	SINC_FILTER *filter ;
	double		input_index, src_ratio, count, float_increment, terminate, rem ;
	increment_t	increment, start_filter_index ;
	int			half_filter_chan_len, samples_in_hand ;

	if (state->private_data == NULL)
		return SRC_ERR_NO_PRIVATE ;

	filter = (SINC_FILTER*) state->private_data ;

	/* If there is not a problem, this will be optimised out. */
	if (sizeof (filter->buffer [0]) != sizeof (data->data_in [0]))
		return SRC_ERR_SIZE_INCOMPATIBILITY ;

	filter->in_count = data->input_frames * state->channels ;
	filter->out_count = data->output_frames * state->channels ;
	filter->in_used = filter->out_gen = 0 ;

	src_ratio = state->last_ratio ;

	if (is_bad_src_ratio (src_ratio))
		return SRC_ERR_BAD_INTERNAL_STATE ;

	/* Check the sample rate ratio wrt the buffer len. */
	count = (filter->coeff_half_len + 2.0) / filter->index_inc ;
	if (MIN (state->last_ratio, data->src_ratio) < 1.0)
		count /= MIN (state->last_ratio, data->src_ratio) ;

	/* Maximum coefficientson either side of center point. */
	half_filter_chan_len = state->channels * (int) (lrint (count) + 1) ;

	input_index = state->last_position ;

	rem = fmod_one (input_index) ;
	filter->b_current = (filter->b_current + state->channels * lrint (input_index - rem)) % filter->b_len ;
	input_index = rem ;

	terminate = 1.0 / src_ratio + 1e-20 ;

	/* Main processing loop. */
	while (filter->out_gen < filter->out_count)
	{
		/* Need to reload buffer? */
		samples_in_hand = (filter->b_end - filter->b_current + filter->b_len) % filter->b_len ;

		if (samples_in_hand <= half_filter_chan_len)
		{	if ((state->error = prepare_data (filter, state->channels, data, half_filter_chan_len)) != 0)
				return state->error ;

			samples_in_hand = (filter->b_end - filter->b_current + filter->b_len) % filter->b_len ;
			if (samples_in_hand <= half_filter_chan_len)
				break ;
			} ;

		/* This is the termination condition. */
		if (filter->b_real_end >= 0)
		{	if (filter->b_current + input_index + terminate >= filter->b_real_end)
				break ;
			} ;

		if (filter->out_count > 0 && fabs (state->last_ratio - data->src_ratio) > 1e-10)
			src_ratio = state->last_ratio + filter->out_gen * (data->src_ratio - state->last_ratio) / filter->out_count ;

		float_increment = filter->index_inc * (src_ratio < 1.0 ? src_ratio : 1.0) ;
		increment = double_to_fp (float_increment) ;

		start_filter_index = double_to_fp (input_index * float_increment) ;

		calc_output_stereo (filter, state->channels, increment, start_filter_index, float_increment / filter->index_inc, data->data_out + filter->out_gen) ;
		filter->out_gen += 2 ;

		/* Figure out the next index. */
		input_index += 1.0 / src_ratio ;
		rem = fmod_one (input_index) ;

		filter->b_current = (filter->b_current + state->channels * lrint (input_index - rem)) % filter->b_len ;
		input_index = rem ;
		} ;

	state->last_position = input_index ;

	/* Save current ratio rather then target ratio. */
	state->last_ratio = src_ratio ;

	data->input_frames_used = filter->in_used / state->channels ;
	data->output_frames_gen = filter->out_gen / state->channels ;

	return SRC_ERR_NO_ERROR ;
} /* sinc_stereo_vari_process_ref */

#if defined(MA2_SINC_SSE2)
static SRC_ERROR
sinc_stereo_vari_process_fast (SRC_STATE *state, SRC_DATA *data)
{	SINC_FILTER *filter ;
	double		input_index, src_ratio, count, float_increment, terminate, rem ;
	increment_t	increment, start_filter_index ;
	int			half_filter_chan_len, samples_in_hand ;
	double		inv_of = 0.0, inv_ratio = 0.0 ;

	if (state->private_data == NULL)
		return SRC_ERR_NO_PRIVATE ;

	filter = (SINC_FILTER*) state->private_data ;

	/* If there is not a problem, this will be optimised out. */
	if (sizeof (filter->buffer [0]) != sizeof (data->data_in [0]))
		return SRC_ERR_SIZE_INCOMPATIBILITY ;

	filter->in_count = data->input_frames * state->channels ;
	filter->out_count = data->output_frames * state->channels ;
	filter->in_used = filter->out_gen = 0 ;

	src_ratio = state->last_ratio ;

	if (is_bad_src_ratio (src_ratio))
		return SRC_ERR_BAD_INTERNAL_STATE ;

	/* Check the sample rate ratio wrt the buffer len. */
	count = (filter->coeff_half_len + 2.0) / filter->index_inc ;
	if (MIN (state->last_ratio, data->src_ratio) < 1.0)
		count /= MIN (state->last_ratio, data->src_ratio) ;

	/* Maximum coefficientson either side of center point. */
	half_filter_chan_len = state->channels * (int) (sinc_lrint (count) + 1) ;

	input_index = state->last_position ;

	rem = sinc_fmod_one (input_index) ;
	filter->b_current = sinc_wrap (filter->b_current + state->channels * sinc_lrint (input_index - rem), filter->b_len) ;
	input_index = rem ;

	terminate = 1.0 / src_ratio + 1e-20 ;

	/* Main processing loop. */
	while (filter->out_gen < filter->out_count)
	{
		/* Need to reload buffer? */
		samples_in_hand = sinc_wrap (filter->b_end - filter->b_current + filter->b_len, filter->b_len) ;

		if (samples_in_hand <= half_filter_chan_len)
		{	if ((state->error = prepare_data (filter, state->channels, data, half_filter_chan_len)) != 0)
				return state->error ;

			samples_in_hand = sinc_wrap (filter->b_end - filter->b_current + filter->b_len, filter->b_len) ;
			if (samples_in_hand <= half_filter_chan_len)
				break ;
			} ;

		/* This is the termination condition. */
		if (filter->b_real_end >= 0)
		{	if (filter->b_current + input_index + terminate >= filter->b_real_end)
				break ;
			} ;

		if (filter->out_count > 0 && fabs (state->last_ratio - data->src_ratio) > 1e-10)
			src_ratio = state->last_ratio + filter->out_gen * (data->src_ratio - state->last_ratio) / filter->out_count ;

		float_increment = filter->index_inc * (src_ratio < 1.0 ? src_ratio : 1.0) ;
		increment = sinc_double_to_fp (float_increment) ;

		start_filter_index = sinc_double_to_fp (input_index * float_increment) ;

		calc_output_stereo_fast (filter, increment, start_filter_index, float_increment / filter->index_inc, data->data_out + filter->out_gen) ;
		filter->out_gen += 2 ;

		/* Figure out the next index. */
		if (memcmp (&inv_of, &src_ratio, sizeof (double)))
		{	inv_of = src_ratio ;
			inv_ratio = 1.0 / src_ratio ;
			} ;
		input_index += inv_ratio ;
		rem = sinc_fmod_one (input_index) ;

		filter->b_current = sinc_wrap (filter->b_current + state->channels * sinc_lrint (input_index - rem), filter->b_len) ;
		input_index = rem ;
		} ;

	state->last_position = input_index ;

	/* Save current ratio rather then target ratio. */
	state->last_ratio = src_ratio ;

	data->input_frames_used = filter->in_used / state->channels ;
	data->output_frames_gen = filter->out_gen / state->channels ;

	return SRC_ERR_NO_ERROR ;
} /* sinc_stereo_vari_process_fast */

/* Verify mode: every RECOMP_APU_SRC_VERIFY_EVERY-th call (default 16) runs the fast loop on a clone
 * of the converter with the same input, runs the reference loop on the real one, and compares the
 * outputs, the counters and the whole converter state bit for bit. The reference result ships. */
static int
sinc_state_same (const SRC_STATE *a, const SRC_STATE *b)
{	const SINC_FILTER *fa = (const SINC_FILTER *) a->private_data, *fb = (const SINC_FILTER *) b->private_data ;
	if (memcmp (&a->last_ratio, &b->last_ratio, sizeof (double)) || memcmp (&a->last_position, &b->last_position, sizeof (double)) || a->error != b->error)
		return 0 ;
	if (fa->in_count != fb->in_count || fa->in_used != fb->in_used || fa->out_count != fb->out_count || fa->out_gen != fb->out_gen
		|| fa->b_current != fb->b_current || fa->b_end != fb->b_end || fa->b_real_end != fb->b_real_end || fa->b_len != fb->b_len
		|| memcmp (&fa->input_index, &fb->input_index, sizeof (double)) || memcmp (&fa->src_ratio, &fb->src_ratio, sizeof (double)))
		return 0 ;
	return !memcmp (fa->buffer, fb->buffer, sizeof (float) * (fa->b_len + a->channels)) ;
}

SRC_ERROR
sinc_stereo_vari_process (SRC_STATE *state, SRC_DATA *data)
{	int mode = sinc_src_mode_get () ;
	SRC_STATE *clone ;
	SRC_DATA d2 ;
	SRC_ERROR e1, e2 ;
	float *out2 ;
	int same ;

	if (mode == 1)
		return sinc_stereo_vari_process_fast (state, data) ;
	if (mode != 2 || state->private_data == NULL || data->output_frames <= 0 || ++sinc_verify_tick % sinc_verify_every)
		return sinc_stereo_vari_process_ref (state, data) ;

	clone = sinc_copy (state) ;
	out2 = (float *) malloc (sizeof (float) * 2 * (size_t) data->output_frames) ;
	if (!clone || !out2)
	{	if (clone) sinc_close (clone) ;
		free (out2) ;
		return sinc_stereo_vari_process_ref (state, data) ;
		} ;
	d2 = *data ;
	d2.data_out = out2 ;
	e2 = sinc_stereo_vari_process_fast (clone, &d2) ;
	e1 = sinc_stereo_vari_process_ref (state, data) ;
	same = e1 == e2 && d2.input_frames_used == data->input_frames_used && d2.output_frames_gen == data->output_frames_gen
		&& !memcmp (out2, data->data_out, sizeof (float) * 2 * (size_t) (data->output_frames_gen > 0 ? data->output_frames_gen : 0))
		&& sinc_state_same (state, clone) ;
	sinc_verify_n++ ;
	if (!same && sinc_verify_bad++ < 8)
		fprintf (stderr, "[APU-SRC-VERIFY] mismatch call=%lld err=%d/%d used=%ld/%ld gen=%ld/%ld ratio=%.6f\n", sinc_verify_n,
			(int) e1, (int) e2, data->input_frames_used, d2.input_frames_used, data->output_frames_gen, d2.output_frames_gen, data->src_ratio) ;
	sinc_close (clone) ;
	free (out2) ;
	return e1 ;
} /* sinc_stereo_vari_process */
#else
#if defined(_MSC_VER)
#include <intrin.h>
#endif
void
apu_sinc_verify_report (void)
{	/* The reference kernel is the only implementation on this host. */
}

SRC_ERROR
sinc_stereo_vari_process (SRC_STATE *state, SRC_DATA *data)
{	static volatile long reported ;
#if defined(_MSC_VER)
	int first = !_InterlockedExchange (&reported, 1) ;
#else
	int first = !__atomic_exchange_n (&reported, 1, __ATOMIC_RELAXED) ;
#endif
	if (first)
	{	const char *mode = getenv ("RECOMP_APU_SRC") ;
		if (mode && *mode && strcmp (mode, "ref") && strcmp (mode, "0"))
			fprintf (stderr, "[APU-SRC] %s unavailable on this host; using scalar reference\n", mode) ;
		} ;
	return sinc_stereo_vari_process_ref (state, data) ;
}
#endif

static inline void
calc_output_quad (SINC_FILTER *filter, int channels, increment_t increment, increment_t start_filter_index, double scale, float * output)
{	double		fraction, left [4], right [4], icoeff ;
	increment_t	filter_index, max_filter_index ;
	int			data_index, coeff_count, indx ;

	/* Convert input parameters into fixed point. */
	max_filter_index = int_to_fp (filter->coeff_half_len) ;

	/* First apply the left half of the filter. */
	filter_index = start_filter_index ;
	coeff_count = (max_filter_index - filter_index) / increment ;
	filter_index = filter_index + coeff_count * increment ;
	data_index = filter->b_current - channels * coeff_count ;

	if (data_index < 0) /* Avoid underflow access to filter->buffer. */
	{	int steps = int_div_ceil (-data_index, 4) ;
		/* If the assert triggers we would have to take care not to underflow/overflow */
		assert (steps <= int_div_ceil (filter_index, increment)) ;
		filter_index -= increment * steps ;
		data_index += steps * 4;
	}
	left [0] = left [1] = left [2] = left [3] = 0.0 ;
	while (filter_index >= MAKE_INCREMENT_T (0))
	{	fraction = fp_to_double (filter_index) ;
		indx = fp_to_int (filter_index) ;
		assert (indx >= 0 && indx + 1 < filter->coeff_half_len + 2) ;
		icoeff = filter->coeffs [indx] + fraction * (filter->coeffs [indx + 1] - filter->coeffs [indx]) ;
		assert (data_index >= 0 && data_index + 3 < filter->b_len) ;
		assert (data_index + 3 < filter->b_end) ;
		for (int ch = 0; ch < 4; ch++)
			left [ch] += icoeff * filter->buffer [data_index + ch] ;

		filter_index -= increment ;
		data_index = data_index + 4 ;
		} ;

	/* Now apply the right half of the filter. */
	filter_index = increment - start_filter_index ;
	coeff_count = (max_filter_index - filter_index) / increment ;
	filter_index = filter_index + coeff_count * increment ;
	data_index = filter->b_current + channels * (1 + coeff_count) ;

	right [0] = right [1] = right [2] = right [3] = 0.0 ;
	do
	{	fraction = fp_to_double (filter_index) ;
		indx = fp_to_int (filter_index) ;
		assert (indx >= 0 && indx + 1 < filter->coeff_half_len + 2) ;
		icoeff = filter->coeffs [indx] + fraction * (filter->coeffs [indx + 1] - filter->coeffs [indx]) ;
		assert (data_index >= 0 && data_index + 3 < filter->b_len) ;
		assert (data_index + 3 < filter->b_end) ;
		for (int ch = 0; ch < 4; ch++)
			right [ch] += icoeff * filter->buffer [data_index + ch] ;


		filter_index -= increment ;
		data_index = data_index - 4 ;
		}
	while (filter_index > MAKE_INCREMENT_T (0)) ;

	for (int ch = 0; ch < 4; ch++)
		output [ch] = (float) (scale * (left [ch] + right [ch])) ;
} /* calc_output_quad */

SRC_ERROR
sinc_quad_vari_process (SRC_STATE *state, SRC_DATA *data)
{	SINC_FILTER *filter ;
	double		input_index, src_ratio, count, float_increment, terminate, rem ;
	increment_t	increment, start_filter_index ;
	int			half_filter_chan_len, samples_in_hand ;

	if (state->private_data == NULL)
		return SRC_ERR_NO_PRIVATE ;

	filter = (SINC_FILTER*) state->private_data ;

	/* If there is not a problem, this will be optimised out. */
	if (sizeof (filter->buffer [0]) != sizeof (data->data_in [0]))
		return SRC_ERR_SIZE_INCOMPATIBILITY ;

	filter->in_count = data->input_frames * state->channels ;
	filter->out_count = data->output_frames * state->channels ;
	filter->in_used = filter->out_gen = 0 ;

	src_ratio = state->last_ratio ;

	if (is_bad_src_ratio (src_ratio))
		return SRC_ERR_BAD_INTERNAL_STATE ;

	/* Check the sample rate ratio wrt the buffer len. */
	count = (filter->coeff_half_len + 2.0) / filter->index_inc ;
	if (MIN (state->last_ratio, data->src_ratio) < 1.0)
		count /= MIN (state->last_ratio, data->src_ratio) ;

	/* Maximum coefficientson either side of center point. */
	half_filter_chan_len = state->channels * (int) (lrint (count) + 1) ;

	input_index = state->last_position ;

	rem = fmod_one (input_index) ;
	filter->b_current = (filter->b_current + state->channels * lrint (input_index - rem)) % filter->b_len ;
	input_index = rem ;

	terminate = 1.0 / src_ratio + 1e-20 ;

	/* Main processing loop. */
	while (filter->out_gen < filter->out_count)
	{
		/* Need to reload buffer? */
		samples_in_hand = (filter->b_end - filter->b_current + filter->b_len) % filter->b_len ;

		if (samples_in_hand <= half_filter_chan_len)
		{	if ((state->error = prepare_data (filter, state->channels, data, half_filter_chan_len)) != 0)
				return state->error ;

			samples_in_hand = (filter->b_end - filter->b_current + filter->b_len) % filter->b_len ;
			if (samples_in_hand <= half_filter_chan_len)
				break ;
			} ;

		/* This is the termination condition. */
		if (filter->b_real_end >= 0)
		{	if (filter->b_current + input_index + terminate >= filter->b_real_end)
				break ;
			} ;

		if (filter->out_count > 0 && fabs (state->last_ratio - data->src_ratio) > 1e-10)
			src_ratio = state->last_ratio + filter->out_gen * (data->src_ratio - state->last_ratio) / filter->out_count ;

		float_increment = filter->index_inc * (src_ratio < 1.0 ? src_ratio : 1.0) ;
		increment = double_to_fp (float_increment) ;

		start_filter_index = double_to_fp (input_index * float_increment) ;

		calc_output_quad (filter, state->channels, increment, start_filter_index, float_increment / filter->index_inc, data->data_out + filter->out_gen) ;
		filter->out_gen += 4 ;

		/* Figure out the next index. */
		input_index += 1.0 / src_ratio ;
		rem = fmod_one (input_index) ;

		filter->b_current = (filter->b_current + state->channels * lrint (input_index - rem)) % filter->b_len ;
		input_index = rem ;
		} ;

	state->last_position = input_index ;

	/* Save current ratio rather then target ratio. */
	state->last_ratio = src_ratio ;

	data->input_frames_used = filter->in_used / state->channels ;
	data->output_frames_gen = filter->out_gen / state->channels ;

	return SRC_ERR_NO_ERROR ;
} /* sinc_quad_vari_process */

static inline void
calc_output_hex (SINC_FILTER *filter, int channels, increment_t increment, increment_t start_filter_index, double scale, float * output)
{	double		fraction, left [6], right [6], icoeff ;
	increment_t	filter_index, max_filter_index ;
	int			data_index, coeff_count, indx ;

	/* Convert input parameters into fixed point. */
	max_filter_index = int_to_fp (filter->coeff_half_len) ;

	/* First apply the left half of the filter. */
	filter_index = start_filter_index ;
	coeff_count = (max_filter_index - filter_index) / increment ;
	filter_index = filter_index + coeff_count * increment ;
	data_index = filter->b_current - channels * coeff_count ;

	if (data_index < 0) /* Avoid underflow access to filter->buffer. */
	{	int steps = int_div_ceil (-data_index, 6) ;
		/* If the assert triggers we would have to take care not to underflow/overflow */
		assert (steps <= int_div_ceil (filter_index, increment)) ;
		filter_index -= increment * steps ;
		data_index += steps * 6;
	}
	left [0] = left [1] = left [2] = left [3] = left [4] = left [5] = 0.0 ;
	while (filter_index >= MAKE_INCREMENT_T (0))
	{	fraction = fp_to_double (filter_index) ;
		indx = fp_to_int (filter_index) ;
		assert (indx >= 0 && indx + 1 < filter->coeff_half_len + 2) ;
		icoeff = filter->coeffs [indx] + fraction * (filter->coeffs [indx + 1] - filter->coeffs [indx]) ;
		assert (data_index >= 0 && data_index + 5 < filter->b_len) ;
		assert (data_index + 5 < filter->b_end) ;
		for (int ch = 0; ch < 6; ch++)
			left [ch] += icoeff * filter->buffer [data_index + ch] ;

		filter_index -= increment ;
		data_index = data_index + 6 ;
		} ;

	/* Now apply the right half of the filter. */
	filter_index = increment - start_filter_index ;
	coeff_count = (max_filter_index - filter_index) / increment ;
	filter_index = filter_index + coeff_count * increment ;
	data_index = filter->b_current + channels * (1 + coeff_count) ;

	right [0] = right [1] = right [2] = right [3] = right [4] = right [5] = 0.0 ;
	do
	{	fraction = fp_to_double (filter_index) ;
		indx = fp_to_int (filter_index) ;
		assert (indx >= 0 && indx + 1 < filter->coeff_half_len + 2) ;
		icoeff = filter->coeffs [indx] + fraction * (filter->coeffs [indx + 1] - filter->coeffs [indx]) ;
		assert (data_index >= 0 && data_index + 5 < filter->b_len) ;
		assert (data_index + 5 < filter->b_end) ;
		for (int ch = 0; ch < 6; ch++)
			right [ch] += icoeff * filter->buffer [data_index + ch] ;

		filter_index -= increment ;
		data_index = data_index - 6 ;
		}
	while (filter_index > MAKE_INCREMENT_T (0)) ;

	for (int ch = 0; ch < 6; ch++)
		output [ch] = (float) (scale * (left [ch] + right [ch])) ;
} /* calc_output_hex */

SRC_ERROR
sinc_hex_vari_process (SRC_STATE *state, SRC_DATA *data)
{	SINC_FILTER *filter ;
	double		input_index, src_ratio, count, float_increment, terminate, rem ;
	increment_t	increment, start_filter_index ;
	int			half_filter_chan_len, samples_in_hand ;

	if (state->private_data == NULL)
		return SRC_ERR_NO_PRIVATE ;

	filter = (SINC_FILTER*) state->private_data ;

	/* If there is not a problem, this will be optimised out. */
	if (sizeof (filter->buffer [0]) != sizeof (data->data_in [0]))
		return SRC_ERR_SIZE_INCOMPATIBILITY ;

	filter->in_count = data->input_frames * state->channels ;
	filter->out_count = data->output_frames * state->channels ;
	filter->in_used = filter->out_gen = 0 ;

	src_ratio = state->last_ratio ;

	if (is_bad_src_ratio (src_ratio))
		return SRC_ERR_BAD_INTERNAL_STATE ;

	/* Check the sample rate ratio wrt the buffer len. */
	count = (filter->coeff_half_len + 2.0) / filter->index_inc ;
	if (MIN (state->last_ratio, data->src_ratio) < 1.0)
		count /= MIN (state->last_ratio, data->src_ratio) ;

	/* Maximum coefficientson either side of center point. */
	half_filter_chan_len = state->channels * (int) (lrint (count) + 1) ;

	input_index = state->last_position ;

	rem = fmod_one (input_index) ;
	filter->b_current = (filter->b_current + state->channels * lrint (input_index - rem)) % filter->b_len ;
	input_index = rem ;

	terminate = 1.0 / src_ratio + 1e-20 ;

	/* Main processing loop. */
	while (filter->out_gen < filter->out_count)
	{
		/* Need to reload buffer? */
		samples_in_hand = (filter->b_end - filter->b_current + filter->b_len) % filter->b_len ;

		if (samples_in_hand <= half_filter_chan_len)
		{	if ((state->error = prepare_data (filter, state->channels, data, half_filter_chan_len)) != 0)
				return state->error ;

			samples_in_hand = (filter->b_end - filter->b_current + filter->b_len) % filter->b_len ;
			if (samples_in_hand <= half_filter_chan_len)
				break ;
			} ;

		/* This is the termination condition. */
		if (filter->b_real_end >= 0)
		{	if (filter->b_current + input_index + terminate >= filter->b_real_end)
				break ;
			} ;

		if (filter->out_count > 0 && fabs (state->last_ratio - data->src_ratio) > 1e-10)
			src_ratio = state->last_ratio + filter->out_gen * (data->src_ratio - state->last_ratio) / filter->out_count ;

		float_increment = filter->index_inc * (src_ratio < 1.0 ? src_ratio : 1.0) ;
		increment = double_to_fp (float_increment) ;

		start_filter_index = double_to_fp (input_index * float_increment) ;

		calc_output_hex (filter, state->channels, increment, start_filter_index, float_increment / filter->index_inc, data->data_out + filter->out_gen) ;
		filter->out_gen += 6 ;

		/* Figure out the next index. */
		input_index += 1.0 / src_ratio ;
		rem = fmod_one (input_index) ;

		filter->b_current = (filter->b_current + state->channels * lrint (input_index - rem)) % filter->b_len ;
		input_index = rem ;
		} ;

	state->last_position = input_index ;

	/* Save current ratio rather then target ratio. */
	state->last_ratio = src_ratio ;

	data->input_frames_used = filter->in_used / state->channels ;
	data->output_frames_gen = filter->out_gen / state->channels ;

	return SRC_ERR_NO_ERROR ;
} /* sinc_hex_vari_process */

static inline void
calc_output_multi (SINC_FILTER *filter, increment_t increment, increment_t start_filter_index, int channels, double scale, float * output)
{	double		fraction, icoeff ;
	/* The following line is 1999 ISO Standard C. If your compiler complains, get a better compiler. */
	double		*left, *right ;
	increment_t	filter_index, max_filter_index ;
	int			data_index, coeff_count, indx ;

	left = filter->left_calc ;
	right = filter->right_calc ;

	/* Convert input parameters into fixed point. */
	max_filter_index = int_to_fp (filter->coeff_half_len) ;

	/* First apply the left half of the filter. */
	filter_index = start_filter_index ;
	coeff_count = (max_filter_index - filter_index) / increment ;
	filter_index = filter_index + coeff_count * increment ;
	data_index = filter->b_current - channels * coeff_count ;

	if (data_index < 0) /* Avoid underflow access to filter->buffer. */
	{	int steps = int_div_ceil (-data_index, channels) ;
		/* If the assert triggers we would have to take care not to underflow/overflow */
		assert (steps <= int_div_ceil (filter_index, increment)) ;
		filter_index -= increment * steps ;
		data_index += steps * channels ;
	}

	memset (left, 0, sizeof (left [0]) * channels) ;

	while (filter_index >= MAKE_INCREMENT_T (0))
	{	fraction = fp_to_double (filter_index) ;
		indx = fp_to_int (filter_index) ;
		assert (indx >= 0 && indx + 1 < filter->coeff_half_len + 2) ;
		icoeff = filter->coeffs [indx] + fraction * (filter->coeffs [indx + 1] - filter->coeffs [indx]) ;

		assert (data_index >= 0 && data_index + channels - 1 < filter->b_len) ;
		assert (data_index + channels - 1 < filter->b_end) ;
		for (int ch = 0; ch < channels; ch++)
			left [ch] += icoeff * filter->buffer [data_index + ch] ;

		filter_index -= increment ;
		data_index = data_index + channels ;
		} ;

	/* Now apply the right half of the filter. */
	filter_index = increment - start_filter_index ;
	coeff_count = (max_filter_index - filter_index) / increment ;
	filter_index = filter_index + coeff_count * increment ;
	data_index = filter->b_current + channels * (1 + coeff_count) ;

	memset (right, 0, sizeof (right [0]) * channels) ;
	do
	{	fraction = fp_to_double (filter_index) ;
		indx = fp_to_int (filter_index) ;
		assert (indx >= 0 && indx + 1 < filter->coeff_half_len + 2) ;
		icoeff = filter->coeffs [indx] + fraction * (filter->coeffs [indx + 1] - filter->coeffs [indx]) ;
		assert (data_index >= 0 && data_index + channels - 1 < filter->b_len) ;
		assert (data_index + channels - 1 < filter->b_end) ;
		for (int ch = 0; ch < channels; ch++)
			right [ch] += icoeff * filter->buffer [data_index + ch] ;

		filter_index -= increment ;
		data_index = data_index - channels ;
		}
	while (filter_index > MAKE_INCREMENT_T (0)) ;

	for(int ch = 0; ch < channels; ch++)
		output [ch] = (float) (scale * (left [ch] + right [ch])) ;

	return ;
} /* calc_output_multi */

static SRC_ERROR
sinc_multichan_vari_process (SRC_STATE *state, SRC_DATA *data)
{	SINC_FILTER *filter ;
	double		input_index, src_ratio, count, float_increment, terminate, rem ;
	increment_t	increment, start_filter_index ;
	int			half_filter_chan_len, samples_in_hand ;

	if (state->private_data == NULL)
		return SRC_ERR_NO_PRIVATE ;

	filter = (SINC_FILTER*) state->private_data ;

	/* If there is not a problem, this will be optimised out. */
	if (sizeof (filter->buffer [0]) != sizeof (data->data_in [0]))
		return SRC_ERR_SIZE_INCOMPATIBILITY ;

	filter->in_count = data->input_frames * state->channels ;
	filter->out_count = data->output_frames * state->channels ;
	filter->in_used = filter->out_gen = 0 ;

	src_ratio = state->last_ratio ;

	if (is_bad_src_ratio (src_ratio))
		return SRC_ERR_BAD_INTERNAL_STATE ;

	/* Check the sample rate ratio wrt the buffer len. */
	count = (filter->coeff_half_len + 2.0) / filter->index_inc ;
	if (MIN (state->last_ratio, data->src_ratio) < 1.0)
		count /= MIN (state->last_ratio, data->src_ratio) ;

	/* Maximum coefficientson either side of center point. */
	half_filter_chan_len = state->channels * (int) (lrint (count) + 1) ;

	input_index = state->last_position ;

	rem = fmod_one (input_index) ;
	filter->b_current = (filter->b_current + state->channels * lrint (input_index - rem)) % filter->b_len ;
	input_index = rem ;

	terminate = 1.0 / src_ratio + 1e-20 ;

	/* Main processing loop. */
	while (filter->out_gen < filter->out_count)
	{
		/* Need to reload buffer? */
		samples_in_hand = (filter->b_end - filter->b_current + filter->b_len) % filter->b_len ;

		if (samples_in_hand <= half_filter_chan_len)
		{	if ((state->error = prepare_data (filter, state->channels, data, half_filter_chan_len)) != 0)
				return state->error ;

			samples_in_hand = (filter->b_end - filter->b_current + filter->b_len) % filter->b_len ;
			if (samples_in_hand <= half_filter_chan_len)
				break ;
			} ;

		/* This is the termination condition. */
		if (filter->b_real_end >= 0)
		{	if (filter->b_current + input_index + terminate >= filter->b_real_end)
				break ;
			} ;

		if (filter->out_count > 0 && fabs (state->last_ratio - data->src_ratio) > 1e-10)
			src_ratio = state->last_ratio + filter->out_gen * (data->src_ratio - state->last_ratio) / filter->out_count ;

		float_increment = filter->index_inc * (src_ratio < 1.0 ? src_ratio : 1.0) ;
		increment = double_to_fp (float_increment) ;

		start_filter_index = double_to_fp (input_index * float_increment) ;

		calc_output_multi (filter, increment, start_filter_index, state->channels, float_increment / filter->index_inc, data->data_out + filter->out_gen) ;
		filter->out_gen += state->channels ;

		/* Figure out the next index. */
		input_index += 1.0 / src_ratio ;
		rem = fmod_one (input_index) ;

		filter->b_current = (filter->b_current + state->channels * lrint (input_index - rem)) % filter->b_len ;
		input_index = rem ;
		} ;

	state->last_position = input_index ;

	/* Save current ratio rather then target ratio. */
	state->last_ratio = src_ratio ;

	data->input_frames_used = filter->in_used / state->channels ;
	data->output_frames_gen = filter->out_gen / state->channels ;

	return SRC_ERR_NO_ERROR ;
} /* sinc_multichan_vari_process */

/*----------------------------------------------------------------------------------------
*/

static SRC_ERROR
prepare_data (SINC_FILTER *filter, int channels, SRC_DATA *data, int half_filter_chan_len)
{	int len = 0 ;

	if (filter->b_real_end >= 0)
		return SRC_ERR_NO_ERROR ;	/* Should be terminating. Just return. */

	if (data->data_in == NULL)
		return SRC_ERR_NO_ERROR ;

	if (filter->b_current == 0)
	{	/* Initial state. Set up zeros at the start of the buffer and
		** then load new data after that.
		*/
		len = filter->b_len - 2 * half_filter_chan_len ;

		filter->b_current = filter->b_end = half_filter_chan_len ;
		}
	else if (filter->b_end + half_filter_chan_len + channels < filter->b_len)
	{	/*  Load data at current end position. */
		len = MAX (filter->b_len - filter->b_current - half_filter_chan_len, 0) ;
		}
	else
	{	/* Move data at end of buffer back to the start of the buffer. */
		len = filter->b_end - filter->b_current ;
		memmove (filter->buffer, filter->buffer + filter->b_current - half_filter_chan_len,
						(half_filter_chan_len + len) * sizeof (filter->buffer [0])) ;

		filter->b_current = half_filter_chan_len ;
		filter->b_end = filter->b_current + len ;

		/* Now load data at current end of buffer. */
		len = MAX (filter->b_len - filter->b_current - half_filter_chan_len, 0) ;
		} ;

	len = MIN ((int) (filter->in_count - filter->in_used), len) ;
	len -= (len % channels) ;

	if (len < 0 || filter->b_end + len > filter->b_len)
		return SRC_ERR_SINC_PREPARE_DATA_BAD_LEN ;

	memcpy (filter->buffer + filter->b_end, data->data_in + filter->in_used,
						len * sizeof (filter->buffer [0])) ;

	filter->b_end += len ;
	filter->in_used += len ;

	if (filter->in_used == filter->in_count &&
			filter->b_end - filter->b_current < 2 * half_filter_chan_len && data->end_of_input)
	{	/* Handle the case where all data in the current buffer has been
		** consumed and this is the last buffer.
		*/

		if (filter->b_len - filter->b_end < half_filter_chan_len + 5)
		{	/* If necessary, move data down to the start of the buffer. */
			len = filter->b_end - filter->b_current ;
			memmove (filter->buffer, filter->buffer + filter->b_current - half_filter_chan_len,
							(half_filter_chan_len + len) * sizeof (filter->buffer [0])) ;

			filter->b_current = half_filter_chan_len ;
			filter->b_end = filter->b_current + len ;
			} ;

		filter->b_real_end = filter->b_end ;
		len = half_filter_chan_len + 5 ;

		if (len < 0 || filter->b_end + len > filter->b_len)
			len = filter->b_len - filter->b_end ;

		memset (filter->buffer + filter->b_end, 0, len * sizeof (filter->buffer [0])) ;
		filter->b_end += len ;
		} ;

	return SRC_ERR_NO_ERROR ;
} /* prepare_data */

static void
sinc_close (SRC_STATE *state)
{
	if (state)
	{
		SINC_FILTER *sinc = (SINC_FILTER *) state->private_data ;
		if (sinc)
		{
			if (sinc->buffer)
			{
				free (sinc->buffer) ;
				sinc->buffer = NULL ;
			}
			free (sinc) ;
			sinc = NULL ;
		}
		free (state) ;
		state = NULL ;
	}
} /* sinc_close */
