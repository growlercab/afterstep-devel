/*
 * Copyright (c) 2000,2001 Sasha Vasko <sashav@sprintmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "config.h"

/* #define LOCAL_DEBUG */
/* #define DO_CLOCKING */

#define USE_64BIT_FPU

#include <malloc.h>
#ifdef DO_CLOCKING
#include <sys/time.h>
#endif
#include <stdarg.h>


#include "afterbase.h"
#include "asvisual.h"
#include "blender.h"
#include "asimage.h"
#include "transform.h"

/* ******************************************************************************/
/* below goes all kinds of funky stuff we can do with scanlines : 			   */
/* ******************************************************************************/
/* this will enlarge array based on count of items in dst per PAIR of src item with smoothing/scatter/dither */
/* the following formulas use linear approximation to calculate   */
/* color values for new pixels : 				  				  */
/* for scale factor of 2 we use this formula :    */
/* C = (-C1+3*C2+3*C3-C4)/4 					  */
/* or better :				 					  */
/* C = (-C1+5*C2+5*C3-C4)/8 					  */
#define INTERPOLATE_COLOR1(c) 			   	((c)<<QUANT_ERR_BITS)  /* nothing really to interpolate here */
#define INTERPOLATE_COLOR2(c1,c2,c3,c4)    	((((c2)<<2)+(c2)+((c3)<<2)+(c3)-(c1)-(c4))<<(QUANT_ERR_BITS-3))
#define INTERPOLATE_COLOR2_V(c1,c2,c3,c4)    	((((c2)<<2)+(c2)+((c3)<<2)+(c3)-(c1)-(c4))>>3)
/* for scale factor of 3 we use these formulas :  */
/* Ca = (-2C1+8*C2+5*C3-2C4)/9 		  			  */
/* Cb = (-2C1+5*C2+8*C3-2C4)/9 		  			  */
/* or better : 									  */
/* Ca = (-C1+5*C2+3*C3-C4)/6 		  			  */
/* Cb = (-C1+3*C2+5*C3-C4)/6 		  			  */
#define INTERPOLATE_A_COLOR3(c1,c2,c3,c4)  	(((((c2)<<2)+(c2)+((c3)<<1)+(c3)-(c1)-(c4))<<QUANT_ERR_BITS)/6)
#define INTERPOLATE_B_COLOR3(c1,c2,c3,c4)  	(((((c2)<<1)+(c2)+((c3)<<2)+(c3)-(c1)-(c4))<<QUANT_ERR_BITS)/6)
#define INTERPOLATE_A_COLOR3_V(c1,c2,c3,c4)  	((((c2)<<2)+(c2)+((c3)<<1)+(c3)-(c1)-(c4))/6)
#define INTERPOLATE_B_COLOR3_V(c1,c2,c3,c4)  	((((c2)<<1)+(c2)+((c3)<<2)+(c3)-(c1)-(c4))/6)
/* just a hypotesus, but it looks good for scale factors S > 3: */
/* Cn = (-C1+(2*(S-n)+1)*C2+(2*n+1)*C3-C4)/2S  	  			   */
/* or :
 * Cn = (-C1+(2*S+1)*C2+C3-C4+n*(2*C3-2*C2)/2S  			   */
/*       [ T                   [C2s]  [C3s]]   			       */
#define INTERPOLATION_Cs(c)	 		 	    ((c)<<1)
/*#define INTERPOLATION_TOTAL_START(c1,c2,c3,c4,S) 	(((S)<<1)*(c2)+((c3)<<1)+(c3)-c2-c1-c4)*/
#define INTERPOLATION_TOTAL_START(c1,c2,c3,c4,S) 	((((S)<<1)+1)*(c2)+(c3)-(c1)-(c4))
#define INTERPOLATION_TOTAL_STEP(c2,c3)  	((c3<<1)-(c2<<1))
#define INTERPOLATE_N_COLOR(T,S)		  	(((T)<<(QUANT_ERR_BITS-1))/(S))

#define AVERAGE_COLOR1(c) 					((c)<<QUANT_ERR_BITS)
#define AVERAGE_COLOR2(c1,c2)				(((c1)+(c2))<<(QUANT_ERR_BITS-1))
#define AVERAGE_COLORN(T,N)					(((T)<<QUANT_ERR_BITS)/N)

static inline void
enlarge_component12( register CARD32 *src, register CARD32 *dst, int *scales, int len )
{/* expected len >= 2  */
	register int i = 0, k = 0;
	register int c1 = src[0], c4;
	--len; --len ;
	while( i < len )
	{
		c4 = src[i+2];
		/* that's right we can do that PRIOR as we calculate nothing */
		dst[k] = INTERPOLATE_COLOR1(src[i]) ;
		if( scales[i] == 2 )
		{
			register int c2 = src[i], c3 = src[i+1] ;
			c3 = INTERPOLATE_COLOR2(c1,c2,c3,c4);
			dst[++k] = (c3&0xFF000000 )?0:c3;
		}
		c1 = src[i];
		++k;
		++i;
	}

	/* to avoid one more if() in loop we moved tail part out of the loop : */
	if( scales[i] == 1 )
		dst[k] = INTERPOLATE_COLOR1(src[i]);
	else
	{
		register int c2 = src[i], c3 = src[i+1] ;
		c2 = INTERPOLATE_COLOR2(c1,c2,c3,c3);
		dst[k] = (c2&0xFF000000 )?0:c2;
	}
	dst[k+1] = INTERPOLATE_COLOR1(src[i+1]);
}

static inline void
enlarge_component23( register CARD32 *src, register CARD32 *dst, int *scales, int len )
{/* expected len >= 2  */
	register int i = 0, k = 0;
	register int c1 = src[0], c4 = src[1];
	if( scales[0] == 1 )
	{/* special processing for first element - it can be 1 - others can only be 2 or 3 */
		dst[k] = INTERPOLATE_COLOR1(src[0]) ;
		++k;
		++i;
	}
	--len; --len ;
	while( i < len )
	{
		register int c2 = src[i], c3 = src[i+1] ;
		c4 = src[i+2];
		dst[k] = INTERPOLATE_COLOR1(c2) ;
		if( scales[i] == 2 )
		{
			c3 = INTERPOLATE_COLOR2(c1,c2,c3,c3);
			dst[++k] = (c3&0x7F000000 )?0:c3;
		}else
		{
			dst[++k] = INTERPOLATE_A_COLOR3(c1,c2,c3,c4);
			if( dst[k]&0x7F000000 )
				dst[k] = 0 ;
			c3 = INTERPOLATE_B_COLOR3(c1,c2,c3,c3);
			dst[++k] = (c3&0x7F000000 )?0:c3;
		}
		c1 = c2 ;
		++k;
		++i;
	}
	/* to avoid one more if() in loop we moved tail part out of the loop : */
	{
		register int c2 = src[i], c3 = src[i+1] ;
		dst[k] = INTERPOLATE_COLOR1(c2) ;
		if( scales[i] == 2 )
		{
			c2 = INTERPOLATE_COLOR2(c1,c2,c3,c3);
			dst[k+1] = (c2&0x7F000000 )?0:c2;
		}else
		{
			if( scales[i] == 1 )
				--k;
			else
			{
				dst[++k] = INTERPOLATE_A_COLOR3(c1,c2,c3,c3);
				if( dst[k]&0x7F000000 )
					dst[k] = 0 ;
				c2 = INTERPOLATE_B_COLOR3(c1,c2,c3,c3);
				dst[k+1] = (c2&0x7F000000 )?0:c2;
			}
		}
	}
 	dst[k+2] = INTERPOLATE_COLOR1(src[i+1]) ;
}

