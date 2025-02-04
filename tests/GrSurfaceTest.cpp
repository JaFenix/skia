/*
 * Copyright 2013 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "SkTypes.h"

#if SK_SUPPORT_GPU

#include "GrContext.h"
#include "GrGpu.h"
#include "GrRenderTarget.h"
#include "GrTexture.h"
#include "GrTextureProvider.h"
#include "GrSurfacePriv.h"
#include "Test.h"

// Tests that GrSurface::asTexture(), GrSurface::asRenderTarget(), and static upcasting of texture
// and render targets to GrSurface all work as expected.
DEF_GPUTEST_FOR_NULLGL_CONTEXT(GrSurface, reporter, ctxInfo) {
    GrContext* context = ctxInfo.grContext();
    GrSurfaceDesc desc;
    desc.fConfig = kRGBA_8888_GrPixelConfig;
    desc.fFlags = kRenderTarget_GrSurfaceFlag;
    desc.fWidth = 256;
    desc.fHeight = 256;
    desc.fSampleCnt = 0;
    GrSurface* texRT1 = context->textureProvider()->createTexture(
            desc, SkBudgeted::kNo, nullptr, 0);

    REPORTER_ASSERT(reporter, texRT1 == texRT1->asRenderTarget());
    REPORTER_ASSERT(reporter, texRT1 == texRT1->asTexture());
    REPORTER_ASSERT(reporter, static_cast<GrSurface*>(texRT1->asRenderTarget()) ==
                    texRT1->asTexture());
    REPORTER_ASSERT(reporter, texRT1->asRenderTarget() ==
                    static_cast<GrSurface*>(texRT1->asTexture()));
    REPORTER_ASSERT(reporter, static_cast<GrSurface*>(texRT1->asRenderTarget()) ==
                    static_cast<GrSurface*>(texRT1->asTexture()));

    desc.fFlags = kNone_GrSurfaceFlags;
    GrSurface* tex1 = context->textureProvider()->createTexture(desc, SkBudgeted::kNo, nullptr, 0);
    REPORTER_ASSERT(reporter, nullptr == tex1->asRenderTarget());
    REPORTER_ASSERT(reporter, tex1 == tex1->asTexture());
    REPORTER_ASSERT(reporter, static_cast<GrSurface*>(tex1) == tex1->asTexture());

    GrBackendObject backendTex = context->getGpu()->createTestingOnlyBackendTexture(
        nullptr, 256, 256, kRGBA_8888_GrPixelConfig);

    GrBackendTextureDesc backendDesc;
    backendDesc.fConfig = kRGBA_8888_GrPixelConfig;
    backendDesc.fFlags = kRenderTarget_GrBackendTextureFlag;
    backendDesc.fWidth = 256;
    backendDesc.fHeight = 256;
    backendDesc.fSampleCnt = 0;
    backendDesc.fTextureHandle = backendTex;
    sk_sp<GrSurface> texRT2 = context->textureProvider()->wrapBackendTexture(
        backendDesc, kBorrow_GrWrapOwnership);
    REPORTER_ASSERT(reporter, texRT2.get() == texRT2->asRenderTarget());
    REPORTER_ASSERT(reporter, texRT2.get() == texRT2->asTexture());
    REPORTER_ASSERT(reporter, static_cast<GrSurface*>(texRT2->asRenderTarget()) ==
                    texRT2->asTexture());
    REPORTER_ASSERT(reporter, texRT2->asRenderTarget() ==
                    static_cast<GrSurface*>(texRT2->asTexture()));
    REPORTER_ASSERT(reporter, static_cast<GrSurface*>(texRT2->asRenderTarget()) ==
                    static_cast<GrSurface*>(texRT2->asTexture()));

    texRT1->unref();
    tex1->unref();
    context->getGpu()->deleteTestingOnlyBackendTexture(backendTex);
}

#endif
