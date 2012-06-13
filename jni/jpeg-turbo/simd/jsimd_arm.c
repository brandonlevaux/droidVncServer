/*
 * jsimd_arm.c
 *
 * Copyright 2009 Pierre Ossman <ossman@cendio.se> for Cendio AB
 * Copyright 2009-2011 D. R. Commander
 * 
 * Based on the x86 SIMD extension for IJG JPEG library,
 * Copyright (C) 1999-2006, MIYASAKA Masaru.
 * For conditions of distribution and use, see copyright notice in jsimdext.inc
 *
 * This file contains the interface between the "normal" portions
 * of the library and the SIMD implementations when running on
 * ARM architecture.
 *
 * Based on the stubs from 'jsimd_none.c'
 */

#define JPEG_INTERNALS
#include "../jinclude.h"
#include "../jpeglib.h"
#include "../jsimd.h"
#include "../jdct.h"
#include "../jsimddct.h"
#include "jsimd.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

typedef JMETHOD(void, upsample1_ptr,
                (j_decompress_ptr cinfo, jpeg_component_info * compptr,
                 JSAMPARRAY input_data, JSAMPARRAY * output_data_ptr));

typedef struct {
  struct jpeg_upsampler pub;    /* public fields */

  /* Color conversion buffer.  When using separate upsampling and color
   * conversion steps, this buffer holds one upsampled row group until it
   * has been color converted and output.
   * Note: we do not allocate any storage for component(s) which are full-size,
   * ie do not need rescaling.  The corresponding entry of color_buf[] is
   * simply set to point to the input data array, thereby avoiding copying.
   */
  JSAMPARRAY color_buf[MAX_COMPONENTS];

  /* Per-component upsampling method pointers */
  upsample1_ptr methods[MAX_COMPONENTS];

  int next_row_out;             /* counts rows emitted from color_buf */
  JDIMENSION rows_to_go;        /* counts rows remaining in image */

  /* Height of an input row group for each component. */
  int rowgroup_height[MAX_COMPONENTS];

  /* These arrays save pixel expansion factors so that int_expand need not
   * recompute them each time.  They are unused for other upsampling methods.
   */
  UINT8 h_expand[MAX_COMPONENTS];
  UINT8 v_expand[MAX_COMPONENTS];
} my_upsampler;

typedef my_upsampler * my_upsample_ptr;

static unsigned int simd_support = ~0;

#if defined(__linux__) || defined(ANDROID) || defined(__ANDROID__)

#define SOMEWHAT_SANE_PROC_CPUINFO_SIZE_LIMIT (1024 * 1024)

LOCAL(int)
check_feature (char *buffer, char *feature)
{
  char *p;
  if (*feature == 0)
    return 0;
  if (strncmp(buffer, "Features", 8) != 0)
    return 0;
  buffer += 8;
  while (isspace(*buffer))
    buffer++;

  /* Check if 'feature' is present in the buffer as a separate word */
  while ((p = strstr(buffer, feature))) {
    if (p > buffer && !isspace(*(p - 1))) {
      buffer++;
      continue;
    }
    p += strlen(feature);
    if (*p != 0 && !isspace(*p)) {
      buffer++;
      continue;
    }
    return 1;
  }
  return 0;
}

LOCAL(int)
parse_proc_cpuinfo (int bufsize)
{
  char *buffer = (char *)malloc(bufsize);
  FILE *fd;
  simd_support = 0;

  if (!buffer)
    return 0;

  fd = fopen("/proc/cpuinfo", "r");
  if (fd) {
    while (fgets(buffer, bufsize, fd)) {
      if (!strchr(buffer, '\n') && !feof(fd)) {
        /* "impossible" happened - insufficient size of the buffer! */
        fclose(fd);
        free(buffer);
        return 0;
      }
      if (check_feature(buffer, "neon"))
        simd_support |= JSIMD_ARM_NEON;
    }
    fclose(fd);
  }
  free(buffer);
  return 1;
}

#endif

/*
 * Check what SIMD accelerations are supported.
 *
 * FIXME: This code is racy under a multi-threaded environment.
 */