/* this case is more complex since we cannot really hardcode coefficients
 * visible artifacts on smooth gradient-like images
 */
static inline void
enlarge_component( register CARD32 *src, register CARD32 *dst, int *scales, int len )
{/* we skip all checks as it is static function and we want to optimize it
  * as much as possible */
	int i = 0;
	int c1 = src[0];
	--len ;
	do
	{
		register int step = INTERPOLATION_TOTAL_STEP(src[i],src[i+1]);
		register int T ;
		register short S = scales[i];

/*		LOCAL_DEBUG_OUT( "pixel %d, S = %d, step = %d", i, S, step );*/
		T = INTERPOLATION_TOTAL_START(c1,src[i],src[i+1],src[i+2],S);
		if( step )
		{
			register int n = 0 ;
			do
			{
				dst[n] = (T&0x7F000000)?0:INTERPOLATE_N_COLOR(T,S);
				if( ++n >= S ) break;
				(int)T += (int)step;
			}while(1);
			dst += n ;
		}else
		{
			register CARD32 c = (T&0x7F000000)?0:INTERPOLATE_N_COLOR(T,S);
			while(--S >= 0){	dst[S] = c;	}
			dst += scales[i] ;
		}
		c1 = src[i];
		if( ++i >= len )
			break;
	}while(1);
	*dst = INTERPOLATE_COLOR1(src[i]) ;
/*LOCAL_DEBUG_OUT( "%d pixels written", k );*/
}

/* this will shrink array based on count of items in src per one dst item with averaging */
static inline void
shrink_component( register CARD32 *src, register CARD32 *dst, int *scales, int len )
{/* we skip all checks as it is static function and we want to optimize it
  * as much as possible */
	register int i = -1, k = -1;
	while( ++k < len )
	{
		register int reps = scales[k] ;
		register int c1 = src[++i];
/*LOCAL_DEBUG_OUT( "pixel = %d, scale[k] = %d", k, reps );*/
		if( reps == 1 )
			dst[k] = AVERAGE_COLOR1(c1);
		else if( reps == 2 )
		{
			++i;
			dst[k] = AVERAGE_COLOR2(c1,src[i]);
		}else
		{
			reps += i-1;
			while( reps > i )
			{
				++i ;
				c1 += src[i];
			}
			{
				register short S = scales[k];
				dst[k] = AVERAGE_COLORN(c1,S);
			}
		}
	}
}
static inline void
shrink_component11( register CARD32 *src, register CARD32 *dst, int *scales, int len )
{
	register int i ;
	for( i = 0 ; i < len ; ++i )
		dst[i] = AVERAGE_COLOR1(src[i]);
}


static inline void
reverse_component( register CARD32 *src, register CARD32 *dst, int *unused, int len )
{
	register int i = 0;
	src += len-1 ;
	do
	{
		dst[i] = src[-i];
	}while(++i < len );
}

static inline void
add_component( CARD32 *src, CARD32 *incr, int *scales, int len )
{
	int i = 0;

	len += len&0x01;
#if 1
#ifdef HAVE_MMX
	if( asimage_use_mmx )
	{
		double *ddst = (double*)&(src[0]);
		double *dinc = (double*)&(incr[0]);
		len = len>>1;
		do{
			asm volatile
       		(
            	"movq %0, %%mm0  \n\t" /* load 8 bytes from src[i] into MM0 */
            	"paddd %1, %%mm0 \n\t" /* MM0=src[i]>>1              */
            	"movq %%mm0, %0  \n\t" /* store the result in dest */
				: "=m" (ddst[i])       /* %0 */
				:  "m"  (dinc[i])       /* %2 */
	        );
		}while( ++i < len );
	}else
#endif
#endif
	{
		register int c1, c2;
		do{
			c1 = (int)src[i] + (int)incr[i] ;
			c2 = (int)src[i+1] + (int)incr[i+1] ;
			src[i] = c1;
			src[i+1] = c2;
			i += 2 ;
		}while( i < len );
	}
}

static inline void
rbitshift_component( register CARD32 *src, register CARD32 *dst, int shift, int len )
{
	register int i ;
	for( i = 0 ; i < len ; ++i )
		dst[i] = src[i]>>shift;
}

static inline void
start_component_interpolation( CARD32 *c1, CARD32 *c2, CARD32 *c3, CARD32 *c4, register CARD32 *T, register CARD32 *step, CARD16 S, int len)
{
	register int i;
	for( i = 0 ; i < len ; i++ )
	{
		register int rc2 = c2[i], rc3 = c3[i] ;
		T[i] = INTERPOLATION_TOTAL_START(c1[i],rc2,rc3,c4[i],S)/(S<<1);
		step[i] = INTERPOLATION_TOTAL_STEP(rc2,rc3)/(S<<1);
	}
}

static inline void
component_interpolation_hardcoded( CARD32 *c1, CARD32 *c2, CARD32 *c3, CARD32 *c4, register CARD32 *T, CARD32 *unused, CARD16 kind, int len)
{
	register int i;
	if( kind == 1 )
	{
		for( i = 0 ; i < len ; i++ )
		{
#if 1
			/* its seems that this simple formula is completely sufficient
			   and even better then more complicated one : */
			T[i] = (c2[i]+c3[i])>>1 ;
#else
    		register int minus = c1[i]+c4[i] ;
			register int plus  = (c2[i]<<1)+c2[i]+(c3[i]<<1)+c3[i];

			T[i] = ( (plus>>1) < minus )?(c2[i]+c3[i])>>1 :
								   		 (plus-minus)>>2;
#endif
		}
	}else if( kind == 2 )
	{
		for( i = 0 ; i < len ; i++ )
		{
    		register int rc1 = c1[i], rc2 = c2[i], rc3 = c3[i] ;
			T[i] = INTERPOLATE_A_COLOR3_V(rc1,rc2,rc3,c4[i]);
		}
	}else
		for( i = 0 ; i < len ; i++ )
		{
    		register int rc1 = c1[i], rc2 = c2[i], rc3 = c3[i] ;
			T[i] = INTERPOLATE_B_COLOR3_V(rc1,rc2,rc3,c4[i]);
		}
}

static inline void
divide_component_mod( register CARD32 *data, CARD16 ratio, int len )
{
	register int i ;
	for( i = 0 ; i < len ; ++i )
		data[i] /= ratio;
}

