/*
    Copyright 2011 Google Inc.

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

         http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
 */

#include "GrContext.h"
#include "GrGpu.h"
#include "GrTextureCache.h"
#include "GrTextStrike.h"
#include "GrMemory.h"
#include "GrPathIter.h"
#include "GrClipIterator.h"
#include "GrIndexBuffer.h"
#include "GrInOrderDrawBuffer.h"
#include "GrBufferAllocPool.h"
#include "GrPathRenderer.h"

#define ENABLE_OFFSCREEN_AA 0

#define DEFER_TEXT_RENDERING 1

#define BATCH_RECT_TO_RECT (1 && !GR_STATIC_RECT_VB)

static const size_t MAX_TEXTURE_CACHE_COUNT = 128;
static const size_t MAX_TEXTURE_CACHE_BYTES = 8 * 1024 * 1024;

static const size_t DRAW_BUFFER_VBPOOL_BUFFER_SIZE = 1 << 18;
static const int DRAW_BUFFER_VBPOOL_PREALLOC_BUFFERS = 4;

// We are currently only batching Text and drawRectToRect, both
// of which use the quad index buffer.
static const size_t DRAW_BUFFER_IBPOOL_BUFFER_SIZE = 0;
static const int DRAW_BUFFER_IBPOOL_PREALLOC_BUFFERS = 0;

GrContext* GrContext::Create(GrEngine engine,
                             GrPlatform3DContext context3D) {
    GrContext* ctx = NULL;
    GrGpu* fGpu = GrGpu::Create(engine, context3D);
    if (NULL != fGpu) {
        ctx = new GrContext(fGpu);
        fGpu->unref();
    }
    return ctx;
}

GrContext* GrContext::CreateGLShaderContext() {
    return GrContext::Create(kOpenGL_Shaders_GrEngine, NULL);
}

GrContext::~GrContext() {
    this->flush();
    delete fTextureCache;
    delete fFontCache;
    delete fDrawBuffer;
    delete fDrawBufferVBAllocPool;
    delete fDrawBufferIBAllocPool;
    GrSafeUnref(fCustomPathRenderer);
    GrSafeUnref(fAAFillRectIndexBuffer);
    GrSafeUnref(fAAStrokeRectIndexBuffer);
    fGpu->unref();
}

void GrContext::contextLost() {
    // abandon first to so destructors
    // don't try to free the resources in the API.
    fGpu->abandonResources();

    delete fDrawBuffer;
    fDrawBuffer = NULL;

    delete fDrawBufferVBAllocPool;
    fDrawBufferVBAllocPool = NULL;

    delete fDrawBufferIBAllocPool;
    fDrawBufferIBAllocPool = NULL;

    GrSafeSetNull(fAAFillRectIndexBuffer);
    GrSafeSetNull(fAAStrokeRectIndexBuffer);

    fTextureCache->removeAll();
    fFontCache->freeAll();
    fGpu->markContextDirty();

    this->setupDrawBuffer();
}

void GrContext::resetContext() {
    fGpu->markContextDirty();
}

void GrContext::freeGpuResources() {
    this->flush();
    fTextureCache->removeAll();
    fFontCache->freeAll();
}

////////////////////////////////////////////////////////////////////////////////

enum {
    kNPOTBit    = 0x1,
    kFilterBit  = 0x2,
    kKeylessBit = 0x4,
};

bool GrContext::finalizeTextureKey(GrTextureKey* key,
                                   const GrSamplerState& sampler,
                                   bool keyless) const {
    uint32_t bits = 0;
    uint16_t width = key->width();
    uint16_t height = key->height();

    if (!fGpu->npotTextureTileSupport()) {
        bool isPow2 = GrIsPow2(width) && GrIsPow2(height);

        bool tiled = (sampler.getWrapX() != GrSamplerState::kClamp_WrapMode) ||
                     (sampler.getWrapY() != GrSamplerState::kClamp_WrapMode);

        if (tiled && !isPow2) {
            bits |= kNPOTBit;
            if (GrSamplerState::kNearest_Filter != sampler.getFilter()) {
                bits |= kFilterBit;
            }
        }
    }

    if (keyless) {
        bits |= kKeylessBit;
    }
    key->finalize(bits);
    return 0 != bits;
}

GrTextureEntry* GrContext::findAndLockTexture(GrTextureKey* key,
                                              const GrSamplerState& sampler) {
    finalizeTextureKey(key, sampler, false);
    return fTextureCache->findAndLock(*key);
}

static void stretchImage(void* dst,
                         int dstW,
                         int dstH,
                         void* src,
                         int srcW,
                         int srcH,
                         int bpp) {
    GrFixed dx = (srcW << 16) / dstW;
    GrFixed dy = (srcH << 16) / dstH;

    GrFixed y = dy >> 1;

    int dstXLimit = dstW*bpp;
    for (int j = 0; j < dstH; ++j) {
        GrFixed x = dx >> 1;
        void* srcRow = (uint8_t*)src + (y>>16)*srcW*bpp;
        void* dstRow = (uint8_t*)dst + j*dstW*bpp;
        for (int i = 0; i < dstXLimit; i += bpp) {
            memcpy((uint8_t*) dstRow + i,
                   (uint8_t*) srcRow + (x>>16)*bpp,
                   bpp);
            x += dx;
        }
        y += dy;
    }
}