LOCAL(void)
init_simd (void)
{
  char *env = NULL;
#if !defined(__ARM_NEON__) && defined(__linux__) || defined(ANDROID) || defined(__ANDROID__)
  int bufsize = 1024; /* an initial guess for the line buffer size limit */
#endif

  if (simd_support != ~0)
    return;

  simd_support = 0;

/*#if defined(__ARM_NEON__)
  simd_support |= JSIMD_ARM_NEON;
#elif defined(__linux__) || defined(ANDROID) || defined(__ANDROID__)
 */
  /* We still have a chance to use NEON regardless of globally used
   * -mcpu/-mfpu options passed to gcc by performing runtime detection via
   * /proc/cpuinfo parsing on linux/android */
  while (!parse_proc_cpuinfo(bufsize)) {
    bufsize *= 2;
    if (bufsize > SOMEWHAT_SANE_PROC_CPUINFO_SIZE_LIMIT)
      break;
  }
//#endif

  /* Force different settings through environment variables */
  env = getenv("JSIMD_FORCE_ARM_NEON");
  if ((env != NULL) && (strcmp(env, "1") == 0))
    simd_support &= JSIMD_ARM_NEON;
  env = getenv("JSIMD_FORCE_NO_SIMD");
  if ((env != NULL) && (strcmp(env, "1") == 0))
    simd_support = 0;
}

GLOBAL(int)
jsimd_can_rgb_ycc (void)
{
  init_simd();

  /* The code is optimised for these values only */
  if (BITS_IN_JSAMPLE != 8)
    return 0;
  if (sizeof(JDIMENSION) != 4)
    return 0;
  if ((RGB_PIXELSIZE != 3) && (RGB_PIXELSIZE != 4))
    return 0;

  if (simd_support & JSIMD_ARM_NEON)
    return 1;

  return 0;
}

GLOBAL(int)
jsimd_can_rgb_gray (void)
{
  init_simd();

  return 0;
}

GLOBAL(int)
jsimd_can_ycc_rgb (void)
{
  init_simd();

  /* The code is optimised for these values only */
  if (BITS_IN_JSAMPLE != 8)
    return 0;
  if (sizeof(JDIMENSION) != 4)
    return 0;
  if ((RGB_PIXELSIZE != 3) && (RGB_PIXELSIZE != 4))
    return 0;
  if (simd_support & JSIMD_ARM_NEON)
    return 1;

  return 0;
}

GLOBAL(void)
jsimd_rgb_ycc_convert (j_compress_ptr cinfo,
                       JSAMPARRAY input_buf, JSAMPIMAGE output_buf,
                       JDIMENSION output_row, int num_rows)
{
  void (*neonfct)(JDIMENSION, JSAMPARRAY, JSAMPIMAGE, JDIMENSION, int);

  switch(cinfo->in_color_space)
  {
    case JCS_EXT_RGB:
      neonfct=jsimd_extrgb_ycc_convert_neon;
      break;
    case JCS_EXT_RGBX:
      neonfct=jsimd_extrgbx_ycc_convert_neon;
      break;
    case JCS_EXT_BGR:
      neonfct=jsimd_extbgr_ycc_convert_neon;
      break;
    case JCS_EXT_BGRX:
      neonfct=jsimd_extbgrx_ycc_convert_neon;
      break;
    case JCS_EXT_XBGR:
      neonfct=jsimd_extxbgr_ycc_convert_neon;
      break;
    case JCS_EXT_XRGB:
      neonfct=jsimd_extxrgb_ycc_convert_neon;
      break;
    default:
      neonfct=jsimd_extrgb_ycc_convert_neon;
      break;
  }

  if (simd_support & JSIMD_ARM_NEON)
    neonfct(cinfo->image_width, input_buf,
        output_buf, output_row, num_rows);
}

GLOBAL(void)
jsimd_rgb_gray_convert (j_compress_ptr cinfo,
                        JSAMPARRAY input_buf, JSAMPIMAGE output_buf,
                        JDIMENSION output_row, int num_rows)
{
}

GLOBAL(void)
jsimd_ycc_rgb_convert (j_decompress_ptr cinfo,
                       JSAMPIMAGE input_buf, JDIMENSION input_row,
                       JSAMPARRAY output_buf, int num_rows)
{
  void (*neonfct)(JDIMENSION, JSAMPIMAGE, JDIMENSION, JSAMPARRAY, int);

  switch(cinfo->out_color_space)
  {
    case JCS_EXT_RGB:
      neonfct=jsimd_ycc_extrgb_convert_neon;
      break;
    case JCS_EXT_RGBX:
      neonfct=jsimd_ycc_extrgbx_convert_neon;
      break;
    case JCS_EXT_BGR:
      neonfct=jsimd_ycc_extbgr_convert_neon;
      break;
    case JCS_EXT_BGRX:
      neonfct=jsimd_ycc_extbgrx_convert_neon;
      break;
    case JCS_EXT_XBGR:
      neonfct=jsimd_ycc_extxbgr_convert_neon;
      break;
    case JCS_EXT_XRGB:
      neonfct=jsimd_ycc_extxrgb_convert_neon;
      break;
  default:
      neonfct=jsimd_ycc_extrgb_convert_neon;
      break;
  }

  if (simd_support & JSIMD_ARM_NEON)
    neonfct(cinfo->output_width, input_buf,
        input_row, output_buf, num_rows);
}

