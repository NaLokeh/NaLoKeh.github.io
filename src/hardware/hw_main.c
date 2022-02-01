// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 1998-2000 by DooM Legacy Team.
// Copyright (C) 1999-2021 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file hw_main.c
/// \brief hardware renderer, using the standard HardWareRender driver DLL for SRB2

#include <math.h>

#include "../doomstat.h"

#ifdef HWRENDER
#include "hw_glob.h"
#include "hw_light.h"
#include "hw_drv.h"
#include "hw_batching.h"

#include "../i_video.h" // for rendermode == render_glide
#include "../v_video.h"
#include "../p_local.h"
#include "../p_setup.h"
#include "../r_local.h"
#include "../r_patch.h"
#include "../r_picformats.h"
#include "../r_bsp.h"
#include "../d_clisrv.h"
#include "../w_wad.h"
#include "../z_zone.h"
#include "../r_splats.h"
#include "../g_game.h"
#include "../st_stuff.h"
#include "../i_system.h"
#include "../m_cheat.h"
#include "../f_finale.h"
#include "../r_things.h" // R_GetShadowZ
#include "../p_slopes.h"
#include "hw_md2.h"
#include "hw_clip.h"

// ==========================================================================
// the hardware driver object
// ==========================================================================
struct hwdriver_s hwdriver;

// ==========================================================================
//                                                                    GLOBALS
// ==========================================================================

// base values set at SetViewSize
static float gl_basecentery;

float gl_baseviewwindowy, gl_basewindowcentery;
float gl_viewwidth, gl_viewheight; // viewport clipping boundaries (screen coords)
float gl_viewwindowx;

static float gl_centerx, gl_centery;
static float gl_viewwindowy; // top left corner of view window
static float gl_windowcenterx; // center of view window, for projection
static float gl_windowcentery;

static float gl_pspritexscale, gl_pspriteyscale;

seg_t *gl_curline;
side_t *gl_sidedef;
line_t *gl_linedef;
sector_t *gl_frontsector;
sector_t *gl_backsector;

// Render stats
ps_metric_t ps_hw_skyboxtime = {0};
ps_metric_t ps_hw_nodesorttime = {0};
ps_metric_t ps_hw_nodedrawtime = {0};
ps_metric_t ps_hw_spritesorttime = {0};
ps_metric_t ps_hw_spritedrawtime = {0};

// Render stats for batching
ps_metric_t ps_hw_numpolys = {0};
ps_metric_t ps_hw_numverts = {0};
ps_metric_t ps_hw_numcalls = {0};
ps_metric_t ps_hw_numshaders = {0};
ps_metric_t ps_hw_numtextures = {0};
ps_metric_t ps_hw_numpolyflags = {0};
ps_metric_t ps_hw_numcolors = {0};
ps_metric_t ps_hw_batchsorttime = {0};
ps_metric_t ps_hw_batchdrawtime = {0};

boolean gl_init = false;
boolean gl_maploaded = false;
boolean gl_sessioncommandsadded = false;
// false if shaders have not been initialized yet, or if shaders are not available
boolean gl_shadersavailable = false;

// Whether the internal state is set to palette rendering or not.
static boolean gl_palette_rendering_state = false;

// --------------------------------------------------------------------------
//                                              STUFF FOR THE PROJECTION CODE
// --------------------------------------------------------------------------

FTransform atransform;
// duplicates of the main code, set after R_SetupFrame() passed them into sharedstruct,
// copied here for local use
fixed_t dup_viewx, dup_viewy, dup_viewz;
angle_t dup_viewangle;

float gl_viewx, gl_viewy, gl_viewz;
float gl_viewsin, gl_viewcos;

// Maybe not necessary with the new T&L code (needs to be checked!)
float gl_viewludsin, gl_viewludcos; // look up down kik test
static float gl_fovlud;

static angle_t gl_aimingangle;

// ==========================================================================
// Lighting
// ==========================================================================

// Returns true if shaders can be used.
boolean HWR_UseShader(void)
{
	return (cv_glshaders.value && gl_shadersavailable);
}

