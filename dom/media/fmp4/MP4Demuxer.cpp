/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <algorithm>
#include <limits>
#include <stdint.h>

#include "MP4Demuxer.h"
#include "mp4_demuxer/MoofParser.h"
#include "mp4_demuxer/MP4Metadata.h"
#include "mp4_demuxer/ResourceStream.h"
#include "mp4_demuxer/BufferStream.h"
#include "mp4_demuxer/Index.h"

PRLogModuleInfo* GetDemuxerLog() {
  static PRLogModuleInfo* log = nullptr;
  if (!log) {
    log = PR_NewLogModule("MP4Demuxer");
  }
  return log;
}

namespace mozilla {

class MP4TrackDemuxer : public MediaTrackDemuxer
{
public:
  MP4TrackDemuxer(MP4Demuxer* aParent,
                  UniquePtr<TrackInfo>&& aInfo,
                  const nsTArray<mp4_demuxer::Index::Indice>& indices);

  virtual UniquePtr<TrackInfo> GetInfo() const override;

  virtual nsRefPtr<SeekPromise> Seek(media::TimeUnit aTime) override;

  virtual nsRefPtr<SamplesPromise> GetSamples(int32_t aNumSamples = 1) override;

  virtual void Reset() override;

  virtual nsresult GetNextRandomAccessPoint(media::TimeUnit* aTime) override;

  nsRefPtr<SkipAccessPointPromise> SkipToNextRandomAccessPoint(media::TimeUnit aTimeThreshold) override;

  virtual media::TimeIntervals GetBuffered() override;