static inline void
rbitshift_component_mod( register CARD32 *data, int bits, int len )
{
	register int i ;
	for( i = 0 ; i < len ; ++i )
		data[i] = data[i]>>bits;
}

void
print_component( register CARD32 *data, int nonsense, int len )
{
	register int i ;
	for( i = 0 ; i < len ; ++i )
		fprintf( stderr, " %8.8lX", data[i] );
	fprintf( stderr, "\n");
}

static inline void
tint_component_mod( register CARD32 *data, CARD16 ratio, int len )
{
	register int i ;
	if( ratio == 255 )
		for( i = 0 ; i < len ; ++i )
			data[i] = data[i]<<8;
	else if( ratio == 128 )
		for( i = 0 ; i < len ; ++i )
			data[i] = data[i]<<7;
	else if( ratio == 0 )
		for( i = 0 ; i < len ; ++i )
			data[i] = 0;
	else
		for( i = 0 ; i < len ; ++i )
			data[i] = data[i]*ratio;
}

static inline void
make_component_gradient16( register CARD32 *data, CARD16 from, CARD16 to, CARD8 seed, int len )
{
	register int i ;
	long incr = (((long)to<<8)-((long)from<<8))/len ;

	if( incr == 0 )
		for( i = 0 ; i < len ; ++i )
			data[i] = from;
	else
	{
		long curr = from<<8;
		curr += ((((CARD32)seed)<<8) > incr)?incr:((CARD32)seed)<<8 ;
		for( i = 0 ; i < len ; ++i )
		{/* we make calculations in 24bit per chan, then convert it back to 16 and
		  * carry over half of the quantization error onto the next pixel */
			data[i] = curr>>8;
			curr += ((curr&0x00FF)>>1)+incr ;
		}
	}
}


static inline void
copytintpad_scanline( ASScanline *src, ASScanline *dst, int offset, ARGB32 tint )
{
	register int i ;
	CARD32 chan_tint[4], chan_fill[4] ;
	int color ;
	int copy_width = src->width, dst_offset = 0, src_offset = 0;

	if( offset+src->width < 0 || offset > dst->width )
		return;
	chan_tint[IC_RED]   = ARGB32_RED8  (tint)<<1;
	chan_tint[IC_GREEN] = ARGB32_GREEN8(tint)<<1;
	chan_tint[IC_BLUE]  = ARGB32_BLUE8 (tint)<<1;
	chan_tint[IC_ALPHA] = ARGB32_ALPHA8(tint)<<1;
	chan_fill[IC_RED]   = ARGB32_RED8  (dst->back_color)<<dst->shift;
	chan_fill[IC_GREEN] = ARGB32_GREEN8(dst->back_color)<<dst->shift;
	chan_fill[IC_BLUE]  = ARGB32_BLUE8 (dst->back_color)<<dst->shift;
	chan_fill[IC_ALPHA] = ARGB32_ALPHA8(dst->back_color)<<dst->shift;
	if( offset < 0 )
		src_offset = -offset ;
	else
		dst_offset = offset ;
	copy_width = MIN( src->width-src_offset, dst->width-dst_offset );

	dst->flags = src->flags ;
	for( color = 0 ; color < IC_NUM_CHANNELS ; ++color )
	{
		register CARD32 *psrc = src->channels[color]+src_offset;
		register CARD32 *pdst = dst->channels[color];
		int ratio = chan_tint[color];
/*	fprintf( stderr, "channel %d, tint is %d(%X), src_width = %d, src_offset = %d, dst_width = %d, dst_offset = %d psrc = %p, pdst = %p\n", color, ratio, ratio, src->width, src_offset, dst->width, dst_offset, psrc, pdst );
*/
		{
/*			register CARD32 fill = chan_fill[color]; */
			for( i = 0 ; i < dst_offset ; ++i )
				pdst[i] = 0;
			pdst += dst_offset ;
		}

		if( get_flags(src->flags, 0x01<<color) )
		{
			if( ratio >= 254 )
				for( i = 0 ; i < copy_width ; ++i )
					pdst[i] = psrc[i]<<8;
			else if( ratio == 128 )
				for( i = 0 ; i < copy_width ; ++i )
					pdst[i] = psrc[i]<<7;
			else if( ratio == 0 )
				for( i = 0 ; i < copy_width ; ++i )
					pdst[i] = 0;
			else
				for( i = 0 ; i < copy_width ; ++i )
					pdst[i] = psrc[i]*ratio;
		}else
		{
		    ratio = ratio*chan_fill[color];
			for( i = 0 ; i < copy_width ; ++i )
				pdst[i] = ratio;
			set_flags( dst->flags, (0x01<<color));
		}
		{
/*			register CARD32 fill = chan_fill[color]; */
			for( ; i < dst->width-dst_offset ; ++i )
				pdst[i] = 0;
/*				print_component(pdst, 0, dst->width ); */
		}
	}
}

/* **********************************************************************************************/
/* drawing gradient on scanline :  															   */
/* **********************************************************************************************/
void
make_gradient_scanline( ASScanline *scl, ASGradient *grad, ASFlagType filter, ARGB32 seed )
{
	if( scl && grad && filter != 0 )
	{
		int offset = 0, step, i, max_i = grad->npoints - 1 ;
		for( i = 0  ; i < max_i ; i++ )
		{
			if( i == max_i-1 )
				step = scl->width - offset;
			else
				step = grad->offset[i+1] * scl->width - offset ;
			if( step > 0 && step <= scl->width-offset )
			{
				int color ;
				for( color = 0 ; color < IC_NUM_CHANNELS ; ++color )
					if( get_flags( filter, 0x01<<color ) )
						make_component_gradient16( scl->channels[color]+offset,
												   ARGB32_CHAN8(grad->color[i],color)<<8,
												   ARGB32_CHAN8(grad->color[i+1],color)<<8,
												   ARGB32_CHAN8(seed,color),
												   step);
				offset += step ;
			}
		}
		scl->flags = filter ;
	}
}

/* **********************************************************************************************/
/* Scaling code ; 																			   */
/* **********************************************************************************************/
Bool
check_scale_parameters( ASImage *src, int *to_width, int *to_height )
{
	if( src == NULL )
		return False;

	if( *to_width < 0 )
		*to_width = src->width ;
	else if( *to_width < 2 )
		*to_width = 2 ;
	if( *to_height < 0 )
		*to_height = src->height ;
	else if( *to_height < 2 )
		*to_height = 2 ;
	return True;
}

