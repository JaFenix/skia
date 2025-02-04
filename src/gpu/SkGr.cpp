/*
 * Copyright 2010 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "SkGr.h"
#include "SkGrPriv.h"

#include "GrBitmapTextureMaker.h"
#include "GrCaps.h"
#include "GrContext.h"
#include "GrGpuResourcePriv.h"
#include "GrRenderTargetContext.h"
#include "GrTexturePriv.h"
#include "GrTextureProvider.h"
#include "GrTextureProxy.h"
#include "GrTypes.h"
#include "GrXferProcessor.h"

#include "SkAutoMalloc.h"
#include "SkBlendModePriv.h"
#include "SkCanvas.h"
#include "SkColorFilter.h"
#include "SkConvertPixels.h"
#include "SkData.h"
#include "SkImageInfoPriv.h"
#include "SkMaskFilter.h"
#include "SkMessageBus.h"
#include "SkMipMap.h"
#include "SkPM4fPriv.h"
#include "SkPixelRef.h"
#include "SkResourceCache.h"
#include "SkTemplates.h"
#include "effects/GrBicubicEffect.h"
#include "effects/GrConstColorProcessor.h"
#include "effects/GrDitherEffect.h"
#include "effects/GrPorterDuffXferProcessor.h"
#include "effects/GrXfermodeFragmentProcessor.h"

#ifndef SK_IGNORE_ETC1_SUPPORT
#  include "etc1.h"
#endif

GrSurfaceDesc GrImageInfoToSurfaceDesc(const SkImageInfo& info, const GrCaps& caps) {
    GrSurfaceDesc desc;
    desc.fFlags = kNone_GrSurfaceFlags;
    desc.fWidth = info.width();
    desc.fHeight = info.height();
    desc.fConfig = SkImageInfo2GrPixelConfig(info, caps);
    desc.fSampleCnt = 0;
    return desc;
}

void GrMakeKeyFromImageID(GrUniqueKey* key, uint32_t imageID, const SkIRect& imageBounds) {
    SkASSERT(key);
    SkASSERT(imageID);
    SkASSERT(!imageBounds.isEmpty());
    static const GrUniqueKey::Domain kImageIDDomain = GrUniqueKey::GenerateDomain();
    GrUniqueKey::Builder builder(key, kImageIDDomain, 5);
    builder[0] = imageID;
    builder[1] = imageBounds.fLeft;
    builder[2] = imageBounds.fTop;
    builder[3] = imageBounds.fRight;
    builder[4] = imageBounds.fBottom;
}

GrPixelConfig GrIsCompressedTextureDataSupported(GrContext* ctx, SkData* data,
                                                 int expectedW, int expectedH,
                                                 const void** outStartOfDataToUpload) {
    *outStartOfDataToUpload = nullptr;
#ifndef SK_IGNORE_ETC1_SUPPORT
    if (!ctx->caps()->isConfigTexturable(kETC1_GrPixelConfig)) {
        return kUnknown_GrPixelConfig;
    }

    const uint8_t* bytes = data->bytes();
    if (data->size() > ETC_PKM_HEADER_SIZE && etc1_pkm_is_valid(bytes)) {
        // Does the data match the dimensions of the bitmap? If not,
        // then we don't know how to scale the image to match it...
        if (etc1_pkm_get_width(bytes) != (unsigned)expectedW ||
            etc1_pkm_get_height(bytes) != (unsigned)expectedH)
        {
            return kUnknown_GrPixelConfig;
        }

        *outStartOfDataToUpload = bytes + ETC_PKM_HEADER_SIZE;
        return kETC1_GrPixelConfig;
    }
#endif
    return kUnknown_GrPixelConfig;
}

//////////////////////////////////////////////////////////////////////////////

GrTexture* GrUploadBitmapToTexture(GrContext* ctx, const SkBitmap& bitmap) {
    SkAutoLockPixels alp(bitmap);
    if (!bitmap.readyToDraw()) {
        return nullptr;
    }
    SkPixmap pixmap;
    if (!bitmap.peekPixels(&pixmap)) {
        return nullptr;
    }
    return GrUploadPixmapToTexture(ctx, pixmap, SkBudgeted::kYes);
}

GrTexture* GrUploadPixmapToTexture(GrContext* ctx, const SkPixmap& pixmap, SkBudgeted budgeted) {
    const SkPixmap* pmap = &pixmap;
    SkPixmap tmpPixmap;
    SkBitmap tmpBitmap;

    if (!SkImageInfoIsValid(pixmap.info())) {
        return nullptr;
    }

    const GrCaps* caps = ctx->caps();
    GrSurfaceDesc desc = GrImageInfoToSurfaceDesc(pixmap.info(), *caps);

    // TODO: We're checking for srgbSupport, but we can then end up picking sBGRA as our pixel
    // config (which may not be supported). We need better fallback management here.

    if (caps->srgbSupport() &&
        pixmap.info().colorSpace() && pixmap.info().colorSpace()->gammaCloseToSRGB() &&
        !GrPixelConfigIsSRGB(desc.fConfig)) {
        // We were supplied an sRGB-like color space, but we don't have a suitable pixel config.
        // Convert to 8888 sRGB so we can handle the data correctly. The raster backend doesn't
        // handle sRGB Index8 -> sRGB 8888 correctly (yet), so lie about both the source and
        // destination (claim they're linear):
        SkImageInfo linSrcInfo = SkImageInfo::Make(pixmap.width(), pixmap.height(),
                                                   pixmap.colorType(), pixmap.alphaType());
        SkPixmap linSrcPixmap(linSrcInfo, pixmap.addr(), pixmap.rowBytes(), pixmap.ctable());

        SkImageInfo dstInfo = SkImageInfo::Make(pixmap.width(), pixmap.height(),
                                                kN32_SkColorType, kPremul_SkAlphaType,
                                                pixmap.info().refColorSpace());

        tmpBitmap.allocPixels(dstInfo);

        SkImageInfo linDstInfo = SkImageInfo::MakeN32Premul(pixmap.width(), pixmap.height());
        if (!linSrcPixmap.readPixels(linDstInfo, tmpBitmap.getPixels(), tmpBitmap.rowBytes())) {
            return nullptr;
        }
        if (!tmpBitmap.peekPixels(&tmpPixmap)) {
            return nullptr;
        }
        pmap = &tmpPixmap;
        // must rebuild desc, since we've forced the info to be N32
        desc = GrImageInfoToSurfaceDesc(pmap->info(), *caps);
    } else if (kIndex_8_SkColorType == pixmap.colorType()) {
        SkImageInfo info = SkImageInfo::MakeN32Premul(pixmap.width(), pixmap.height());
        tmpBitmap.allocPixels(info);
        if (!pixmap.readPixels(info, tmpBitmap.getPixels(), tmpBitmap.rowBytes())) {
            return nullptr;
        }
        if (!tmpBitmap.peekPixels(&tmpPixmap)) {
            return nullptr;
        }
        pmap = &tmpPixmap;
        // must rebuild desc, since we've forced the info to be N32
        desc = GrImageInfoToSurfaceDesc(pmap->info(), *caps);
    }

    return ctx->textureProvider()->createTexture(desc, budgeted, pmap->addr(),
                                                 pmap->rowBytes());
}


////////////////////////////////////////////////////////////////////////////////

void GrInstallBitmapUniqueKeyInvalidator(const GrUniqueKey& key, SkPixelRef* pixelRef) {
    class Invalidator : public SkPixelRef::GenIDChangeListener {
    public:
        explicit Invalidator(const GrUniqueKey& key) : fMsg(key) {}
    private:
        GrUniqueKeyInvalidatedMessage fMsg;

        void onChange() override { SkMessageBus<GrUniqueKeyInvalidatedMessage>::Post(fMsg); }
    };

    pixelRef->addGenIDChangeListener(new Invalidator(key));
}

GrTexture* GrGenerateMipMapsAndUploadToTexture(GrContext* ctx, const SkBitmap& bitmap,
                                               SkColorSpace* dstColorSpace)
{
    SkDestinationSurfaceColorMode colorMode = dstColorSpace
        ? SkDestinationSurfaceColorMode::kGammaAndColorSpaceAware
        : SkDestinationSurfaceColorMode::kLegacy;

    if (!SkImageInfoIsValid(bitmap.info())) {
        return nullptr;
    }

    GrSurfaceDesc desc = GrImageInfoToSurfaceDesc(bitmap.info(), *ctx->caps());

    // We don't support Gray8 directly in the GL backend, so fail-over to GrUploadBitmapToTexture.
    // That will transform the Gray8 to 8888, then use the driver/GPU to build mipmaps. If we build
    // the mips on the CPU here, they'll all be Gray8, which isn't useful. (They get treated as A8).
    // TODO: A better option might be to transform the initial bitmap here to 8888, then run the
    // CPU mip-mapper on that data before uploading. This is much less code for a rare case though:
    if (kGray_8_SkColorType == bitmap.colorType()) {
        return nullptr;
    }

    SkAutoPixmapUnlock srcUnlocker;
    if (!bitmap.requestLock(&srcUnlocker)) {
        return nullptr;
    }
    const SkPixmap& pixmap = srcUnlocker.pixmap();
    // Try to catch where we might have returned nullptr for src crbug.com/492818
    if (nullptr == pixmap.addr()) {
        sk_throw();
    }

    std::unique_ptr<SkMipMap> mipmaps(SkMipMap::Build(pixmap, colorMode, nullptr));
    if (!mipmaps) {
        return nullptr;
    }

    const int mipLevelCount = mipmaps->countLevels() + 1;
    if (mipLevelCount < 1) {
        return nullptr;
    }

    const bool isMipMapped = mipLevelCount > 1;
    desc.fIsMipMapped = isMipMapped;

    std::unique_ptr<GrMipLevel[]> texels(new GrMipLevel[mipLevelCount]);

    texels[0].fPixels = pixmap.addr();
    texels[0].fRowBytes = pixmap.rowBytes();

    for (int i = 1; i < mipLevelCount; ++i) {
        SkMipMap::Level generatedMipLevel;
        mipmaps->getLevel(i - 1, &generatedMipLevel);
        texels[i].fPixels = generatedMipLevel.fPixmap.addr();
        texels[i].fRowBytes = generatedMipLevel.fPixmap.rowBytes();
    }

    {
        GrTexture* texture = ctx->textureProvider()->createMipMappedTexture(desc,
                                                                            SkBudgeted::kYes,
                                                                            texels.get(),
                                                                            mipLevelCount);
        if (texture) {
            texture->texturePriv().setMipColorMode(colorMode);
        }
        return texture;
    }
}

GrTexture* GrUploadMipMapToTexture(GrContext* ctx, const SkImageInfo& info,
                                   const GrMipLevel* texels, int mipLevelCount) {
    if (!SkImageInfoIsValid(info)) {
        return nullptr;
    }

    const GrCaps* caps = ctx->caps();
    return ctx->textureProvider()->createMipMappedTexture(GrImageInfoToSurfaceDesc(info, *caps),
                                                          SkBudgeted::kYes, texels,
                                                          mipLevelCount);
}

GrTexture* GrRefCachedBitmapTexture(GrContext* ctx, const SkBitmap& bitmap,
                                    const GrSamplerParams& params, SkScalar scaleAdjust[2]) {
    // Caller doesn't care about the texture's color space (they can always get it from the bitmap)
    return GrBitmapTextureMaker(ctx, bitmap).refTextureForParams(params, nullptr,
                                                                 nullptr, scaleAdjust);
}

// MDB TODO (caching): For better or for worse, this method currently side-steps the issue of
// caching an uninstantiated proxy via a key.
sk_sp<GrTextureProxy> GrMakeCachedBitmapProxy(GrContext* context, const SkBitmap& bitmap) {
    GrUniqueKey originalKey;

    if (!bitmap.isVolatile()) {
        SkIPoint origin = bitmap.pixelRefOrigin();
        SkIRect subset = SkIRect::MakeXYWH(origin.fX, origin.fY, bitmap.width(), bitmap.height());
        GrMakeKeyFromImageID(&originalKey, bitmap.pixelRef()->getGenerationID(), subset);
    }

    sk_sp<GrTexture> tex;

    if (originalKey.isValid()) {
        tex.reset(context->textureProvider()->findAndRefTextureByUniqueKey(originalKey));
    }
    if (!tex) {
        tex.reset(GrUploadBitmapToTexture(context, bitmap));
        if (tex && originalKey.isValid()) {
            context->textureProvider()->assignUniqueKeyToTexture(originalKey, tex.get());
            GrInstallBitmapUniqueKeyInvalidator(originalKey, bitmap.pixelRef());
        }
    }

    if (!tex) {
        return nullptr;
    }

    sk_sp<GrSurfaceProxy> proxy = GrSurfaceProxy::MakeWrapped(std::move(tex));
    if (!proxy) {
        return nullptr;
    }

    return sk_ref_sp(proxy->asTextureProxy());
}

sk_sp<GrTexture> GrMakeCachedBitmapTexture(GrContext* ctx, const SkBitmap& bitmap,
                                           const GrSamplerParams& params, SkScalar scaleAdjust[2]) {
    // Caller doesn't care about the texture's color space (they can always get it from the bitmap)
    GrTexture* tex = GrBitmapTextureMaker(ctx, bitmap).refTextureForParams(params, nullptr,
                                                                           nullptr, scaleAdjust);
    return sk_sp<GrTexture>(tex);
}

///////////////////////////////////////////////////////////////////////////////

GrColor4f SkColorToPremulGrColor4f(SkColor c, SkColorSpace* dstColorSpace) {
    // We want to premultiply after linearizing, so this is easy:
    return SkColorToUnpremulGrColor4f(c, dstColorSpace).premul();
}

GrColor4f SkColorToUnpremulGrColor4f(SkColor c, SkColorSpace* dstColorSpace) {
    if (dstColorSpace) {
        auto srgbColorSpace = SkColorSpace::MakeSRGB();
        auto gamutXform = GrColorSpaceXform::Make(srgbColorSpace.get(), dstColorSpace);
        return SkColorToUnpremulGrColor4f(c, true, gamutXform.get());
    } else {
        return SkColorToUnpremulGrColor4f(c, false, nullptr);
    }
}

GrColor4f SkColorToPremulGrColor4f(SkColor c, bool gammaCorrect, GrColorSpaceXform* gamutXform) {
    // We want to premultiply after linearizing, so this is easy:
    return SkColorToUnpremulGrColor4f(c, gammaCorrect, gamutXform).premul();
}

GrColor4f SkColorToUnpremulGrColor4f(SkColor c, bool gammaCorrect, GrColorSpaceXform* gamutXform) {
    // You can't be color-space aware in legacy mode
    SkASSERT(gammaCorrect || !gamutXform);

    GrColor4f color;
    if (gammaCorrect) {
        // SkColor4f::FromColor does sRGB -> Linear
        color = GrColor4f::FromSkColor4f(SkColor4f::FromColor(c));
    } else {
        // GrColor4f::FromGrColor just multiplies by 1/255
        color = GrColor4f::FromGrColor(SkColorToUnpremulGrColor(c));
    }

    if (gamutXform) {
        color = gamutXform->apply(color);
    }

    return color;
}

///////////////////////////////////////////////////////////////////////////////

GrPixelConfig SkImageInfo2GrPixelConfig(const SkImageInfo& info, const GrCaps& caps) {
    // We intentionally ignore profile type for non-8888 formats. Anything we can't support
    // in hardware will be expanded to sRGB 8888 in GrUploadPixmapToTexture.
    SkColorSpace* cs = info.colorSpace();
    switch (info.colorType()) {
        case kUnknown_SkColorType:
            return kUnknown_GrPixelConfig;
        case kAlpha_8_SkColorType:
            return kAlpha_8_GrPixelConfig;
        case kRGB_565_SkColorType:
            return kRGB_565_GrPixelConfig;
        case kARGB_4444_SkColorType:
            return kRGBA_4444_GrPixelConfig;
        case kRGBA_8888_SkColorType:
            return (caps.srgbSupport() && cs && cs->gammaCloseToSRGB())
                   ? kSRGBA_8888_GrPixelConfig : kRGBA_8888_GrPixelConfig;
        case kBGRA_8888_SkColorType:
            return (caps.srgbSupport() && cs && cs->gammaCloseToSRGB())
                   ? kSBGRA_8888_GrPixelConfig : kBGRA_8888_GrPixelConfig;
        case kIndex_8_SkColorType:
            return kSkia8888_GrPixelConfig;
        case kGray_8_SkColorType:
            return kGray_8_GrPixelConfig;
        case kRGBA_F16_SkColorType:
            return kRGBA_half_GrPixelConfig;
    }
    SkASSERT(0);    // shouldn't get here
    return kUnknown_GrPixelConfig;
}

bool GrPixelConfigToColorType(GrPixelConfig config, SkColorType* ctOut) {
    SkColorType ct;
    switch (config) {
        case kAlpha_8_GrPixelConfig:
            ct = kAlpha_8_SkColorType;
            break;
        case kGray_8_GrPixelConfig:
            ct = kGray_8_SkColorType;
            break;
        case kRGB_565_GrPixelConfig:
            ct = kRGB_565_SkColorType;
            break;
        case kRGBA_4444_GrPixelConfig:
            ct = kARGB_4444_SkColorType;
            break;
        case kRGBA_8888_GrPixelConfig:
            ct = kRGBA_8888_SkColorType;
            break;
        case kBGRA_8888_GrPixelConfig:
            ct = kBGRA_8888_SkColorType;
            break;
        case kSRGBA_8888_GrPixelConfig:
            ct = kRGBA_8888_SkColorType;
            break;
        case kSBGRA_8888_GrPixelConfig:
            ct = kBGRA_8888_SkColorType;
            break;
        case kRGBA_half_GrPixelConfig:
            ct = kRGBA_F16_SkColorType;
            break;
        default:
            return false;
    }
    if (ctOut) {
        *ctOut = ct;
    }
    return true;
}

GrPixelConfig GrRenderableConfigForColorSpace(const SkColorSpace* colorSpace) {
    if (!colorSpace) {
        return kRGBA_8888_GrPixelConfig;
    } else if (colorSpace->gammaIsLinear()) {
        return kRGBA_half_GrPixelConfig;
    } else if (colorSpace->gammaCloseToSRGB()) {
        return kSRGBA_8888_GrPixelConfig;
    } else {
        SkDEBUGFAIL("No renderable config exists for color space with strange gamma");
        return kUnknown_GrPixelConfig;
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////

static inline bool blend_requires_shader(const SkBlendMode mode, bool primitiveIsSrc) {
    if (primitiveIsSrc) {
        return SkBlendMode::kSrc != mode;
    } else {
        return SkBlendMode::kDst != mode;
    }
}

static inline bool skpaint_to_grpaint_impl(GrContext* context,
                                           GrRenderTargetContext* rtc,
                                           const SkPaint& skPaint,
                                           const SkMatrix& viewM,
                                           sk_sp<GrFragmentProcessor>* shaderProcessor,
                                           SkBlendMode* primColorMode,
                                           bool primitiveIsSrc,
                                           GrPaint* grPaint) {
    grPaint->setAllowSRGBInputs(rtc->isGammaCorrect());

    // Convert SkPaint color to 4f format, including optional linearizing and gamut conversion.
    GrColor4f origColor = SkColorToUnpremulGrColor4f(skPaint.getColor(), rtc->isGammaCorrect(),
                                                     rtc->getColorXformFromSRGB());

    // Setup the initial color considering the shader, the SkPaint color, and the presence or not
    // of per-vertex colors.
    sk_sp<GrFragmentProcessor> shaderFP;
    if (!primColorMode || blend_requires_shader(*primColorMode, primitiveIsSrc)) {
        if (shaderProcessor) {
            shaderFP = *shaderProcessor;
        } else if (const SkShader* shader = skPaint.getShader()) {
            shaderFP = shader->asFragmentProcessor(SkShader::AsFPArgs(context, &viewM, nullptr,
                                                                      skPaint.getFilterQuality(),
                                                                      rtc->getColorSpace()));
            if (!shaderFP) {
                return false;
            }
        }
    }

    // Set this in below cases if the output of the shader/paint-color/paint-alpha/primXfermode is
    // a known constant value. In that case we can simply apply a color filter during this
    // conversion without converting the color filter to a GrFragmentProcessor.
    bool applyColorFilterToPaintColor = false;
    if (shaderFP) {
        if (primColorMode) {
            // There is a blend between the primitive color and the shader color. The shader sees
            // the opaque paint color. The shader's output is blended using the provided mode by
            // the primitive color. The blended color is then modulated by the paint's alpha.

            // The geometry processor will insert the primitive color to start the color chain, so
            // the GrPaint color will be ignored.

            GrColor4f shaderInput = origColor.opaque();
            shaderFP = GrFragmentProcessor::OverrideInput(shaderFP, shaderInput);
            if (primitiveIsSrc) {
                shaderFP = GrXfermodeFragmentProcessor::MakeFromDstProcessor(std::move(shaderFP),
                                                                             *primColorMode);
            } else {
                shaderFP = GrXfermodeFragmentProcessor::MakeFromSrcProcessor(std::move(shaderFP),
                                                                             *primColorMode);
            }
            // The above may return null if compose results in a pass through of the prim color.
            if (shaderFP) {
                grPaint->addColorFragmentProcessor(shaderFP);
            }

            // We can ignore origColor here - alpha is unchanged by gamma
            GrColor paintAlpha = SkColorAlphaToGrColor(skPaint.getColor());
            if (GrColor_WHITE != paintAlpha) {
                // No gamut conversion - paintAlpha is a (linear) alpha value, splatted to all
                // color channels. It's value should be treated as the same in ANY color space.
                grPaint->addColorFragmentProcessor(GrConstColorProcessor::Make(
                    GrColor4f::FromGrColor(paintAlpha),
                    GrConstColorProcessor::kModulateRGBA_InputMode));
            }
        } else {
            // The shader's FP sees the paint unpremul color
            grPaint->setColor4f(origColor);
            grPaint->addColorFragmentProcessor(std::move(shaderFP));
        }
    } else {
        if (primColorMode) {
            // There is a blend between the primitive color and the paint color. The blend considers
            // the opaque paint color. The paint's alpha is applied to the post-blended color.
            sk_sp<GrFragmentProcessor> processor(
                GrConstColorProcessor::Make(origColor.opaque(),
                                            GrConstColorProcessor::kIgnore_InputMode));
            if (primitiveIsSrc) {
                processor = GrXfermodeFragmentProcessor::MakeFromDstProcessor(std::move(processor),
                                                                              *primColorMode);
            } else {
                processor = GrXfermodeFragmentProcessor::MakeFromSrcProcessor(std::move(processor),
                                                                              *primColorMode);
            }
            if (processor) {
                grPaint->addColorFragmentProcessor(std::move(processor));
            }

            grPaint->setColor4f(origColor.opaque());

            // We can ignore origColor here - alpha is unchanged by gamma
            GrColor paintAlpha = SkColorAlphaToGrColor(skPaint.getColor());
            if (GrColor_WHITE != paintAlpha) {
                // No gamut conversion - paintAlpha is a (linear) alpha value, splatted to all
                // color channels. It's value should be treated as the same in ANY color space.
                grPaint->addColorFragmentProcessor(GrConstColorProcessor::Make(
                    GrColor4f::FromGrColor(paintAlpha),
                    GrConstColorProcessor::kModulateRGBA_InputMode));
            }
        } else {
            // No shader, no primitive color.
            grPaint->setColor4f(origColor.premul());
            applyColorFilterToPaintColor = true;
        }
    }

    SkColorFilter* colorFilter = skPaint.getColorFilter();
    if (colorFilter) {
        if (applyColorFilterToPaintColor) {
            // If we're in legacy mode, we *must* avoid using the 4f version of the color filter,
            // because that will combine with the linearized version of the stored color.
            if (rtc->isGammaCorrect()) {
                grPaint->setColor4f(GrColor4f::FromSkColor4f(
                    colorFilter->filterColor4f(origColor.toSkColor4f())).premul());
            } else {
                grPaint->setColor4f(SkColorToPremulGrColor4f(
                    colorFilter->filterColor(skPaint.getColor()), false, nullptr));
            }
        } else {
            sk_sp<GrFragmentProcessor> cfFP(colorFilter->asFragmentProcessor(context,
                                                                             rtc->getColorSpace()));
            if (cfFP) {
                grPaint->addColorFragmentProcessor(std::move(cfFP));
            } else {
                return false;
            }
        }
    }

    SkMaskFilter* maskFilter = skPaint.getMaskFilter();
    if (maskFilter) {
        GrFragmentProcessor* mfFP;
        if (maskFilter->asFragmentProcessor(&mfFP, nullptr, viewM)) {
            grPaint->addCoverageFragmentProcessor(sk_sp<GrFragmentProcessor>(mfFP));
        }
    }

    // When the xfermode is null on the SkPaint (meaning kSrcOver) we need the XPFactory field on
    // the GrPaint to also be null (also kSrcOver).
    SkASSERT(!grPaint->getXPFactory());
    if (!skPaint.isSrcOver()) {
        grPaint->setXPFactory(SkBlendMode_AsXPFactory(skPaint.getBlendMode()));
    }

#ifndef SK_IGNORE_GPU_DITHER
    if (skPaint.isDither() && grPaint->numColorFragmentProcessors() > 0 && !rtc->isGammaCorrect()) {
        grPaint->addColorFragmentProcessor(GrDitherEffect::Make());
    }
#endif
    return true;
}

bool SkPaintToGrPaint(GrContext* context, GrRenderTargetContext* rtc, const SkPaint& skPaint,
                      const SkMatrix& viewM, GrPaint* grPaint) {
    return skpaint_to_grpaint_impl(context, rtc, skPaint, viewM, nullptr, nullptr, false, grPaint);
}

/** Replaces the SkShader (if any) on skPaint with the passed in GrFragmentProcessor. */
bool SkPaintToGrPaintReplaceShader(GrContext* context,
                                   GrRenderTargetContext* rtc,
                                   const SkPaint& skPaint,
                                   sk_sp<GrFragmentProcessor> shaderFP,
                                   GrPaint* grPaint) {
    if (!shaderFP) {
        return false;
    }
    return skpaint_to_grpaint_impl(context, rtc, skPaint, SkMatrix::I(), &shaderFP, nullptr, false,
                                   grPaint);
}

