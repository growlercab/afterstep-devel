/*
 * Copyright (c) 2000,2001 Sasha Vasko <sashav@sprintmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License.
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

/*#define LOCAL_DEBUG*/
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


void decode_image_scanline_normal( ASImageDecoder *imdec );
void decode_image_scanline_beveled( ASImageDecoder *imdec );
void decode_image_scl_bevel_solid( ASImageDecoder *imdec );


Bool create_image_xim( ASVisual *asv, ASImage *im, ASAltImFormats format );
Bool create_image_argb32( ASVisual *asv, ASImage *im, ASAltImFormats format );

void encode_image_scanline_asim( ASImageOutput *imout, ASScanline *to_store );
void encode_image_scanline_xim( ASImageOutput *imout, ASScanline *to_store );
void encode_image_scanline_mask_xim( ASImageOutput *imout, ASScanline *to_store );
void encode_image_scanline_argb32( ASImageOutput *imout, ASScanline *to_store );

static struct ASImageFormatHandlers
{
	Bool (*check_create_asim_format)( ASVisual *asv, ASImage *im, ASAltImFormats format );
	void (*encode_image_scanline)( ASImageOutput *imout, ASScanline *to_store );
}asimage_format_handlers[ASA_Formats] =
{
	{ NULL, encode_image_scanline_asim },
	{ create_image_xim, encode_image_scanline_xim },
	{ create_image_xim, encode_image_scanline_mask_xim },
	{ create_image_argb32, encode_image_scanline_argb32 }
};



void output_image_line_top( ASImageOutput *, ASScanline *, int );
void output_image_line_fine( ASImageOutput *, ASScanline *, int );
void output_image_line_fast( ASImageOutput *, ASScanline *, int );
void output_image_line_direct( ASImageOutput *, ASScanline *, int );


/* *********************************************************************
 * quality control: we support several levels of quality to allow for
 * smooth work on older computers.
 * *********************************************************************/
static int asimage_quality_level = ASIMAGE_QUALITY_GOOD;
#ifdef HAVE_MMX
Bool asimage_use_mmx = True;
#else
Bool asimage_use_mmx = False;
#endif
/* ********************************************************************/
/* initialization routines 											  */
/* *********************************************************************/
/* ************************** MMX **************************************/
/*inline extern*/
int mmx_init(void)
{
int mmx_available;
#ifdef HAVE_MMX
	asm volatile (
                      /* Get CPU version information */
                      "pushl %%eax\n\t"
                      "pushl %%ebx\n\t"
                      "pushl %%ecx\n\t"
                      "pushl %%edx\n\t"
                      "movl $1, %%eax\n\t"
                      "cpuid\n\t"
                      "andl $0x800000, %%edx \n\t"
					  "movl %%edx, %0\n\t"
                      "popl %%edx\n\t"
                      "popl %%ecx\n\t"
                      "popl %%ebx\n\t"
                      "popl %%eax\n\t"
                      : "=m" (mmx_available)
                      : /* no input */
					  : "ebx", "ecx", "edx"
			  );
#endif
	return mmx_available;
}

int mmx_off(void)
{
#ifdef HAVE_MMX
	__asm__ __volatile__ (
                      /* exit mmx state : */
                      "emms \n\t"
                      : /* no output */
                      : /* no input  */
			  );
#endif
	return 0;
}

/* *********************   ASImage  ************************************/
void
asimage_init (ASImage * im, Bool free_resources)
{
	if (im != NULL)
	{
		if (free_resources)
		{
			register int i ;
			for( i = im->height*4-1 ; i>= 0 ; --i )
				if( im->red[i] )
					free( im->red[i] );
			if( im->red )
				free(im->red);
			if (im->buffer)
				free (im->buffer);
#ifndef X_DISPLAY_MISSING
			if( im->alt.ximage )
				XDestroyImage( im->alt.ximage );
			if( im->alt.mask_ximage )
				XDestroyImage( im->alt.mask_ximage );
#endif
			if( im->alt.argb32 )
				free( im->alt.argb32 );
		}
		memset (im, 0x00, sizeof (ASImage));
		im->magic = MAGIC_ASIMAGE ;
		im->back_color = ARGB32_DEFAULT_BACK_COLOR ;
	}
}

void
asimage_start (ASImage * im, unsigned int width, unsigned int height, unsigned int compression)
{
	if (im)
	{
		register int i;
		CARD32 *tmp ;

		asimage_init (im, True);
		im->width = width;
		im->buf_len = width + width;
		/* we want result to be 32bit aligned and padded */
		tmp = safemalloc (sizeof(CARD32)*(im->buf_len+1));
		im->buffer = (CARD8*)tmp;

		im->height = height;

		im->red = safemalloc (sizeof (CARD8*) * height * 4);
		for( i = 0 ; i < height<<2 ; i++ )
			im->red[i] = 0 ;
		im->green = im->red+height;
		im->blue = 	im->red+(height*2);
		im->alpha = im->red+(height*3);
		im->channels[IC_RED] = im->red ;
		im->channels[IC_GREEN] = im->green ;
		im->channels[IC_BLUE] = im->blue ;
		im->channels[IC_ALPHA] = im->alpha ;

		im->max_compressed_width = width*compression/100;
		if( im->max_compressed_width > im->width )
			im->max_compressed_width = im->width ;
	}
}

ASImage *
create_asimage( unsigned int width, unsigned int height, unsigned int compression)
{
	ASImage *im = safecalloc( 1, sizeof(ASImage) );
	asimage_start( im, width, height, compression );
	return im;
}

void
destroy_asimage( ASImage **im )
{
	if( im )
		if( *im && (*im)->imageman == NULL)
		{
			asimage_init( *im, True );
fprintf( stderr, "destroying image : %p\n", *im );
			(*im)->magic = 0;
			free( *im );
			*im = NULL ;
		}
}

Bool create_image_xim( ASVisual *asv, ASImage *im, ASAltImFormats format )
{
	XImage **dst = (format == ASA_MaskXImage )? &(im->alt.mask_ximage):&(im->alt.ximage);
	if( *dst == NULL )
		if( (*dst = create_visual_ximage( asv, im->width, im->height, (format == ASA_MaskXImage )?1:0 )) == NULL )
			show_error( "Unable to create %sXImage for the visual %d",
				        (format == ASA_MaskXImage )?"mask ":"",
						asv->visual_info.visualid );
	return ( *dst != NULL );
}

Bool create_image_argb32( ASVisual *asv, ASImage *im, ASAltImFormats format )
{
	if( im->alt.argb32 == NULL )
		im->alt.argb32 = safemalloc( im->width*im->height*sizeof(ARGB32) );
	return True;
}

/* ******************** ASImageManager ****************************/
void
asimage_destroy (ASHashableValue value, void *data)
{
	if( data )
	{
		ASImage *im = (ASImage*)data ;
		if( im != NULL )
		{
			if( im->magic != MAGIC_ASIMAGE )
				im = NULL ;
			else
				im->imageman = NULL ;
		}
		free( value.string_val );
		destroy_asimage( &im );
	}
}

ASImageManager *create_image_manager( struct ASImageManager *reusable_memory, double gamma, ... )
{
	ASImageManager *imman = reusable_memory ;
	int i ;
	va_list ap;

	if( imman == NULL )
		imman = safecalloc( 1, sizeof(ASImageManager));
	else
		memset( imman, 0x00, sizeof(ASImageManager));

	va_start (ap, gamma);
	for( i = 0 ; i < MAX_SEARCH_PATHS ; i++ )
	{
		char *path = va_arg(ap,char*);
		if( path == NULL )
			break;
		imman->search_path[i] = mystrdup( path );
	}
	va_end (ap);

	imman->search_path[MAX_SEARCH_PATHS] = NULL ;
	imman->gamma = gamma ;

	imman->image_hash = create_ashash( 7, string_hash_value, string_compare, asimage_destroy );

	return imman;
}

void
destroy_image_manager( struct ASImageManager *imman, Bool reusable )
{
	if( imman )
	{
		int i = MAX_SEARCH_PATHS;
		destroy_ashash( &(imman->image_hash) );
		while( --i >= 0 )
			if(imman->search_path[i])
				free( imman->search_path[i] );

		if( !reusable )
			free( imman );
		else
			memset( imman, 0x00, sizeof(ASImageManager));
	}
}

Bool
store_asimage( ASImageManager* imageman, ASImage *im, const char *name )
{
	Bool res = False ;
	if( im != NULL )
		if( im->magic != MAGIC_ASIMAGE )
			im = NULL ;

	if( imageman && im != NULL && name )
		if( im->imageman == NULL )
		{
			im->name = mystrdup( name );
			res = (add_hash_item( imageman->image_hash, (ASHashableValue)(char*)im->name, im) == ASH_Success);
			if( !res )
			{
				free( im->name );
				im->name = NULL ;
			}else
			{
				im->imageman = imageman ;
				im->ref_count++ ;
			}
		}
	return res ;
}