void HWR_Lighting(FSurfaceInfo *Surface, INT32 light_level, extracolormap_t *colormap)
{
	RGBA_t poly_color, tint_color, fade_color;

	poly_color.rgba = 0xFFFFFFFF;
	tint_color.rgba = (colormap != NULL) ? (UINT32)colormap->rgba : GL_DEFAULTMIX;
	fade_color.rgba = (colormap != NULL) ? (UINT32)colormap->fadergba : GL_DEFAULTFOG;

	// Crappy backup coloring if you can't do shaders
	if (!HWR_UseShader())
	{
		// be careful, this may get negative for high lightlevel values.
		float tint_alpha, fade_alpha;
		float red, green, blue;

		red = (float)poly_color.s.red;
		green = (float)poly_color.s.green;
		blue = (float)poly_color.s.blue;

		// 48 is just an arbritrary value that looked relatively okay.
		tint_alpha = (float)(sqrt(tint_color.s.alpha) * 48) / 255.0f;

		// 8 is roughly the brightness of the "close" color in Software, and 16 the brightness of the "far" color.
		// 8 is too bright for dark levels, and 16 is too dark for bright levels.
		// 12 is the compromise value. It doesn't look especially good anywhere, but it's the most balanced.
		// (Also, as far as I can tell, fade_color's alpha is actually not used in Software, so we only use light level.)
		fade_alpha = (float)(sqrt(255-light_level) * 12) / 255.0f;

		// Clamp the alpha values
		tint_alpha = min(max(tint_alpha, 0.0f), 1.0f);
		fade_alpha = min(max(fade_alpha, 0.0f), 1.0f);

		red = (tint_color.s.red * tint_alpha) + (red * (1.0f - tint_alpha));
		green = (tint_color.s.green * tint_alpha) + (green * (1.0f - tint_alpha));
		blue = (tint_color.s.blue * tint_alpha) + (blue * (1.0f - tint_alpha));

		red = (fade_color.s.red * fade_alpha) + (red * (1.0f - fade_alpha));
		green = (fade_color.s.green * fade_alpha) + (green * (1.0f - fade_alpha));
		blue = (fade_color.s.blue * fade_alpha) + (blue * (1.0f - fade_alpha));

		poly_color.s.red = (UINT8)red;
		poly_color.s.green = (UINT8)green;
		poly_color.s.blue = (UINT8)blue;
	}

	// Clamp the light level, since it can sometimes go out of the 0-255 range from animations
	light_level = min(max(light_level, 0), 255);

	Surface->PolyColor.rgba = poly_color.rgba;
	Surface->TintColor.rgba = tint_color.rgba;
	Surface->FadeColor.rgba = fade_color.rgba;
	Surface->LightInfo.light_level = light_level;
	Surface->LightInfo.fade_start = (colormap != NULL) ? colormap->fadestart : 0;
	Surface->LightInfo.fade_end = (colormap != NULL) ? colormap->fadeend : 31;

	if (HWR_ShouldUsePaletteRendering())
	{
		boolean default_colormap = false;
		if (!colormap)
		{
			colormap = R_GetDefaultColormap(); // a place to store the hw lighttable id
			// alternatively could just store the id in a global variable if there are issues
			default_colormap = true;
		}
		// create hw lighttable if there isn't one
		if (!colormap->gl_lighttable_id)
		{
			UINT8 *colormap_pointer;

			if (default_colormap)
				colormap_pointer = colormaps; // don't actually use the data from the "default colormap"
			else
				colormap_pointer = colormap->colormap;
			colormap->gl_lighttable_id = HWR_CreateLightTable(colormap_pointer);
		}
		Surface->LightTableId = colormap->gl_lighttable_id;
	}
	else
	{
		Surface->LightTableId = 0;
	}
}

UINT8 HWR_FogBlockAlpha(INT32 light, extracolormap_t *colormap) // Let's see if this can work
{
	RGBA_t realcolor, surfcolor;
	INT32 alpha;

	realcolor.rgba = (colormap != NULL) ? colormap->rgba : GL_DEFAULTMIX;

	if (cv_glshaders.value && gl_shadersavailable)
	{
		surfcolor.s.alpha = (255 - light);
	}
	else
	{
		light = light - (255 - light);

		// Don't go out of bounds
		if (light < 0)
			light = 0;
		else if (light > 255)
			light = 255;

		alpha = (realcolor.s.alpha*255)/25;

		// at 255 brightness, alpha is between 0 and 127, at 0 brightness alpha will always be 255
		surfcolor.s.alpha = (alpha*light) / (2*256) + 255-light;
	}

	return surfcolor.s.alpha;
}

FBITFIELD HWR_GetBlendModeFlag(INT32 style)
{
	switch (style)
	{
		case AST_TRANSLUCENT:
			return PF_Translucent;
		case AST_ADD:
			return PF_Additive;
		case AST_SUBTRACT:
			return PF_Subtractive;
		case AST_REVERSESUBTRACT:
			return PF_ReverseSubtract;
		case AST_MODULATE:
			return PF_Multiplicative;
		default:
			return PF_Masked;
	}
}

UINT8 HWR_GetTranstableAlpha(INT32 transtablenum)
{
	transtablenum = max(min(transtablenum, tr_trans90), 0);

	switch (transtablenum)
	{
		case 0          : return 0xff;
		case tr_trans10 : return 0xe6;
		case tr_trans20 : return 0xcc;
		case tr_trans30 : return 0xb3;
		case tr_trans40 : return 0x99;
		case tr_trans50 : return 0x80;
		case tr_trans60 : return 0x66;
		case tr_trans70 : return 0x4c;
		case tr_trans80 : return 0x33;
		case tr_trans90 : return 0x19;
	}

	return 0xff;
}

FBITFIELD HWR_SurfaceBlend(INT32 style, INT32 transtablenum, FSurfaceInfo *pSurf)
{
	if (!transtablenum || style <= AST_COPY || style >= AST_OVERLAY)
	{
		pSurf->PolyColor.s.alpha = 0xff;
		return PF_Masked;
	}

	pSurf->PolyColor.s.alpha = HWR_GetTranstableAlpha(transtablenum);
	return HWR_GetBlendModeFlag(style);
}

FBITFIELD HWR_TranstableToAlpha(INT32 transtablenum, FSurfaceInfo *pSurf)
{
	if (!transtablenum)
	{
		pSurf->PolyColor.s.alpha = 0x00;
		return PF_Masked;
	}

	pSurf->PolyColor.s.alpha = HWR_GetTranstableAlpha(transtablenum);
	return PF_Translucent;
}

// -----------------+
// HWR_ClearView : clear the viewwindow, with maximum z value
// -----------------+
static inline void HWR_ClearView(void)
{
	//  3--2
	//  | /|
	//  |/ |
	//  0--1

	/// \bug faB - enable depth mask, disable color mask

	HWD.pfnGClipRect((INT32)gl_viewwindowx,
	                 (INT32)gl_viewwindowy,
	                 (INT32)(gl_viewwindowx + gl_viewwidth),
	                 (INT32)(gl_viewwindowy + gl_viewheight),
	                 ZCLIP_PLANE);
	HWD.pfnClearBuffer(false, true, 0);

	//disable clip window - set to full size
	// rem by Hurdler
	// HWD.pfnGClipRect(0, 0, vid.width, vid.height);
}


