/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 2 -*-
//  * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "mozilla/layers/TextureClientX11.h"
#include "mozilla/layers/CompositableClient.h"
#include "mozilla/layers/CompositableForwarder.h"
#include "mozilla/layers/ISurfaceAllocator.h"
#include "mozilla/layers/ShadowLayerUtilsX11.h"
#include "mozilla/gfx/2D.h"
#include "mozilla/gfx/Logging.h"
#include "gfxXlibSurface.h"
#include "gfx2DGlue.h"

#include "mozilla/X11Util.h"
#include <X11/Xlib.h>

using namespace mozilla;
using namespace mozilla::gfx;
using namespace mozilla::layers;

TextureClientX11::TextureClientX11(ISurfaceAllocator* aAllocator,
                                   SurfaceFormat aFormat,
                                   TextureFlags aFlags)
  : TextureClient(aAllocator, aFlags),
    mFormat(aFormat),
    mLocked(false)
{
  MOZ_COUNT_CTOR(TextureClientX11);
}

TextureClientX11::~TextureClientX11()
{
  MOZ_COUNT_DTOR(TextureClientX11);
}

already_AddRefed<TextureClient>
TextureClientX11::CreateSimilar(TextureFlags aFlags,
                                TextureAllocationFlags aAllocFlags) const
{
  RefPtr<TextureClient> tex = new TextureClientX11(mAllocator, mFormat, mFlags);

  // mSize is guaranteed to be non-negative
  MOZ_ASSERT(mSize.width >= 0 && mSize.height >= 0);
  if (!tex->AllocateForSurface(mSize, aAllocFlags)) {
    return nullptr;
  }

  return tex.forget();
}

bool
TextureClientX11::IsAllocated() const
{
  return !!mSurface;
}

bool
TextureClientX11::Lock(OpenMode aMode)
{
  MOZ_ASSERT(!mLocked, "The TextureClient is already Locked!");
  mLocked = IsValid() && IsAllocated();
  return mLocked;
}

void
TextureClientX11::Unlock()
{
  MOZ_ASSERT(mLocked, "The TextureClient is already Unlocked!");
  mLocked = false;

  if (mDrawTarget) {
    // see the comment on TextureClient::BorrowDrawTarget.
    // This DrawTarget is internal to the TextureClient and is only exposed to the
    // outside world between Lock() and Unlock(). This assertion checks that no outside
    // reference remains by the time Unlock() is called.
    MOZ_ASSERT(mDrawTarget->refCount() == 1);

    mDrawTarget->Flush();
    mDrawTarget = nullptr;
  }

  if (mSurface && !mAllocator->IsSameProcess()) {
    FinishX(DefaultXDisplay());
  }
}

bool
TextureClientX11::ToSurfaceDescriptor(SurfaceDescriptor& aOutDescriptor)
{
  MOZ_ASSERT(IsValid());
  if (!mSurface) {
    return false;
  }

  if (!(mFlags & TextureFlags::DEALLOCATE_CLIENT)) {
    // Pass to the host the responsibility of freeing the pixmap. ReleasePixmap means
    // the underlying pixmap will not be deallocated in mSurface's destructor.
    // ToSurfaceDescriptor is at most called once per TextureClient.
    mSurface->ReleasePixmap();
  }

  aOutDescriptor = SurfaceDescriptorX11(mSurface);
  return true;
}

bool
TextureClientX11::AllocateForSurface(IntSize aSize, TextureAllocationFlags aTextureFlags)
{
  MOZ_ASSERT(IsValid());
  MOZ_ASSERT(!IsAllocated());
  //MOZ_ASSERT(mFormat != gfx::FORMAT_YUV, "This TextureClient cannot use YCbCr data");

  MOZ_ASSERT(aSize.width >= 0 && aSize.height >= 0);
  if (aSize.width <= 0 || aSize.height <= 0) {
    gfxDebug() << "Asking for X11 surface of invalid size " << aSize.width << "x" << aSize.height;
    return false;
  }
  gfxImageFormat imageFormat = SurfaceFormatToImageFormat(mFormat);
  nsRefPtr<gfxASurface> surface = gfxPlatform::GetPlatform()->CreateOffscreenSurface(aSize, imageFormat);
  if (!surface || surface->GetType() != gfxSurfaceType::Xlib) {
    NS_ERROR("creating Xlib surface failed!");
    return false;
  }

  mSize = aSize;
  mSurface = static_cast<gfxXlibSurface*>(surface.get());

  if (!mAllocator->IsSameProcess()) {
    FinishX(DefaultXDisplay());
  }

  return true;
}

DrawTarget*
TextureClientX11::BorrowDrawTarget()
{
  MOZ_ASSERT(IsValid());
  MOZ_ASSERT(mLocked);

  if (!mSurface) {
    return nullptr;
  }

  if (!mDrawTarget) {
    IntSize size = mSurface->GetSize();
    mDrawTarget = Factory::CreateDrawTargetForCairoSurface(mSurface->CairoSurface(), size);
  }

  return mDrawTarget;
}

void
TextureClientX11::UpdateFromSurface(gfx::SourceSurface* aSurface)
{
  MOZ_ASSERT(CanExposeDrawTarget());

  DrawTarget* dt = BorrowDrawTarget();

  if (!dt) {
    gfxCriticalError() << "Failed to borrow drawtarget for TextureClientX11::UpdateFromSurface";
    return;
  }

  dt->CopySurface(aSurface, IntRect(IntPoint(), aSurface->GetSize()), IntPoint());
}