int *
make_scales( unsigned short from_size, unsigned short to_size )
{
	int *scales ;
	unsigned short smaller = MIN(from_size,to_size);
	unsigned short bigger  = MAX(from_size,to_size);
	register int i = 0, k = 0;
	int eps;

	if( from_size < to_size )
	{	smaller--; bigger-- ; }
	if( smaller == 0 )
		smaller = 1;
	if( bigger == 0 )
		bigger = 1;
	scales = safecalloc( smaller, sizeof(int));
	eps = -(bigger>>1);
	/* now using Bresengham algoritm to fiill the scales :
	 * since scaling is merely transformation
	 * from 0:bigger space (x) to 0:smaller space(y)*/
	for ( i = 0 ; i < bigger ; i++ )
	{
		++scales[k];
		eps += smaller;
		if( (eps << 1) >= bigger )
		{
			++k ;
			eps -= bigger ;
		}
	}
	return scales;
}

/* *******************************************************************/
void
scale_image_down( ASImageDecoder *imdec, ASImageOutput *imout, int h_ratio, int *scales_h, int* scales_v)
{
	ASScanline dst_line, total ;
	int k = -1;
	int max_k 	 = imout->im->height,
		line_len = MIN(imout->im->width,imdec->im->width);

	prepare_scanline( imout->im->width, QUANT_ERR_BITS, &dst_line, imout->asv->BGR_mode );
	prepare_scanline( imout->im->width, QUANT_ERR_BITS, &total, imout->asv->BGR_mode );
	while( ++k < max_k )
	{
		int reps = scales_v[k] ;
		imdec->decode_image_scanline( imdec );
		total.flags = imdec->buffer.flags ;
		CHOOSE_SCANLINE_FUNC(h_ratio,imdec->buffer,total,scales_h,line_len);

		while( --reps > 0 )
		{
			imdec->decode_image_scanline( imdec );
			total.flags = imdec->buffer.flags ;
			CHOOSE_SCANLINE_FUNC(h_ratio,imdec->buffer,dst_line,scales_h,line_len);
			SCANLINE_FUNC(add_component,total,dst_line,NULL,total.width);
		}

		imout->output_image_scanline( imout, &total, scales_v[k] );
	}
	free_scanline(&dst_line, True);
	free_scanline(&total, True);
}

void
scale_image_up( ASImageDecoder *imdec, ASImageOutput *imout, int h_ratio, int *scales_h, int* scales_v)
{
	ASScanline step, src_lines[4], *c1, *c2, *c3, *c4 = NULL;
	int i = 0, max_i,
		line_len = MIN(imout->im->width,imdec->im->width),
		out_width = imout->im->width;
	for( i = 0 ; i < 4 ; i++ )
		prepare_scanline( out_width, 0, &(src_lines[i]), imout->asv->BGR_mode);
	prepare_scanline( out_width, QUANT_ERR_BITS, &step, imout->asv->BGR_mode );


/*	set_component(src_lines[0].red,0x00000000,0,out_width*3); */
	imdec->decode_image_scanline( imdec );
	step.flags = src_lines[0].flags = src_lines[1].flags = imdec->buffer.flags ;
	CHOOSE_SCANLINE_FUNC(h_ratio,imdec->buffer,src_lines[1],scales_h,line_len);

	SCANLINE_FUNC(copy_component,src_lines[1],src_lines[0],0,out_width);

	imdec->decode_image_scanline( imdec );
	src_lines[2].flags = imdec->buffer.flags ;
	CHOOSE_SCANLINE_FUNC(h_ratio,imdec->buffer,src_lines[2],scales_h,line_len);

	i = 0 ;
	max_i = imdec->im->height-1 ;
	do
	{
		int S = scales_v[i] ;
		c1 = &(src_lines[i&0x03]);
		c2 = &(src_lines[(i+1)&0x03]);
		c3 = &(src_lines[(i+2)&0x03]);
		c4 = &(src_lines[(i+3)&0x03]);

		if( i+1 < max_i )
		{
			imdec->decode_image_scanline( imdec );
			c4->flags = imdec->buffer.flags ;
			CHOOSE_SCANLINE_FUNC(h_ratio,imdec->buffer,*c4,scales_h,line_len);
		}
		/* now we'll prepare total and step : */
		imout->output_image_scanline( imout, c2, 1);
		if( S > 1 )
		{
			if( S == 2 )
			{
				SCANLINE_COMBINE(component_interpolation_hardcoded,*c1,*c2,*c3,*c4,*c1,*c1,1,out_width);
				imout->output_image_scanline( imout, c1, 1);
			}else if( S == 3 )
			{
				SCANLINE_COMBINE(component_interpolation_hardcoded,*c1,*c2,*c3,*c4,*c1,*c1,2,out_width);
				imout->output_image_scanline( imout, c1, 1);
				SCANLINE_COMBINE(component_interpolation_hardcoded,*c1,*c2,*c3,*c4,*c1,*c1,3,out_width);
				imout->output_image_scanline( imout, c1, 1);
			}else
			{
				SCANLINE_COMBINE(start_component_interpolation,*c1,*c2,*c3,*c4,*c1,step,S,out_width);
				do
				{
					imout->output_image_scanline( imout, c1, 1);
					if((--S)<=1)
						break;
					SCANLINE_FUNC(add_component,*c1,step,NULL,out_width );
 				}while(1);
			}
		}
	}while( ++i < max_i );
	imout->output_image_scanline( imout, c4, 1);

	for( i = 0 ; i < 4 ; i++ )
		free_scanline(&(src_lines[i]), True);
	free_scanline(&step, True);
}

/* *****************************************************************************/
/* ASImage transformations : 												  */
/* *****************************************************************************/
ASImage *
scale_asimage( ASVisual *asv, ASImage *src, unsigned int to_width, unsigned int to_height,
			   ASAltImFormats out_format, unsigned int compression_out, int quality )
{
	ASImage *dst = NULL ;
	ASImageOutput  *imout ;
	ASImageDecoder *imdec;
	int h_ratio ;
	int *scales_h = NULL, *scales_v = NULL;
#ifdef DO_CLOCKING
	time_t started = clock ();
#endif
	if( !check_scale_parameters(src,&to_width,&to_height) )
		return NULL;
	if( (imdec = start_image_decoding(asv, src, SCL_DO_ALL, 0, 0, 0, 0, NULL)) == NULL )
		return NULL;
	dst = create_asimage(to_width, to_height, compression_out);
	if( to_width == src->width )
		h_ratio = 0;
	else if( to_width < src->width )
		h_ratio = 1;
	else if( src->width > 1 )
	{
		h_ratio = to_width/(src->width-1);
		if( h_ratio*(src->width-1) < to_width )
			h_ratio++ ;
	}else
		h_ratio = to_width ;
	scales_h = make_scales( src->width, to_width );
	scales_v = make_scales( src->height, to_height );
#ifdef LOCAL_DEBUG
	{
	  register int i ;
	  for( i = 0 ; i < MIN(src->width, to_width) ; i++ )
		fprintf( stderr, " %d", scales_h[i] );
	  fprintf( stderr, "\n" );
	  for( i = 0 ; i < MIN(src->height, to_height) ; i++ )
		fprintf( stderr, " %d", scales_v[i] );
	  fprintf( stderr, "\n" );
	}
#endif
#ifdef HAVE_MMX
	mmx_init();
#endif
	if((imout = start_image_output( asv, dst, out_format, QUANT_ERR_BITS, quality )) == NULL )
	{
		asimage_init(dst, True);
		free( dst );
		dst = NULL ;
	}else
	{
		if( to_height <= src->height ) 					   /* scaling down */
			scale_image_down( imdec, imout, h_ratio, scales_h, scales_v );
		else
			scale_image_up( imdec, imout, h_ratio, scales_h, scales_v );
		stop_image_output( &imout );
	}
#ifdef HAVE_MMX
	mmx_off();
#endif
	free( scales_h );
	free( scales_v );
	stop_image_decoding( &imdec );
#ifdef DO_CLOCKING
	fprintf (stderr, __FUNCTION__ " time (clocks): %lu mlsec\n", ((clock () - started)*100)/CLOCKS_PER_SEC);
#endif
	return dst;
}