// -----------------+
// HWR_SetViewSize  : set projection and scaling values
// -----------------+
void HWR_SetViewSize(void)
{
	// setup view size
	gl_viewwidth = (float)vid.width;
	gl_viewheight = (float)vid.height;

	if (splitscreen)
		gl_viewheight /= 2;

	gl_centerx = gl_viewwidth / 2;
	gl_basecentery = gl_viewheight / 2; //note: this is (gl_centerx * gl_viewheight / gl_viewwidth)

	gl_viewwindowx = (vid.width - gl_viewwidth) / 2;
	gl_windowcenterx = (float)(vid.width / 2);
	if (fabsf(gl_viewwidth - vid.width) < 1.0E-36f)
	{
		gl_baseviewwindowy = 0;
		gl_basewindowcentery = gl_viewheight / 2;               // window top left corner at 0,0
	}
	else
	{
		gl_baseviewwindowy = (vid.height-gl_viewheight) / 2;
		gl_basewindowcentery = (float)(vid.height / 2);
	}

	gl_pspritexscale = gl_viewwidth / BASEVIDWIDTH;
	gl_pspriteyscale = ((vid.height*gl_pspritexscale*BASEVIDWIDTH)/BASEVIDHEIGHT)/vid.width;

	HWD.pfnFlushScreenTextures();
}

// Set view aiming, for the sky dome, the skybox,
// and the normal view, all with a single function.
void HWR_SetTransformAiming(FTransform *trans, player_t *player, boolean skybox)
{
	// 1 = always on
	// 2 = chasecam only
	if (cv_glshearing.value == 1 || (cv_glshearing.value == 2 && R_IsViewpointThirdPerson(player, skybox)))
	{
		fixed_t fixedaiming = AIMINGTODY(aimingangle);
		trans->viewaiming = FIXED_TO_FLOAT(fixedaiming);
		trans->shearing = true;
		gl_aimingangle = 0;
	}
	else
	{
		trans->shearing = false;
		gl_aimingangle = aimingangle;
	}

	trans->anglex = (float)(gl_aimingangle>>ANGLETOFINESHIFT)*(360.0f/(float)FINEANGLES);
}

//
// Sets the shader state.
//
static void HWR_SetShaderState(void)
{
	HWD.pfnSetSpecialState(HWD_SET_SHADERS, (INT32)HWR_UseShader());
}

// ==========================================================================
// Same as rendering the player view, but from the skybox object
// ==========================================================================
void HWR_RenderSkyboxView(INT32 viewnumber, player_t *player)
{
	const float fpov = FIXED_TO_FLOAT(cv_fov.value+player->fovadd);
	postimg_t *type;

	if (splitscreen && player == &players[secondarydisplayplayer])
		type = &postimgtype2;
	else
		type = &postimgtype;

	if (!HWR_ShouldUsePaletteRendering())
	{
		// do we really need to save player (is it not the same)?
		player_t *saved_player = stplyr;
		stplyr = player;
		ST_doPaletteStuff();
		stplyr = saved_player;
#ifdef ALAM_LIGHTING
		HWR_SetLights(viewnumber);
#endif
	}

	// note: sets viewangle, viewx, viewy, viewz
	R_SkyboxFrame(player);

	// copy view cam position for local use
	dup_viewx = viewx;
	dup_viewy = viewy;
	dup_viewz = viewz;
	dup_viewangle = viewangle;

	// set window position
	gl_centery = gl_basecentery;
	gl_viewwindowy = gl_baseviewwindowy;
	gl_windowcentery = gl_basewindowcentery;
	if (splitscreen && viewnumber == 1)
	{
		gl_viewwindowy += (vid.height/2);
		gl_windowcentery += (vid.height/2);
	}

	// check for new console commands.
	NetUpdate();

	gl_viewx = FIXED_TO_FLOAT(dup_viewx);
	gl_viewy = FIXED_TO_FLOAT(dup_viewy);
	gl_viewz = FIXED_TO_FLOAT(dup_viewz);
	gl_viewsin = FIXED_TO_FLOAT(viewsin);
	gl_viewcos = FIXED_TO_FLOAT(viewcos);

	//04/01/2000: Hurdler: added for T&L
	//                     It should replace all other gl_viewxxx when finished
	memset(&atransform, 0x00, sizeof(FTransform));

	HWR_SetTransformAiming(&atransform, player, true);
	atransform.angley = (float)(viewangle>>ANGLETOFINESHIFT)*(360.0f/(float)FINEANGLES);

	gl_viewludsin = FIXED_TO_FLOAT(FINECOSINE(gl_aimingangle>>ANGLETOFINESHIFT));
	gl_viewludcos = FIXED_TO_FLOAT(-FINESINE(gl_aimingangle>>ANGLETOFINESHIFT));

	if (*type == postimg_flip)
		atransform.flip = true;
	else
		atransform.flip = false;

	atransform.x      = gl_viewx;  // FIXED_TO_FLOAT(viewx)
	atransform.y      = gl_viewy;  // FIXED_TO_FLOAT(viewy)
	atransform.z      = gl_viewz;  // FIXED_TO_FLOAT(viewz)
	atransform.scalex = 1;
	atransform.scaley = (float)vid.width/vid.height;
	atransform.scalez = 1;

	atransform.fovxangle = fpov; // Tails
	atransform.fovyangle = fpov; // Tails
	if (player->viewrollangle != 0)
	{
		fixed_t rol = AngleFixed(player->viewrollangle);
		atransform.rollangle = FIXED_TO_FLOAT(rol);
		atransform.roll = true;
	}
	atransform.splitscreen = splitscreen;

	gl_fovlud = (float)(1.0l/tan((double)(fpov*M_PIl/360l)));

	//------------------------------------------------------------------------
	HWR_ClearView();

	if (drawsky)
		HWR_DrawSkyBackground(player);

	//Hurdler: it doesn't work in splitscreen mode
	drawsky = splitscreen;

	HWR_ClearSprites();

	drawcount = 0;

	if (rendermode == render_opengl)
	{
		angle_t a1 = gld_FrustumAngle(gl_aimingangle);
		gld_clipper_Clear();
		gld_clipper_SafeAddClipRange(viewangle + a1, viewangle - a1);
#ifdef HAVE_SPHEREFRUSTRUM
		gld_FrustrumSetup();
#endif
	}

	//04/01/2000: Hurdler: added for T&L
	//                     Actually it only works on Walls and Planes
	HWD.pfnSetTransform(&atransform);

	// Reset the shader state.
	HWR_SetShaderState();

	validcount++;

	if (cv_glbatching.value)
		HWR_StartBatching();

	HWR_RenderBSPNode((INT32)numnodes-1);

	if (cv_glbatching.value)
		HWR_RenderBatches();

	// Check for new console commands.
	NetUpdate();

#ifdef ALAM_LIGHTING
	//14/11/99: Hurdler: moved here because it doesn't work with
	// subsector, see other comments;
	HWR_ResetLights();
#endif

	// Draw MD2 and sprites
	HWR_SortVisSprites();
	HWR_DrawSprites();

#ifdef NEWCORONAS
	//Hurdler: they must be drawn before translucent planes, what about gl fog?
	HWR_DrawCoronas();
#endif

	HWR_CreateDrawNodes(); //Hurdler: render 3D water and transparent walls after everything

	HWD.pfnSetTransform(NULL);
	HWD.pfnUnSetShader();

	// Check for new console commands.
	NetUpdate();

	// added by Hurdler for correct splitscreen
	// moved here by hurdler so it works with the new near clipping plane
	HWD.pfnGClipRect(0, 0, vid.width, vid.height, NZCLIP_PLANE);
}

