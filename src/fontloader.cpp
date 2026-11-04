// ImGui's glyph loader, over FreeType, with each glyph's bitmap padded
//
// ImGui draws a glyph as one quad over the rectangle its ink fills, so a
// quad edge that falls between pixel centres covers a row or column only
// partly, and the rasterizer drops it rather than averaging it: a row off
// the foot of letters whose ink ends halfway down a pixel. Here every glyph
// is rasterized as ImGui's own FreeType loader does, into a bitmap grown by
// a display pixel of transparency on each side, and its quad covers all of
// it: an edge between pixel centres then samples that transparency and the
// GPU averages the row, as the rest of the desktop does. The atlas' own
// TexGlyphPadding (1) keeps the padded rectangles from touching.
//
// The rest is the loader ImGui's misc/freetype/imgui_freetype.cpp provides:
// the same load flags from ImFontConfig::FontLoaderFlags (fonts.cpp says
// which), FreeType's synthetic bold and oblique for the faces that ask, and
// color for COLR glyphs. Its loader draws the bitmap without the padding.
//
// This is free and unencumbered software released into the public domain.

#include <math.h>

#include "imgui.h"
#include "imgui_internal.h"
#include "misc/freetype/imgui_freetype.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H
#include FT_GLYPH_H
#include FT_SIZES_H
#include FT_SYNTHESIS_H

// The library the atlas' faces are opened from (atlas->FontLoaderData).
struct PadLib {
    FT_Library lib;
};

// A font source's face and the flags its glyphs are loaded with
// (src->FontLoaderData).
struct PadSrc {
    FT_Face       face;
    ImGuiFreeTypeLoaderFlags flags;
    i32           loadflags;
    ImFontBaked  *last;  // the size last activated for its glyphs
};

// A baked size's FT_Size (the core's per-baked, per-source buffer).
struct PadBaked {
    FT_Size size;
};

static bool padinit(ImFontAtlas *atlas)
{
    if (atlas->FontLoaderData) {
        return 1;  // already up
    }
    PadLib *l = IM_NEW(PadLib)();
    if (FT_Init_FreeType(&l->lib)) {
        IM_DELETE(l);
        return 0;
    }
    atlas->FontLoaderData = l;
    return 1;
}

static void padshutdown(ImFontAtlas *atlas)
{
    PadLib *l = (PadLib *)atlas->FontLoaderData;
    if (!l) {
        return;
    }
    FT_Done_FreeType(l->lib);
    IM_DELETE(l);
    atlas->FontLoaderData = 0;
}

static bool padsrcinit(ImFontAtlas *atlas, ImFontConfig *src)
{
    PadLib *l = (PadLib *)atlas->FontLoaderData;
    FT_Face face = 0;
    if (FT_New_Memory_Face(l->lib, (FT_Byte *)src->FontData, (FT_Long)src->FontDataSize,
                           (FT_Long)src->FontNo, &face))
    {
        return 0;
    }
    if (FT_Select_Charmap(face, FT_ENCODING_UNICODE)) {
        FT_Done_Face(face);
        return 0;
    }
    PadSrc *s      = IM_NEW(PadSrc)();
    s->face        = face;
    s->flags       = (ImGuiFreeTypeLoaderFlags)(src->FontLoaderFlags |
                                                atlas->FontLoaderFlags);
    s->loadflags   = 0;
    if (!(s->flags & ImGuiFreeTypeLoaderFlags_Bitmap)) {
        s->loadflags |= FT_LOAD_NO_BITMAP;
    }
    if (s->flags & ImGuiFreeTypeLoaderFlags_NoHinting) {
        s->loadflags |= FT_LOAD_NO_HINTING;
    }
    if (s->flags & ImGuiFreeTypeLoaderFlags_NoAutoHint) {
        s->loadflags |= FT_LOAD_NO_AUTOHINT;
    }
    if (s->flags & ImGuiFreeTypeLoaderFlags_ForceAutoHint) {
        s->loadflags |= FT_LOAD_FORCE_AUTOHINT;
    }
    if (s->flags & ImGuiFreeTypeLoaderFlags_LightHinting) {
        s->loadflags |= FT_LOAD_TARGET_LIGHT;
    } else if (s->flags & ImGuiFreeTypeLoaderFlags_MonoHinting) {
        s->loadflags |= FT_LOAD_TARGET_MONO;
    } else {
        s->loadflags |= FT_LOAD_TARGET_NORMAL;
    }
    if (s->flags & ImGuiFreeTypeLoaderFlags_LoadColor) {
        s->loadflags |= FT_LOAD_COLOR;
    }
    src->FontLoaderData = s;
    return 1;
}