ASImage *
fetch_asimage( ASImageManager* imageman, const char *name )
{
	ASImage *im = NULL ;
	if( imageman && name )
		if( get_hash_item( imageman->image_hash, (ASHashableValue)((char*)name), (void**)&im) == ASH_Success )
		{
			if( im->magic != MAGIC_ASIMAGE )
				im = NULL ;
			else
				im->ref_count++ ;
		}
	return im;
}

ASImage *
dup_asimage( ASImage* im )
{
	if( im != NULL )
		if( im->magic != MAGIC_ASIMAGE )
			im = NULL ;

	if( im && im->imageman )
	{
		im->ref_count++ ;
		return im;
	}
	return NULL ;
}

int
release_asimage( ASImage *im )
{
	int res = -1 ;
	if( im )
	{
		if( im->magic == MAGIC_ASIMAGE )
		{
			if( --(im->ref_count) < 0 )
			{
				ASImageManager *imman = im->imageman ;
				if( imman )
					remove_hash_item(imman->image_hash, (ASHashableValue)(char*)im->name, NULL, True);
			}else
				res = im->ref_count ;
		}
	}
	return res ;
}

int
release_asimage_by_name( ASImageManager *imageman, char *name )
{
	int res = -1 ;
	ASImage *im = NULL ;
	if( imageman && name )
		if( get_hash_item( imageman->image_hash, (ASHashableValue)((char*)name), (void**)&im) == ASH_Success )
			if( im->magic == MAGIC_ASIMAGE )
				res = release_asimage( im );
	return res ;
}

/* ******************** ASImageDecoder ****************************/
ASImageDecoder *
start_image_decoding( ASVisual *asv,ASImage *im, ASFlagType filter,
					  int offset_x, int offset_y,
					  unsigned int out_width,
					  unsigned int out_height,
					  ASImageBevel *bevel )
{
	ASImageDecoder *imdec = NULL;

	if( filter == 0 || asv == NULL )
		return NULL;
	if( im != NULL )
		if( im->magic != MAGIC_ASIMAGE )
			im = NULL ;

	if( im == NULL )
	{
		offset_x = offset_y = 0 ;
		if( out_width == 0 || out_height == 0 )
			return NULL ;
	}else
	{
		if( offset_x < 0 )
			offset_x = im->width - (offset_x%im->width);
		else
			offset_x %= im->width ;
		if( offset_y < 0 )
			offset_y = im->height - (offset_y%im->height);
		else
			offset_y %= im->height ;
		if( out_width == 0 )
			out_width = im->width ;
		if( out_height == 0 )
			out_height = im->height ;
	}

	imdec = safecalloc( 1, sizeof(ASImageDecoder));
	imdec->im = im ;
	imdec->filter = filter ;
	imdec->offset_x = offset_x ;
	imdec->out_width = out_width;
	imdec->offset_y = offset_y ;
	imdec->out_height = out_height;
	imdec->next_line = offset_y ;
	imdec->back_color = (im != NULL)?im->back_color:ARGB32_DEFAULT_BACK_COLOR ;
	imdec->bevel = bevel ;
	if( bevel )
	{
		if( bevel->left_outline > MAX_BEVEL_OUTLINE )
			bevel->left_outline = MAX_BEVEL_OUTLINE ;
		if( bevel->top_outline > MAX_BEVEL_OUTLINE )
			bevel->top_outline = MAX_BEVEL_OUTLINE ;
		if( bevel->right_outline > MAX_BEVEL_OUTLINE )
			bevel->right_outline = MAX_BEVEL_OUTLINE ;
		if( bevel->bottom_outline > MAX_BEVEL_OUTLINE )
			bevel->bottom_outline = MAX_BEVEL_OUTLINE ;
		if( bevel->left_inline >= out_width )
			bevel->left_inline = out_width-1 ;
		if( bevel->top_inline >= out_height )
			bevel->top_inline = out_height-1 ;
		if( bevel->left_inline+bevel->right_inline >= out_width )
			bevel->right_inline = out_width-5-bevel->left_inline ;
		if( bevel->top_inline+bevel->bottom_inline >= out_height )
			bevel->bottom_inline = out_height-1-bevel->top_inline ;

		imdec->bevel_h_addon = bevel->left_outline+bevel->right_outline ;
		imdec->bevel_v_addon = bevel->top_outline+bevel->bottom_outline ;

		if( get_flags( bevel->type, BEVEL_SOLID_INLINE ) )
			imdec->decode_image_scanline = decode_image_scl_bevel_solid ;
		else
			imdec->decode_image_scanline = decode_image_scanline_beveled ;
	}else
		imdec->decode_image_scanline = decode_image_scanline_normal ;


	prepare_scanline(out_width+imdec->bevel_h_addon, 0, &(imdec->buffer), asv->BGR_mode );
	imdec->buffer.back_color = ARGB32_DEFAULT_BACK_COLOR;

	return imdec;
}

void
set_decoder_shift( ASImageDecoder *imdec, int shift )
{
	if( shift != 0 )
		shift = 8 ;

	if( imdec )
		imdec->buffer.shift = shift ;
}

void set_decoder_back_color( ASImageDecoder *imdec, ARGB32 back_color )
{
	if( imdec )
	{
		imdec->back_color = back_color ;
		imdec->buffer.back_color = back_color ;
	}
}


void
stop_image_decoding( ASImageDecoder **pimdec )
{
	if( pimdec )
		if( *pimdec )
		{
			free_scanline( &((*pimdec)->buffer), True );
			free( *pimdec );
			*pimdec = NULL;
		}
}

/* ******************** ASImageOutput ****************************/

ASImageOutput *
start_image_output( ASVisual *asv, ASImage *im, ASAltImFormats format,
                    int shift, int quality )
{
	register ASImageOutput *imout= NULL;

	if( im != NULL )
		if( im->magic != MAGIC_ASIMAGE )
			im = NULL ;

	if( im == NULL || asv == NULL || format < 0 || format >= ASA_Formats)
		return imout;

	if( asimage_format_handlers[format].check_create_asim_format )
		if( !asimage_format_handlers[format].check_create_asim_format(asv, im, format) )
			return NULL;

	imout = safecalloc( 1, sizeof(ASImageOutput));
	imout->asv = asv;
	imout->im = im ;

	imout->out_format = format ;
	imout->encode_image_scanline = asimage_format_handlers[format].encode_image_scanline;

	prepare_scanline( im->width, 0, &(imout->buffer[0]), asv->BGR_mode);
	prepare_scanline( im->width, 0, &(imout->buffer[1]), asv->BGR_mode);

	imout->chan_fill[IC_RED]   = ARGB32_RED8(im->back_color);
	imout->chan_fill[IC_GREEN] = ARGB32_GREEN8(im->back_color);
	imout->chan_fill[IC_BLUE]  = ARGB32_BLUE8(im->back_color);
	imout->chan_fill[IC_ALPHA] = ARGB32_ALPHA8(im->back_color);

	imout->available = &(imout->buffer[0]);
	imout->used 	 = NULL;
	imout->buffer_shift = shift;
	imout->next_line = 0 ;
	imout->bottom_to_top = 1 ;
	if( quality > ASIMAGE_QUALITY_TOP || quality < ASIMAGE_QUALITY_POOR )
		quality = asimage_quality_level;

	imout->quality = quality ;
	if( shift > 0 )
	{/* choose what kind of error diffusion we'll use : */
		switch( quality )
		{
			case ASIMAGE_QUALITY_POOR :
			case ASIMAGE_QUALITY_FAST :
				imout->output_image_scanline = output_image_line_fast ;
				break;
			case ASIMAGE_QUALITY_GOOD :
				imout->output_image_scanline = output_image_line_fine ;
				break;
			case ASIMAGE_QUALITY_TOP  :
				imout->output_image_scanline = output_image_line_top ;
				break;
		}
	}else /* no quanitzation - no error diffusion */
		imout->output_image_scanline = output_image_line_direct ;

	return imout;
}

void set_image_output_back_color( ASImageOutput *imout, ARGB32 back_color )
{
	if( imout )
	{
		imout->chan_fill[IC_RED]   = ARGB32_RED8  (back_color);
		imout->chan_fill[IC_GREEN] = ARGB32_GREEN8(back_color);
		imout->chan_fill[IC_BLUE]  = ARGB32_BLUE8 (back_color);
		imout->chan_fill[IC_ALPHA] = ARGB32_ALPHA8(back_color);
	}
}

void toggle_image_output_direction( ASImageOutput *imout )
{
	if( imout )
	{
		if( imout->bottom_to_top < 0 )
		{
			if( imout->next_line >= imout->im->height-1 )
				imout->next_line = 0 ;
			imout->bottom_to_top = 1 ;
		}else if( imout->next_line <= 0 )
		{
		 	imout->next_line = imout->im->height-1 ;
			imout->bottom_to_top = -1 ;
		}
	}
}


void
stop_image_output( ASImageOutput **pimout )
{
	if( pimout )
	{
		register ASImageOutput *imout = *pimout;
		if( imout )
		{
			if( imout->used )
				imout->output_image_scanline( imout, NULL, 1);
			free_scanline(&(imout->buffer[0]), True);
			free_scanline(&(imout->buffer[1]), True);
			free( imout );
			*pimout = NULL;
		}
	}
}