GLOBAL(int)
jsimd_can_h2v2_downsample (void)
{
  init_simd();

  return 0;
}

GLOBAL(int)
jsimd_can_h2v1_downsample (void)
{
  init_simd();

  return 0;
}

GLOBAL(void)
jsimd_h2v2_downsample (j_compress_ptr cinfo, jpeg_component_info * compptr,
                       JSAMPARRAY input_data, JSAMPARRAY output_data)
{
}

GLOBAL(void)
jsimd_h2v1_downsample (j_compress_ptr cinfo, jpeg_component_info * compptr,
                       JSAMPARRAY input_data, JSAMPARRAY output_data)
{
}

GLOBAL(int)
jsimd_can_h2v2_upsample (void)
{
  init_simd();

  return 0;
}

GLOBAL(int)
jsimd_can_h2v1_upsample (void)
{
  init_simd();

  return 0;
}

GLOBAL(void)
jsimd_h2v2_upsample (j_decompress_ptr cinfo,
                     jpeg_component_info * compptr, 
                     JSAMPARRAY input_data,
                     JSAMPARRAY * output_data_ptr)
{
}

GLOBAL(void)
jsimd_h2v1_upsample (j_decompress_ptr cinfo,
                     jpeg_component_info * compptr, 
                     JSAMPARRAY input_data,
                     JSAMPARRAY * output_data_ptr)
{
}

GLOBAL(int)
jsimd_can_h2v2_fancy_upsample (void)
{
  init_simd();

  return 0;
}

GLOBAL(int)
jsimd_can_h2v1_fancy_upsample (void)
{
  init_simd();

  return 0;
}

GLOBAL(void)
jsimd_h2v2_fancy_upsample (j_decompress_ptr cinfo,
                           jpeg_component_info * compptr, 
                           JSAMPARRAY input_data,
                           JSAMPARRAY * output_data_ptr)
{
}

GLOBAL(void)
jsimd_h2v1_fancy_upsample (j_decompress_ptr cinfo,
                           jpeg_component_info * compptr, 
                           JSAMPARRAY input_data,
                           JSAMPARRAY * output_data_ptr)
{
}

GLOBAL(int)
jsimd_can_h2v2_merged_upsample (void)
{
  init_simd();

#ifdef ANDROID_JPEG_USE_VENUM
  return 1;
#else
  return 0;
#endif
}

GLOBAL(int)
jsimd_can_h2v1_merged_upsample (void)
{
  init_simd();


#ifdef ANDROID_JPEG_USE_VENUM
  return 1;
#else
  return 0;
#endif
}

GLOBAL(void)
jsimd_h2v2_merged_upsample (j_decompress_ptr cinfo,
                            JSAMPIMAGE input_buf,
                            JDIMENSION in_row_group_ctr,
                            JSAMPARRAY output_buf)
{
#ifdef ANDROID_JPEG_USE_VENUM
  my_upsample_ptr upsample = (my_upsample_ptr) cinfo->upsample;
  JSAMPROW outptr0, outptr1;
  JSAMPROW inptr00, inptr01, inptr1, inptr2;
  inptr00 = input_buf[0][in_row_group_ctr*2];
  inptr01 = input_buf[0][in_row_group_ctr*2 + 1];
  inptr1  = input_buf[1][in_row_group_ctr];
  inptr2  = input_buf[2][in_row_group_ctr];
  outptr0 = output_buf[0];
  outptr1 = output_buf[1];

#ifdef ANDROID_RGB
  if (cinfo->out_color_space == JCS_RGBA_8888) {
    yyvup2abgr8888_venum((UINT8*) inptr00,
                         (UINT8*) inptr2,
                         (UINT8*) inptr1,
                         (UINT8*) outptr0,
                         cinfo->output_width);
    yyvup2abgr8888_venum((UINT8*) inptr01,
                         (UINT8*) inptr2,
                         (UINT8*) inptr1,
                         (UINT8*) outptr1,
                         cinfo->output_width);
  } else
#endif
  {
    yyvup2bgr888_venum((UINT8*) inptr00,
                       (UINT8*) inptr2,
                       (UINT8*) inptr1,
                       (UINT8*) outptr0,
                       cinfo->output_width);

    yyvup2bgr888_venum((UINT8*) inptr01,
                       (UINT8*) inptr2,
                       (UINT8*) inptr1,
                       (UINT8*) outptr1,
                       cinfo->output_width);
  }

#endif
}