GrTextureEntry* GrContext::createAndLockTexture(GrTextureKey* key,
                                                const GrSamplerState& sampler,
                                                const GrTextureDesc& desc,
                                                void* srcData, size_t rowBytes) {
    GrAssert(key->width() == desc.fWidth);
    GrAssert(key->height() == desc.fHeight);

#if GR_DUMP_TEXTURE_UPLOAD
    GrPrintf("GrContext::createAndLockTexture [%d %d]\n", desc.fWidth, desc.fHeight);
#endif

    GrTextureEntry* entry = NULL;
    bool special = finalizeTextureKey(key, sampler, false);
    if (special) {
        GrTextureEntry* clampEntry;
        GrTextureKey clampKey(*key);
        clampEntry = findAndLockTexture(&clampKey, GrSamplerState::ClampNoFilter());

        if (NULL == clampEntry) {
            clampEntry = createAndLockTexture(&clampKey,
                                              GrSamplerState::ClampNoFilter(),
                                              desc, srcData, rowBytes);
            GrAssert(NULL != clampEntry);
            if (NULL == clampEntry) {
                return NULL;
            }
        }
        GrTextureDesc rtDesc = desc;
        rtDesc.fFlags =  rtDesc.fFlags |
                         kRenderTarget_GrTextureFlagBit |
                         kNoStencil_GrTextureFlagBit;
        rtDesc.fWidth  = GrNextPow2(GrMax<int>(desc.fWidth,
                                               fGpu->minRenderTargetWidth()));
        rtDesc.fHeight = GrNextPow2(GrMax<int>(desc.fHeight,
                                               fGpu->minRenderTargetHeight()));

        GrTexture* texture = fGpu->createTexture(rtDesc, NULL, 0);

        if (NULL != texture) {
            GrDrawTarget::AutoStateRestore asr(fGpu);
            fGpu->setRenderTarget(texture->asRenderTarget());
            fGpu->setTexture(0, clampEntry->texture());
            fGpu->disableStencil();
            fGpu->setViewMatrix(GrMatrix::I());
            fGpu->setAlpha(0xff);
            fGpu->setBlendFunc(kOne_BlendCoeff, kZero_BlendCoeff);
            fGpu->disableState(GrDrawTarget::kDither_StateBit |
                               GrDrawTarget::kClip_StateBit   |
                               GrDrawTarget::kAntialias_StateBit);
            GrSamplerState::Filter filter;
            // if filtering is not desired then we want to ensure all
            // texels in the resampled image are copies of texels from
            // the original.
            if (GrSamplerState::kNearest_Filter == sampler.getFilter()) {
                filter = GrSamplerState::kNearest_Filter;
            } else {
                filter = GrSamplerState::kBilinear_Filter;
            }
            GrSamplerState stretchSampler(GrSamplerState::kClamp_WrapMode,
                                          GrSamplerState::kClamp_WrapMode,
                                          filter);
            fGpu->setSamplerState(0, stretchSampler);

            static const GrVertexLayout layout =
                                GrDrawTarget::StageTexCoordVertexLayoutBit(0,0);
            GrDrawTarget::AutoReleaseGeometry arg(fGpu, layout, 4, 0);

            if (arg.succeeded()) {
                GrPoint* verts = (GrPoint*) arg.vertices();
                verts[0].setIRectFan(0, 0,
                                     texture->width(),
                                     texture->height(),
                                     2*sizeof(GrPoint));
                verts[1].setIRectFan(0, 0, 1, 1, 2*sizeof(GrPoint));
                fGpu->drawNonIndexed(kTriangleFan_PrimitiveType,
                                     0, 4);
                entry = fTextureCache->createAndLock(*key, texture);
            }
            texture->releaseRenderTarget();
        } else {
            // TODO: Our CPU stretch doesn't filter. But we create separate
            // stretched textures when the sampler state is either filtered or
            // not. Either implement filtered stretch blit on CPU or just create
            // one when FBO case fails.

            rtDesc.fFlags = kNone_GrTextureFlags;
            // no longer need to clamp at min RT size.
            rtDesc.fWidth  = GrNextPow2(desc.fWidth);
            rtDesc.fHeight = GrNextPow2(desc.fHeight);
            int bpp = GrBytesPerPixel(desc.fFormat);
            GrAutoSMalloc<128*128*4> stretchedPixels(bpp *
                                                     rtDesc.fWidth *
                                                     rtDesc.fHeight);
            stretchImage(stretchedPixels.get(), rtDesc.fWidth, rtDesc.fHeight,
                         srcData, desc.fWidth, desc.fHeight, bpp);

            size_t stretchedRowBytes = rtDesc.fWidth * bpp;

            GrTexture* texture = fGpu->createTexture(rtDesc,
                                                     stretchedPixels.get(),
                                                     stretchedRowBytes);
            GrAssert(NULL != texture);
            entry = fTextureCache->createAndLock(*key, texture);
        }
        fTextureCache->unlock(clampEntry);

    } else {
        GrTexture* texture = fGpu->createTexture(desc, srcData, rowBytes);
        if (NULL != texture) {
            entry = fTextureCache->createAndLock(*key, texture);
        } else {
            entry = NULL;
        }
    }
    return entry;
}

GrTextureEntry* GrContext::lockKeylessTexture(const GrTextureDesc& desc) {
    uint32_t p0 = desc.fFormat;
    uint32_t p1 = (desc.fAALevel << 16) | desc.fFlags;
    GrTextureKey key(p0, p1, desc.fWidth, desc.fHeight);
    this->finalizeTextureKey(&key, GrSamplerState::ClampNoFilter(), true);
    
    GrTextureEntry* entry = fTextureCache->findAndLock(key);
    if (NULL == entry) {
        GrTexture* texture = fGpu->createTexture(desc, NULL, 0);
        if (NULL != texture) {
            entry = fTextureCache->createAndLock(key, texture);
        }
    }
    // If the caller gives us the same desc/sampler twice we don't want
    // to return the same texture the second time (unless it was previously
    // released). So we detach the entry from the cache and reattach at release.
    if (NULL != entry) {
        fTextureCache->detach(entry);
    }
    return entry;
}

void GrContext::unlockTexture(GrTextureEntry* entry) {
    if (kKeylessBit & entry->key().getPrivateBits()) {
        fTextureCache->reattachAndUnlock(entry);
    } else {
        fTextureCache->unlock(entry);
    }
}

GrTexture* GrContext::createUncachedTexture(const GrTextureDesc& desc,
                                            void* srcData,
                                            size_t rowBytes) {
    return fGpu->createTexture(desc, srcData, rowBytes);
}

void GrContext::getTextureCacheLimits(int* maxTextures,
                                      size_t* maxTextureBytes) const {
    fTextureCache->getLimits(maxTextures, maxTextureBytes);
}

void GrContext::setTextureCacheLimits(int maxTextures, size_t maxTextureBytes) {
    fTextureCache->setLimits(maxTextures, maxTextureBytes);
}

int GrContext::getMaxTextureDimension() {
    return fGpu->maxTextureDimension();
}

///////////////////////////////////////////////////////////////////////////////

GrResource* GrContext::createPlatformSurface(const GrPlatformSurfaceDesc& desc) {
    // validate flags here so that GrGpu subclasses don't have to check
    if (kTexture_GrPlatformSurfaceType == desc.fSurfaceType &&
        0 != desc.fRenderTargetFlags) {
            return NULL;
    }
    if (!(kIsMultisampled_GrPlatformRenderTargetFlagBit & desc.fRenderTargetFlags) &&
        (kGrCanResolve_GrPlatformRenderTargetFlagBit & desc.fRenderTargetFlags)) {
            return NULL;
    }
    if (kTextureRenderTarget_GrPlatformSurfaceType == desc.fSurfaceType &&
        (kIsMultisampled_GrPlatformRenderTargetFlagBit & desc.fRenderTargetFlags) &&
        !(kGrCanResolve_GrPlatformRenderTargetFlagBit & desc.fRenderTargetFlags)) {
        return NULL;
    }
    return fGpu->createPlatformSurface(desc);
}

GrRenderTarget* GrContext::createPlatformRenderTarget(intptr_t platformRenderTarget,
                                                      int stencilBits,
                                                      bool isMultisampled,
                                                      int width, int height) {
#if GR_DEBUG
    GrPrintf("Using deprecated createPlatformRenderTarget API.");
#endif
    return fGpu->createPlatformRenderTarget(platformRenderTarget, 
                                            stencilBits, isMultisampled, 
                                            width, height);
}