// ==========================================================================
//
// ==========================================================================
void HWR_RenderPlayerView(INT32 viewnumber, player_t *player)
{
	const float fpov = FIXED_TO_FLOAT(cv_fov.value+player->fovadd);
	postimg_t *type;

	const boolean skybox = (skyboxmo[0] && cv_skybox.value); // True if there's a skybox object and skyboxes are on

	FRGBAFloat ClearColor;

	if (splitscreen && player == &players[secondarydisplayplayer])
		type = &postimgtype2;
	else
		type = &postimgtype;

	ClearColor.red = 0.0f;
	ClearColor.green = 0.0f;
	ClearColor.blue = 0.0f;
	ClearColor.alpha = 1.0f;

	if (cv_glshaders.value)
		HWD.pfnSetShaderInfo(HWD_SHADERINFO_LEVELTIME, (INT32)leveltime); // The water surface shader needs the leveltime.

	if (viewnumber == 0) // Only do it if it's the first screen being rendered
		HWD.pfnClearBuffer(true, false, &ClearColor); // Clear the Color Buffer, stops HOMs. Also seems to fix the skybox issue on Intel GPUs.

	PS_START_TIMING(ps_hw_skyboxtime);
	if (skybox && drawsky) // If there's a skybox and we should be drawing the sky, draw the skybox
		HWR_RenderSkyboxView(viewnumber, player); // This is drawn before everything else so it is placed behind
	PS_STOP_TIMING(ps_hw_skyboxtime);

	if (!HWR_ShouldUsePaletteRendering())
	{
		// do we really need to save player (is it not the same)?
		player_t *saved_player = stplyr;
		stplyr = player;
		ST_doPaletteStuff();
		stplyr = saved_player;
#ifdef ALAM_LIGHTING
		HWR_SetLights(viewnumber);
#endif
	}

	// note: sets viewangle, viewx, viewy, viewz
	R_SetupFrame(player);
	framecount++; // timedemo

	// copy view cam position for local use
	dup_viewx = viewx;
	dup_viewy = viewy;
	dup_viewz = viewz;
	dup_viewangle = viewangle;

	// set window position
	gl_centery = gl_basecentery;
	gl_viewwindowy = gl_baseviewwindowy;
	gl_windowcentery = gl_basewindowcentery;
	if (splitscreen && viewnumber == 1)
	{
		gl_viewwindowy += (vid.height/2);
		gl_windowcentery += (vid.height/2);
	}

	// check for new console commands.
	NetUpdate();

	gl_viewx = FIXED_TO_FLOAT(dup_viewx);
	gl_viewy = FIXED_TO_FLOAT(dup_viewy);
	gl_viewz = FIXED_TO_FLOAT(dup_viewz);
	gl_viewsin = FIXED_TO_FLOAT(viewsin);
	gl_viewcos = FIXED_TO_FLOAT(viewcos);

	//04/01/2000: Hurdler: added for T&L
	//                     It should replace all other gl_viewxxx when finished
	memset(&atransform, 0x00, sizeof(FTransform));

	HWR_SetTransformAiming(&atransform, player, false);
	atransform.angley = (float)(viewangle>>ANGLETOFINESHIFT)*(360.0f/(float)FINEANGLES);

	gl_viewludsin = FIXED_TO_FLOAT(FINECOSINE(gl_aimingangle>>ANGLETOFINESHIFT));
	gl_viewludcos = FIXED_TO_FLOAT(-FINESINE(gl_aimingangle>>ANGLETOFINESHIFT));

	if (*type == postimg_flip)
		atransform.flip = true;
	else
		atransform.flip = false;

	atransform.x      = gl_viewx;  // FIXED_TO_FLOAT(viewx)
	atransform.y      = gl_viewy;  // FIXED_TO_FLOAT(viewy)
	atransform.z      = gl_viewz;  // FIXED_TO_FLOAT(viewz)
	atransform.scalex = 1;
	atransform.scaley = (float)vid.width/vid.height;
	atransform.scalez = 1;

	atransform.fovxangle = fpov; // Tails
	atransform.fovyangle = fpov; // Tails
	if (player->viewrollangle != 0)
	{
		fixed_t rol = AngleFixed(player->viewrollangle);
		atransform.rollangle = FIXED_TO_FLOAT(rol);
		atransform.roll = true;
	}
	atransform.splitscreen = splitscreen;

	gl_fovlud = (float)(1.0l/tan((double)(fpov*M_PIl/360l)));

	//------------------------------------------------------------------------
	HWR_ClearView(); // Clears the depth buffer and resets the view I believe

	if (!skybox && drawsky) // Don't draw the regular sky if there's a skybox
		HWR_DrawSkyBackground(player);

	//Hurdler: it doesn't work in splitscreen mode
	drawsky = splitscreen;

	HWR_ClearSprites();

	drawcount = 0;

	if (rendermode == render_opengl)
	{
		angle_t a1 = gld_FrustumAngle(gl_aimingangle);
		gld_clipper_Clear();
		gld_clipper_SafeAddClipRange(viewangle + a1, viewangle - a1);
#ifdef HAVE_SPHEREFRUSTRUM
		gld_FrustrumSetup();
#endif
	}

	//04/01/2000: Hurdler: added for T&L
	//                     Actually it only works on Walls and Planes
	HWD.pfnSetTransform(&atransform);

	// Reset the shader state.
	HWR_SetShaderState();

	ps_numbspcalls.value.i = 0;
	ps_numpolyobjects.value.i = 0;
	PS_START_TIMING(ps_bsptime);

	validcount++;

	if (cv_glbatching.value)
		HWR_StartBatching();

	HWR_RenderBSPNode((INT32)numnodes-1);

	PS_STOP_TIMING(ps_bsptime);

	if (cv_glbatching.value)
		HWR_RenderBatches();

	// Check for new console commands.
	NetUpdate();

#ifdef ALAM_LIGHTING
	//14/11/99: Hurdler: moved here because it doesn't work with
	// subsector, see other comments;
	HWR_ResetLights();
#endif

	// Draw MD2 and sprites
	ps_numsprites.value.i = gl_visspritecount;
	PS_START_TIMING(ps_hw_spritesorttime);
	HWR_SortVisSprites();
	PS_STOP_TIMING(ps_hw_spritesorttime);
	PS_START_TIMING(ps_hw_spritedrawtime);
	HWR_DrawSprites();
	PS_STOP_TIMING(ps_hw_spritedrawtime);

#ifdef NEWCORONAS
	//Hurdler: they must be drawn before translucent planes, what about gl fog?
	HWR_DrawCoronas();
#endif

	ps_numdrawnodes.value.i = 0;
	ps_hw_nodesorttime.value.p = 0;
	ps_hw_nodedrawtime.value.p = 0;
	HWR_CreateDrawNodes(); //Hurdler: render 3D water and transparent walls after everything

	HWD.pfnSetTransform(NULL);
	HWD.pfnUnSetShader();

	HWR_DoPostProcessor(player);

	// Check for new console commands.
	NetUpdate();

	// added by Hurdler for correct splitscreen
	// moved here by hurdler so it works with the new near clipping plane
	HWD.pfnGClipRect(0, 0, vid.width, vid.height, NZCLIP_PLANE);
}