/** Ignores the SkShader (if any) on skPaint. */
bool SkPaintToGrPaintNoShader(GrContext* context,
                              GrRenderTargetContext* rtc,
                              const SkPaint& skPaint,
                              GrPaint* grPaint) {
    // Use a ptr to a nullptr to to indicate that the SkShader is ignored and not replaced.
    static sk_sp<GrFragmentProcessor> kNullShaderFP(nullptr);
    static sk_sp<GrFragmentProcessor>* kIgnoreShader = &kNullShaderFP;
    return skpaint_to_grpaint_impl(context, rtc, skPaint, SkMatrix::I(), kIgnoreShader, nullptr,
                                   false, grPaint);
}

/** Blends the SkPaint's shader (or color if no shader) with a per-primitive color which must
be setup as a vertex attribute using the specified SkBlendMode. */
bool SkPaintToGrPaintWithXfermode(GrContext* context,
                                  GrRenderTargetContext* rtc,
                                  const SkPaint& skPaint,
                                  const SkMatrix& viewM,
                                  SkBlendMode primColorMode,
                                  bool primitiveIsSrc,
                                  GrPaint* grPaint) {
    return skpaint_to_grpaint_impl(context, rtc, skPaint, viewM, nullptr, &primColorMode,
                                   primitiveIsSrc, grPaint);
}