GLOBAL(void)
jsimd_h2v1_merged_upsample (j_decompress_ptr cinfo,
                            JSAMPIMAGE input_buf,
                            JDIMENSION in_row_group_ctr,
                            JSAMPARRAY output_buf)
{
#ifdef ANDROID_JPEG_USE_VENUM

  my_upsample_ptr upsample = (my_upsample_ptr) cinfo->upsample;
  JSAMPROW inptr0, inptr1, inptr2;
  JSAMPROW outptr;

  inptr0 = input_buf[0][in_row_group_ctr];
  inptr1 = input_buf[1][in_row_group_ctr];
  inptr2 = input_buf[2][in_row_group_ctr];
  outptr = output_buf[0];

#ifdef ANDROID_RGB
  if (cinfo->out_color_space == JCS_RGBA_8888) {
    yyvup2abgr8888_venum((UINT8*) inptr0,
                         (UINT8*) inptr2,
                         (UINT8*) inptr1,
                         (UINT8*) outptr,
                         cinfo->output_width);
  } else
#endif
  {
    yyvup2bgr888_venum((UINT8*) inptr0,
                       (UINT8*) inptr2,
                       (UINT8*) inptr1,
                       (UINT8*) outptr,
                       cinfo->output_width);
  }
#endif
}

GLOBAL(int)
jsimd_can_convsamp (void)
{
  init_simd();

  /* The code is optimised for these values only */
  if (DCTSIZE != 8)
    return 0;
  if (BITS_IN_JSAMPLE != 8)
    return 0;
  if (sizeof(JDIMENSION) != 4)
    return 0;
  if (sizeof(DCTELEM) != 2)
    return 0;

  if (simd_support & JSIMD_ARM_NEON)
    return 1;

  return 0;
}

GLOBAL(int)
jsimd_can_convsamp_float (void)
{
  init_simd();

  return 0;
}

GLOBAL(void)
jsimd_convsamp (JSAMPARRAY sample_data, JDIMENSION start_col,
                DCTELEM * workspace)
{
  if (simd_support & JSIMD_ARM_NEON)
    jsimd_convsamp_neon(sample_data, start_col, workspace);
}

GLOBAL(void)
jsimd_convsamp_float (JSAMPARRAY sample_data, JDIMENSION start_col,
                      FAST_FLOAT * workspace)
{
}

GLOBAL(int)
jsimd_can_fdct_islow (void)
{
  init_simd();

  return 0;
}

GLOBAL(int)
jsimd_can_fdct_ifast (void)
{
  init_simd();

  /* The code is optimised for these values only */
  if (DCTSIZE != 8)
    return 0;
  if (sizeof(DCTELEM) != 2)
    return 0;

  if (simd_support & JSIMD_ARM_NEON)
    return 1;

  return 0;
}

GLOBAL(int)
jsimd_can_fdct_float (void)
{
  init_simd();

  return 0;
}

GLOBAL(void)
jsimd_fdct_islow (DCTELEM * data)
{
}

GLOBAL(void)
jsimd_fdct_ifast (DCTELEM * data)
{
  if (simd_support & JSIMD_ARM_NEON)
    jsimd_fdct_ifast_neon(data);
}

GLOBAL(void)
jsimd_fdct_float (FAST_FLOAT * data)
{
}

GLOBAL(int)
jsimd_can_quantize (void)
{
  init_simd();

  /* The code is optimised for these values only */
  if (DCTSIZE != 8)
    return 0;
  if (sizeof(JCOEF) != 2)
    return 0;
  if (sizeof(DCTELEM) != 2)
    return 0;

  if (simd_support & JSIMD_ARM_NEON)
    return 1;

  return 0;
}

GLOBAL(int)
jsimd_can_quantize_float (void)
{
  init_simd();

  return 0;
}

GLOBAL(void)
jsimd_quantize (JCOEFPTR coef_block, DCTELEM * divisors,
                DCTELEM * workspace)
{
  if (simd_support & JSIMD_ARM_NEON)
    jsimd_quantize_neon(coef_block, divisors, workspace);
}