static void padsrcdestroy(ImFontAtlas *, ImFontConfig *src)
{
    PadSrc *s = (PadSrc *)src->FontLoaderData;
    if (!s) {
        return;
    }
    FT_Done_Face(s->face);
    IM_DELETE(s);
    src->FontLoaderData = 0;
}

static int padglyphindex(ImFontAtlas *, ImFontConfig *src, ImWchar cp)
{
    PadSrc *s = (PadSrc *)src->FontLoaderData;
    return (int)FT_Get_Char_Index(s->face, cp);
}

static bool padcontains(ImFontAtlas *, ImFontConfig *src, int glyph)
{
    PadSrc *s = (PadSrc *)src->FontLoaderData;
    return glyph > 0 && glyph < s->face->num_glyphs;
}

// A baked size's FT_Size: the same request ImGui's loader makes, a real
// dimension of size times the density, and the ascent and descent it reads
// from it, in display pixels (the first source's, merged sources sharing).
static bool padbakedinit(ImFontAtlas *, ImFontConfig *src, ImFontBaked *baked, void *data)
{
    PadSrc *s = (PadSrc *)src->FontLoaderData;
    f32     size = baked->Size;
    f32     ref  = baked->OwnerFont->Sources[0]->SizePixels;
    if (src->MergeMode && src->SizePixels != 0) {
        size *= src->SizePixels / ref;
    }
    size *= src->ExtraSizeScale;
    f32 density = src->RasterizerDensity * baked->RasterizerDensity;
    PadBaked *b  = (PadBaked *)data;
    memset((void *)b, 0, sizeof(*b));    if (FT_New_Size(s->face, &b->size) ||
        FT_Activate_Size(b->size))
    {
        return 0;
    }
    s->last = baked;  // it is active now, as padglyph assumes
    FT_Size_RequestRec req;
    req.type            = (s->flags & ImGuiFreeTypeLoaderFlags_Bitmap)
                          ? FT_SIZE_REQUEST_TYPE_NOMINAL : FT_SIZE_REQUEST_TYPE_REAL_DIM;
    req.width           = 0;
    req.height          = (FT_UInt)(size * 64 * density);
    req.horiResolution  = 0;
    req.vertResolution  = 0;
    if (FT_Request_Size(s->face, &req)) {
        return 0;
    }
    if (!src->MergeMode) {
        FT_Size_Metrics m = b->size->metrics;
        f32             k = 1.0f / (density * src->ExtraSizeScale);
        baked->Ascent     = roundf((f32)m.ascender  / 64 * k * density) / density;
        baked->Descent    = roundf((f32)m.descender / 64 * k * density) / density;
    }
    return 1;
}

static void padbakeddestroy(ImFontAtlas *, ImFontConfig *src, ImFontBaked *baked, void *data)
{
    PadSrc *s = (PadSrc *)src->FontLoaderData;
    if (s && s->last == baked) {
        s->last = 0;  // else a later baked at this address skips activation
    }
    PadBaked *b = (PadBaked *)data;
    if (b->size) {
        FT_Done_Size(b->size);
        b->size = 0;
    }
}

// A glyph's bitmap as ImGui's loader blits it, into a buffer grown by pad
// transparent pixels on each side and pitch wide.
static void padblit(FT_Bitmap const *bm, u32 *dst, i32 pitch, i32 pad)
{
    i32      w   = (i32)bm->width;
    i32      h   = (i32)bm->rows;
    u8 const *s  = bm->buffer;
    i32      sp  = bm->pitch;
    u32     *row = dst + pad*pitch + pad;
    switch (bm->pixel_mode) {
    case FT_PIXEL_MODE_GRAY:
        for (i32 y = 0; y < h; y++, s += sp, row += pitch)
            for (i32 x = 0; x < w; x++)
                row[x] = IM_COL32(255, 255, 255, s[x]);
        break;
    case FT_PIXEL_MODE_MONO:
        for (i32 y = 0; y < h; y++, s += sp, row += pitch) {
            u8 const *bits = s;
            for (i32 x = 0; x < w; x++) {
                if ((x & 7) == 0 && x) {
                    bits++;
                }
                row[x] = IM_COL32(255, 255, 255, (*bits<<(x&7)) & 0x80 ? 255 : 0);
            }
        }
        break;
    case FT_PIXEL_MODE_BGRA:
        for (i32 y = 0; y < h; y++, s += sp, row += pitch)
            for (i32 x = 0; x < w; x++) {
                u8 const *p = s + x*4;
                u8        a = p[3];
                u32       r = a ? (255u*p[2] + a/2)/a : 0;
                u32       g = a ? (255u*p[1] + a/2)/a : 0;
                u32       b = a ? (255u*p[0] + a/2)/a : 0;
                row[x] = IM_COL32(r>255?255:r, g>255?255:g, b>255?255:b, a);
            }
        break;
    default:
        break;
    }
}