/* ******************** ASImageLayer ****************************/

inline void
init_image_layers( register ASImageLayer *l, int count )
{
	memset( l, 0x00, sizeof(ASImageLayer)*count );
	while( --count >= 0 )
	{
		l[count].merge_scanlines = alphablend_scanlines ;
/*		l[count].solid_color = ARGB32_DEFAULT_BACK_COLOR ; */
	}
}

ASImageLayer *
create_image_layers( int count )
{
	ASImageLayer *l = NULL;

	if( count > 0 )
	{
		l = safecalloc( count, sizeof(ASImageLayer) );
		init_image_layers( l, count );
	}
	return l;
}

void
destroy_image_layers( register ASImageLayer *l, int count, Bool reusable )
{
	if( l )
	{
		while( --count >= 0 )
		{
			if( l[count].im )
			{
				if( l[count].im->imageman )
					release_asimage( l[count].im );
				else
					destroy_asimage( &(l[count].im) );
			}
			if( l[count].bevel )
				free( l[count].bevel );
		}
		if( !reusable )
			free( l );
		else
			memset( l, 0x00, sizeof(ASImageLayer)*count );
	}
}



/* **********************************************************************/
/*  Compression/decompression 										   */
/* **********************************************************************/
static void
asimage_apply_buffer (ASImage * im, ColorPart color, unsigned int y)
{
	size_t   len = (im->buf_used>>2)+1 ;
	CARD8  **part = im->channels[color];
	register CARD32  *dwdst = safemalloc( sizeof(CARD32)*len);
	register CARD32  *dwsrc = (CARD32*)(im->buffer);
	register int i ;
	for( i = 0 ; i < len ; ++i )
		dwdst[i] = dwsrc[i];

	if (part[y] != NULL)
		free (part[y]);
	part[y] = (CARD8*)dwdst;
}

static void
asimage_dup_line (ASImage * im, ColorPart color, unsigned int y1, unsigned int y2, unsigned int length)
{
	CARD8       **part = im->channels[color];
	if (part[y2] != NULL)
		free (part[y2]);
	if( part[y1] )
	{
		register int i ;
		register CARD32 *dwsrc = (CARD32*)part[y1];
		register CARD32 *dwdst ;
		length = (length>>2)+1;
		dwdst = safemalloc (sizeof(CARD32)*length);
		/* 32bit copy gives us about 15% performance gain */
		for( i = 0 ; i < length ; ++i )
			dwdst[i] = dwsrc[i];
		part[y2] = (CARD8*)dwdst;
	}else
		part[y2] = NULL;
}

void
asimage_erase_line( ASImage * im, ColorPart color, unsigned int y )
{
	if( im )
	{
		if( color < IC_NUM_CHANNELS )
		{
			CARD8       **part = im->channels[color];
			if( part[y] )
			{
				free( part[y] );
				part[y] = NULL;
			}
		}else
		{
			int c ;
			for( c = 0 ; c < IC_NUM_CHANNELS ; c++ )
			{
				CARD8       **part = im->channels[color];
				if( part[y] )
				{
					free( part[y] );
					part[y] = NULL;
				}
			}
		}
	}
}


size_t
asimage_add_line_mono (ASImage * im, ColorPart color, register CARD8 value, unsigned int y)
{
	register CARD8 *dst;
	int rep_count;

	if (im == NULL || color <0 || color >= IC_NUM_CHANNELS )
		return 0;
	if (im->buffer == NULL || y >= im->height)
		return 0;

	dst = im->buffer;
	rep_count = im->width - RLE_THRESHOLD;
	if (rep_count <= RLE_MAX_SIMPLE_LEN)
	{
		dst[0] = (CARD8) rep_count;
		dst[1] = (CARD8) value;
		dst[2] = 0 ;
		im->buf_used = 3;
	} else
	{
		dst[0] = (CARD8) ((rep_count >> 8) & RLE_LONG_D)|RLE_LONG_B;
		dst[1] = (CARD8) ((rep_count) & 0x00FF);
		dst[2] = (CARD8) value;
		dst[3] = 0 ;
		im->buf_used = 4;
	}
	asimage_apply_buffer (im, color, y);
	return im->buf_used;
}

size_t
asimage_add_line (ASImage * im, ColorPart color, register CARD32 * data, unsigned int y)
{
	int             i = 0, bstart = 0, ccolor = 0;
	unsigned int    width;
	register CARD8 *dst;
	register int 	tail = 0;
	int best_size, best_bstart = 0, best_tail = 0;

	if (im == NULL || data == NULL || color <0 || color >= IC_NUM_CHANNELS )
		return 0;
	if (im->buffer == NULL || y >= im->height)
		return 0;

	best_size = 0 ;
	dst = im->buffer;
/*	fprintf( stderr, "max = %d, width = %d, %d:%d:%d<%2.2X ", im->max_compressed_width, im->width, y, color, 0, data[0] );*/

	if( im->width == 1 )
	{
		dst[0] = RLE_DIRECT_TAIL ;
		dst[1] = data[0] ;
		im->buf_used = 2;
	}else
	{
/*		width = im->width; */
		width = im->max_compressed_width ;
		while( i < width )
		{
			while( i < width && data[i] == data[ccolor])
			{
/*				fprintf( stderr, "%d<%2.2X ", i, data[i] ); */
				++i ;
			}
			if( i > ccolor + RLE_THRESHOLD )
			{ /* we have to write repetition count and length */
				register unsigned int rep_count = i - ccolor - RLE_THRESHOLD;

				if (rep_count <= RLE_MAX_SIMPLE_LEN)
				{
					dst[tail] = (CARD8) rep_count;
					dst[++tail] = (CARD8) data[ccolor];
/*					fprintf( stderr, "\n%d:%d: >%d: %2.2X %d: %2.2X ", y, color, tail-1, dst[tail-1], tail, dst[tail] ); */
				} else
				{
					dst[tail] = (CARD8) ((rep_count >> 8) & RLE_LONG_D)|RLE_LONG_B;
					dst[++tail] = (CARD8) ((rep_count) & 0x00FF);
					dst[++tail] = (CARD8) data[ccolor];
/*					fprintf( stderr, "\n%d:%d: >%d: %2.2X %d: %2.2X %d: %2.2X ", y, color, tail-2, dst[tail-2], tail-1, dst[tail-1], tail, dst[tail] );*/
				}
				++tail ;
				bstart = ccolor = i;
			}
/*			fprintf( stderr, "\n"); */
			while( i < width )
			{
/*				fprintf( stderr, "%d<%2.2X ", i, data[i] ); */
				if( data[i] != data[ccolor] )
					ccolor = i ;
				else if( i-ccolor > RLE_THRESHOLD )
					break;
				++i ;
			}
			if( i == width )
				ccolor = i ;
			while( ccolor > bstart )
			{/* we have to write direct block */
				int dist = ccolor-bstart ;

				if( tail-bstart < best_size )
				{
					best_tail = tail ;
					best_bstart = bstart ;
					best_size = tail-bstart ;
				}
				if( dist > RLE_MAX_DIRECT_LEN )
					dist = RLE_MAX_DIRECT_LEN ;
				dst[tail] = RLE_DIRECT_B | ((CARD8)(dist-1));
/*				fprintf( stderr, "\n%d:%d: >%d: %2.2X ", y, color, tail, dst[tail] ); */
				dist += bstart ;
				++tail ;
				while ( bstart < dist )
				{
					dst[tail] = (CARD8) data[bstart];
/*					fprintf( stderr, "%d: %2.2X ", tail, dst[tail]); */
					++tail ;
					++bstart;
				}
			}
/*			fprintf( stderr, "\n"); */
		}
		if( best_size+im->width < tail )
		{
			width = im->width;
/*			LOCAL_DEBUG_OUT( " %d:%d >resetting bytes starting with offset %d(%d) (0x%2.2X) to DIRECT_TAIL( %d bytes total )", y, color, best_tail, best_bstart, dst[best_tail], width-best_bstart ); */
			dst[best_tail] = RLE_DIRECT_TAIL;
			dst += best_tail+1;
			data += best_bstart;
			for( i = width-best_bstart-1 ; i >=0 ; --i )
				dst[i] = data[i] ;
			im->buf_used = best_tail+1+width-best_bstart ;
		}else
		{
			if( i < im->width )
			{
				dst[tail] = RLE_DIRECT_TAIL;
				dst += tail+1 ;
				data += i;
				im->buf_used = tail+1+im->width-i ;
				for( i = im->width-(i+1) ; i >=0 ; --i )
					dst[i] = data[i] ;
			}else
			{
				dst[tail] = RLE_EOL;
				im->buf_used = tail+1;
			}
		}
	}
	asimage_apply_buffer (im, color, y);
	return im->buf_used;
}

ASFlagType
get_asimage_chanmask( ASImage *im)
{
    ASFlagType mask = 0 ;
	int color ;

	if( im )
		for( color = 0; color < IC_NUM_CHANNELS ; color++ )
		{
			register CARD8 **chan = im->channels[color];
			register int y, height = im->height ;
			for( y = 0 ; y < height ; y++ )
				if( chan[y] )
				{
					set_flags( mask, 0x01<<color );
					break;
				}
		}
    return mask ;
}