GLOBAL(void)
jsimd_quantize_float (JCOEFPTR coef_block, FAST_FLOAT * divisors,
                      FAST_FLOAT * workspace)
{
}

GLOBAL(int)
jsimd_can_idct_2x2 (void)
{
  init_simd();

  /* The code is optimised for these values only */
  if (DCTSIZE != 8)
    return 0;
  if (sizeof(JCOEF) != 2)
    return 0;
  if (BITS_IN_JSAMPLE != 8)
    return 0;
  if (sizeof(JDIMENSION) != 4)
    return 0;
  if (sizeof(ISLOW_MULT_TYPE) != 2)
    return 0;

  if ((simd_support & JSIMD_ARM_NEON))
    return 1;

  return 0;
}

GLOBAL(int)
jsimd_can_idct_4x4 (void)
{
  init_simd();

  /* The code is optimised for these values only */
  if (DCTSIZE != 8)
    return 0;
  if (sizeof(JCOEF) != 2)
    return 0;
  if (BITS_IN_JSAMPLE != 8)
    return 0;
  if (sizeof(JDIMENSION) != 4)
    return 0;
  if (sizeof(ISLOW_MULT_TYPE) != 2)
    return 0;

  if ((simd_support & JSIMD_ARM_NEON))
    return 1;

  return 0;
}

GLOBAL(void)
jsimd_idct_2x2 (j_decompress_ptr cinfo, jpeg_component_info * compptr,
                JCOEFPTR coef_block, JSAMPARRAY output_buf,
                JDIMENSION output_col)
{
  if ((simd_support & JSIMD_ARM_NEON))
    jsimd_idct_2x2_neon(compptr->dct_table, coef_block, output_buf, output_col);
}

GLOBAL(void)
jsimd_idct_4x4 (j_decompress_ptr cinfo, jpeg_component_info * compptr,
                JCOEFPTR coef_block, JSAMPARRAY output_buf,
                JDIMENSION output_col)
{
  if ((simd_support & JSIMD_ARM_NEON))
    jsimd_idct_4x4_neon(compptr->dct_table, coef_block, output_buf, output_col);
}

GLOBAL(int)
jsimd_can_idct_islow (void)
{
  init_simd();

  /* The code is optimised for these values only */
  if (DCTSIZE != 8)
    return 0;
  if (sizeof(JCOEF) != 2)
    return 0;
  if (BITS_IN_JSAMPLE != 8)
    return 0;
  if (sizeof(JDIMENSION) != 4)
    return 0;
  if (sizeof(ISLOW_MULT_TYPE) != 2)
    return 0;

  if (simd_support & JSIMD_ARM_NEON)
    return 1;

  return 0;
}

GLOBAL(int)
jsimd_can_idct_ifast (void)
{
  init_simd();

  /* The code is optimised for these values only */
  if (DCTSIZE != 8)
    return 0;
  if (sizeof(JCOEF) != 2)
    return 0;
  if (BITS_IN_JSAMPLE != 8)
    return 0;
  if (sizeof(JDIMENSION) != 4)
    return 0;
  if (sizeof(IFAST_MULT_TYPE) != 2)
    return 0;
  if (IFAST_SCALE_BITS != 2)
    return 0;

  if ((simd_support & JSIMD_ARM_NEON))
    return 1;

  return 0;
}

GLOBAL(int)
jsimd_can_idct_float (void)
{
  init_simd();

  return 0;
}

GLOBAL(void)
jsimd_idct_islow (j_decompress_ptr cinfo, jpeg_component_info * compptr,
                JCOEFPTR coef_block, JSAMPARRAY output_buf,
                JDIMENSION output_col)
{
  if ((simd_support & JSIMD_ARM_NEON))
    jsimd_idct_islow_neon(compptr->dct_table, coef_block, output_buf, output_col);
}

GLOBAL(void)
jsimd_idct_ifast (j_decompress_ptr cinfo, jpeg_component_info * compptr,
                JCOEFPTR coef_block, JSAMPARRAY output_buf,
                JDIMENSION output_col)
{
  if ((simd_support & JSIMD_ARM_NEON))
    jsimd_idct_ifast_neon(compptr->dct_table, coef_block, output_buf, output_col);
}

GLOBAL(void)
jsimd_idct_float (j_decompress_ptr cinfo, jpeg_component_info * compptr,
                JCOEFPTR coef_block, JSAMPARRAY output_buf,
                JDIMENSION output_col)
{
}