ASImage *
tile_asimage( ASVisual *asv, ASImage *src,
		      int offset_x, int offset_y,
			  unsigned int to_width,
			  unsigned int to_height,
			  ARGB32 tint,
			  ASAltImFormats out_format, unsigned int compression_out, int quality )
{
	ASImage *dst = NULL ;
	ASImageDecoder *imdec ;
	ASImageOutput  *imout ;
#ifdef DO_CLOCKING
	time_t started = clock ();
#endif

LOCAL_DEBUG_CALLER_OUT( "offset_x = %d, offset_y = %d, to_width = %d, to_height = %d", offset_x, offset_y, to_width, to_height );
	if( (imdec = start_image_decoding(asv, src, SCL_DO_ALL, offset_x, offset_y, to_width, 0, NULL)) == NULL )
		return NULL;

	dst = safecalloc(1, sizeof(ASImage));
	asimage_start (dst, to_width, to_height, compression_out);
#ifdef HAVE_MMX
	mmx_init();
#endif
	if((imout = start_image_output( asv, dst, out_format, (tint!=0)?8:0, quality)) == NULL )
	{
		asimage_init(dst, True);
		free( dst );
		dst = NULL ;
	}else
	{
		int y, max_y = to_height;
LOCAL_DEBUG_OUT("tiling actually...%s", "");
		if( to_height > src->height )
		{
			imout->tiling_step = src->height ;
			max_y = src->height ;
		}
		if( tint != 0 )
		{
			for( y = 0 ; y < max_y ; y++  )
			{
				imdec->decode_image_scanline( imdec );
				tint_component_mod( imdec->buffer.red, ARGB32_RED8(tint)<<1, to_width );
				tint_component_mod( imdec->buffer.green, ARGB32_GREEN8(tint)<<1, to_width );
  				tint_component_mod( imdec->buffer.blue, ARGB32_BLUE8(tint)<<1, to_width );
				tint_component_mod( imdec->buffer.alpha, ARGB32_ALPHA8(tint)<<1, to_width );
				imout->output_image_scanline( imout, &(imdec->buffer), 1);
			}
		}else
			for( y = 0 ; y < max_y ; y++  )
			{
				imdec->decode_image_scanline( imdec );
				imout->output_image_scanline( imout, &(imdec->buffer), 1);
			}
		stop_image_output( &imout );
	}
#ifdef HAVE_MMX
	mmx_off();
#endif
	stop_image_decoding( &imdec );
#ifdef DO_CLOCKING
	fprintf (stderr, __FUNCTION__ " time (clocks): %lu mlsec\n", ((clock () - started)*100)/CLOCKS_PER_SEC);
#endif
	return dst;
}

ASImage *
merge_layers( ASVisual *asv,
				ASImageLayer *layers, int count,
			  	unsigned int dst_width,
			  	unsigned int dst_height,
			  	ASAltImFormats out_format, unsigned int compression_out, int quality )
{
	ASImage *fake_bg = NULL ;
	ASImage *dst = NULL ;
	ASImageDecoder **imdecs ;
	ASImageOutput  *imout ;
	ASScanline dst_line, tmp_line ;
	ASImageLayer *pcurr = layers;
	int i ;
#ifdef DO_CLOCKING
	time_t started = clock ();
#endif

LOCAL_DEBUG_CALLER_OUT( "dst_width = %d, dst_height = %d", dst_width, dst_height );
	dst = safecalloc(1, sizeof(ASImage));
	asimage_start (dst, dst_width, dst_height, compression_out);
	prepare_scanline( dst->width, QUANT_ERR_BITS, &dst_line, asv->BGR_mode );
	prepare_scanline( dst->width, QUANT_ERR_BITS, &tmp_line, asv->BGR_mode );
	dst_line.flags = SCL_DO_ALL ;
	tmp_line.flags = SCL_DO_ALL ;

	imdecs = safecalloc( count, sizeof(ASImageDecoder*));
	if( pcurr->im == NULL )
		pcurr->im = fake_bg = create_asimage( 1, 1, 0 );

	for( i = 0 ; i < count ; i++ )
	{
		if( pcurr->im )
		{
			imdecs[i] = start_image_decoding(asv, pcurr->im, SCL_DO_ALL,
				                             pcurr->clip_x, pcurr->clip_y,
											 pcurr->clip_width, pcurr->clip_height,
											 pcurr->bevel);
			imdecs[i]->back_color = pcurr->back_color ;
		}
		if( pcurr->next == pcurr )
			break;
		else
			pcurr = (pcurr->next!=NULL)?pcurr->next:pcurr+1 ;
	}
	if( i < count )
		count = i+1 ;
#ifdef HAVE_MMX
	mmx_init();
#endif

	if((imout = start_image_output( asv, dst, out_format, QUANT_ERR_BITS, quality)) == NULL )
	{
		asimage_init(dst, True);
		free( dst );
		dst = NULL ;
	}else
	{
		int y, max_y = 0;
		int min_y = dst_height;
		int bg_tint = (layers[0].tint==0)?0x7F7F7F7F:layers[0].tint ;
		int bg_bottom = layers[0].dst_y+layers[0].clip_height+imdecs[0]->bevel_v_addon ;
LOCAL_DEBUG_OUT("blending actually...%s", "");
		pcurr = layers ;
		for( i = 0 ; i < count ; i++ )
		{
			if( imdecs[i] )
			{
				unsigned int layer_bottom = pcurr->dst_y+pcurr->clip_height ;
				if( pcurr->dst_y < min_y )
					min_y = pcurr->dst_y;
				layer_bottom += imdecs[i]->bevel_v_addon ;
				if( layer_bottom > max_y )
					max_y = layer_bottom;
			}
			pcurr = (pcurr->next!=NULL)?pcurr->next:pcurr+1 ;
		}
		if( min_y < 0 )
			min_y = 0 ;
		if( max_y > dst_height )
			max_y = dst_height ;
		else
			imout->tiling_step = max_y ;

/*		for( i = 0 ; i < count ; i++ )
			if( imdecs[i] )
				imdecs[i]->next_line = min_y - layers[i].dst_y ;
 */
LOCAL_DEBUG_OUT( "min_y = %d, max_y = %d", min_y, max_y );
		dst_line.back_color = layers[0].back_color ;
		dst_line.flags = 0 ;
		for( y = 0 ; y < min_y ; y++  )
			imout->output_image_scanline( imout, &dst_line, 1);
		dst_line.flags = SCL_DO_ALL ;
		for( ; y < max_y ; y++  )
		{
			if( layers[0].dst_y <= y && bg_bottom > y )
				imdecs[0]->decode_image_scanline( imdecs[0] );
			else
			{
				imdecs[0]->buffer.back_color = layers[0].back_color ;
				imdecs[0]->buffer.flags = 0 ;
			}
			copytintpad_scanline( &(imdecs[0]->buffer), &dst_line, layers[0].dst_x, bg_tint );
			pcurr = layers[0].next?layers[0].next:&(layers[1]) ;
			for( i = 1 ; i < count ; i++ )
			{
				if( imdecs[i] && pcurr->dst_y <= y &&
					pcurr->dst_y+pcurr->clip_height+imdecs[i]->bevel_v_addon > y )
				{
					imdecs[i]->decode_image_scanline( imdecs[i] );
					copytintpad_scanline( &(imdecs[i]->buffer), &tmp_line, pcurr->dst_x, (pcurr->tint==0)?0x7F7F7F7F:pcurr->tint );
					pcurr->merge_scanlines( &dst_line, &tmp_line, pcurr->merge_mode );
				}
				pcurr = (pcurr->next!=NULL)?pcurr->next:pcurr+1 ;
			}
			imout->output_image_scanline( imout, &dst_line, 1);
		}
		dst_line.back_color = layers[0].back_color ;
		dst_line.flags = 0 ;
		for( ; y < dst_height ; y++  )
			imout->output_image_scanline( imout, &dst_line, 1);
		stop_image_output( &imout );
	}
#ifdef HAVE_MMX
	mmx_off();
#endif
	for( i = 0 ; i < count ; i++ )
		if( imdecs[i] != NULL )
			stop_image_decoding( &(imdecs[i]) );
	free( imdecs );
	if( fake_bg )
		destroy_asimage( &fake_bg );
	free_scanline( &tmp_line, True );
	free_scanline( &dst_line, True );
#ifdef DO_CLOCKING
	fprintf (stderr, __FUNCTION__ " time (clocks): %lu mlsec\n", ((clock () - started)*100)/CLOCKS_PER_SEC);
#endif
	return dst;
}