unsigned int
asimage_print_line (ASImage * im, ColorPart color, unsigned int y, unsigned long verbosity)
{
	CARD8       **color_ptr;
	register CARD8 *ptr;
	int           to_skip = 0;
	int 		  uncopressed_size = 0 ;

	if (im == NULL || color < 0 || color >= IC_NUM_CHANNELS )
		return 0;
	if (y >= im->height)
		return 0;

	if((color_ptr = im->channels[color]) == NULL )
		return 0;
	ptr = color_ptr[y];
	if( ptr == NULL )
	{
		if(  verbosity != 0 )
			show_error( "no data available for line %d", y );
		return 0;
	}
	while (*ptr != RLE_EOL)
	{
		while (to_skip-- > 0)
		{
			if (get_flags (verbosity, VRB_LINE_CONTENT))
				fprintf (stderr, " %2.2X", *ptr);
			ptr++;
		}

		if (get_flags (verbosity, VRB_CTRL_EXPLAIN))
			fprintf (stderr, "\nControl byte encountered : %2.2X", *ptr);

		if (((*ptr) & RLE_DIRECT_B) != 0)
		{
			if( *ptr == RLE_DIRECT_TAIL )
			{
				if (get_flags (verbosity, VRB_CTRL_EXPLAIN))
					fprintf (stderr, " is RLE_DIRECT_TAIL ( %d bytes ) !", im->width-uncopressed_size);
				if (get_flags (verbosity, VRB_LINE_CONTENT))
				{
					to_skip = im->width-uncopressed_size ;
					while (to_skip-- > 0)
					{
						fprintf (stderr, " %2.2X", *ptr);
						ptr++;
					}
				}else
					ptr += im->width-uncopressed_size ;
				break;
			}
			to_skip = 1 + ((*ptr) & (RLE_DIRECT_D));
			uncopressed_size += to_skip;
			if (get_flags (verbosity, VRB_CTRL_EXPLAIN))
				fprintf (stderr, " is RLE_DIRECT !");
		} else if (((*ptr) & RLE_SIMPLE_B_INV) == 0)
		{
			if( *ptr == RLE_EOL )
			{
				if (get_flags (verbosity, VRB_CTRL_EXPLAIN))
					fprintf (stderr, " is RLE_EOL !");
				break;
			}
			if (get_flags (verbosity, VRB_CTRL_EXPLAIN))
				fprintf (stderr, " is RLE_SIMPLE !");
			uncopressed_size += ((int)ptr[0])+ RLE_THRESHOLD;
			to_skip = 1;
		} else if (((*ptr) & RLE_LONG_B) != 0)
		{
			if (get_flags (verbosity, VRB_CTRL_EXPLAIN))
				fprintf (stderr, " is RLE_LONG !");
			uncopressed_size += ((int)ptr[1])+((((int)ptr[0])&RLE_LONG_D ) << 8)+RLE_THRESHOLD;
			to_skip = 1 + 1;
		}
		to_skip++;
		if (get_flags (verbosity, VRB_CTRL_EXPLAIN))
			fprintf (stderr, " to_skip = %d, uncompressed size = %d\n", to_skip, uncopressed_size);
	}
	if (get_flags (verbosity, VRB_LINE_CONTENT))
		fprintf (stderr, " %2.2X\n", *ptr);

	ptr++;

	if (get_flags (verbosity, VRB_LINE_SUMMARY))
		fprintf (stderr, "Row %d, Component %d, Memory Used %d\n", y, color, ptr - color_ptr[y]);
	return ptr - color_ptr[y];
}

void print_asimage( ASImage *im, int flags, char * func, int line )
{
	if( im )
	{
		register int k ;
		int total_mem = 0 ;
		fprintf( stderr, "%s:%d> printing ASImage %p.\n", func, line, im);
		for( k = 0 ; k < im->height ; k++ )
    	{
 			fprintf( stderr, "%s:%d> ******* %d *******\n", func, line, k );
			total_mem+=asimage_print_line( im, IC_RED  , k, flags );
			total_mem+=asimage_print_line( im, IC_GREEN, k, flags );
			total_mem+=asimage_print_line( im, IC_BLUE , k, flags );
    	}
    	fprintf( stderr, "%s:%d> Total memory : %u - image size %dx%d ratio %d%%\n", func, line, total_mem, im->width, im->height, (total_mem*100)/(im->width*im->height*3) );
	}else
		fprintf( stderr, "%s:%d> Attempted to print NULL ASImage.\n", func, line);
}



inline static CARD32*
asimage_decode_block32 (register CARD8 *src, CARD32 *to_buf, unsigned int width )
{
	register CARD32 *dst = to_buf;
	while ( *src != RLE_EOL)
	{
		if( src[0] == RLE_DIRECT_TAIL )
		{
			register int i = width - (dst-to_buf) ;
			dst += i ;
			src += i+1 ;
			i = -i ;
			while( i < 0 )
			{
				dst[i] = src[i] ;
				++i ;
			}
			break;
		}else if( ((*src)&RLE_DIRECT_B) != 0 )
		{
			register int i = (((int)src[0])&RLE_DIRECT_D) + 1;
			dst += i ;
			src += i+1 ;
			i = -i ;
			while( i < 0 )
			{
				dst[i] = src[i] ;
				++i ;
			}
		}else if( ((*src)&RLE_LONG_B) != 0 )
		{
			register int i = ((((int)src[0])&RLE_LONG_D)<<8|src[1]) + RLE_THRESHOLD;
			dst += i ;
			i = -i ;
			while( i < 0 )
			{
				dst[i] = src[2] ;
				++i ;
			}
			src += 3;
		}else
		{
			register int i = (int)src[0] + RLE_THRESHOLD ;
			dst += i ;
			i = -i;
			while( i < 0 )
			{
				dst[i] = src[1] ;
				++i ;
			}
			src += 2;
		}
	}
	return dst;
}

inline static CARD8*
asimage_decode_block8 (register CARD8 *src, CARD8 *to_buf, unsigned int width )
{
	register CARD8 *dst = to_buf;
	while ( *src != RLE_EOL)
	{
		if( src[0] == RLE_DIRECT_TAIL )
		{
			register int i = width - (dst-to_buf) ;
			dst += i ;
			src += i+1 ;
			i = -i ;
			while( i < 0 )
			{
				dst[i] = src[i] ;
				++i ;
			}
			break;
		}else if( ((*src)&RLE_DIRECT_B) != 0 )
		{
			register int i = (((int)src[0])&RLE_DIRECT_D) + 1;
			dst += i ;
			src += i+1 ;
			i = -i ;
			while( i < 0 )
			{
				dst[i] = src[i] ;
				++i ;
			}
		}else if( ((*src)&RLE_LONG_B) != 0 )
		{
			register int i = ((((int)src[0])&RLE_LONG_D)<<8|src[1]) + RLE_THRESHOLD;
			dst += i ;
			i = -i ;
			while( i < 0 )
			{
				dst[i] = src[2] ;
				++i ;
			}
			src += 3;
		}else
		{
			register int i = (int)src[0] + RLE_THRESHOLD ;
			dst += i ;
			i = -i;
			while( i < 0 )
			{
				dst[i] = src[1] ;
				++i ;
			}
			src += 2;
		}
	}
	return dst;
}

void print_component( register CARD32 *data, int nonsense, int len );

inline int
asimage_decode_line (ASImage * im, ColorPart color, CARD32 * to_buf, unsigned int y, unsigned int skip, unsigned int out_width)
{
	register CARD8  *src = im->channels[color][y];
	/* that thing below is supposedly highly optimized : */
LOCAL_DEBUG_CALLER_OUT( "im->width = %d, color = %d, y = %d, skip = %d, out_width = %d, src = %p", im->width, color, y, skip, out_width, src );
	if( src )
	{
		register int i = 0;
#if 1
  		if( skip > 0 || out_width+skip < im->width)
		{
			int max_i ;

			asimage_decode_block8( src, im->buffer, im->width );
			skip = skip%im->width ;
			max_i = MIN(out_width,im->width-skip);
			src = im->buffer+skip ;
			while( i < out_width )
			{
				while( i < max_i )
				{
					to_buf[i] = src[i] ;
					++i ;
				}
				src = im->buffer-i ;
				max_i = MIN(out_width,im->width+i) ;
			}
		}else
		{
#endif
			i = asimage_decode_block32( src, to_buf, im->width ) - to_buf;
#if 1
	  		while( i < out_width )
			{   /* tiling code : */
				register CARD32 *src = to_buf-i ;
				int max_i = MIN(out_width,im->width+i);
				while( i < max_i )
				{
					to_buf[i] = src[i] ;
					++i ;
				}
			}
#endif
		}
		return i;
	}
	return 0;
}