// Returns whether palette rendering is "actually enabled."
// Can't have palette rendering if shaders are disabled.
boolean HWR_ShouldUsePaletteRendering(void)
{
	return (cv_glpaletterendering.value && HWR_UseShader());
}

// enable or disable palette rendering state depending on settings and availability
// called when relevant settings change
// shader recompilation is done in the cvar callback
static void HWR_TogglePaletteRendering(void)
{
	// which state should we go to?
	if (HWR_ShouldUsePaletteRendering())
	{
		// are we not in that state already?
		if (!gl_palette_rendering_state)
		{
			gl_palette_rendering_state = true;

			// The textures will still be converted to RGBA by r_opengl.
			// This however makes hw_cache use paletted blending for composite textures!
			// (patchformat is not touched)
			textureformat = GL_TEXFMT_P_8;

			HWR_SetMapPalette();
			HWR_SetPalette(pLocalPalette);

			// If the r_opengl "texture palette" stays the same during this switch, these textures
			// will not be cleared out. However they are still out of date since the
			// composite texture blending method has changed. Therefore they need to be cleared.
			HWR_LoadMapTextures(numtextures);
		}
	}
	else
	{
		// are we not in that state already?
		if (gl_palette_rendering_state)
		{
			gl_palette_rendering_state = false;
			textureformat = GL_TEXFMT_RGBA;
			HWR_SetPalette(pLocalPalette);
			// If the r_opengl "texture palette" stays the same during this switch, these textures
			// will not be cleared out. However they are still out of date since the
			// composite texture blending method has changed. Therefore they need to be cleared.
			HWR_LoadMapTextures(numtextures);
		}
	}
}

void HWR_LoadLevel(void)
{
#ifdef ALAM_LIGHTING
	// BP: reset light between levels (we draw preview frame lights on current frame)
	HWR_ResetLights();
#endif

	HWR_CreatePlanePolygons((INT32)numnodes - 1);

	// Build the sky dome
	HWR_ClearSkyDome();
	HWR_BuildSkyDome();

	if (HWR_ShouldUsePaletteRendering())
		HWR_SetMapPalette();

	gl_maploaded = true;
}

// ==========================================================================
//                                                         3D ENGINE COMMANDS
// ==========================================================================