/* **************************************************************************************/
/* GRADIENT drawing : 																   */
/* **************************************************************************************/
static void
make_gradient_left2right( ASImageOutput *imout, ASScanline *dither_lines, int dither_lines_num, ASFlagType filter )
{
	int line ;

	imout->tiling_step = dither_lines_num;
	for( line = 0 ; line < dither_lines_num ; line++ )
		imout->output_image_scanline( imout, &(dither_lines[line]), 1);
}

static void
make_gradient_top2bottom( ASImageOutput *imout, ASScanline *dither_lines, int dither_lines_num, ASFlagType filter )
{
	int y, height = imout->im->height, width = imout->im->width ;
	int line ;
	ASScanline result;
	CARD32 chan_data[MAX_GRADIENT_DITHER_LINES] = {0,0,0,0};
LOCAL_DEBUG_CALLER_OUT( "width = %d, height = %d, filetr = 0x%lX, dither_count = %d", width, height, filter, dither_lines_num );
	prepare_scanline( width, QUANT_ERR_BITS, &result, imout->asv->BGR_mode );
	for( y = 0 ; y < height ; y++ )
	{
		int color ;

		result.flags = 0 ;
		result.back_color = ARGB32_DEFAULT_BACK_COLOR ;
		for( color = 0 ; color < IC_NUM_CHANNELS ; color++ )
			if( get_flags( filter, 0x01<<color ) )
			{
				Bool dithered = False ;
				for( line = 0 ; line < dither_lines_num ; line++ )
				{
					/* we want to do error diffusion here since in other places it only works
						* in horisontal direction : */
					CARD32 c = dither_lines[line].channels[color][y] + ((dither_lines[line].channels[color][y+1]&0xFF)>>1);
					if( (c&0xFFFF0000) != 0 )
						chan_data[line] = ( c&0x7F000000 )?0:0x0000FF00;
					else
						chan_data[line] = c ;

					if( chan_data[line] != chan_data[0] )
						dithered = True;
				}
				if( !dithered )
				{
					result.back_color = (result.back_color&(~MAKE_ARGB32_CHAN8(0xFF,color)))|
										MAKE_ARGB32_CHAN16(chan_data[0],color);
				}else
				{
					register CARD32  *dst = result.channels[color] ;
					for( line = 0 ; line  < dither_lines_num ; line++ )
					{
						register int x ;
						register CARD32 d = chan_data[line] ;
						for( x = line ; x < width ; x+=dither_lines_num )
							dst[x] = d ;
					}
					set_flags(result.flags, 0x01<<color);
				}
			}
		imout->output_image_scanline( imout, &result, 1);
	}
	free_scanline( &result, True );
}

static void
make_gradient_diag_width( ASImageOutput *imout, ASScanline *dither_lines, int dither_lines_num, ASFlagType filter, Bool from_bottom )
{
	int line = 0;
	/* using bresengham algorithm again to trigger horizontal shift : */
	unsigned short smaller = imout->im->height;
	unsigned short bigger  = imout->im->width;
	register int i = 0;
	int eps;

	if( from_bottom )
		toggle_image_output_direction( imout );
	eps = -(bigger>>1);
	for ( i = 0 ; i < bigger ; i++ )
	{
		eps += smaller;
		if( (eps << 1) >= bigger )
		{
			/* put scanline with the same x offset */
			dither_lines[line].offset_x = i ;
			imout->output_image_scanline( imout, &(dither_lines[line]), 1);
			if( ++line >= dither_lines_num )
				line = 0;
			eps -= bigger ;
		}
	}
}