GrRenderTarget* GrContext::createRenderTargetFrom3DApiState() {
#if GR_DEBUG
    GrPrintf("Using deprecated createRenderTargetFrom3DApiState API.");
#endif
    return fGpu->createRenderTargetFrom3DApiState();
}

///////////////////////////////////////////////////////////////////////////////

bool GrContext::supportsIndex8PixelConfig(const GrSamplerState& sampler,
                                          int width, int height) {
    if (!fGpu->supports8BitPalette()) {
        return false;
    }


    bool isPow2 = GrIsPow2(width) && GrIsPow2(height);

    if (!isPow2) {
        if (!fGpu->npotTextureSupport()) {
            return false;
        }

        bool tiled = sampler.getWrapX() != GrSamplerState::kClamp_WrapMode ||
                     sampler.getWrapY() != GrSamplerState::kClamp_WrapMode;
        if (tiled && !fGpu->npotTextureTileSupport()) {
            return false;
        }
    }
    return true;
}

////////////////////////////////////////////////////////////////////////////////

const GrClip& GrContext::getClip() const { return fGpu->getClip(); }

void GrContext::setClip(const GrClip& clip) {
    fGpu->setClip(clip);
    fGpu->enableState(GrDrawTarget::kClip_StateBit);
}

void GrContext::setClip(const GrIRect& rect) {
    GrClip clip;
    clip.setFromIRect(rect);
    fGpu->setClip(clip);
}

////////////////////////////////////////////////////////////////////////////////

void GrContext::clear(const GrIRect* rect, const GrColor color) {
    this->flush();
    fGpu->clear(rect, color);
}

void GrContext::drawPaint(const GrPaint& paint) {
    // set rect to be big enough to fill the space, but not super-huge, so we
    // don't overflow fixed-point implementations
    GrRect r;
    r.setLTRB(0, 0,
              GrIntToScalar(getRenderTarget()->width()),
              GrIntToScalar(getRenderTarget()->height()));
    GrMatrix inverse;
    if (fGpu->getViewInverse(&inverse)) {
        inverse.mapRect(&r);
    } else {
        GrPrintf("---- fGpu->getViewInverse failed\n");
    }
    this->drawRect(paint, r);
}

////////////////////////////////////////////////////////////////////////////////

bool GrContext::doOffscreenAA(GrDrawTarget* target, 
                              const GrPaint& paint,
                              bool isLines) const {
#if !ENABLE_OFFSCREEN_AA
    return false;
#else
    if (!paint.fAntiAlias) {
        return false;
    }
    if (isLines && fGpu->supportsAALines()) {
        return false;
    }
    if (target->getRenderTarget()->isMultisampled()) {
        return false;
    }
    // we have to be sure that the blend equation is expressible
    // as simple src / dst coeffecients when the source 
    // is already modulated by the coverage fraction.
    // We could use dual-source blending to get the correct per-pixel
    // dst coeffecient for the remaining cases.
    if (kISC_BlendCoeff != paint.fDstBlendCoeff &&
        kOne_BlendCoeff != paint.fDstBlendCoeff &&
        kISA_BlendCoeff != paint.fDstBlendCoeff) {
        return false;
    }
    return true;
#endif
}

bool GrContext::setupOffscreenAAPass1(GrDrawTarget* target,
                                      bool requireStencil,
                                      const GrIRect& boundRect,
                                      OffscreenRecord* record) {
    GrAssert(ENABLE_OFFSCREEN_AA);

    GrAssert(NULL == record->fEntry0);
    GrAssert(NULL == record->fEntry1);

    int boundW = boundRect.width();
    int boundH = boundRect.height();
    int size  = GrMax(64, (int)GrNextPow2(GrMax(boundW, boundH)));

    GrTextureDesc desc;
    if (requireStencil) {
        desc.fFlags = kRenderTarget_GrTextureFlagBit;
    } else {
        desc.fFlags = kRenderTarget_GrTextureFlagBit | 
                      kNoStencil_GrTextureFlagBit;
    }

    desc.fFormat = kRGBA_8888_GrPixelConfig;

    int scale;
    // Using MSAA seems to be slower for some yet unknown reason.
    if (false && fGpu->supportsFullsceneAA()) {
        record->fDownsample = OffscreenRecord::kFSAA_Downsample;
        scale = GR_Scalar1;
        desc.fAALevel = kMed_GrAALevel;
    } else {
        record->fDownsample = (fGpu->supports4x4DownsampleFilter()) ?
                                OffscreenRecord::k4x4SinglePass_Downsample :
                                OffscreenRecord::k4x4TwoPass_Downsample;
        scale = 4;
        desc.fAALevel = kNone_GrAALevel;
    }
    
    desc.fWidth = scale * size;
    desc.fHeight = scale * size;

    record->fEntry0 = this->lockKeylessTexture(desc);

    if (NULL == record->fEntry0) {
        return false;
    }

    if (OffscreenRecord::k4x4TwoPass_Downsample == record->fDownsample) {
        desc.fWidth /= 2;
        desc.fHeight /= 2;
        record->fEntry1 = this->lockKeylessTexture(desc);
        if (NULL == record->fEntry1) {
            this->unlockTexture(record->fEntry0);
            record->fEntry0 = NULL;
            return false;
        }
    }

    GrRenderTarget* offRT0 = record->fEntry0->texture()->asRenderTarget();
    GrAssert(NULL != offRT0);

    target->saveCurrentDrawState(&record->fSavedState);

    GrPaint tempPaint;
    tempPaint.reset();
    SetPaint(tempPaint, target);
    target->setRenderTarget(offRT0);

    GrMatrix transM;
    transM.setTranslate(-boundRect.fLeft, -boundRect.fTop);
    target->postConcatViewMatrix(transM);
    GrMatrix scaleM;
    scaleM.setScale(scale * GR_Scalar1, scale * GR_Scalar1);
    target->postConcatViewMatrix(scaleM);

    // clip gets applied in second pass
    target->disableState(GrDrawTarget::kClip_StateBit);

    GrIRect clear(0, 0, scale * boundW, scale * boundH);
    target->clear(&clear, 0x0);

    return true;
}

