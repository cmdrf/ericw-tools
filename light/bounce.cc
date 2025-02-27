/*  Copyright (C) 1996-1997  Id Software, Inc.
    Copyright (C) 2017 Eric Wasylishen

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

    See file, 'COPYING', for details.
*/

#include <light/bounce.hh>

#include <cstdint>
#include <atomic>

#include <light/light.hh>
#include <light/entities.hh> // for EstimateVisibleBoundsAtPoint
#include <light/ltface.hh>
#include <light/surflight.hh>
#include <light/trace.hh> // for Light_PointInLeaf

#include <common/polylib.hh>
#include <common/bsputils.hh>

#include <vector>
#include <unordered_map>
#include <mutex>

#include <common/qvec.hh>
#include <common/parallel.hh>

using namespace std;
using namespace polylib;

static std::atomic_size_t bouncelightpoints;

void ResetBounce()
{
    bouncelightpoints = 0;
}

static bool Face_ShouldBounce(const mbsp_t *bsp, const mface_t *face)
{
    // make bounce light, only if this face is shadow casting
    const modelinfo_t *mi = ModelInfoForFace(bsp, Face_GetNum(bsp, face));
    if (!mi || !mi->shadow.boolValue()) {
        return false;
    }

    if (!Face_IsLightmapped(bsp, face)) {
        return false;
    }

    const char *texname = Face_TextureName(bsp, face);
    if (!Q_strcasecmp("skip", texname)) {
        return false;
    }

    // check for "_bounce" "-1"
    const auto &ext_info = extended_texinfo_flags[face->texinfo];
    if (ext_info.no_bounce) {
        return false;
    }

    // don't bounce *from* emission surfaces
    if (IsSurfaceLitFace(bsp, face)) {
        return false;
    }

    // don't bounce from faces on non-default object channels
    if (mi->object_channel_mask.value() != CHANNEL_MASK_DEFAULT) {
        return false;
    }
    if (ext_info.object_channel_mask.value_or(CHANNEL_MASK_DEFAULT) != CHANNEL_MASK_DEFAULT) {
        return false;
    }

    return true;
}

static void MakeBounceLight(const mbsp_t *bsp, const settings::worldspawn_keys &cfg, lightsurf_t &surf,
    qvec3d texture_color, int32_t style, const std::vector<qvec3f> &points, const winding_t &winding, const vec_t &area,
    const qvec3d &facenormal, const qvec3d &facemidpoint)
{
    if (!Face_IsEmissive(bsp, surf.face)) {
        return;
    }

    bouncelightpoints += points.size();

    // Calculate emit color and intensity...

    // Calculate intensity...
    vec_t intensity = qv::max(texture_color);

    if (intensity <= 0.0) {
        return;
    }

    if (!surf.vpl) {
        auto &l = surf.vpl = std::make_unique<surfacelight_t>();

        // Normalize color...
        if (intensity > 1.0) {
            texture_color *= 1.0 / intensity;
        }

        // Sanity checks...
        Q_assert(!points.empty());

        // Add surfacelight...
        l->surfnormal = facenormal;
        l->points = points;

        // Init bbox...
        if (light_options.visapprox.value() == visapprox_t::RAYS) {
            l->bounds = EstimateVisibleBoundsAtPoint(facemidpoint);
        }

        for (auto &pt : l->points) {
            if (light_options.visapprox.value() == visapprox_t::VIS) {
                l->leaves.push_back(Light_PointInLeaf(bsp, pt));
            } else if (light_options.visapprox.value() == visapprox_t::RAYS) {
                l->bounds += EstimateVisibleBoundsAtPoint(pt);
            }
        }

        l->pos = facemidpoint;
    }

    // Store surfacelight settings...
    {
        auto &l = surf.vpl;
        auto &setting = l->styles.emplace_back();
        setting.bounce = true;
        setting.style = style;
        setting.totalintensity = intensity * area;
        setting.intensity = setting.totalintensity / l->points.size();
        setting.color = texture_color;
    }
}

static void MakeBounceLightsThread(const settings::worldspawn_keys &cfg, const mbsp_t *bsp, const mface_t &face)
{
    if (!Face_ShouldBounce(bsp, &face)) {
        return;
    }

    auto &surf_ptr = LightSurfaces()[&face - bsp->dfaces.data()];

    if (!surf_ptr) {
        return;
    }

    auto &surf = *surf_ptr.get();

    // no lights
    if (!surf.lightmapsByStyle.size()) {
        return;
    }

    winding_t winding = winding_t::from_face(bsp, &face);
    vec_t area = winding.area();

    if (area < 1.f) {
        return;
    }

    // Create winding...
    winding.remove_colinear();

    // grab the average color across the whole set of lightmaps for this face.
    // this doesn't change regardless of the above settings.
    std::unordered_map<int, qvec3d> sum;
    vec_t sample_divisor = surf.lightmapsByStyle.front().samples.size();

    bool has_any_color = false;

    for (const auto &lightmap : surf.lightmapsByStyle) {

        if (lightmap.style && !cfg.bouncestyled.value()) {
            continue;
        }

        for (auto &sample : lightmap.samples) {
            sum[lightmap.style] += sample.color;
        }
    }

    for (auto &sample : sum) {
        if (!qv::emptyExact(sample.second)) {
            sample.second /= sample_divisor;
            has_any_color = true;
        }
    }

    // no bounced color, we can leave early
    if (!has_any_color) {
        return;
    }

    // lerp between gray and the texture color according to `bouncecolorscale` (0 = use gray, 1 = use texture color)
    const qvec3d &blendedcolor = Face_LookupTextureBounceColor(bsp, &face);

    // final colors to emit
    std::unordered_map<int, qvec3d> emitcolors;

    for (const auto &styleColor : sum) {
        emitcolors[styleColor.first] = styleColor.second * blendedcolor;
    }

    qplane3d faceplane = winding.plane();

    // Get face normal and midpoint...
    qvec3d facenormal = faceplane.normal;
    qvec3d facemidpoint = winding.center() + facenormal; // Lift 1 unit

    vector<qvec3f> points;

    if (light_options.emissivequality.value() == emissivequality_t::LOW ||
        light_options.emissivequality.value() == emissivequality_t::MEDIUM) {
        points = {facemidpoint};

        if (light_options.emissivequality.value() == emissivequality_t::MEDIUM) {

            for (auto &pt : winding) {
                points.push_back(pt + faceplane.normal);
            }
        }
    } else {
        winding.dice(cfg.bouncelightsubdivision.value(),
            [&points, &faceplane](winding_t &w) { points.push_back(w.center() + faceplane.normal); });
    }

    for (auto &style : emitcolors) {
        MakeBounceLight(bsp, cfg, surf, style.second, style.first, points, winding, area, facenormal, facemidpoint);
    }
}

void MakeBounceLights(const settings::worldspawn_keys &cfg, const mbsp_t *bsp)
{
    logging::funcheader();

    logging::parallel_for_each(bsp->dfaces, [&](const mface_t &face) { MakeBounceLightsThread(cfg, bsp, face); });

    // logging::print("{} bounce lights created, with {} points\n", bouncelights.size(), bouncelightpoints);
}