static void
make_gradient_diag_height( ASImageOutput *imout, ASScanline *dither_lines, int dither_lines_num, ASFlagType filter, Bool from_bottom )
{
	int line = 0;
	unsigned short width = imout->im->width, height = imout->im->height ;
	/* using bresengham algorithm again to trigger horizontal shift : */
	unsigned short smaller = width;
	unsigned short bigger  = height;
	register int i = 0, k =0;
	int eps;
	ASScanline result;
	int *offsets ;

	prepare_scanline( width, QUANT_ERR_BITS, &result, imout->asv->BGR_mode );
	offsets = safemalloc( sizeof(int)*width );
	offsets[0] = 0 ;

	eps = -(bigger>>1);
	for ( i = 0 ; i < bigger ; i++ )
	{
		++offsets[k];
		eps += smaller;
		if( (eps << 1) >= bigger )
		{
			++k ;
			if( k < width )
				offsets[k] = offsets[k-1] ; /* seeding the offset */
			eps -= bigger ;
		}
	}

	if( from_bottom )
		toggle_image_output_direction( imout );

	result.flags = (filter&SCL_DO_ALL);
	if( (filter&SCL_DO_ALL) == SCL_DO_ALL )
	{
		for( i = 0 ; i < height ; i++ )
		{
			for( k = 0 ; k < width ; k++ )
			{
				int offset = i+offsets[k] ;
				CARD32 **src_chan = &(dither_lines[line].channels[0]) ;
				result.alpha[k] = src_chan[IC_ALPHA][offset] ;
				result.red  [k] = src_chan[IC_RED]  [offset] ;
				result.green[k] = src_chan[IC_GREEN][offset] ;
				result.blue [k] = src_chan[IC_BLUE] [offset] ;
				if( ++line >= dither_lines_num )
					line = 0 ;
			}
			imout->output_image_scanline( imout, &result, 1);
		}
	}else
	{
		for( i = 0 ; i < height ; i++ )
		{
			for( k = 0 ; k < width ; k++ )
			{
				int offset = i+offsets[k] ;
				CARD32 **src_chan = &(dither_lines[line].channels[0]) ;
				if( get_flags(filter, SCL_DO_ALPHA) )
					result.alpha[k] = src_chan[IC_ALPHA][offset] ;
				if( get_flags(filter, SCL_DO_RED) )
					result.red[k]   = src_chan[IC_RED]  [offset] ;
				if( get_flags(filter, SCL_DO_GREEN) )
					result.green[k] = src_chan[IC_GREEN][offset] ;
				if( get_flags(filter, SCL_DO_BLUE) )
					result.blue[k]  = src_chan[IC_BLUE] [offset] ;
				if( ++line >= dither_lines_num )
					line = 0 ;
			}
			imout->output_image_scanline( imout, &result, 1);
		}
	}

	free( offsets );
	free_scanline( &result, True );
}

ASImage*
make_gradient( ASVisual *asv, ASGradient *grad,
               unsigned int width, unsigned int height, ASFlagType filter,
  			   ASAltImFormats out_format, unsigned int compression_out, int quality  )
{
	ASImage *im = NULL ;
	ASImageOutput *imout;
	int line_len = width;
#ifdef DO_CLOCKING
	time_t started = clock ();
#endif

	if( asv == NULL || grad == NULL )
		return NULL;
	if( width == 0 )
		width = 2;
 	if( height == 0 )
		height = 2;
	im = safecalloc( 1, sizeof(ASImage) );
	asimage_start (im, width, height, compression_out);
	if( get_flags(grad->type,GRADIENT_TYPE_ORIENTATION) )
		line_len = height ;
	if( get_flags(grad->type,GRADIENT_TYPE_DIAG) )
		line_len = MAX(width,height)<<1 ;
	if((imout = start_image_output( asv, im, out_format, QUANT_ERR_BITS, quality)) == NULL )
	{
		asimage_init(im, True);
		free( im );
		im = NULL ;
	}else
	{
		int dither_lines = MIN(imout->quality+1, MAX_GRADIENT_DITHER_LINES) ;
		ASScanline *lines;
		int line;
		static ARGB32 dither_seeds[MAX_GRADIENT_DITHER_LINES] = { 0, 0xFFFFFFFF, 0x7F0F7F0F, 0x0F7F0F7F };

		if( dither_lines > im->height || dither_lines > im->width )
			dither_lines = MIN(im->height, im->width) ;

		lines = safecalloc( dither_lines, sizeof(ASScanline));
		for( line = 0 ; line < dither_lines ; line++ )
		{
			prepare_scanline( line_len, QUANT_ERR_BITS, &(lines[line]), asv->BGR_mode );
			make_gradient_scanline( &(lines[line]), grad, filter, dither_seeds[line] );
		}
		switch( get_flags(grad->type,GRADIENT_TYPE_MASK) )
		{
			case GRADIENT_Left2Right :
				make_gradient_left2right( imout, lines, dither_lines, filter );
  	    		break ;
			case GRADIENT_Top2Bottom :
				make_gradient_top2bottom( imout, lines, dither_lines, filter );
				break ;
			case GRADIENT_TopLeft2BottomRight :
			case GRADIENT_BottomLeft2TopRight :
				if( width >= height )
					make_gradient_diag_width( imout, lines, dither_lines, filter,
											 (grad->type==GRADIENT_BottomLeft2TopRight));
				else
					make_gradient_diag_height( imout, lines, dither_lines, filter,
											  (grad->type==GRADIENT_BottomLeft2TopRight));
				break ;
			default:
		}
		stop_image_output( &imout );
		for( line = 0 ; line < dither_lines ; line++ )
			free_scanline( &(lines[line]), True );
		free( lines );
	}
#ifdef DO_CLOCKING
	fprintf (stderr, __FUNCTION__ " time (clocks): %lu mlsec\n", ((clock () - started)*100)/CLOCKS_PER_SEC);
#endif
	return im;
}