void GrContext::offscreenAAPass2(GrDrawTarget* target,
                                 const GrPaint& paint,
                                 const GrIRect& boundRect,
                                 OffscreenRecord* record) {

    GrAssert(NULL != record->fEntry0);

    GrSamplerState::Filter filter;
    if (OffscreenRecord::k4x4SinglePass_Downsample == record->fDownsample) {
        filter = GrSamplerState::k4x4Downsample_Filter;
    } else {
        filter = GrSamplerState::kBilinear_Filter;
    }

    GrMatrix sampleM;
    GrSamplerState sampler(GrSamplerState::kClamp_WrapMode, 
                           GrSamplerState::kClamp_WrapMode, filter);

    GrTexture* src = record->fEntry0->texture();
    int scale;

    if (OffscreenRecord::k4x4TwoPass_Downsample == record->fDownsample) {
        GrAssert(NULL != record->fEntry1);
        scale = 2;
        GrRenderTarget* dst = record->fEntry1->texture()->asRenderTarget();
        
        // Do 2x2 downsample from first to second
        target->setTexture(kOffscreenStage, src);
        target->setRenderTarget(dst);
        target->setViewMatrix(GrMatrix::I());
        sampleM.setScale(scale * GR_Scalar1 / src->width(),
                         scale * GR_Scalar1 / src->height());
        sampler.setMatrix(sampleM);
        target->setSamplerState(kOffscreenStage, sampler);
        GrRect rect(0, 0,
                    scale * boundRect.width(),
                    scale * boundRect.height());
        target->drawSimpleRect(rect, NULL, 1 << kOffscreenStage);
        
        src = record->fEntry1->texture();
    } else if (OffscreenRecord::kFSAA_Downsample == record->fDownsample) {
        scale = 1;
        GrIRect rect(0, 0, boundRect.width(), boundRect.height());
        src->asRenderTarget()->overrideResolveRect(rect);
    } else {
        GrAssert(OffscreenRecord::k4x4SinglePass_Downsample == 
                 record->fDownsample);
        scale = 4;
    }

    // setup for draw back to main RT
    target->restoreDrawState(record->fSavedState);
    if (NULL != paint.getTexture()) {
        GrMatrix invVM;
        if (target->getViewInverse(&invVM)) {
            target->preConcatSamplerMatrix(0, invVM);
        }
    }
    target->setViewMatrix(GrMatrix::I());

    target->setTexture(kOffscreenStage, src);
    sampleM.setScale(scale * GR_Scalar1 / src->width(),
                     scale * GR_Scalar1 / src->height());
    sampler.setMatrix(sampleM);
    sampleM.setTranslate(-boundRect.fLeft, -boundRect.fTop);
    sampler.preConcatMatrix(sampleM);
    target->setSamplerState(kOffscreenStage, sampler);

    GrRect dstRect(boundRect);
    int stages = (1 << kOffscreenStage) | (NULL == paint.getTexture() ? 0 : 1);
    target->drawSimpleRect(dstRect, NULL, stages);

    this->unlockTexture(record->fEntry0);
    record->fEntry0 = NULL;
    if (NULL != record->fEntry1) {
        this->unlockTexture(record->fEntry1);
        record->fEntry1 = NULL;
    }
    target->restoreDrawState(record->fSavedState);
}

////////////////////////////////////////////////////////////////////////////////

/*  create a triangle strip that strokes the specified triangle. There are 8
 unique vertices, but we repreat the last 2 to close up. Alternatively we
 could use an indices array, and then only send 8 verts, but not sure that
 would be faster.
 */
static void setStrokeRectStrip(GrPoint verts[10], GrRect rect,
                               GrScalar width) {
    const GrScalar rad = GrScalarHalf(width);
    rect.sort();

    verts[0].set(rect.fLeft + rad, rect.fTop + rad);
    verts[1].set(rect.fLeft - rad, rect.fTop - rad);
    verts[2].set(rect.fRight - rad, rect.fTop + rad);
    verts[3].set(rect.fRight + rad, rect.fTop - rad);
    verts[4].set(rect.fRight - rad, rect.fBottom - rad);
    verts[5].set(rect.fRight + rad, rect.fBottom + rad);
    verts[6].set(rect.fLeft + rad, rect.fBottom - rad);
    verts[7].set(rect.fLeft - rad, rect.fBottom + rad);
    verts[8] = verts[0];
    verts[9] = verts[1];
}

static GrColor getColorForMesh(const GrPaint& paint) {
    if (NULL == paint.getTexture()) {
        return paint.fColor;
    } else {
        unsigned a = GrColorUnpackA(paint.fColor);
        return GrColorPackRGBA(a, a, a, a);
    }
}

static void setInsetFan(GrPoint* pts, size_t stride,
                        const GrRect& r, GrScalar dx, GrScalar dy) {
    pts->setRectFan(r.fLeft + dx, r.fTop + dy, r.fRight - dx, r.fBottom - dy, stride);
}

static const uint16_t gFillAARectIdx[] = {
    0, 1, 5, 5, 4, 0,
    1, 2, 6, 6, 5, 1,
    2, 3, 7, 7, 6, 2,
    3, 0, 4, 4, 7, 3,
    4, 5, 6, 6, 7, 4,
};

int GrContext::aaFillRectIndexCount() const {
    return GR_ARRAY_COUNT(gFillAARectIdx);
}

GrIndexBuffer* GrContext::aaFillRectIndexBuffer() {
    if (NULL == fAAFillRectIndexBuffer) {
        fAAFillRectIndexBuffer = fGpu->createIndexBuffer(sizeof(gFillAARectIdx),
                                                         false);
        GrAssert(NULL != fAAFillRectIndexBuffer);
#if GR_DEBUG
        bool updated =
#endif
        fAAFillRectIndexBuffer->updateData(gFillAARectIdx,
                                           sizeof(gFillAARectIdx));
        GR_DEBUGASSERT(updated);
    }
    return fAAFillRectIndexBuffer;
}

static const uint16_t gStrokeAARectIdx[] = {
    0 + 0, 1 + 0, 5 + 0, 5 + 0, 4 + 0, 0 + 0,
    1 + 0, 2 + 0, 6 + 0, 6 + 0, 5 + 0, 1 + 0,
    2 + 0, 3 + 0, 7 + 0, 7 + 0, 6 + 0, 2 + 0,
    3 + 0, 0 + 0, 4 + 0, 4 + 0, 7 + 0, 3 + 0,

    0 + 4, 1 + 4, 5 + 4, 5 + 4, 4 + 4, 0 + 4,
    1 + 4, 2 + 4, 6 + 4, 6 + 4, 5 + 4, 1 + 4,
    2 + 4, 3 + 4, 7 + 4, 7 + 4, 6 + 4, 2 + 4,
    3 + 4, 0 + 4, 4 + 4, 4 + 4, 7 + 4, 3 + 4,

    0 + 8, 1 + 8, 5 + 8, 5 + 8, 4 + 8, 0 + 8,
    1 + 8, 2 + 8, 6 + 8, 6 + 8, 5 + 8, 1 + 8,
    2 + 8, 3 + 8, 7 + 8, 7 + 8, 6 + 8, 2 + 8,
    3 + 8, 0 + 8, 4 + 8, 4 + 8, 7 + 8, 3 + 8,
};