CARD8*
asimage_copy_line (register CARD8 *src, int width)
{
	register int i = 0;

	/* merely copying the data */
	if ( src == NULL )
		return NULL;
	while (src[i] != RLE_EOL && width )
	{
		if ((src[i] & RLE_DIRECT_B) != 0)
		{
			if( src[i] == RLE_DIRECT_TAIL )
			{
				i += width+1 ;
				break;
			}else
			{
				register int to_skip = (src[i] & (RLE_DIRECT_D))+1;
				width -= to_skip ;
				i += to_skip ;
			}
		} else if ((src[i]&RLE_SIMPLE_B_INV) == 0)
		{
			width -= ((int)src[i])+ RLE_THRESHOLD;
			++i ;
		} else if ((src[i] & RLE_LONG_B) != 0)
		{
			register int to_skip = ((((int)src[i])&RLE_LONG_D ) << 8) ;
			width -= ((int)src[++i])+to_skip+RLE_THRESHOLD;
			++i;
		}
		++i;
	}
	if( i > 0 )
	{
		CARD8 *res = safemalloc( i+1 );
		memcpy( res, src, i+1 );
		return res;
	}else
		return NULL ;
}

void
move_asimage_channel( ASImage *dst, int channel_dst, ASImage *src, int channel_src )
{
	if( dst && src && channel_src >= 0 && channel_src < IC_NUM_CHANNELS &&
		channel_dst >= 0 && channel_dst < IC_NUM_CHANNELS )
		if( dst->width == src->width )
		{
			register int i = MIN(dst->height, src->height);
			register CARD8 **dst_rows = dst->channels[channel_dst] ;
			register CARD8 **src_rows = src->channels[channel_src] ;
			while( --i >= 0 )
			{
				if( dst_rows[i] )
					free( dst_rows[i] );
				dst_rows[i] = src_rows[i] ;
				src_rows[i] = NULL ;
			}
		}
}


void
copy_asimage_channel( ASImage *dst, int channel_dst, ASImage *src, int channel_src )
{
	if( dst && src && channel_src >= 0 && channel_src < IC_NUM_CHANNELS &&
		channel_dst >= 0 && channel_dst < IC_NUM_CHANNELS )
		if( dst->width == src->width )
		{
			register int i = MIN(dst->height, src->height);
			register CARD8 **dst_rows = dst->channels[channel_dst] ;
			register CARD8 **src_rows = src->channels[channel_src] ;
			while( --i >= 0 )
			{
				if( dst_rows[i] )
					free( dst_rows[i] );
				dst_rows[i] = asimage_copy_line( src_rows[i], dst->width );
			}
		}
}

void
copy_asimage_lines( ASImage *dst, unsigned int offset_dst,
                    ASImage *src, unsigned int offset_src,
					unsigned int nlines, ASFlagType filter )
{
	if( dst && src &&
		offset_src < src->height && offset_dst < dst->height &&
		dst->width == src->width )
	{
		int chan;

		if( offset_src+nlines > src->height )
			nlines = src->height - offset_src ;
		if( offset_dst+nlines > dst->height )
			nlines = dst->height - offset_dst ;

		for( chan = 0 ; chan < IC_NUM_CHANNELS ; ++chan )
			if( get_flags( filter, 0x01<<chan ) )
			{
				register int i = -1;
				register CARD8 **dst_rows = &(dst->channels[chan][offset_dst]) ;
				register CARD8 **src_rows = &(src->channels[chan][offset_src]) ;
LOCAL_DEBUG_OUT( "copying %d lines of channel %d...", nlines, chan );
				while( ++i < nlines )
				{
					if( dst_rows[i] )
						free( dst_rows[i] );
					dst_rows[i] = asimage_copy_line( src_rows[i], dst->width );
				}
			}
#if 0
		for( i = 0 ; i < nlines ; ++i )
		{
			asimage_print_line( src, IC_ALPHA, i, (i==4)?VRB_EVERYTHING:VRB_LINE_SUMMARY );
			asimage_print_line( dst, IC_ALPHA, i, (i==4)?VRB_EVERYTHING:VRB_LINE_SUMMARY );
		}
#endif
	}
}

Bool
asimage_compare_line (ASImage *im, ColorPart color, CARD32 *to_buf, CARD32 *tmp, unsigned int y, Bool verbose)
{
	register int i;
	asimage_decode_line( im, color, tmp, y, 0, im->width );
	for( i = 0 ; i < im->width ; i++ )
		if( tmp[i] != to_buf[i] )
		{
			if( verbose )
				show_error( "line %d, component %d differ at offset %d ( 0x%lX(compresed) != 0x%lX(orig) )\n", y, color, i, tmp[i], to_buf[i] );
			return False ;
		}
	return True;
}

/* for consistency sake : */
inline void
copy_component( register CARD32 *src, register CARD32 *dst, int *unused, int len )
{
#if 1
#ifdef CARD64
	CARD64 *dsrc = (CARD64*)src;
	CARD64 *ddst = (CARD64*)dst;
#else
	double *dsrc = (double*)src;
	double *ddst = (double*)dst;
#endif
	register int i = 0;

	len += len&0x01;
	len = len>>1 ;
	do
	{
		ddst[i] = dsrc[i];
	}while(++i < len );
#else
	register int i = 0;

	len += len&0x01;
	do
	{
		dst[i] = src[i];
	}while(++i < len );
#endif
}

static inline int
set_component( register CARD32 *src, register CARD32 value, int offset, int len )
{
	register int i ;
	for( i = offset ; i < len ; ++i )
		src[i] = value;
	return len-offset;
}



static inline void
divide_component( register CARD32 *src, register CARD32 *dst, CARD16 ratio, int len )
{
	register int i = 0;
	len += len&0x00000001;                     /* we are 8byte aligned/padded anyways */
	if( ratio == 2 )
	{
#ifdef HAVE_MMX
		if( asimage_use_mmx )
		{
			double *ddst = (double*)&(dst[0]);
			double *dsrc = (double*)&(src[0]);
			len = len>>1;
			do{
				asm volatile
       		    (
            		"movq %1, %%mm0  \n\t" // load 8 bytes from src[i] into MM0
            		"psrld $1, %%mm0 \n\t" // MM0=src[i]>>1
            		"movq %%mm0, %0  \n\t" // store the result in dest
					: "=m" (ddst[i]) // %0
					: "m"  (dsrc[i]) // %1
	            );
			}while( ++i < len );
		}else
#endif
			do{
				dst[i] = src[i] >> 1;
				dst[i+1] = src[i+1]>> 1;
				i += 2 ;
			}while( i < len );
	}else
	{
		do{
			register int c1 = src[i];
			register int c2 = src[i+1];
			dst[i] = c1/ratio;
			dst[i+1] = c2/ratio;
			i+=2;
		}while( i < len );
	}
}

/* diffusingly combine src onto self and dst, and rightbitshift src by quantization shift */
static inline void
best_output_filter( register CARD32 *line1, register CARD32 *line2, int unused, int len )
{/* we carry half of the quantization error onto the surrounding pixels : */
 /*        X    7/16 */
 /* 3/16  5/16  1/16 */
	register int i ;
	register CARD32 errp = 0, err = 0, c;
	c = line1[0];
	if( (c&0xFFFF0000)!= 0 )
		c = ( c&0x7F000000 )?0:0x0000FFFF;
	errp = c&QUANT_ERR_MASK;
	line1[0] = c>>QUANT_ERR_BITS ;
	line2[0] += (errp*5)>>4 ;

	for( i = 1 ; i < len ; ++i )
	{
		c = line1[i];
		if( (c&0xFFFF0000)!= 0 )
			c = (c&0x7F000000)?0:0x0000FFFF;
		c += ((errp*7)>>4) ;
		err = c&QUANT_ERR_MASK;
		line1[i] = (c&0x7FFF0000)?0x000000FF:(c>>QUANT_ERR_BITS);
		line2[i-1] += (err*3)>>4 ;
		line2[i] += ((err*5)>>4)+(errp>>4);
		errp = err ;
	}
}

static inline void
fine_output_filter( register CARD32 *src, register CARD32 *dst, short ratio, int len )
{/* we carry half of the quantization error onto the following pixel and store it in dst: */
	register int i = 0;
	if( ratio <= 1 )
	{
		register int c = src[0];
  	    do
		{
			if( (c&0xFFFF0000)!= 0 )
				c = ( c&0x7F000000 )?0:0x0000FFFF;
			dst[i] = c>>(QUANT_ERR_BITS) ;
			if( ++i >= len )
				break;
			c = ((c&QUANT_ERR_MASK)>>1)+src[i];
		}while(1);
	}else if( ratio == 2 )
	{
		register CARD32 c = src[0];
  	    do
		{
			c = c>>1 ;
			if( (c&0xFFFF0000) != 0 )
				c = ( c&0x7F000000 )?0:0x0000FFFF;
			dst[i] = c>>(QUANT_ERR_BITS) ;
			if( ++i >= len )
				break;
			c = ((c&QUANT_ERR_MASK)>>1)+src[i];
		}while( 1 );
	}else
	{
		register CARD32 c = src[0];
  	    do
		{
			c = c/ratio ;
			if( c&0xFFFF0000 )
				c = ( c&0x7F000000 )?0:0x0000FFFF;
			dst[i] = c>>(QUANT_ERR_BITS) ;
			if( ++i >= len )
				break;
			c = ((c&QUANT_ERR_MASK)>>1)+src[i];
		}while(1);
	}
}