/* ***************************************************************************/
/* Image flipping(rotation)													*/
/* ***************************************************************************/
ASImage *
flip_asimage( ASVisual *asv, ASImage *src,
		      int offset_x, int offset_y,
			  unsigned int to_width,
			  unsigned int to_height,
			  int flip,
			  ASAltImFormats out_format, unsigned int compression_out, int quality )
{
	ASImage *dst = NULL ;
	ASImageOutput  *imout ;
	ASFlagType filter = SCL_DO_ALL;
#ifdef DO_CLOCKING
	time_t started = clock ();
#endif

LOCAL_DEBUG_CALLER_OUT( "offset_x = %d, offset_y = %d, to_width = %d, to_height = %d", offset_x, offset_y, to_width, to_height );
	if( src )
		filter = get_asimage_chanmask(src);

/*	if( get_flags( flip, FLIP_VERTICAL ) )
	{
		if( to_width > src->height )
			to_width = src->height ;
		if( to_height > src->width )
			to_height = src->width ;
	}
 */
	dst = safecalloc(1, sizeof(ASImage));
	asimage_start (dst, to_width, to_height, compression_out);
#ifdef HAVE_MMX
	mmx_init();
#endif
	if((imout = start_image_output( asv, dst, out_format, 0, quality)) == NULL )
	{
		asimage_init(dst, True);
		free( dst );
		dst = NULL ;
	}else
	{
		ASImageDecoder *imdec ;
		ASScanline result ;
		int y;
LOCAL_DEBUG_OUT("flip-flopping actually...%s", "");
		prepare_scanline( to_width, 0, &result, asv->BGR_mode );
		if( (imdec = start_image_decoding(asv, src, filter, offset_x, offset_y,
		                                  get_flags( flip, FLIP_VERTICAL )?to_height:to_width,
										  get_flags( flip, FLIP_VERTICAL )?to_width:to_height, NULL)) != NULL )
		{
			if( get_flags( flip, FLIP_VERTICAL ) )
			{
				CARD32 *chan_data ;
				size_t  pos = 0;
				int x ;
				CARD32 *a = imdec->buffer.alpha ;
				CARD32 *r = imdec->buffer.red ;
				CARD32 *g = imdec->buffer.green ;
				CARD32 *b = imdec->buffer.blue;

				chan_data = safemalloc( to_width*to_height*sizeof(CARD32));
				result.back_color = ARGB32_DEFAULT_BACK_COLOR ;
				result.flags = filter ;
/*				memset( a, 0x00, to_height*sizeof(CARD32));
				memset( r, 0x00, to_height*sizeof(CARD32));
				memset( g, 0x00, to_height*sizeof(CARD32));
				memset( b, 0x00, to_height*sizeof(CARD32));
  */			for( y = 0 ; y < to_width ; y++ )
				{
					imdec->decode_image_scanline( imdec );
					for( x = 0; x < to_height ; x++ )
					{
						chan_data[pos++] = MAKE_ARGB32( a[x],r[x],g[x],b[x] );
					}
				}

				if( get_flags( flip, FLIP_UPSIDEDOWN ) )
				{
					for( y = 0 ; y < to_height ; ++y )
					{
						pos = y + (to_width-1)*(to_height) ;
						for( x = 0 ; x < to_width ; ++x )
						{
							result.alpha[x] = ARGB32_ALPHA8(chan_data[pos]);
							result.red  [x] = ARGB32_RED8(chan_data[pos]);
							result.green[x] = ARGB32_GREEN8(chan_data[pos]);
							result.blue [x] = ARGB32_BLUE8(chan_data[pos]);
							pos -= to_height ;
						}
						imout->output_image_scanline( imout, &result, 1);
					}
				}else
				{
					for( y = to_height-1 ; y >= 0 ; --y )
					{
						pos = y ;
						for( x = 0 ; x < to_width ; ++x )
						{
							result.alpha[x] = ARGB32_ALPHA8(chan_data[pos]);
							result.red  [x] = ARGB32_RED8(chan_data[pos]);
							result.green[x] = ARGB32_GREEN8(chan_data[pos]);
							result.blue [x] = ARGB32_BLUE8(chan_data[pos]);
							pos += to_height ;
						}
						imout->output_image_scanline( imout, &result, 1);
					}
				}
				free( chan_data );
			}else
			{
				toggle_image_output_direction( imout );
				for( y = 0 ; y < to_height ; y++  )
				{
					imdec->decode_image_scanline( imdec );
					result.flags = imdec->buffer.flags ;
					result.back_color = imdec->buffer.back_color ;
					SCANLINE_FUNC(reverse_component,imdec->buffer,result,0,to_width);
					imout->output_image_scanline( imout, &result, 1);
				}
			}
			stop_image_decoding( &imdec );
		}
		free_scanline( &result, True );
		stop_image_output( &imout );
	}
#ifdef HAVE_MMX
	mmx_off();
#endif
#ifdef DO_CLOCKING
	fprintf (stderr, __FUNCTION__ " time (clocks): %lu mlsec\n", ((clock () - started)*100)/CLOCKS_PER_SEC);
#endif
	return dst;
}

ASImage *
mirror_asimage( ASVisual *asv, ASImage *src,
		      int offset_x, int offset_y,
			  unsigned int to_width,
			  unsigned int to_height,
			  Bool vertical, ASAltImFormats out_format,
			  unsigned int compression_out, int quality )
{
	ASImage *dst = NULL ;
	ASImageOutput  *imout ;
	START_TIME(started);

LOCAL_DEBUG_CALLER_OUT( "offset_x = %d, offset_y = %d, to_width = %d, to_height = %d", offset_x, offset_y, to_width, to_height );
	dst = create_asimage(to_width, to_height, compression_out);
#ifdef HAVE_MMX
	mmx_init();
#endif
	if((imout = start_image_output( asv, dst, out_format, 0, quality)) == NULL )
	{
		asimage_init(dst, True);
		free( dst );
		dst = NULL ;
	}else
	{
		ASImageDecoder *imdec ;
		ASScanline result ;
		int y;
		if( !vertical )
			prepare_scanline( to_width, 0, &result, asv->BGR_mode );
LOCAL_DEBUG_OUT("miroring actually...%s", "");
		if( (imdec = start_image_decoding(asv, src, SCL_DO_ALL, offset_x, offset_y,
		                                  to_width, to_height, NULL)) != NULL )
		{
			if( vertical )
			{
				toggle_image_output_direction( imout );
				for( y = 0 ; y < to_height ; y++  )
				{
					imdec->decode_image_scanline( imdec );
					imout->output_image_scanline( imout, &(imdec->buffer), 1);
				}
			}else
			{
				for( y = 0 ; y < to_height ; y++  )
				{
					imdec->decode_image_scanline( imdec );
					result.flags = imdec->buffer.flags ;
					result.back_color = imdec->buffer.back_color ;
					SCANLINE_FUNC(reverse_component,imdec->buffer,result,0,to_width);
					imout->output_image_scanline( imout, &result, 1);
				}
			}
			stop_image_decoding( &imdec );
		}
		if( !vertical )
			free_scanline( &result, True );
		stop_image_output( &imout );
	}
#ifdef HAVE_MMX
	mmx_off();
#endif
	SHOW_TIME("", started);
	return dst;
}

/**********************************************************************/

Bool fill_asimage( ASVisual *asv, ASImage *im,
               	   int x, int y, int width, int height,
				   ARGB32 color )
{
	ASImageOutput *imout;
	ASImageDecoder *imdec;
	START_TIME(started);

	if( asv == NULL || im == NULL )
		return False;
	if( x < 0 )
	{	width -= x ; x = 0 ; }
	if( y < 0 )
	{	height -= y ; y = 0 ; }

	if( width <= 0 || height <= 0 || x >= im->width || y >= im->height )
		return False;
	if( x+width > im->width )
		width = im->width-x ;
	if( y+height > im->height )
		height = im->height-y ;

	if((imout = start_image_output( asv, im, ASA_ASImage, 0, ASIMAGE_QUALITY_DEFAULT)) == NULL )
		return False ;
	else
	{
		int i ;
		imout->next_line = y ;
		if( x == 0 && width == im->width )
		{
			ASScanline result ;
			result.flags = 0 ;
			result.back_color = color ;
			for( i = 0 ; i < height ; i++ )
				imout->output_image_scanline( imout, &result, 1);
		}else if ((imdec = start_image_decoding(asv, im, SCL_DO_ALL, 0, y, im->width, height, NULL)) != NULL )
		{
			for( i = 0 ; i < height ; i++ )
			{
				imdec->decode_image_scanline( imdec );
				imout->output_image_scanline( imout, &(imdec->buffer), 1);
			}
			stop_image_decoding( &imdec );
		}
	}
	stop_image_output( &imout );
	SHOW_TIME("", started);
	return True;
}


/* ********************************************************************************/
/* The end !!!! 																 */
/* ********************************************************************************/