int GrContext::aaStrokeRectIndexCount() const {
    return GR_ARRAY_COUNT(gStrokeAARectIdx);
}

GrIndexBuffer* GrContext::aaStrokeRectIndexBuffer() {
    if (NULL == fAAStrokeRectIndexBuffer) {
        fAAStrokeRectIndexBuffer = fGpu->createIndexBuffer(sizeof(gStrokeAARectIdx),
                                                           false);
        GrAssert(NULL != fAAStrokeRectIndexBuffer);
#if GR_DEBUG
        bool updated =
#endif
        fAAStrokeRectIndexBuffer->updateData(gStrokeAARectIdx,
                                             sizeof(gStrokeAARectIdx));
        GR_DEBUGASSERT(updated);
    }
    return fAAStrokeRectIndexBuffer;
}

void GrContext::fillAARect(GrDrawTarget* target,
                           const GrPaint& paint,
                           const GrRect& devRect) {

    GrVertexLayout layout = GrDrawTarget::kColor_VertexLayoutBit;
    if (NULL != paint.getTexture()) {
        layout |= GrDrawTarget::StagePosAsTexCoordVertexLayoutBit(0);
    }

    size_t vsize = GrDrawTarget::VertexSize(layout);

    GrDrawTarget::AutoReleaseGeometry geo(target, layout, 8, 0);

    intptr_t verts = reinterpret_cast<intptr_t>(geo.vertices());

    GrPoint* fan0Pos = reinterpret_cast<GrPoint*>(verts);
    GrPoint* fan1Pos = reinterpret_cast<GrPoint*>(verts + 4 * vsize);

    setInsetFan(fan0Pos, vsize, devRect, -GR_ScalarHalf, -GR_ScalarHalf);
    setInsetFan(fan1Pos, vsize, devRect,  GR_ScalarHalf,  GR_ScalarHalf);

    verts += sizeof(GrPoint);
    for (int i = 0; i < 4; ++i) {
        *reinterpret_cast<GrColor*>(verts + i * vsize) = 0;
    }

    GrColor innerColor = getColorForMesh(paint);
    verts += 4 * vsize;
    for (int i = 0; i < 4; ++i) {
        *reinterpret_cast<GrColor*>(verts + i * vsize) = innerColor;
    }

    target->setIndexSourceToBuffer(this->aaFillRectIndexBuffer());

    target->drawIndexed(kTriangles_PrimitiveType, 0,
                         0, 8, this->aaFillRectIndexCount());
}

void GrContext::strokeAARect(GrDrawTarget* target, const GrPaint& paint,
                             const GrRect& devRect, const GrVec& devStrokeSize) {
    const GrScalar& dx = devStrokeSize.fX;
    const GrScalar& dy = devStrokeSize.fY;
    const GrScalar rx = GrMul(dx, GR_ScalarHalf);
    const GrScalar ry = GrMul(dy, GR_ScalarHalf);

    GrVertexLayout layout = GrDrawTarget::kColor_VertexLayoutBit;

    if (NULL != paint.getTexture()) {
        layout |= GrDrawTarget::StagePosAsTexCoordVertexLayoutBit(0);
    }

    GrScalar spare;
    {
        GrScalar w = devRect.width() - dx;
        GrScalar h = devRect.height() - dy;
        spare = GrMin(w, h);
    }

    if (spare <= 0) {
        GrRect r(devRect);
        r.inset(-rx, -ry);
        fillAARect(target, paint, r);
        return;
    }

    size_t vsize = GrDrawTarget::VertexSize(layout);

    GrDrawTarget::AutoReleaseGeometry geo(target, layout, 16, 0);

    intptr_t verts = reinterpret_cast<intptr_t>(geo.vertices());

    GrPoint* fan0Pos = reinterpret_cast<GrPoint*>(verts);
    GrPoint* fan1Pos = reinterpret_cast<GrPoint*>(verts + 4 * vsize);
    GrPoint* fan2Pos = reinterpret_cast<GrPoint*>(verts + 8 * vsize);
    GrPoint* fan3Pos = reinterpret_cast<GrPoint*>(verts + 12 * vsize);

    setInsetFan(fan0Pos, vsize, devRect, -rx - GR_ScalarHalf, -ry - GR_ScalarHalf);
    setInsetFan(fan1Pos, vsize, devRect, -rx + GR_ScalarHalf, -ry + GR_ScalarHalf);
    setInsetFan(fan2Pos, vsize, devRect,  rx - GR_ScalarHalf,  ry - GR_ScalarHalf);
    setInsetFan(fan3Pos, vsize, devRect,  rx + GR_ScalarHalf,  ry + GR_ScalarHalf);

    verts += sizeof(GrPoint);
    for (int i = 0; i < 4; ++i) {
        *reinterpret_cast<GrColor*>(verts + i * vsize) = 0;
    }

    GrColor innerColor = getColorForMesh(paint);
    verts += 4 * vsize;
    for (int i = 0; i < 8; ++i) {
        *reinterpret_cast<GrColor*>(verts + i * vsize) = innerColor;
    }

    verts += 8 * vsize;
    for (int i = 0; i < 8; ++i) {
        *reinterpret_cast<GrColor*>(verts + i * vsize) = 0;
    }

    target->setIndexSourceToBuffer(aaStrokeRectIndexBuffer());
    target->drawIndexed(kTriangles_PrimitiveType,
                        0, 0, 16, aaStrokeRectIndexCount());
}

static bool apply_aa_to_rect(GrDrawTarget* target,
                             GrGpu* gpu,
                             const GrPaint& paint,
                             const GrRect& rect,
                             GrScalar width, 
                             const GrMatrix* matrix,
                             GrMatrix* combinedMatrix,
                             GrRect* devRect) {
    // we use a simple alpha ramp to do aa on axis-aligned rects
    // do AA with alpha ramp if the caller requested AA, the rect 
    // will be axis-aligned,the render target is not
    // multisampled, and the rect won't land on integer coords.

    if (!paint.fAntiAlias) {
        return false;
    }

    if (target->getRenderTarget()->isMultisampled()) {
        return false;
    }

    if (0 == width && gpu->supportsAALines()) {
        return false;
    }

    if (!target->getViewMatrix().preservesAxisAlignment()) {
        return false;
    }

    if (NULL != matrix && 
        !matrix->preservesAxisAlignment()) {
        return false;
    }

    *combinedMatrix = target->getViewMatrix();
    if (NULL != matrix) {
        combinedMatrix->preConcat(*matrix);
        GrAssert(combinedMatrix->preservesAxisAlignment());
    }
    
    combinedMatrix->mapRect(devRect, rect);
    devRect->sort();

    if (width < 0) {
        return !devRect->isIRect();
    } else {
        return true;
    }
}