static bool padglyph(ImFontAtlas *atlas, ImFontConfig *src, ImFontBaked *baked, void *data,
                     int glyph, ImFontGlyph *out, float *outadvance)
{
    PadSrc *s = (PadSrc *)src->FontLoaderData;
    if (s->last != baked) {
        FT_Activate_Size(((PadBaked *)data)->size);
        s->last = baked;
    }
    IM_ASSERT(glyph != 0);
    if (FT_Load_Glyph(s->face, (FT_UInt)glyph, s->loadflags)) {
        return 0;
    }
    // Synthetic bold and oblique before the advance is read, so that the
    // advance and the bitmap agree, as ImGui's own loader does them here.
    FT_GlyphSlot slot = s->face->glyph;
    if (s->flags & ImGuiFreeTypeLoaderFlags_Bold) {
        FT_GlyphSlot_Embolden(slot);
    }
    if (s->flags & ImGuiFreeTypeLoaderFlags_Oblique) {
        FT_GlyphSlot_Oblique(slot);
    }
    f32          dens = src->RasterizerDensity * baked->RasterizerDensity;
    f32          adv  = (f32)slot->advance.x / 64 / dens;
    if (outadvance) {
        *outadvance = adv;
        return 1;
    }
    FT_Render_Mode mode = (s->flags & ImGuiFreeTypeLoaderFlags_Monochrome)
                          ? FT_RENDER_MODE_MONO : FT_RENDER_MODE_NORMAL;
    if (FT_Render_Glyph(slot, mode)) {
        return 0;
    }
    FT_Bitmap const *bm = &slot->bitmap;
    out->AdvanceX       = adv;
    if (bm->width && bm->rows) {
        // A display pixel of transparency each side: with the quad drawn
        // over all of it, an edge that falls between pixel centres is
        // averaged in rather than dropped.
        i32 pad = (i32)ceilf(dens);
        if (pad < 1) {
            pad = 1;
        }
        i32 W = (i32)bm->width + 2*pad;
        i32 H = (i32)bm->rows  + 2*pad;
        ImFontAtlasRectId id = ImFontAtlasPackAddRect(atlas, W, H);
        if (id == ImFontAtlasRectId_Invalid) {
            return 0;
        }
        ImTextureRect *r = ImFontAtlasPackGetRect(atlas, id);
        ImVector<unsigned char> &tmp = atlas->Builder->TempBuffer;
        tmp.resize(W*H*4);
        memset(tmp.Data, 0, (uz)(W*H*4));
        padblit(bm, (u32 *)tmp.Data, W, pad);

        f32 offs = baked->OwnerFont->Sources[0]->SizePixels != 0
                   ? baked->Size / baked->OwnerFont->Sources[0]->SizePixels : 1;
        f32 ox   = floorf(src->GlyphOffset.x * offs + 0.5f);
        f32 oy   = floorf(src->GlyphOffset.y * offs + 0.5f) + baked->Ascent;
        f32 inv  = 1.0f / dens;
        f32 fpad = (f32)pad;
        f32 x    = ((f32)slot->bitmap_left - fpad) * inv + ox;
        f32 y    = (-(f32)slot->bitmap_top - fpad) * inv + oy;
        out->X0      = x;
        out->Y0      = y;
        out->X1      = x + (f32)W * inv;
        out->Y1      = y + (f32)H * inv;
        out->Visible = 1;
        out->Colored = bm->pixel_mode == FT_PIXEL_MODE_BGRA;
        out->PackId  = id;
        ImFontAtlasBakedSetFontGlyphBitmap(atlas, baked, src, out, r, tmp.Data,
                                           ImTextureFormat_RGBA32, W*4);
    }
    return 1;
}

// The loader, filled once: the atlas takes a pointer to it and keeps it.
static ImFontLoader const *paddedfreetype()
{
    static ImFontLoader loader;
    static b32          ready;
    if (!ready) {
        ready                = 1;
        loader.Name          = "FreeType padded";
        loader.LoaderInit    = padinit;
        loader.LoaderShutdown= padshutdown;
        loader.FontSrcInit   = padsrcinit;
        loader.FontSrcDestroy= padsrcdestroy;
        loader.FontSrcGetGlyphIndexFromCodepoint = padglyphindex;
        loader.FontSrcContainsGlyph = padcontains;
        loader.FontBakedInit = padbakedinit;
        loader.FontBakedDestroy = padbakeddestroy;
        loader.FontBakedLoadGlyph = padglyph;
        loader.FontBakedSrcLoaderDataSize = sizeof(PadBaked);
    }
    return &loader;
}