bool SkPaintToGrPaintWithTexture(GrContext* context,
                                 GrRenderTargetContext* rtc,
                                 const SkPaint& paint,
                                 const SkMatrix& viewM,
                                 sk_sp<GrFragmentProcessor> fp,
                                 bool textureIsAlphaOnly,
                                 GrPaint* grPaint) {
    sk_sp<GrFragmentProcessor> shaderFP;
    if (textureIsAlphaOnly) {
        if (const SkShader* shader = paint.getShader()) {
            shaderFP = shader->asFragmentProcessor(SkShader::AsFPArgs(context,
                                                                      &viewM,
                                                                      nullptr,
                                                                      paint.getFilterQuality(),
                                                                      rtc->getColorSpace()));
            if (!shaderFP) {
                return false;
            }
            sk_sp<GrFragmentProcessor> fpSeries[] = { std::move(shaderFP), std::move(fp) };
            shaderFP = GrFragmentProcessor::RunInSeries(fpSeries, 2);
        } else {
            shaderFP = GrFragmentProcessor::MakeInputPremulAndMulByOutput(fp);
        }
    } else {
        shaderFP = GrFragmentProcessor::MulOutputByInputAlpha(fp);
    }

    return SkPaintToGrPaintReplaceShader(context, rtc, paint, std::move(shaderFP), grPaint);
}