void GrContext::drawRect(const GrPaint& paint,
                         const GrRect& rect,
                         GrScalar width,
                         const GrMatrix* matrix) {

    bool textured = NULL != paint.getTexture();

    GrDrawTarget* target = this->prepareToDraw(paint, kUnbuffered_DrawCategory);

    GrRect devRect = rect;
    GrMatrix combinedMatrix;
    bool doAA = apply_aa_to_rect(target, fGpu, paint, rect, width, matrix, 
                                 &combinedMatrix, &devRect);

    if (doAA) {
        GrDrawTarget::AutoViewMatrixRestore avm(target);
        if (textured) {
            GrMatrix inv;
            if (combinedMatrix.invert(&inv)) {
                target->preConcatSamplerMatrix(0, inv);
            }
        }
        target->setViewMatrix(GrMatrix::I());
        if (width >= 0) {
            GrVec strokeSize;;
            if (width > 0) {
                strokeSize.set(width, width);
                combinedMatrix.mapVec(&strokeSize);
                strokeSize.setAbs(strokeSize);
            } else {
                strokeSize.set(GR_Scalar1, GR_Scalar1);
            }
            strokeAARect(target, paint, devRect, strokeSize);
        } else {
            fillAARect(target, paint, devRect);
        }
        return;
    }

    if (width >= 0) {
        // TODO: consider making static vertex buffers for these cases.
        // Hairline could be done by just adding closing vertex to
        // unitSquareVertexBuffer()
        GrVertexLayout layout = textured ?
                            GrDrawTarget::StagePosAsTexCoordVertexLayoutBit(0) :
                            0;
        static const int worstCaseVertCount = 10;
        GrDrawTarget::AutoReleaseGeometry geo(target, layout, worstCaseVertCount, 0);

        if (!geo.succeeded()) {
            return;
        }

        GrPrimitiveType primType;
        int vertCount;
        GrPoint* vertex = geo.positions();

        if (width > 0) {
            vertCount = 10;
            primType = kTriangleStrip_PrimitiveType;
            setStrokeRectStrip(vertex, rect, width);
        } else {
            // hairline
            vertCount = 5;
            primType = kLineStrip_PrimitiveType;
            vertex[0].set(rect.fLeft, rect.fTop);
            vertex[1].set(rect.fRight, rect.fTop);
            vertex[2].set(rect.fRight, rect.fBottom);
            vertex[3].set(rect.fLeft, rect.fBottom);
            vertex[4].set(rect.fLeft, rect.fTop);
        }

        GrDrawTarget::AutoViewMatrixRestore avmr;
        if (NULL != matrix) {
            avmr.set(target);
            target->preConcatViewMatrix(*matrix);
            if (textured) {
                target->preConcatSamplerMatrix(0, *matrix);
            }
        }

        target->drawNonIndexed(primType, 0, vertCount);
    } else {
        #if GR_STATIC_RECT_VB
            GrVertexLayout layout = (textured) ?
                            GrDrawTarget::StagePosAsTexCoordVertexLayoutBit(0) :
                            0;
            target->setVertexSourceToBuffer(layout,
                                            fGpu->getUnitSquareVertexBuffer());
            GrDrawTarget::AutoViewMatrixRestore avmr(target);
            GrMatrix m;
            m.setAll(rect.width(),    0,             rect.fLeft,
                        0,            rect.height(), rect.fTop,
                        0,            0,             GrMatrix::I()[8]);

            if (NULL != matrix) {
                m.postConcat(*matrix);
            }

            target->preConcatViewMatrix(m);

            if (textured) {
                target->preConcatSamplerMatrix(0, m);
            }
            target->drawNonIndexed(kTriangleFan_PrimitiveType, 0, 4);
        #else
            target->drawSimpleRect(rect, matrix, textured ? 1 : 0);
        #endif
    }
}

void GrContext::drawRectToRect(const GrPaint& paint,
                               const GrRect& dstRect,
                               const GrRect& srcRect,
                               const GrMatrix* dstMatrix,
                               const GrMatrix* srcMatrix) {

    if (NULL == paint.getTexture()) {
        drawRect(paint, dstRect, -1, dstMatrix);
        return;
    }

    GR_STATIC_ASSERT(!BATCH_RECT_TO_RECT || !GR_STATIC_RECT_VB);

#if GR_STATIC_RECT_VB
    GrDrawTarget* target = this->prepareToDraw(paint, kUnbuffered_DrawCategory);

    GrVertexLayout layout = GrDrawTarget::StagePosAsTexCoordVertexLayoutBit(0);
    GrDrawTarget::AutoViewMatrixRestore avmr(target);

    GrMatrix m;

    m.setAll(dstRect.width(), 0,                dstRect.fLeft,
             0,               dstRect.height(), dstRect.fTop,
             0,               0,                GrMatrix::I()[8]);
    if (NULL != dstMatrix) {
        m.postConcat(*dstMatrix);
    }
    target->preConcatViewMatrix(m);

    m.setAll(srcRect.width(), 0,                srcRect.fLeft,
             0,               srcRect.height(), srcRect.fTop,
             0,               0,                GrMatrix::I()[8]);
    if (NULL != srcMatrix) {
        m.postConcat(*srcMatrix);
    }
    target->preConcatSamplerMatrix(0, m);

    target->setVertexSourceToBuffer(layout, fGpu->getUnitSquareVertexBuffer());
    target->drawNonIndexed(kTriangleFan_PrimitiveType, 0, 4);
#else

    GrDrawTarget* target;
#if BATCH_RECT_TO_RECT
    target = this->prepareToDraw(paint, kBuffered_DrawCategory);
#else
    target = this->prepareToDraw(paint, kUnbuffered_DrawCategory);
#endif

    const GrRect* srcRects[GrDrawTarget::kNumStages] = {NULL};
    const GrMatrix* srcMatrices[GrDrawTarget::kNumStages] = {NULL};
    srcRects[0] = &srcRect;
    srcMatrices[0] = srcMatrix;

    target->drawRect(dstRect, dstMatrix, 1, srcRects, srcMatrices);
#endif
}