  virtual void BreakCycles() override;

private:
  friend class MP4Demuxer;
  void NotifyDataArrived();
  void UpdateSamples(nsTArray<nsRefPtr<MediaRawData>>& aSamples);
  void EnsureUpToDateIndex();
  void SetNextKeyFrameTime();
  nsRefPtr<MP4Demuxer> mParent;
  nsRefPtr<mp4_demuxer::ResourceStream> mStream;
  UniquePtr<TrackInfo> mInfo;
  // We do not actually need a monitor, however MoofParser (in mIndex) will
  // assert if a monitor isn't held.
  Monitor mMonitor;
  nsRefPtr<mp4_demuxer::Index> mIndex;
  UniquePtr<mp4_demuxer::SampleIterator> mIterator;
  Maybe<media::TimeUnit> mNextKeyframeTime;
  // Queued samples extracted by the demuxer, but not yet returned.
  nsRefPtr<MediaRawData> mQueuedSample;
  bool mNeedReIndex;
};

MP4Demuxer::MP4Demuxer(MediaResource* aResource)
  : mResource(aResource)
  , mStream(new mp4_demuxer::ResourceStream(aResource))
  , mInitData(new MediaByteBuffer)
{
}

nsRefPtr<MP4Demuxer::InitPromise>
MP4Demuxer::Init()
{
  AutoPinned<mp4_demuxer::ResourceStream> stream(mStream);

  // Check that we have enough data to read the metadata.
  if (!mp4_demuxer::MP4Metadata::HasCompleteMetadata(stream)) {
    return InitPromise::CreateAndReject(DemuxerFailureReason::DEMUXER_ERROR, __func__);
  }

  mInitData = mp4_demuxer::MP4Metadata::Metadata(stream);
  if (!mInitData) {
    // OOM
    return InitPromise::CreateAndReject(DemuxerFailureReason::DEMUXER_ERROR, __func__);
  }

  nsRefPtr<mp4_demuxer::BufferStream> bufferstream =
    new mp4_demuxer::BufferStream(mInitData);

  mMetadata = MakeUnique<mp4_demuxer::MP4Metadata>(bufferstream);

  if (!mMetadata->GetNumberTracks(mozilla::TrackInfo::kAudioTrack) &&
      !mMetadata->GetNumberTracks(mozilla::TrackInfo::kVideoTrack)) {
    return InitPromise::CreateAndReject(DemuxerFailureReason::DEMUXER_ERROR, __func__);
  }

  return InitPromise::CreateAndResolve(NS_OK, __func__);
}

bool
MP4Demuxer::HasTrackType(TrackInfo::TrackType aType) const
{
  return !!GetNumberTracks(aType);
}

uint32_t
MP4Demuxer::GetNumberTracks(TrackInfo::TrackType aType) const
{
  return mMetadata->GetNumberTracks(aType);
}

already_AddRefed<MediaTrackDemuxer>
MP4Demuxer::GetTrackDemuxer(TrackInfo::TrackType aType, uint32_t aTrackNumber)
{
  if (mMetadata->GetNumberTracks(aType) <= aTrackNumber) {
    return nullptr;
  }
  UniquePtr<TrackInfo> info = mMetadata->GetTrackInfo(aType, aTrackNumber);
  if (!info) {
    return nullptr;
  }
  FallibleTArray<mp4_demuxer::Index::Indice> indices;
  if (!mMetadata->ReadTrackIndex(indices, info->mTrackId)) {
    return nullptr;
  }
  nsRefPtr<MP4TrackDemuxer> e = new MP4TrackDemuxer(this, Move(info), indices);
  mDemuxers.AppendElement(e);

  return e.forget();
}

bool
MP4Demuxer::IsSeekable() const
{
  return mMetadata->CanSeek();
}

void
MP4Demuxer::NotifyDataArrived()
{
  for (uint32_t i = 0; i < mDemuxers.Length(); i++) {
    mDemuxers[i]->NotifyDataArrived();
  }
}

void
MP4Demuxer::NotifyDataRemoved()
{
  for (uint32_t i = 0; i < mDemuxers.Length(); i++) {
    mDemuxers[i]->NotifyDataArrived();
  }
}

UniquePtr<EncryptionInfo>
MP4Demuxer::GetCrypto()
{
  const mp4_demuxer::CryptoFile& cryptoFile = mMetadata->Crypto();
  if (!cryptoFile.valid) {
    return nullptr;
  }

  const nsTArray<mp4_demuxer::PsshInfo>& psshs = cryptoFile.pssh;
  nsTArray<uint8_t> initData;
  for (uint32_t i = 0; i < psshs.Length(); i++) {
    initData.AppendElements(psshs[i].data);
  }

  if (initData.IsEmpty()) {
    return nullptr;
  }

  auto crypto = MakeUnique<EncryptionInfo>();
  crypto->AddInitData(NS_LITERAL_STRING("cenc"), Move(initData));

  return crypto;
}

MP4TrackDemuxer::MP4TrackDemuxer(MP4Demuxer* aParent,
                                 UniquePtr<TrackInfo>&& aInfo,
                                 const nsTArray<mp4_demuxer::Index::Indice>& indices)
  : mParent(aParent)
  , mStream(new mp4_demuxer::ResourceStream(mParent->mResource))
  , mInfo(Move(aInfo))
  , mMonitor("MP4TrackDemuxer")
  , mIndex(new mp4_demuxer::Index(indices,
                                  mStream,
                                  mInfo->mTrackId,
                                  mInfo->IsAudio(),
                                  &mMonitor))
  , mIterator(MakeUnique<mp4_demuxer::SampleIterator>(mIndex))
  , mNeedReIndex(true)
{
  EnsureUpToDateIndex(); // Force update of index
}

UniquePtr<TrackInfo>
MP4TrackDemuxer::GetInfo() const
{
  return mInfo->Clone();
}

void
MP4TrackDemuxer::EnsureUpToDateIndex()
{
  if (!mNeedReIndex) {
    return;
  }
  AutoPinned<MediaResource> resource(mParent->mResource);
  nsTArray<MediaByteRange> byteRanges;
  nsresult rv = resource->GetCachedRanges(byteRanges);
  if (NS_FAILED(rv)) {
    return;
  }
  MonitorAutoLock mon(mMonitor);
  mIndex->UpdateMoofIndex(byteRanges);
  mNeedReIndex = false;
}

nsRefPtr<MP4TrackDemuxer::SeekPromise>
MP4TrackDemuxer::Seek(media::TimeUnit aTime)
{
  int64_t seekTime = aTime.ToMicroseconds();
  mQueuedSample = nullptr;

  MonitorAutoLock mon(mMonitor);
  mIterator->Seek(seekTime);

  // Check what time we actually seeked to.
  mQueuedSample = mIterator->GetNext();
  if (mQueuedSample) {
    seekTime = mQueuedSample->mTime;
  }
  SetNextKeyFrameTime();

  return SeekPromise::CreateAndResolve(media::TimeUnit::FromMicroseconds(seekTime), __func__);
}

nsRefPtr<MP4TrackDemuxer::SamplesPromise>
MP4TrackDemuxer::GetSamples(int32_t aNumSamples)
{
  EnsureUpToDateIndex();
  nsRefPtr<SamplesHolder> samples = new SamplesHolder;
  if (!aNumSamples) {
    return SamplesPromise::CreateAndReject(DemuxerFailureReason::DEMUXER_ERROR, __func__);
  }

  if (mQueuedSample) {
    samples->mSamples.AppendElement(mQueuedSample);
    mQueuedSample = nullptr;
    aNumSamples--;
  }
  MonitorAutoLock mon(mMonitor);
  nsRefPtr<MediaRawData> sample;
  while (aNumSamples && (sample = mIterator->GetNext())) {
    samples->mSamples.AppendElement(sample);
    aNumSamples--;
  }

  if (samples->mSamples.IsEmpty()) {
    return SamplesPromise::CreateAndReject(DemuxerFailureReason::END_OF_STREAM, __func__);
  } else {
    UpdateSamples(samples->mSamples);
    return SamplesPromise::CreateAndResolve(samples, __func__);
  }
}

void
MP4TrackDemuxer::SetNextKeyFrameTime()
{
  mNextKeyframeTime.reset();
  mp4_demuxer::Microseconds frameTime = mIterator->GetNextKeyframeTime();
  if (frameTime != -1) {
    mNextKeyframeTime.emplace(
      media::TimeUnit::FromMicroseconds(frameTime));
  }
}

void
MP4TrackDemuxer::Reset()
{
  mQueuedSample = nullptr;
  // TODO, Seek to first frame available, which isn't always 0.
  MonitorAutoLock mon(mMonitor);
  mIterator->Seek(0);
  SetNextKeyFrameTime();
}

void
MP4TrackDemuxer::UpdateSamples(nsTArray<nsRefPtr<MediaRawData>>& aSamples)
{
  for (size_t i = 0; i < aSamples.Length(); i++) {
    MediaRawData* sample = aSamples[i];
    if (sample->mCrypto.valid) {
      nsAutoPtr<MediaRawDataWriter> writer(sample->CreateWriter());
      writer->mCrypto.mode = mInfo->mCrypto.mode;
      writer->mCrypto.iv_size = mInfo->mCrypto.iv_size;
      writer->mCrypto.key.AppendElements(mInfo->mCrypto.key);
    }
    if (mInfo->GetAsVideoInfo()) {
      sample->mExtraData = mInfo->GetAsVideoInfo()->mExtraData;
    }
  }
  if (mNextKeyframeTime.isNothing() ||
      aSamples.LastElement()->mTime >= mNextKeyframeTime.value().ToMicroseconds()) {
    SetNextKeyFrameTime();
  }
}

nsresult
MP4TrackDemuxer::GetNextRandomAccessPoint(media::TimeUnit* aTime)
{
  if (mNextKeyframeTime.isNothing()) {
    // There's no next key frame.
    *aTime =
      media::TimeUnit::FromMicroseconds(std::numeric_limits<int64_t>::max());
  } else {
    *aTime = mNextKeyframeTime.value();
  }
  return NS_OK;
}

nsRefPtr<MP4TrackDemuxer::SkipAccessPointPromise>
MP4TrackDemuxer::SkipToNextRandomAccessPoint(media::TimeUnit aTimeThreshold)
{
  MonitorAutoLock mon(mMonitor);
  mQueuedSample = nullptr;
  // Loop until we reach the next keyframe after the threshold.
  uint32_t parsed = 0;
  bool found = false;
  nsRefPtr<MediaRawData> sample;
  while (!found && (sample = mIterator->GetNext())) {
    parsed++;
    if (sample->mKeyframe && sample->mTime >= aTimeThreshold.ToMicroseconds()) {
      found = true;
      mQueuedSample = sample;
    }
  }
  SetNextKeyFrameTime();
  if (found) {
    return SkipAccessPointPromise::CreateAndResolve(parsed, __func__);
  } else {
    SkipFailureHolder failure(DemuxerFailureReason::END_OF_STREAM, parsed);
    return SkipAccessPointPromise::CreateAndReject(Move(failure), __func__);
  }
}

media::TimeIntervals
MP4TrackDemuxer::GetBuffered()
{
  EnsureUpToDateIndex();
  AutoPinned<MediaResource> resource(mParent->mResource);
  nsTArray<MediaByteRange> byteRanges;
  nsresult rv = resource->GetCachedRanges(byteRanges);

  if (NS_FAILED(rv)) {
    return media::TimeIntervals();
  }
  nsTArray<mp4_demuxer::Interval<int64_t>> timeRanges;

  MonitorAutoLock mon(mMonitor);
  mIndex->ConvertByteRangesToTimeRanges(byteRanges, &timeRanges);
  // convert timeRanges.
  media::TimeIntervals ranges = media::TimeIntervals();
  for (size_t i = 0; i < timeRanges.Length(); i++) {
    ranges +=
      media::TimeInterval(media::TimeUnit::FromMicroseconds(timeRanges[i].start),
                          media::TimeUnit::FromMicroseconds(timeRanges[i].end));
  }
  return ranges;
}

void
MP4TrackDemuxer::NotifyDataArrived()
{
  mNeedReIndex = true;
}

void
MP4TrackDemuxer::BreakCycles()
{
  mParent = nullptr;
}

} // namespace mozilla
