//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//

#include <stdlib.h>
#include <float.h>
#include "templates.h"
#include "i_system.h"
#include "w_wad.h"
#include "doomdef.h"
#include "doomstat.h"
#include "r_sky.h"
#include "stats.h"
#include "v_video.h"
#include "a_sharedglobal.h"
#include "c_console.h"
#include "cmdlib.h"
#include "d_net.h"
#include "g_level.h"
#include "swrenderer/scene/r_opaque_pass.h"
#include "r_skyplane.h"
#include "swrenderer/scene/r_3dfloors.h"
#include "v_palette.h"
#include "r_data/colormaps.h"
#include "swrenderer/drawers/r_draw_rgba.h"
#include "gl/dynlights/gl_dynlight.h"
#include "swrenderer/segments/r_clipsegment.h"
#include "swrenderer/segments/r_drawsegment.h"
#include "swrenderer/line/r_wallsetup.h"
#include "swrenderer/line/r_walldraw.h"
#include "swrenderer/scene/r_portal.h"
#include "swrenderer/scene/r_scene.h"
#include "swrenderer/scene/r_viewport.h"
#include "swrenderer/scene/r_light.h"
#include "swrenderer/r_memory.h"
#include "g_levellocals.h"

CVAR(Bool, r_linearsky, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
EXTERN_CVAR(Int, r_skymode)

namespace swrenderer
{
	void RenderSkyPlane::Render(VisiblePlane *pl)
	{
		FTextureID sky1tex, sky2tex;
		double frontdpos = 0, backdpos = 0;

		if ((level.flags & LEVEL_SWAPSKIES) && !(level.flags & LEVEL_DOUBLESKY))
		{
			sky1tex = sky2texture;
		}
		else
		{
			sky1tex = sky1texture;
		}
		sky2tex = sky2texture;
		skymid = skytexturemid;
		skyangle = ViewAngle.BAMs();

		if (pl->picnum == skyflatnum)
		{
			if (!(pl->sky & PL_SKYFLAT))
			{	// use sky1
			sky1:
				frontskytex = TexMan(sky1tex, true);
				if (level.flags & LEVEL_DOUBLESKY)
					backskytex = TexMan(sky2tex, true);
				else
					backskytex = NULL;
				skyflip = 0;
				frontdpos = sky1pos;
				backdpos = sky2pos;
				frontcyl = sky1cyl;
				backcyl = sky2cyl;
			}
			else if (pl->sky == PL_SKYFLAT)
			{	// use sky2
				frontskytex = TexMan(sky2tex, true);
				backskytex = NULL;
				frontcyl = sky2cyl;
				skyflip = 0;
				frontdpos = sky2pos;
			}
			else
			{	// MBF's linedef-controlled skies
				// Sky Linedef
				const line_t *l = &level.lines[(pl->sky & ~PL_SKYFLAT) - 1];

				// Sky transferred from first sidedef
				const side_t *s = l->sidedef[0];
				int pos;

				// Texture comes from upper texture of reference sidedef
				// [RH] If swapping skies, then use the lower sidedef
				if (level.flags & LEVEL_SWAPSKIES && s->GetTexture(side_t::bottom).isValid())
				{
					pos = side_t::bottom;
				}
				else
				{
					pos = side_t::top;
				}

				frontskytex = TexMan(s->GetTexture(pos), true);
				if (frontskytex == NULL || frontskytex->UseType == FTexture::TEX_Null)
				{ // [RH] The blank texture: Use normal sky instead.
					goto sky1;
				}
				backskytex = NULL;

				// Horizontal offset is turned into an angle offset,
				// to allow sky rotation as well as careful positioning.
				// However, the offset is scaled very small, so that it
				// allows a long-period of sky rotation.
				skyangle += FLOAT2FIXED(s->GetTextureXOffset(pos));

				// Vertical offset allows careful sky positioning.
				skymid = s->GetTextureYOffset(pos);

				// We sometimes flip the picture horizontally.
				//
				// Doom always flipped the picture, so we make it optional,
				// to make it easier to use the new feature, while to still
				// allow old sky textures to be used.
				skyflip = l->args[2] ? 0u : ~0u;

				int frontxscale = int(frontskytex->Scale.X * 1024);
				frontcyl = MAX(frontskytex->GetWidth(), frontxscale);
				if (skystretch)
				{
					skymid = skymid * frontskytex->GetScaledHeightDouble() / SKYSTRETCH_HEIGHT;
				}
			}
		}
		frontpos = int(fmod(frontdpos, sky1cyl * 65536.0));
		if (backskytex != NULL)
		{
			backpos = int(fmod(backdpos, sky2cyl * 65536.0));
		}

		bool fakefixed = false;
		CameraLight *cameraLight = CameraLight::Instance();
		if (cameraLight->fixedcolormap)
		{
			drawerargs.SetColorMapLight(cameraLight->fixedcolormap, 0, 0);
		}
		else
		{
			fakefixed = true;
			cameraLight->fixedcolormap = &NormalLight;
			drawerargs.SetColorMapLight(cameraLight->fixedcolormap, 0, 0);
		}

		DrawSky(pl);

		if (fakefixed)
			cameraLight->fixedcolormap = nullptr;
	}

	void RenderSkyPlane::DrawSkyColumnStripe(int start_x, int y1, int y2, double scale, double texturemid, double yrepeat)
	{
		RenderPortal *renderportal = RenderPortal::Instance();

		uint32_t height = frontskytex->GetHeight();

		double uv_stepd = skyiscale * yrepeat;
		double v = (texturemid + uv_stepd * (y1 - CenterY + 0.5)) / height;
		double v_step = uv_stepd / height;

		uint32_t uv_pos = (uint32_t)(v * 0x01000000);
		uint32_t uv_step = (uint32_t)(v_step * 0x01000000);

		int x = start_x;
		if (renderportal->MirrorFlags & RF_XFLIP)
			x = (viewwidth - x);

		uint32_t ang, angle1, angle2;

		if (r_linearsky)
		{
			angle_t xangle = (angle_t)((0.5 - x / (double)viewwidth) * FocalTangent * ANGLE_90);
			ang = (skyangle + xangle) ^ skyflip;
		}
		else
		{
			ang = (skyangle + xtoviewangle[x]) ^ skyflip;
		}
		angle1 = (uint32_t)((UMulScale16(ang, frontcyl) + frontpos) >> FRACBITS);
		angle2 = (uint32_t)((UMulScale16(ang, backcyl) + backpos) >> FRACBITS);

		if (r_swtruecolor)
		{
			drawerargs.dc_wall_source = (const uint8_t *)frontskytex->GetColumnBgra(angle1, nullptr);
			drawerargs.dc_wall_source2 = backskytex ? (const uint8_t *)backskytex->GetColumnBgra(angle2, nullptr) : nullptr;
		}
		else
		{
			drawerargs.dc_wall_source = (const uint8_t *)frontskytex->GetColumn(angle1, nullptr);
			drawerargs.dc_wall_source2 = backskytex ? (const uint8_t *)backskytex->GetColumn(angle2, nullptr) : nullptr;
		}

		drawerargs.dc_wall_iscale = uv_step;
		drawerargs.dc_wall_texturefrac = uv_pos;

		drawerargs.dc_wall_sourceheight[0] = height;
		drawerargs.dc_wall_sourceheight[1] = backskytex ? backskytex->GetHeight() : height;
		int pixelsize = r_swtruecolor ? 4 : 1;
		drawerargs.SetDest(start_x, y1);
		drawerargs.dc_count = y2 - y1;

		uint32_t solid_top = frontskytex->GetSkyCapColor(false);
		uint32_t solid_bottom = frontskytex->GetSkyCapColor(true);

		bool fadeSky = (r_skymode == 2 && !(level.flags & LEVEL_FORCETILEDSKY));

		if (!backskytex)
			drawerargs.DrawSingleSkyColumn(solid_top, solid_bottom, fadeSky);
		else
			drawerargs.DrawDoubleSkyColumn(solid_top, solid_bottom, fadeSky);
	}

	void RenderSkyPlane::DrawSkyColumn(int start_x, int y1, int y2)
	{
		if (1 << frontskytex->HeightBits == frontskytex->GetHeight())
		{
			double texturemid = skymid * frontskytex->Scale.Y + frontskytex->GetHeight();
			DrawSkyColumnStripe(start_x, y1, y2, frontskytex->Scale.Y, texturemid, frontskytex->Scale.Y);
		}
		else
		{
			double yrepeat = frontskytex->Scale.Y;
			double scale = frontskytex->Scale.Y * skyscale;
			double iscale = 1 / scale;
			short drawheight = short(frontskytex->GetHeight() * scale);
			double topfrac = fmod(skymid + iscale * (1 - CenterY), frontskytex->GetHeight());
			if (topfrac < 0) topfrac += frontskytex->GetHeight();
			double texturemid = topfrac - iscale * (1 - CenterY);
			DrawSkyColumnStripe(start_x, y1, y2, scale, texturemid, yrepeat);
		}
	}

	void RenderSkyPlane::DrawSky(VisiblePlane *pl)
	{
		int x1 = pl->left;
		int x2 = pl->right;
		short *uwal = (short *)pl->top;
		short *dwal = (short *)pl->bottom;

		for (int x = x1; x < x2; x++)
		{
			int y1 = uwal[x];
			int y2 = dwal[x];
			if (y2 <= y1)
				continue;

			DrawSkyColumn(x, y1, y2);
		}
	}
}