void GrContext::drawVertices(const GrPaint& paint,
                             GrPrimitiveType primitiveType,
                             int vertexCount,
                             const GrPoint positions[],
                             const GrPoint texCoords[],
                             const GrColor colors[],
                             const uint16_t indices[],
                             int indexCount) {
    GrVertexLayout layout = 0;
    int vertexSize = sizeof(GrPoint);

    GrDrawTarget::AutoReleaseGeometry geo;

    GrDrawTarget* target = this->prepareToDraw(paint, kUnbuffered_DrawCategory);

    if (NULL != paint.getTexture()) {
        if (NULL == texCoords) {
            layout |= GrDrawTarget::StagePosAsTexCoordVertexLayoutBit(0);
        } else {
            layout |= GrDrawTarget::StageTexCoordVertexLayoutBit(0,0);
            vertexSize += sizeof(GrPoint);
        }
    }

    if (NULL != colors) {
        layout |= GrDrawTarget::kColor_VertexLayoutBit;
        vertexSize += sizeof(GrColor);
    }

    bool doAA = false;
    OffscreenRecord record;
    GrIRect bounds;

    if (sizeof(GrPoint) != vertexSize) {
        if (!geo.set(target, layout, vertexCount, 0)) {
            GrPrintf("Failed to get space for vertices!");
            return;
        }
        int texOffsets[GrDrawTarget::kMaxTexCoords];
        int colorOffset;
        int vsize = GrDrawTarget::VertexSizeAndOffsetsByIdx(layout,
                                                            texOffsets,
                                                            &colorOffset);
        void* curVertex = geo.vertices();

        for (int i = 0; i < vertexCount; ++i) {
            *((GrPoint*)curVertex) = positions[i];

            if (texOffsets[0] > 0) {
                *(GrPoint*)((intptr_t)curVertex + texOffsets[0]) = texCoords[i];
            }
            if (colorOffset > 0) {
                *(GrColor*)((intptr_t)curVertex + colorOffset) = colors[i];
            }
            curVertex = (void*)((intptr_t)curVertex + vsize);
        }
    } else {
        // we don't do offscreen AA when we have per-vertex tex coords or colors
        if (this->doOffscreenAA(target, paint, GrIsPrimTypeLines(primitiveType))) {
            GrRect b;
            b.setBounds(positions, vertexCount);
            target->getViewMatrix().mapRect(&b);
            b.roundOut(&bounds);
            
            if (this->setupOffscreenAAPass1(target, false, bounds, &record)) {
                doAA = true;
            }
        }
        target->setVertexSourceToArray(layout, positions, vertexCount);
    }

    if (NULL != indices) {
        target->setIndexSourceToArray(indices, indexCount);
    }

    if (NULL != indices) {
        target->drawIndexed(primitiveType, 0, 0, vertexCount, indexCount);
    } else {
        target->drawNonIndexed(primitiveType, 0, vertexCount);
    }

    if (doAA) {
        this->offscreenAAPass2(target, paint, bounds, &record);
    }
}


///////////////////////////////////////////////////////////////////////////////

void GrContext::drawPath(const GrPaint& paint,
                         GrPathIter* path,
                         GrPathFill fill,
                         const GrPoint* translate) {

    GrDrawTarget* target = this->prepareToDraw(paint, kUnbuffered_DrawCategory);
    GrPathRenderer* pr = this->getPathRenderer(target, path, fill);

    if (!IsFillInverted(fill) && // will be relaxed soon
        !pr->supportsAA(target, path, fill) &&
        this->doOffscreenAA(target, paint, kHairLine_PathFill == fill)) {

        OffscreenRecord record;
        bool needsStencil = pr->requiresStencilPass(target, path, fill);

        // compute bounds as intersection of rt size, clip, and path
        GrIRect bound(0, 0, 
                      target->getRenderTarget()->width(), 
                      target->getRenderTarget()->height());
        if (target->getClip().hasConservativeBounds()) {
            GrIRect clipIBounds;
            target->getClip().getConservativeBounds().roundOut(&clipIBounds);
            if (!bound.intersectWith(clipIBounds)) {
                return;
            }
        }
        GrRect pathBounds;
        if (path->getConservativeBounds(&pathBounds)) {
            GrIRect pathIBounds;
            target->getViewMatrix().mapRect(&pathBounds, pathBounds);
            pathBounds.roundOut(&pathIBounds);
            if (!bound.intersectWith(pathIBounds)) {
                return;
            }
        }

        if (this->setupOffscreenAAPass1(target, needsStencil, bound, &record)) {
            pr->drawPath(target, 0, path, fill, translate);
            this->offscreenAAPass2(target, paint, bound, &record);
            return;
        }
    } 
    GrDrawTarget::StageBitfield enabledStages = 0;
    if (NULL != paint.getTexture()) {
        enabledStages |= 1;
    }

    pr->drawPath(target, enabledStages, path, fill, translate);
}

void GrContext::drawPath(const GrPaint& paint,
                         const GrPath& path,
                         GrPathFill fill,
                         const GrPoint* translate) {
    GrPath::Iter iter(path);
    this->drawPath(paint, &iter, fill, translate);
}


////////////////////////////////////////////////////////////////////////////////

void GrContext::flush(int flagsBitfield) {
    if (kDiscard_FlushBit & flagsBitfield) {
        fDrawBuffer->reset();
    } else {
        flushDrawBuffer();
    }

    if (kForceCurrentRenderTarget_FlushBit & flagsBitfield) {
        fGpu->forceRenderTargetFlush();
    }
}

void GrContext::flushText() {
    if (kText_DrawCategory == fLastDrawCategory) {
        flushDrawBuffer();
    }
}

void GrContext::flushDrawBuffer() {
#if BATCH_RECT_TO_RECT || DEFER_TEXT_RENDERING
    fDrawBuffer->playback(fGpu);
    fDrawBuffer->reset();
#endif
}

bool GrContext::readTexturePixels(GrTexture* texture,
                                  int left, int top, int width, int height,
                                  GrPixelConfig config, void* buffer) {

    // TODO: code read pixels for textures that aren't rendertargets

    this->flush();
    GrRenderTarget* target = texture->asRenderTarget();
    if (NULL != target) {
        return fGpu->readPixels(target,
                                left, top, width, height, 
                                config, buffer);
    } else {
        return false;
    }
}

bool GrContext::readRenderTargetPixels(GrRenderTarget* target,
                                      int left, int top, int width, int height,
                                      GrPixelConfig config, void* buffer) {
    uint32_t flushFlags = 0;
    if (NULL == target) { 
        flushFlags |= GrContext::kForceCurrentRenderTarget_FlushBit;
    }

    this->flush(flushFlags);
    return fGpu->readPixels(target,
                            left, top, width, height, 
                            config, buffer);
}