static CV_PossibleValue_t glshaders_cons_t[] = {{0, "Off"}, {1, "On"}, {2, "Ignore custom shaders"}, {0, NULL}};
static CV_PossibleValue_t glmodelinterpolation_cons_t[] = {{0, "Off"}, {1, "Sometimes"}, {2, "Always"}, {0, NULL}};
static CV_PossibleValue_t glfakecontrast_cons_t[] = {{0, "Off"}, {1, "On"}, {2, "Smooth"}, {0, NULL}};
static CV_PossibleValue_t glshearing_cons_t[] = {{0, "Off"}, {1, "On"}, {2, "Third-person"}, {0, NULL}};

static void CV_glfiltermode_OnChange(void);
static void CV_glanisotropic_OnChange(void);
static void CV_glmodellighting_OnChange(void);
static void CV_glpaletterendering_OnChange(void);
static void CV_glpalettedepth_OnChange(void);
static void CV_glshaders_OnChange(void);

static CV_PossibleValue_t glfiltermode_cons_t[]= {{HWD_SET_TEXTUREFILTER_POINTSAMPLED, "Nearest"},
	{HWD_SET_TEXTUREFILTER_BILINEAR, "Bilinear"}, {HWD_SET_TEXTUREFILTER_TRILINEAR, "Trilinear"},
	{HWD_SET_TEXTUREFILTER_MIXED1, "Linear_Nearest"},
	{HWD_SET_TEXTUREFILTER_MIXED2, "Nearest_Linear"},
	{HWD_SET_TEXTUREFILTER_MIXED3, "Nearest_Mipmap"},
	{0, NULL}};
CV_PossibleValue_t glanisotropicmode_cons_t[] = {{1, "MIN"}, {16, "MAX"}, {0, NULL}};

consvar_t cv_glshaders = CVAR_INIT ("gr_shaders", "On", CV_SAVE|CV_CALL, glshaders_cons_t, CV_glshaders_OnChange);
consvar_t cv_glallowshaders = CVAR_INIT ("gr_allowclientshaders", "On", CV_NETVAR, CV_OnOff, NULL);
consvar_t cv_fovchange = CVAR_INIT ("gr_fovchange", "Off", CV_SAVE, CV_OnOff, NULL);

#ifdef ALAM_LIGHTING
consvar_t cv_gldynamiclighting = CVAR_INIT ("gr_dynamiclighting", "On", CV_SAVE, CV_OnOff, NULL);
consvar_t cv_glstaticlighting  = CVAR_INIT ("gr_staticlighting", "On", CV_SAVE, CV_OnOff, NULL);
consvar_t cv_glcoronas = CVAR_INIT ("gr_coronas", "On", CV_SAVE, CV_OnOff, NULL);
consvar_t cv_glcoronasize = CVAR_INIT ("gr_coronasize", "1", CV_SAVE|CV_FLOAT, 0, NULL);
#endif

consvar_t cv_glmodels = CVAR_INIT ("gr_models", "Off", CV_SAVE, CV_OnOff, NULL);
consvar_t cv_glmodelinterpolation = CVAR_INIT ("gr_modelinterpolation", "Sometimes", CV_SAVE, glmodelinterpolation_cons_t, NULL);
consvar_t cv_glmodellighting = CVAR_INIT ("gr_modellighting", "Off", CV_SAVE|CV_CALL, CV_OnOff, CV_glmodellighting_OnChange);

consvar_t cv_glshearing = CVAR_INIT ("gr_shearing", "Off", CV_SAVE, glshearing_cons_t, NULL);
consvar_t cv_glspritebillboarding = CVAR_INIT ("gr_spritebillboarding", "Off", CV_SAVE, CV_OnOff, NULL);
consvar_t cv_glskydome = CVAR_INIT ("gr_skydome", "On", CV_SAVE, CV_OnOff, NULL);
consvar_t cv_glfakecontrast = CVAR_INIT ("gr_fakecontrast", "Smooth", CV_SAVE, glfakecontrast_cons_t, NULL);
consvar_t cv_glslopecontrast = CVAR_INIT ("gr_slopecontrast", "Off", CV_SAVE, CV_OnOff, NULL);

consvar_t cv_glfiltermode = CVAR_INIT ("gr_filtermode", "Nearest", CV_SAVE|CV_CALL, glfiltermode_cons_t, CV_glfiltermode_OnChange);
consvar_t cv_glanisotropicmode = CVAR_INIT ("gr_anisotropicmode", "1", CV_CALL, glanisotropicmode_cons_t, CV_glanisotropic_OnChange);

consvar_t cv_glsolvetjoin = CVAR_INIT ("gr_solvetjoin", "On", 0, CV_OnOff, NULL);

consvar_t cv_glbatching = CVAR_INIT ("gr_batching", "On", 0, CV_OnOff, NULL);

static CV_PossibleValue_t glpalettedepth_cons_t[] = {{16, "16 bits"}, {24, "24 bits"}, {0, NULL}};

consvar_t cv_glpaletterendering = CVAR_INIT ("gr_paletterendering", "Off", CV_SAVE|CV_CALL, CV_OnOff, CV_glpaletterendering_OnChange);
consvar_t cv_glpalettedepth = CVAR_INIT ("gr_palettedepth", "16 bits", CV_SAVE|CV_CALL, glpalettedepth_cons_t, CV_glpalettedepth_OnChange);

static void CV_glfiltermode_OnChange(void)
{
	if (rendermode == render_opengl)
		HWD.pfnSetSpecialState(HWD_SET_TEXTUREFILTERMODE, cv_glfiltermode.value);
}

static void CV_glanisotropic_OnChange(void)
{
	if (rendermode == render_opengl)
		HWD.pfnSetSpecialState(HWD_SET_TEXTUREANISOTROPICMODE, cv_glanisotropicmode.value);
}

static void CV_glmodellighting_OnChange(void)
{
	// if shaders have been compiled, then they now need to be recompiled.
	if (gl_shadersavailable)
		HWR_CompileShaders();
}