////////////////////////////////////////////////////////////////////////////////////////////////

GrSamplerParams::FilterMode GrSkFilterQualityToGrFilterMode(SkFilterQuality paintFilterQuality,
                                                            const SkMatrix& viewM,
                                                            const SkMatrix& localM,
                                                            bool* doBicubic) {
    *doBicubic = false;
    GrSamplerParams::FilterMode textureFilterMode;
    switch (paintFilterQuality) {
        case kNone_SkFilterQuality:
            textureFilterMode = GrSamplerParams::kNone_FilterMode;
            break;
        case kLow_SkFilterQuality:
            textureFilterMode = GrSamplerParams::kBilerp_FilterMode;
            break;
        case kMedium_SkFilterQuality: {
            SkMatrix matrix;
            matrix.setConcat(viewM, localM);
            if (matrix.getMinScale() < SK_Scalar1) {
                textureFilterMode = GrSamplerParams::kMipMap_FilterMode;
            } else {
                // Don't trigger MIP level generation unnecessarily.
                textureFilterMode = GrSamplerParams::kBilerp_FilterMode;
            }
            break;
        }
        case kHigh_SkFilterQuality: {
            SkMatrix matrix;
            matrix.setConcat(viewM, localM);
            *doBicubic = GrBicubicEffect::ShouldUseBicubic(matrix, &textureFilterMode);
            break;
        }
        default:
            // Should be unreachable.  If not, fall back to mipmaps.
            textureFilterMode = GrSamplerParams::kMipMap_FilterMode;
            break;

    }
    return textureFilterMode;
}