void GrContext::writePixels(int left, int top, int width, int height,
                            GrPixelConfig config, const void* buffer,
                            size_t stride) {

    // TODO: when underlying api has a direct way to do this we should use it
    // (e.g. glDrawPixels on desktop GL).

    const GrTextureDesc desc = {
        kNone_GrTextureFlags, kNone_GrAALevel, width, height, config
    };
    GrTexture* texture = fGpu->createTexture(desc, buffer, stride);
    if (NULL == texture) {
        return;
    }

    this->flush(true);

    GrAutoUnref                     aur(texture);
    GrDrawTarget::AutoStateRestore  asr(fGpu);

    GrMatrix matrix;
    matrix.setTranslate(GrIntToScalar(left), GrIntToScalar(top));
    fGpu->setViewMatrix(matrix);

    fGpu->disableState(GrDrawTarget::kClip_StateBit);
    fGpu->setAlpha(0xFF);
    fGpu->setBlendFunc(kOne_BlendCoeff,
                       kZero_BlendCoeff);
    fGpu->setTexture(0, texture);

    GrSamplerState sampler;
    sampler.setClampNoFilter();
    matrix.setScale(GR_Scalar1 / width, GR_Scalar1 / height);
    sampler.setMatrix(matrix);
    fGpu->setSamplerState(0, sampler);

    GrVertexLayout layout = GrDrawTarget::StagePosAsTexCoordVertexLayoutBit(0);
    static const int VCOUNT = 4;

    GrDrawTarget::AutoReleaseGeometry geo(fGpu, layout, VCOUNT, 0);
    if (!geo.succeeded()) {
        return;
    }
    ((GrPoint*)geo.vertices())->setIRectFan(0, 0, width, height);
    fGpu->drawNonIndexed(kTriangleFan_PrimitiveType, 0, VCOUNT);
}
////////////////////////////////////////////////////////////////////////////////

void GrContext::SetPaint(const GrPaint& paint, GrDrawTarget* target) {
    target->setTexture(0, paint.getTexture());
    target->setSamplerState(0, paint.fSampler);
    target->setColor(paint.fColor);

    if (paint.fDither) {
        target->enableState(GrDrawTarget::kDither_StateBit);
    } else {
        target->disableState(GrDrawTarget::kDither_StateBit);
    }
    if (paint.fAntiAlias) {
        target->enableState(GrDrawTarget::kAntialias_StateBit);
    } else {
        target->disableState(GrDrawTarget::kAntialias_StateBit);
    }
    target->setBlendFunc(paint.fSrcBlendCoeff, paint.fDstBlendCoeff);
}

GrDrawTarget* GrContext::prepareToDraw(const GrPaint& paint,
                                       DrawCategory category) {
    if (category != fLastDrawCategory) {
        flushDrawBuffer();
        fLastDrawCategory = category;
    }
    SetPaint(paint, fGpu);
    GrDrawTarget* target = fGpu;
    switch (category) {
    case kText_DrawCategory:
#if DEFER_TEXT_RENDERING
        target = fDrawBuffer;
        fDrawBuffer->initializeDrawStateAndClip(*fGpu);
#else
        target = fGpu;
#endif
        break;
    case kUnbuffered_DrawCategory:
        target = fGpu;
        break;
    case kBuffered_DrawCategory:
        target = fDrawBuffer;
        fDrawBuffer->initializeDrawStateAndClip(*fGpu);
        break;
    }
    return target;
}

////////////////////////////////////////////////////////////////////////////////

void GrContext::setRenderTarget(GrRenderTarget* target) {
    this->flush(false);
    fGpu->setRenderTarget(target);
}

GrRenderTarget* GrContext::getRenderTarget() {
    return fGpu->getRenderTarget();
}

const GrRenderTarget* GrContext::getRenderTarget() const {
    return fGpu->getRenderTarget();
}

const GrMatrix& GrContext::getMatrix() const {
    return fGpu->getViewMatrix();
}

void GrContext::setMatrix(const GrMatrix& m) {
    fGpu->setViewMatrix(m);
}

void GrContext::concatMatrix(const GrMatrix& m) const {
    fGpu->preConcatViewMatrix(m);
}

static inline intptr_t setOrClear(intptr_t bits, int shift, intptr_t pred) {
    intptr_t mask = 1 << shift;
    if (pred) {
        bits |= mask;
    } else {
        bits &= ~mask;
    }
    return bits;
}

void GrContext::resetStats() {
    fGpu->resetStats();
}

const GrGpuStats& GrContext::getStats() const {
    return fGpu->getStats();
}

void GrContext::printStats() const {
    fGpu->printStats();
}

GrContext::GrContext(GrGpu* gpu) :
    fDefaultPathRenderer(gpu->supportsTwoSidedStencil(),
                         gpu->supportsStencilWrapOps()) {

    fGpu = gpu;
    fGpu->ref();
    fGpu->setContext(this);

    fCustomPathRenderer = GrPathRenderer::CreatePathRenderer();
    fGpu->setClipPathRenderer(fCustomPathRenderer);

    fTextureCache = new GrTextureCache(MAX_TEXTURE_CACHE_COUNT,
                                       MAX_TEXTURE_CACHE_BYTES);
    fFontCache = new GrFontCache(fGpu);

    fLastDrawCategory = kUnbuffered_DrawCategory;

    fDrawBuffer = NULL;
    fDrawBufferVBAllocPool = NULL;
    fDrawBufferIBAllocPool = NULL;

    fAAFillRectIndexBuffer = NULL;
    fAAStrokeRectIndexBuffer = NULL;

    this->setupDrawBuffer();
}

void GrContext::setupDrawBuffer() {

    GrAssert(NULL == fDrawBuffer);
    GrAssert(NULL == fDrawBufferVBAllocPool);
    GrAssert(NULL == fDrawBufferIBAllocPool);

#if DEFER_TEXT_RENDERING || BATCH_RECT_TO_RECT
    fDrawBufferVBAllocPool =
        new GrVertexBufferAllocPool(fGpu, false,
                                    DRAW_BUFFER_VBPOOL_BUFFER_SIZE,
                                    DRAW_BUFFER_VBPOOL_PREALLOC_BUFFERS);
    fDrawBufferIBAllocPool =
        new GrIndexBufferAllocPool(fGpu, false,
                                   DRAW_BUFFER_IBPOOL_BUFFER_SIZE,
                                   DRAW_BUFFER_IBPOOL_PREALLOC_BUFFERS);

    fDrawBuffer = new GrInOrderDrawBuffer(fDrawBufferVBAllocPool,
                                          fDrawBufferIBAllocPool);
#endif

#if BATCH_RECT_TO_RECT
    fDrawBuffer->setQuadIndexBuffer(this->getQuadIndexBuffer());
#endif
}

GrDrawTarget* GrContext::getTextTarget(const GrPaint& paint) {
    GrDrawTarget* target;
#if DEFER_TEXT_RENDERING
    target = prepareToDraw(paint, kText_DrawCategory);
#else
    target = prepareToDraw(paint, kUnbuffered_DrawCategory);
#endif
    SetPaint(paint, target);
    return target;
}

const GrIndexBuffer* GrContext::getQuadIndexBuffer() const {
    return fGpu->getQuadIndexBuffer();
}

GrPathRenderer* GrContext::getPathRenderer(const GrDrawTarget* target,
                                           GrPathIter* path,
                                           GrPathFill fill) {
    if (NULL != fCustomPathRenderer &&
        fCustomPathRenderer->canDrawPath(target, path, fill)) {
        return fCustomPathRenderer;
    } else {
        GrAssert(fDefaultPathRenderer.canDrawPath(target, path, fill));
        return &fDefaultPathRenderer;
    }
}