static void CV_glpaletterendering_OnChange(void)
{
	if (gl_shadersavailable)
	{
		HWR_CompileShaders();
		HWR_TogglePaletteRendering();
	}
}

static void CV_glpalettedepth_OnChange(void)
{
	// refresh the screen palette
	if (HWR_ShouldUsePaletteRendering())
		HWR_SetPalette(pLocalPalette);
}

static void CV_glshaders_OnChange(void)
{
	if (cv_glpaletterendering.value)
	{
		// can't do palette rendering without shaders, so update the state if needed
		HWR_TogglePaletteRendering();
	}
}

//added by Hurdler: console varibale that are saved
void HWR_AddCommands(void)
{
	CV_RegisterVar(&cv_fovchange);

#ifdef ALAM_LIGHTING
	CV_RegisterVar(&cv_glstaticlighting);
	CV_RegisterVar(&cv_gldynamiclighting);
	CV_RegisterVar(&cv_glcoronasize);
	CV_RegisterVar(&cv_glcoronas);
#endif

	CV_RegisterVar(&cv_glmodellighting);
	CV_RegisterVar(&cv_glmodelinterpolation);
	CV_RegisterVar(&cv_glmodels);

	CV_RegisterVar(&cv_glskydome);
	CV_RegisterVar(&cv_glspritebillboarding);
	CV_RegisterVar(&cv_glfakecontrast);
	CV_RegisterVar(&cv_glshearing);
	CV_RegisterVar(&cv_glshaders);
	CV_RegisterVar(&cv_glallowshaders);

	CV_RegisterVar(&cv_glfiltermode);
	CV_RegisterVar(&cv_glsolvetjoin);

	CV_RegisterVar(&cv_glbatching);

	CV_RegisterVar(&cv_glpaletterendering);
	CV_RegisterVar(&cv_glpalettedepth);
}

void HWR_AddSessionCommands(void)
{
	if (gl_sessioncommandsadded)
		return;
	CV_RegisterVar(&cv_glanisotropicmode);
	gl_sessioncommandsadded = true;
}

// --------------------------------------------------------------------------
// Setup the hardware renderer
// --------------------------------------------------------------------------
void HWR_Startup(void)
{
	if (!gl_init)
	{
		CONS_Printf("HWR_Startup()...\n");

		textureformat = patchformat = GL_TEXFMT_RGBA;

		HWR_InitPolyPool();
		HWR_AddSessionCommands();
		HWR_InitMapTextures();
		HWR_InitModels();
#ifdef ALAM_LIGHTING
		HWR_InitLight();
#endif

		gl_shadersavailable = HWR_InitShaders();
		HWR_LoadAllCustomShaders();
		HWR_TogglePaletteRendering();
	}

	gl_init = true;
}

// --------------------------------------------------------------------------
// Called after switching to the hardware renderer
// --------------------------------------------------------------------------
void HWR_Switch(void)
{
	// Add session commands
	if (!gl_sessioncommandsadded)
		HWR_AddSessionCommands();

	// Set special states from CVARs
	HWD.pfnSetSpecialState(HWD_SET_TEXTUREFILTERMODE, cv_glfiltermode.value);
	HWD.pfnSetSpecialState(HWD_SET_TEXTUREANISOTROPICMODE, cv_glanisotropicmode.value);

	// Load textures
	if (!gl_maptexturesloaded)
		HWR_LoadMapTextures(numtextures);

	// Create plane polygons
	if (!gl_maploaded && (gamestate == GS_LEVEL || (gamestate == GS_TITLESCREEN && titlemapinaction)))
	{
		HWR_ClearAllTextures();
		HWR_LoadLevel();
	}
}

// --------------------------------------------------------------------------
// Free resources allocated by the hardware renderer
// --------------------------------------------------------------------------
void HWR_Shutdown(void)
{
	CONS_Printf("HWR_Shutdown()\n");
	HWR_FreeExtraSubsectors();
	HWR_FreePolyPool();
	HWR_FreeMapTextures();
	HWD.pfnFlushScreenTextures();
}

void transform(float *cx, float *cy, float *cz)
{
	float tr_x,tr_y;
	// translation
	tr_x = *cx - gl_viewx;
	tr_y = *cz - gl_viewy;
//	*cy = *cy;

	// rotation around vertical y axis
	*cx = (tr_x * gl_viewsin) - (tr_y * gl_viewcos);
	tr_x = (tr_x * gl_viewcos) + (tr_y * gl_viewsin);

	//look up/down ----TOTAL SUCKS!!!--- do the 2 in one!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	tr_y = *cy - gl_viewz;

	*cy = (tr_x * gl_viewludcos) + (tr_y * gl_viewludsin);
	*cz = (tr_x * gl_viewludsin) - (tr_y * gl_viewludcos);

	//scale y before frustum so that frustum can be scaled to screen height
	*cy *= ORIGINAL_ASPECT * gl_fovlud;
	*cx *= gl_fovlud;
}

INT32 HWR_GetTextureUsed(void)
{
	return HWD.pfnGetTextureUsed();
}