static inline void
fast_output_filter( register CARD32 *src, register CARD32 *dst, short ratio, int len )
{/*  no error diffusion whatsoever: */
	register int i = 0;
	if( ratio <= 1 )
	{
		for( ; i < len ; ++i )
		{
			register CARD32 c = src[i];
			if( (c&0xFFFF0000) != 0 )
				dst[i] = ( c&0x7F000000 )?0:0x000000FF;
			else
				dst[i] = c>>(QUANT_ERR_BITS) ;
		}
	}else if( ratio == 2 )
	{
		for( ; i < len ; ++i )
		{
			register CARD32 c = src[i]>>1;
			if( (c&0xFFFF0000) != 0 )
				dst[i] = ( c&0x7F000000 )?0:0x000000FF;
			else
				dst[i] = c>>(QUANT_ERR_BITS) ;
		}
	}else
	{
		for( ; i < len ; ++i )
		{
			register CARD32 c = src[i]/ratio;
			if( (c&0xFFFF0000) != 0 )
				dst[i] = ( c&0x7F000000 )?0:0x000000FF;
			else
				dst[i] = c>>(QUANT_ERR_BITS) ;
		}
	}
}

static inline void
fine_output_filter_mod( register CARD32 *data, int unused, int len )
{/* we carry half of the quantization error onto the following pixel : */
	register int i ;
	register CARD32 err = 0, c;
	for( i = 0 ; i < len ; ++i )
	{
		c = data[i];
		if( (c&0xFFFF0000) != 0 )
			c = ( c&0x7E000000 )?0:0x0000FFFF;
		c += err;
		err = (c&QUANT_ERR_MASK)>>1 ;
		data[i] = (c&0x00FF0000)?0x000000FF:c>>QUANT_ERR_BITS ;
	}
}

/* *********************************************************************/
/*					    	 DECODER : 	   							  */

static inline void
decode_asscanline( register ASScanline *scl, ASImage *im, ARGB32 back_color, ARGB32 filter, unsigned int skip, int y, unsigned int offset )
{
	int i ;
	int count, width = scl->width-skip ;
	for( i = 0 ; i < IC_NUM_CHANNELS ; i++ )
		if( get_flags(filter, 0x01<<i) )
		{
			register CARD32 *chan = scl->channels[i]+skip;
			if( im )
				count = asimage_decode_line(im, i, chan, y, offset, width);
			else
				count = 0 ;
			if( scl->shift )
			{
				register int k  = 0;
				for(; k < count ; k++ )
					chan[k] = chan[k]<<8;
			}
			if( count < width )
				set_component( chan, ARGB32_CHAN8(back_color, i)<<scl->shift, count, width );
		}

	clear_flags( scl->flags,SCL_DO_ALL);
	set_flags( scl->flags,filter);
}

void                                           /* normal (unbeveled) */
decode_image_scanline_normal( ASImageDecoder *imdec )
{
	int 	 			 y = imdec->next_line;

	if( imdec->im )
		y %= imdec->im->height;
	decode_asscanline( &(imdec->buffer), imdec->im, imdec->back_color, imdec->filter, 0, y, imdec->offset_x );
	++(imdec->next_line);
}

void
decode_image_scanline_beveled( ASImageDecoder *imdec )
{
	register ASScanline *scl = &(imdec->buffer);
	int 	 			 y = imdec->next_line;
	register ASImageBevel *bevel = imdec->bevel ;
	ARGB32 bevel_color = bevel->hi_color, shade_color = bevel->lo_color;
	ARGB32 hilo_corner_color = bevel->hilo_color, corner_color = bevel->hihi_color;
	Bool partial_bevel = False;
	int offset_shade = 0, corner = 0;
	int channel;

	if( y < bevel->top_outline )
	{
		offset_shade = scl->width - (y*bevel->right_outline)/bevel->top_outline;
		corner = bevel->left_outline*y/bevel->top_outline ;
	}else if( y >= imdec->out_height+bevel->top_outline &&
		      y < imdec->out_height+imdec->bevel_v_addon)
	{
		offset_shade = corner = (imdec->out_height+imdec->bevel_v_addon)-y ;
		if( bevel->left_outline != bevel->bottom_outline )
			offset_shade = ((offset_shade*bevel->left_outline)/bevel->bottom_outline)+1;
		corner_color = bevel->lolo_color ;
		corner = scl->width-corner ;
	}else
		partial_bevel = True ;

	if( !partial_bevel )
	{ /* drawing solid lines at the top and bottom of the image */
		for( channel = 0 ; channel < ARGB32_CHANNELS ; ++channel )
			if( get_flags(imdec->filter, (0x01<<channel)) )
			{
				set_component( scl->channels[channel],
					           ARGB32_CHAN8(bevel_color,channel)<<scl->shift,
							   0, offset_shade );
				set_component( scl->channels[channel], ARGB32_CHAN8(shade_color,channel)<<scl->shift, offset_shade, scl->width );
				scl->channels[channel][offset_shade-1] = ARGB32_CHAN8(hilo_corner_color,channel)<<scl->shift ;
				scl->channels[channel][corner] = ARGB32_CHAN8(corner_color,channel)<<scl->shift ;
			}
		if( y == 0 || y == imdec->out_height+bevel->top_outline-1 )
			scl->alpha[0] = scl->alpha[scl->width-1] = MIN(scl->alpha[0],0x5F<<scl->shift);

		set_flags( scl->flags,imdec->filter);
	}else
	{
		int width = scl->width-imdec->bevel_h_addon ;
		CARD32 a_bevel = ARGB32_ALPHA8(bevel_color), a_shade = ARGB32_ALPHA8(shade_color);
	    CARD32 da_bevel = (a_bevel<<8)/(bevel->left_inline+1), da_shade = (a_shade<<8)/(bevel->right_inline+1) ;
		int inline_bottom = imdec->out_height - bevel->bottom_inline;
		int line = imdec->next_line ;

		if( imdec->im )
			line %= imdec->im->height ;
		decode_asscanline( scl, imdec->im, imdec->back_color, imdec->filter, bevel->left_outline,
						   line, imdec->offset_x );
		y -= bevel->top_outline ;
		offset_shade = scl->width-bevel->right_outline ;
		for( channel = 0 ; channel < ARGB32_CHANNELS ; ++channel )
			if( get_flags(imdec->filter, (0x01<<channel)) )
			{
				int i ;
				CARD32 *chan_img_start = scl->channels[channel]+bevel->left_outline ;
				CARD32 ca = a_bevel, chan_bevel = ARGB32_CHAN8(bevel_color,channel)<<scl->shift ;
				CARD32 chan_shade = ARGB32_CHAN8(shade_color,channel)<<scl->shift ;

				set_component( scl->channels[channel], chan_bevel, 0, bevel->left_outline );
				set_component( scl->channels[channel], chan_shade, offset_shade, scl->width );
				if( da_bevel > 0 )
				{
					int max_i = bevel->left_inline ;
					if( y > inline_bottom )
						max_i = (max_i*(imdec->out_height-y)/bevel->bottom_inline)+1 ;
					ca = ca<<8 ;
					for( i = 0 ; i < max_i ; i++ )
					{							   /* semitransparent pixels at the left */
						ca -= da_bevel ;
						chan_img_start[i] = (chan_img_start[i]*(255-(ca>>8))+chan_bevel*(ca>>8))>>8 ;
					}
				}
				if( y < bevel->top_inline )
				{                              /* semitransparent line at the top */
					int max_i = width - (bevel->right_inline*y/bevel->top_inline) ;

					ca = (a_bevel*(bevel->top_inline-y)/(bevel->top_inline+1));
					for( i = 0 ; i < max_i ; i++ )
						chan_img_start[i] = (chan_img_start[i]*(255-ca)+chan_bevel*ca)>>8 ;
					i = y*bevel->left_inline/bevel->top_inline ;
					chan_img_start[i] = (chan_img_start[i]*(255-ca) + (ARGB32_CHAN8(corner_color,channel)<<scl->shift)*ca)>>8 ;
				}

				if( da_shade > 0 )
				{
					i = 0 ;
					ca = 0 ;
					chan_img_start += width - bevel->right_inline ;
					if( y < bevel->top_inline )
					{
						i = bevel->right_inline*(bevel->top_inline-y)/bevel->top_inline ;
						ca = i*da_shade ;
						chan_img_start[i] = ((chan_img_start[i]<<7) + (ARGB32_CHAN8(hilo_corner_color,channel)<<(scl->shift+7)))>>8 ;
					}
					while( ++i < bevel->right_inline )
					{					           /* semitransparent pixels at the right */
						ca += da_shade ;
						chan_img_start[i] = (chan_img_start[i]*(255-(ca>>8))+chan_shade*(ca>>8))>>8 ;
					}
					chan_img_start = scl->channels[channel]+bevel->left_outline ;
				}
				if( y >= inline_bottom )
				{                              /* semitransparent line at the bottom */
					ca = (a_shade/(bevel->bottom_inline+1));
					ca += ca*(y-inline_bottom);
					i = (bevel->left_inline*(imdec->out_height-y)/bevel->bottom_inline)-1 ;
					while( ++i < width )
						chan_img_start[i] = (chan_img_start[i]*(255-ca)+chan_shade*ca)>>8 ;
					i = width- ((imdec->out_height-y)*bevel->right_inline/bevel->bottom_inline) ;
					chan_img_start[i] = (chan_img_start[i]*(255-ca) + ((ARGB32_CHAN8(bevel->lolo_color,channel)<<scl->shift)*ca))>>8 ;
				}
			}
		set_flags( scl->flags,imdec->filter);
	}
	++(imdec->next_line);
}

