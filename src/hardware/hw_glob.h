// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 1998-2000 by DooM Legacy Team.
// Copyright (C) 1999-2021 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file hw_glob.h
/// \brief globals (shared data & code) for hw_ modules

#ifndef _HWR_GLOB_H_
#define _HWR_GLOB_H_

#include "hw_defs.h"
#include "hw_main.h"
#include "../m_misc.h"
#include "../p_setup.h"

// the original aspect ratio of Doom graphics isn't square
#define ORIGINAL_ASPECT (320.0f/200.0f)

// Uncomment this to enable the OpenGL loading screen
//#define HWR_LOADING_SCREEN

// -----------
// structures
// -----------

// a vertex of a Doom 'plane' polygon
typedef struct
{
	float x;
	float y;
	float z;
} polyvertex_t;

#ifdef _MSC_VER
#pragma warning(disable :  4200)
#endif

// a convex 'plane' polygon, clockwise order
typedef struct
{
	INT32 numpts;
	polyvertex_t pts[0];
} poly_t;

#ifdef _MSC_VER
#pragma warning(default :  4200)
#endif

// holds extra info for 3D render, for each subsector in subsectors[]
typedef struct
{
	poly_t *planepoly;  // the generated convex polygon
} extrasubsector_t;

// needed for sprite rendering
// equivalent of the software renderer's vissprites
typedef struct gl_vissprite_s
{
	float x1, x2;
	float z1, z2;
	float gz, gzt;

	float tz;
	float tracertz; // for MF2_LINKDRAW sprites, this contains tracer's tz for use in sorting

	float scale;
	float shadowheight, shadowscale;

	float spritexscale, spriteyscale;
	float spritexoffset, spriteyoffset;

	UINT32 renderflags;
	UINT8 rotateflags;

	boolean flip, vflip;
	boolean precip; // Tails 08-25-2002
	boolean rotated;
	UINT8 translucency;       //alpha level 0-255

	//Hurdler: 25/04/2000: now support colormap in hardware mode
	UINT8 *colormap;
	INT32 dispoffset; // copy of info->dispoffset, affects ordering but not drawing

	patch_t *gpatch;
	mobj_t *mobj; // NOTE: This is a precipmobj_t if precip is true !!! Watch out.
} gl_vissprite_t;

// --------
// hw_bsp.c
// --------
extern extrasubsector_t *extrasubsectors;
extern size_t addsubsector;

void HWR_InitPolyPool(void);
void HWR_FreePolyPool(void);

void HWR_FreeExtraSubsectors(void);

// --------
// hw_cache.c
// --------
RGBA_t *HWR_GetTexturePalette(void);

void HWR_InitMapTextures(void);
void HWR_LoadMapTextures(size_t pnumtextures);
void HWR_FreeMapTextures(void);

patch_t *HWR_GetCachedGLPatchPwad(UINT16 wad, UINT16 lump);
patch_t *HWR_GetCachedGLPatch(lumpnum_t lumpnum);

void HWR_GetPatch(patch_t *patch);
void HWR_GetMappedPatch(patch_t *patch, const UINT8 *colormap);
void HWR_GetFadeMask(lumpnum_t fademasklumpnum);
patch_t *HWR_GetPic(lumpnum_t lumpnum);

GLMapTexture_t *HWR_GetTexture(INT32 tex);
void HWR_GetLevelFlat(levelflat_t *levelflat);
void HWR_GetRawFlat(lumpnum_t flatlumpnum);

void HWR_FreeTexture(patch_t *patch);
void HWR_FreeTextureData(patch_t *patch);
void HWR_FreeTextureColormaps(patch_t *patch);
void HWR_ClearAllTextures(void);
void HWR_FreeColormapCache(void);
void HWR_UnlockCachedPatch(GLPatch_t *gpatch);

void HWR_SetPalette(RGBA_t *palette);
void HWR_SetMapPalette(void);
UINT32 HWR_CreateLightTable(UINT8 *lighttable);
void HWR_ClearLightTables(void);


// --------
// hw_draw.c
// --------
extern INT32 patchformat;
extern INT32 textureformat;

// --------
// hw_drawnodes.c
// --------
void HWR_AddTransparentWall(FOutVector *wallVerts, FSurfaceInfo *pSurf, INT32 texnum, FBITFIELD blend, boolean fogwall, INT32 lightlevel, extracolormap_t *wallcolormap);
void HWR_AddTransparentFloor(levelflat_t *levelflat, extrasubsector_t *xsub, boolean isceiling, fixed_t fixedheight, INT32 lightlevel, INT32 alpha, sector_t *FOFSector, FBITFIELD blend, boolean fogplane, extracolormap_t *planecolormap);
void HWR_AddTransparentPolyobjectFloor(levelflat_t *levelflat, polyobj_t *polysector, boolean isceiling, fixed_t fixedheight,
                             INT32 lightlevel, INT32 alpha, sector_t *FOFSector, FBITFIELD blend, extracolormap_t *planecolormap);
void HWR_CreateDrawNodes(void);
extern INT32 drawcount;

// --------
// hw_plane.c
// --------
void HWR_RenderPlane(subsector_t *subsector, extrasubsector_t *xsub, boolean isceiling, fixed_t fixedheight, FBITFIELD PolyFlags, INT32 lightlevel, levelflat_t *levelflat, sector_t *FOFsector, UINT8 alpha, extracolormap_t *planecolormap);
void HWR_RenderPolyObjectPlane(polyobj_t *polysector, boolean isceiling, fixed_t fixedheight,
								FBITFIELD blendmode, UINT8 lightlevel, levelflat_t *levelflat, sector_t *FOFsector,
								UINT8 alpha, extracolormap_t *planecolormap);

// --------
// hw_segs.c
// --------
void HWR_ProcessSeg(void);
void HWR_RenderWall(FOutVector *wallVerts, FSurfaceInfo *pSurf, FBITFIELD blend, boolean fogwall, INT32 lightlevel, extracolormap_t *wallcolormap);

// --------
// hw_shaders.c
// --------
boolean HWR_InitShaders(void);
void HWR_CompileShaders(void);

int HWR_GetShaderFromTarget(int shader_target);

void HWR_LoadAllCustomShaders(void);
void HWR_LoadCustomShadersFromFile(UINT16 wadnum, boolean PK3);
const char *HWR_GetShaderName(INT32 shader);

extern customshaderxlat_t shaderxlat[];

// --------
// hw_sky.c
// --------
void HWR_ClearSkyDome(void);
void HWR_BuildSkyDome(void);
void HWR_DrawSkyBackground(player_t *player);

// --------
// hw_things.c
// --------
extern UINT32 gl_visspritecount;

void HWR_ClearSprites(void);
void HWR_AddSprites(sector_t *sec);
void HWR_SortVisSprites(void);
void HWR_DrawSprites(void);

#endif //_HW_GLOB_