void HWR_DoPostProcessor(player_t *player)
{
	postimg_t *type;

	HWD.pfnUnSetShader();

	if (splitscreen && player == &players[secondarydisplayplayer])
		type = &postimgtype2;
	else
		type = &postimgtype;

	// Armageddon Blast Flash!
	// Could this even be considered postprocessor?
	if (player->flashcount && !HWR_ShouldUsePaletteRendering())
	{
		FOutVector      v[4];
		FSurfaceInfo Surf;

		v[0].x = v[2].y = v[3].x = v[3].y = -4.0f;
		v[0].y = v[1].x = v[1].y = v[2].x = 4.0f;
		v[0].z = v[1].z = v[2].z = v[3].z = 4.0f; // 4.0 because of the same reason as with the sky, just after the screen is cleared so near clipping plane is 3.99

		// This won't change if the flash palettes are changed unfortunately, but it works for its purpose
		if (player->flashpal == PAL_NUKE)
		{
			Surf.PolyColor.s.red = 0xff;
			Surf.PolyColor.s.green = Surf.PolyColor.s.blue = 0x7F; // The nuke palette is kind of pink-ish
		}
		else
			Surf.PolyColor.s.red = Surf.PolyColor.s.green = Surf.PolyColor.s.blue = 0xff;

		Surf.PolyColor.s.alpha = 0xc0; // match software mode

		HWD.pfnDrawPolygon(&Surf, v, 4, PF_Modulated|PF_Additive|PF_NoTexture|PF_NoDepthTest);
	}

	// Capture the screen for intermission and screen waving
	if(gamestate != GS_INTERMISSION)
		HWD.pfnMakeScreenTexture(HWD_SCREENTEXTURE_GENERIC1);

	if (splitscreen) // Not supported in splitscreen - someone want to add support?
		return;

	// Drunken vision! WooOOooo~
	if (*type == postimg_water || *type == postimg_heat)
	{
		// 10 by 10 grid. 2 coordinates (xy)
		float v[SCREENVERTS][SCREENVERTS][2];
		static double disStart = 0;
		UINT8 x, y;
		INT32 WAVELENGTH;
		INT32 AMPLITUDE;
		INT32 FREQUENCY;

		// Modifies the wave.
		if (*type == postimg_water)
		{
			WAVELENGTH = 20; // Lower is longer
			AMPLITUDE = 20; // Lower is bigger
			FREQUENCY = 16; // Lower is faster
		}
		else
		{
			WAVELENGTH = 10; // Lower is longer
			AMPLITUDE = 30; // Lower is bigger
			FREQUENCY = 4; // Lower is faster
		}

		for (x = 0; x < SCREENVERTS; x++)
		{
			for (y = 0; y < SCREENVERTS; y++)
			{
				// Change X position based on its Y position.
				v[x][y][0] = (x/((float)(SCREENVERTS-1.0f)/9.0f))-4.5f + (float)sin((disStart+(y*WAVELENGTH))/FREQUENCY)/AMPLITUDE;
				v[x][y][1] = (y/((float)(SCREENVERTS-1.0f)/9.0f))-4.5f;
			}
		}
		HWD.pfnPostImgRedraw(v);
		if (!(paused || P_AutoPause()))
			disStart += 1;

		// Capture the screen again for screen waving on the intermission
		if(gamestate != GS_INTERMISSION)
			HWD.pfnMakeScreenTexture(HWD_SCREENTEXTURE_GENERIC1);
	}
	// Flipping of the screen isn't done here anymore
}

void HWR_StartScreenWipe(void)
{
	//CONS_Debug(DBG_RENDER, "In HWR_StartScreenWipe()\n");
	HWD.pfnMakeScreenTexture(HWD_SCREENTEXTURE_WIPE_START);
}

void HWR_EndScreenWipe(void)
{
	//CONS_Debug(DBG_RENDER, "In HWR_EndScreenWipe()\n");
	HWD.pfnMakeScreenTexture(HWD_SCREENTEXTURE_WIPE_END);
}

void HWR_DrawIntermissionBG(void)
{
	HWD.pfnDrawScreenTexture(HWD_SCREENTEXTURE_GENERIC1);
}

//
// hwr mode wipes
//
static lumpnum_t wipelumpnum;

// puts wipe lumpname in wipename[9]
static boolean HWR_WipeCheck(UINT8 wipenum, UINT8 scrnnum)
{
	static char lumpname[9] = "FADEmmss";
	size_t lsize;

	// not a valid wipe number
	if (wipenum > 99 || scrnnum > 99)
		return false; // shouldn't end up here really, the loop should've stopped running beforehand

	// puts the numbers into the wipename
	lumpname[4] = '0'+(wipenum/10);
	lumpname[5] = '0'+(wipenum%10);
	lumpname[6] = '0'+(scrnnum/10);
	lumpname[7] = '0'+(scrnnum%10);
	wipelumpnum = W_CheckNumForName(lumpname);

	// again, shouldn't be here really
	if (wipelumpnum == LUMPERROR)
		return false;

	lsize = W_LumpLength(wipelumpnum);
	if (!(lsize == 256000 || lsize == 64000 || lsize == 16000 || lsize == 4000))
	{
		CONS_Alert(CONS_WARNING, "Fade mask lump %s of incorrect size, ignored\n", lumpname);
		return false; // again, shouldn't get here if it is a bad size
	}

	return true;
}

void HWR_DoWipe(UINT8 wipenum, UINT8 scrnnum)
{
	if (!HWR_WipeCheck(wipenum, scrnnum))
		return;

	HWR_GetFadeMask(wipelumpnum);
	HWD.pfnDoScreenWipe(HWD_SCREENTEXTURE_WIPE_START, HWD_SCREENTEXTURE_WIPE_END);
}

void HWR_DoTintedWipe(UINT8 wipenum, UINT8 scrnnum)
{
	// It does the same thing
	HWR_DoWipe(wipenum, scrnnum);
}

void HWR_MakeScreenFinalTexture(void)
{
	int tex = HWR_ShouldUsePaletteRendering() ? HWD_SCREENTEXTURE_GENERIC3 : HWD_SCREENTEXTURE_GENERIC2;
	HWD.pfnMakeScreenTexture(tex);
}

void HWR_DrawScreenFinalTexture(int width, int height)
{
	int tex = HWR_ShouldUsePaletteRendering() ? HWD_SCREENTEXTURE_GENERIC3 : HWD_SCREENTEXTURE_GENERIC2;
	HWD.pfnDrawScreenFinalTexture(tex, width, height);
}

#endif // HWRENDER