void
decode_image_scl_bevel_solid( ASImageDecoder *imdec )
{
	register ASScanline *scl = &(imdec->buffer);
	int 	 			 y = imdec->next_line;
	register ASImageBevel *bevel = imdec->bevel ;
	ARGB32 bevel_color = bevel->hi_color, shade_color = bevel->lo_color;
	ARGB32 hilo_corner_color = bevel->hilo_color, corner_color = bevel->hihi_color;
	Bool partial_bevel = False;
	int offset_shade = 0, corner = 0;
	int channel;

	if( y < bevel->top_outline )
	{
		offset_shade = scl->width - (y*bevel->right_outline)/bevel->top_outline;
		corner = bevel->left_outline*y/bevel->top_outline ;
	}else if( y >= imdec->out_height+bevel->top_outline &&
		      y < imdec->out_height+imdec->bevel_v_addon)
	{
		offset_shade = corner = (imdec->out_height+imdec->bevel_v_addon)-y ;
		if( bevel->left_outline != bevel->bottom_outline && bevel->bottom_outline != 0 )
			offset_shade = ((offset_shade*bevel->left_outline)/bevel->bottom_outline)+1;
		corner_color = bevel->lolo_color ;
		corner = scl->width-corner ;
	}else
		partial_bevel = True ;

	if( !partial_bevel )
	{ /* drawing solid lines at the top and bottom of the image */
		for( channel = 0 ; channel < ARGB32_CHANNELS ; ++channel )
			if( get_flags(imdec->filter, (0x01<<channel)) )
			{
				set_component( scl->channels[channel],
					           ARGB32_CHAN8(bevel_color,channel)<<scl->shift,
							   0, offset_shade );
				set_component( scl->channels[channel], ARGB32_CHAN8(shade_color,channel)<<scl->shift, offset_shade, scl->width );
				scl->channels[channel][offset_shade-1] = ARGB32_CHAN8(hilo_corner_color,channel)<<scl->shift ;
				scl->channels[channel][corner] = ARGB32_CHAN8(corner_color,channel)<<scl->shift ;
			}
		if( y == 0 || y == imdec->out_height+bevel->top_outline-1 )
			scl->alpha[0] = scl->alpha[scl->width-1] = MIN(scl->alpha[0],0x5F<<scl->shift);
		set_flags( scl->flags,imdec->filter);
	}else
	{
		int width = scl->width-imdec->bevel_h_addon ;
		int inline_bottom = imdec->out_height - bevel->bottom_inline;
		int line = imdec->next_line ;

		if( imdec->im )
			line %= imdec->im->height ;
		decode_asscanline( scl, imdec->im, imdec->back_color, imdec->filter, bevel->left_outline,
						   line, imdec->offset_x );

		y -= bevel->top_outline ;
		offset_shade = scl->width-bevel->right_outline ;

		for( channel = 0 ; channel < ARGB32_CHANNELS ; ++channel )
			if( get_flags(imdec->filter, (0x01<<channel)) )
			{
				int i ;
				CARD32 *chan_img_start = scl->channels[channel]+bevel->left_outline ;
				CARD32 chan_bevel = ARGB32_CHAN8(bevel_color,channel)<<scl->shift ;
				CARD32 chan_shade = ARGB32_CHAN8(shade_color,channel)<<scl->shift ;
				CARD32 a_bevel = ARGB32_ALPHA8(bevel_color)>>1, a_shade = ARGB32_ALPHA8(shade_color)>>1;

				set_component( scl->channels[channel], chan_bevel, 0, bevel->left_outline );
				set_component( scl->channels[channel], chan_shade, offset_shade, scl->width );
				chan_bevel *= a_bevel ;
				chan_shade *= a_shade ;
				a_bevel = 255-a_bevel ;
				a_shade = 255-a_shade ;
				if( y < bevel->top_inline )   /* semitransparent line at the top */
				{
					i = width-(bevel->right_inline*y/bevel->top_inline);
                    while( ++i < width ) /* semitransparent pixels at the right */
						chan_img_start[i] = (chan_img_start[i]*a_shade+chan_shade)>>8 ;

					i = width-(bevel->right_inline*y/bevel->top_inline);
					chan_img_start[i] = ((chan_img_start[i]<<7)+(ARGB32_CHAN8(hilo_corner_color,channel)<<(7+scl->shift)))>>8 ;
					while( --i >= 0 )
						chan_img_start[i] = (chan_img_start[i]*a_bevel+chan_bevel)>>8 ;

					i = bevel->left_inline*y/bevel->top_inline;
					chan_img_start[i] = ((chan_img_start[i]<<7)+(ARGB32_CHAN8(corner_color,channel)<<(7+scl->shift)))>>8 ;

				}else if( y >= inline_bottom ) /* semitransparent line at the bottom */
				{
					i = (bevel->left_inline*(imdec->out_height-y)/bevel->bottom_inline);
					while( --i >= 0 )
						chan_img_start[i] = (chan_img_start[i]*a_bevel+chan_bevel)>>8 ;
					i = (bevel->left_inline*(imdec->out_height-y)/bevel->bottom_inline);
					chan_img_start[i] = ((chan_img_start[i]<<7)+(ARGB32_CHAN8(hilo_corner_color,channel)<<(7+scl->shift)))>>8 ;
					while( ++i < width )
						chan_img_start[i] = (chan_img_start[i]*a_shade+chan_shade)>>8 ;
					i = width-(bevel->right_inline*(imdec->out_height-y)/bevel->bottom_inline);
					chan_img_start[i] = ((chan_img_start[i]<<7)+(ARGB32_CHAN8(bevel->lolo_color,channel)<<(7+scl->shift)))>>8 ;
				}else
				{
					for( i = 0 ; i < bevel->left_inline ; i++ )
						chan_img_start[i] = (chan_img_start[i]*a_bevel+chan_bevel)>>8 ;
					chan_img_start += width - bevel->right_inline ;
					for( i = 0 ; i < bevel->right_inline ; i++ ) /* semitransparent pixels at the right */
						chan_img_start[i] = (chan_img_start[i]*a_shade+chan_shade)>>8 ;
				}
			}
		set_flags( scl->flags,imdec->filter);
		/* TODO: add alpha shading here for inline : */

	}
	++(imdec->next_line);
}
/* *********************************************************************/
/*						  ENCODER : 								  */
inline static void
tile_ximage_line( XImage *xim, unsigned int line, int step )
{
	register int i ;
	int xim_step = step*xim->bytes_per_line ;
	unsigned char *src_line = xim->data+xim->bytes_per_line*line ;
	unsigned char *dst_line = src_line+xim_step ;
	for( i = line+step ; i < xim->height && i >= 0 ; i+=step )
	{
		memcpy( dst_line, src_line, xim->bytes_per_line );
		dst_line += xim_step ;
	}
}

void
encode_image_scanline_mask_xim( ASImageOutput *imout, ASScanline *to_store )
{
#ifndef X_DISPLAY_MISSING
	register XImage *xim = imout->im->alt.mask_ximage ;
	if( imout->next_line < xim->height && imout->next_line >= 0 )
	{
		if( get_flags(to_store->flags, SCL_DO_ALPHA) )
		{
			register int x ;
			for ( x = MIN(xim->width, to_store->width)-1 ; x >= 0 ; --x )
				XPutPixel( xim, x, imout->next_line,
				           (to_store->alpha[x] >= 0x7F)?1:0 );
		}
		if( imout->tiling_step > 0 )
			tile_ximage_line( xim, imout->next_line,
			                  imout->bottom_to_top*imout->tiling_step );
		imout->next_line += imout->bottom_to_top;
	}
#endif
}

void
encode_image_scanline_xim( ASImageOutput *imout, ASScanline *to_store )
{
	register XImage *xim = imout->im->alt.ximage ;
	if( imout->next_line < xim->height && imout->next_line >= 0 )
	{
		if( !get_flags(to_store->flags, SCL_DO_RED) )
			set_component( to_store->red, ARGB32_RED8(to_store->back_color), 0, to_store->width );
		if( !get_flags(to_store->flags, SCL_DO_GREEN) )
			set_component( to_store->green, ARGB32_GREEN8(to_store->back_color), 0, to_store->width );
		if( !get_flags(to_store->flags, SCL_DO_BLUE) )
			set_component( to_store->blue , ARGB32_BLUE8(to_store->back_color), 0, to_store->width );
		PUT_SCANLINE(imout->asv, xim,to_store,imout->next_line,
			         xim->data+imout->next_line*xim->bytes_per_line);

		if( imout->tiling_step > 0 )
			tile_ximage_line( imout->im->alt.ximage, imout->next_line,
			                  imout->bottom_to_top*imout->tiling_step );
		imout->next_line += imout->bottom_to_top;
	}
}

void
encode_image_scanline_asim( ASImageOutput *imout, ASScanline *to_store )
{
LOCAL_DEBUG_CALLER_OUT( "imout->next_line = %d, imout->im->height = %d", imout->next_line, imout->im->height );
	if( imout->next_line < imout->im->height && imout->next_line >= 0 )
	{
		CARD32 chan_fill[4];
		chan_fill[IC_RED]   = ARGB32_RED8  (to_store->back_color);
		chan_fill[IC_GREEN] = ARGB32_GREEN8(to_store->back_color);
		chan_fill[IC_BLUE]  = ARGB32_BLUE8 (to_store->back_color);
		chan_fill[IC_ALPHA] = ARGB32_ALPHA8(to_store->back_color);
		if( imout->tiling_step > 1 )
		{
			int bytes_count ;
			register int i, color ;
			int max_i = imout->im->height ;
			int step =  imout->bottom_to_top*imout->tiling_step;

			for( color = 0 ; color < IC_NUM_CHANNELS ; color++ )
			{
				if( get_flags(to_store->flags,0x01<<color))
					bytes_count = asimage_add_line(imout->im, color, to_store->channels[color]+to_store->offset_x,   imout->next_line);
				else if( chan_fill[color] != imout->chan_fill[color] )
					bytes_count = asimage_add_line_mono( imout->im, color, chan_fill[color], imout->next_line);
				else
				{
					asimage_erase_line( imout->im, color, imout->next_line );
					for( i = imout->next_line+step ; i < max_i && i >= 0 ; i+=step )
						asimage_erase_line( imout->im, color, i );
					continue;
				}
				for( i = imout->next_line+step ; i < max_i && i >= 0 ; i+=step )
				{
/*						fprintf( stderr, "copy-encoding color %d, from lline %d to %d, %d bytes\n", color, imout->next_line, i, bytes_count );*/
					asimage_dup_line( imout->im, color, imout->next_line, i, bytes_count );
				}
			}
		}else
		{
			register int color ;
			for( color = 0 ; color < IC_NUM_CHANNELS ; color++ )
			{
				if( get_flags(to_store->flags,0x01<<color))
				{
					LOCAL_DEBUG_OUT( "encoding line %d for component %d offset = %d ", imout->next_line, color, to_store->offset_x );
					asimage_add_line(imout->im, color, to_store->channels[color]+to_store->offset_x, imout->next_line);
				}else if( chan_fill[color] != imout->chan_fill[color] )
				{
					asimage_add_line_mono( imout->im, color, chan_fill[color], imout->next_line);
				}else
				{
					LOCAL_DEBUG_OUT( "erasing line %d for component %d", imout->next_line, color );
					asimage_erase_line( imout->im, color, imout->next_line );
				}
			}
		}
	}
	imout->next_line += imout->bottom_to_top;
}

inline static void
tile_argb32_line( ARGB32 *data, unsigned int line, int step, unsigned int width, unsigned int height )
{
	register int i ;
	ARGB32 *src_line = data+width*line ;
	ARGB32 *dst_line = src_line+width ;

	for( i = line+step ; i < height && i >= 0 ; i+=step )
	{
		memcpy( dst_line, src_line, width*sizeof(ARGB32));
		dst_line += step;
	}
}

void
encode_image_scanline_argb32( ASImageOutput *imout, ASScanline *to_store )
{
	register ARGB32 *data = imout->im->alt.argb32 ;
	if( imout->next_line < imout->im->height && imout->next_line >= 0 )
	{
		register int x = imout->im->width;
		register CARD32 *alpha = to_store->alpha ;
		register CARD32 *red = to_store->red ;
		register CARD32 *green = to_store->green ;
		register CARD32 *blue = to_store->blue ;
		if( !get_flags(to_store->flags, SCL_DO_RED) )
			set_component( red, ARGB32_RED8(to_store->back_color), 0, to_store->width );
		if( !get_flags(to_store->flags, SCL_DO_GREEN) )
			set_component( green, ARGB32_GREEN8(to_store->back_color), 0, to_store->width );
		if( !get_flags(to_store->flags, SCL_DO_BLUE) )
			set_component( blue , ARGB32_BLUE8(to_store->back_color), 0, to_store->width );

		data += x*imout->next_line ;
		if( !get_flags(to_store->flags, SCL_DO_ALPHA) )
			while( --x >= 0 )
				data[x] = MAKE_ARGB32( 0xFF, red[x], green[x], blue[x] );
		else
			while( --x >= 0 )
				data[x] = MAKE_ARGB32( alpha[x], red[x], green[x], blue[x] );

		if( imout->tiling_step > 0 )
			tile_argb32_line( imout->im->alt.argb32, imout->next_line,
			                  imout->bottom_to_top*imout->tiling_step,
							  imout->im->width, imout->im->height );
		imout->next_line += imout->bottom_to_top;
	}
}


void
output_image_line_top( ASImageOutput *imout, ASScanline *new_line, int ratio )
{
	ASScanline *to_store = NULL ;
	/* caching and preprocessing line into our buffer : */
	if( new_line )
	{
		if( ratio > 1 )
			SCANLINE_FUNC(divide_component,*(new_line),*(imout->available),ratio,imout->available->width);
		else
			SCANLINE_FUNC(copy_component,*(new_line),*(imout->available),NULL,imout->available->width);
		imout->available->flags = new_line->flags ;
		imout->available->back_color = new_line->back_color ;
	}
	/* copying/encoding previously cahced line into destination image : */
	if( imout->used != NULL )
	{
		if( new_line != NULL )
			SCANLINE_FUNC(best_output_filter,*(imout->used),*(imout->available),0,imout->available->width);
		else
			SCANLINE_MOD(fine_output_filter_mod,*(imout->used),0,imout->used->width);
		to_store = imout->used ;
	}
	if( to_store )
		imout->encode_image_scanline( imout, to_store );

	/* rotating the buffers : */
	if( imout->buffer_shift > 0 )
	{
		if( new_line == NULL )
			imout->used = NULL ;
		else
			imout->used = imout->available ;
		imout->available = &(imout->buffer[0]);
		if( imout->available == imout->used )
			imout->available = &(imout->buffer[1]);
	}
}

void
output_image_line_fine( ASImageOutput *imout, ASScanline *new_line, int ratio )
{
	/* caching and preprocessing line into our buffer : */
	if( new_line )
	{
		SCANLINE_FUNC(fine_output_filter, *(new_line),*(imout->available),ratio,imout->available->width);
		imout->available->flags = new_line->flags ;
		imout->available->back_color = new_line->back_color ;
/*		SCANLINE_MOD(print_component,*(imout->available),0, new_line->width ); */
		/* copying/encoding previously cached line into destination image : */
		imout->encode_image_scanline( imout, imout->available );
	}
}

void
output_image_line_fast( ASImageOutput *imout, ASScanline *new_line, int ratio )
{
	/* caching and preprocessing line into our buffer : */
	if( new_line )
	{
		SCANLINE_FUNC(fast_output_filter,*(new_line),*(imout->available),ratio,imout->available->width);
		imout->available->flags = new_line->flags ;
		imout->available->back_color = new_line->back_color ;
		imout->encode_image_scanline( imout, imout->available );
	}
}

void
output_image_line_direct( ASImageOutput *imout, ASScanline *new_line, int ratio )
{
	/* caching and preprocessing line into our buffer : */
	if( new_line )
	{
		if( ratio > 1)
		{
			SCANLINE_FUNC(divide_component,*(new_line),*(imout->available),ratio,imout->available->width);
			imout->available->flags = new_line->flags ;
			imout->available->back_color = new_line->back_color ;
			imout->encode_image_scanline( imout, imout->available );
		}else
			imout->encode_image_scanline( imout, new_line );
	}
}

/* ********************************************************************************/
/* Convinience function - very fast image cloning :                               */
/* ********************************************************************************/
ASImage*
clone_asimage(ASVisual *asv, ASImage *src, ASFlagType filter )
{
	ASImage *dst = NULL ;
	START_TIME(started);

	if( src )
	{
		int chan ;
		dst = create_asimage(src->width, src->height, (src->max_compressed_width*100)/src->width);
		dst->back_color = src->back_color ;
		for( chan = 0 ; chan < IC_NUM_CHANNELS;  chan++ )
			if( get_flags( filter, 0x01<<chan) )
			{
				register int i = dst->height;
				register CARD8 **dst_rows = dst->channels[chan] ;
				register CARD8 **src_rows = src->channels[chan] ;
				while( --i >= 0 )
					dst_rows[i] = asimage_copy_line( src_rows[i], dst->width );
			}
	}
	SHOW_TIME("", started);
	return dst;
}

/* ********************************************************************************/
/* The end !!!! 																 */
/* ********************************************************************************/

