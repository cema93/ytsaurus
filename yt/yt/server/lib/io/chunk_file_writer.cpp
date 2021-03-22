#include "chunk_file_writer.h"
#include "io_engine.h"
#include "private.h"

#include <yt/yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/yt/ytlib/chunk_client/deferred_chunk_meta.h>
#include <yt/yt/ytlib/chunk_client/format.h>
#include <yt/yt/ytlib/chunk_client/block.h>

#include <yt/yt/client/chunk_client/chunk_replica.h>

#include <yt/yt/core/misc/fs.h>
#include <yt/yt/core/misc/checksum.h>

#include <util/system/align.h>
#include <util/system/compiler.h>

namespace NYT::NIO {

using namespace NConcurrency;
using namespace NChunkClient;
using namespace NChunkClient::NProto;

////////////////////////////////////////////////////////////////////////////////

static constexpr auto FileMode =
    CreateAlways |
    WrOnly |
    Seq |
    CloseOnExec |
    AR |
    AWUser |
    AWGroup;

static const auto& Logger = IOLogger;

////////////////////////////////////////////////////////////////////////////////

TChunkFileWriter::TChunkFileWriter(
    IIOEnginePtr ioEngine,
    TChunkId chunkId,
    TString fileName,
    bool syncOnClose)
    : IOEngine_(std::move(ioEngine))
    , ChunkId_(chunkId)
    , FileName_(std::move(fileName))
    , SyncOnClose_(syncOnClose)
{
    BlocksExt_.set_sync_on_close(SyncOnClose_);
}

void TChunkFileWriter::TryLockDataFile(TPromise<void> promise)
{
    YT_VERIFY(State_.load() == EState::Opening);

    if (DataFile_->Flock(LOCK_EX | LOCK_NB) >= 0) {
        promise.Set();
        return;
    }

    if (errno != EWOULDBLOCK) {
        promise.Set(TError::FromSystem(errno));
        return;
    }

    YT_LOG_WARNING("Error locking chunk data file, retrying (Path: %v)",
        FileName_);

    TDelayedExecutor::Submit(
        BIND(&TChunkFileWriter::TryLockDataFile, MakeStrong(this), promise),
        TDuration::MilliSeconds(10),
        IOEngine_->GetAuxPoolInvoker());
}

TFuture<void> TChunkFileWriter::Open()
{
    YT_VERIFY(State_.exchange(EState::Opening) == EState::Created);

    // NB: Races are possible between file creation and a call to flock.
    // Unfortunately in Linux we can't create'n'flock a file atomically.
    return IOEngine_->Open({FileName_ + NFS::TempFileSuffix, FileMode})
        .Apply(BIND([=, this_ = MakeStrong(this)] (const std::shared_ptr<TFileHandle>& file) {
            YT_VERIFY(State_.load() == EState::Opening);

            DataFile_ = file;

            auto promise = NewPromise<void>();
            TryLockDataFile(promise);
            return promise.ToFuture();
        }).AsyncVia(IOEngine_->GetAuxPoolInvoker()))
        .Apply(BIND([=, this_ = MakeStrong(this)] (const TError& error) {
            YT_VERIFY(State_.load() == EState::Opening);

            if (!error.IsOK()) {
                State_.store(EState::Failed);
                THROW_ERROR_EXCEPTION("Failed to open chunk data file %v",
                    FileName_)
                    << error;
            }

            State_.store(EState::Ready);
        }));
}

bool TChunkFileWriter::WriteBlock(const TBlock& block)
{
    return WriteBlocks({block});
}

bool TChunkFileWriter::WriteBlocks(const std::vector<TBlock>& blocks)
{
    YT_VERIFY(State_.exchange(EState::WritingBlocks) == EState::Ready);

    i64 startOffset = DataSize_;
    i64 currentOffset = startOffset;

    std::vector<TSharedRef> buffers;
    buffers.reserve(blocks.size());

    for (const auto& block : blocks) {
        auto error = block.ValidateChecksum();
        YT_LOG_FATAL_UNLESS(error.IsOK(), error, "Block checksum mismatch during file writing");

        auto* blockInfo = BlocksExt_.add_blocks();
        blockInfo->set_offset(currentOffset);
        blockInfo->set_size(static_cast<int>(block.Size()));
        blockInfo->set_checksum(block.GetOrComputeChecksum());

        currentOffset += block.Size();
        buffers.push_back(block.Data);
    }

    ReadyEvent_ =
        IOEngine_->Write({
            *DataFile_,
            startOffset,
            std::move(buffers)
        })
        .Apply(BIND([=, this_ = MakeStrong(this), newDataSize = currentOffset] (const TError& error) {
            YT_VERIFY(State_.load() == EState::WritingBlocks);

            if (!error.IsOK()) {
                State_.store(EState::Failed);
                THROW_ERROR_EXCEPTION("Failed to write chunk data file %v",
                    FileName_)
                    << error;
            }

            DataSize_ = newDataSize;
            State_.store(EState::Ready);
        }));

    return false;
}

TFuture<void> TChunkFileWriter::GetReadyEvent()
{
    auto state = State_.load();
    YT_VERIFY(state == EState::WritingBlocks || state == EState::Ready);

    return ReadyEvent_;
}

TFuture<void> TChunkFileWriter::Close(const TDeferredChunkMetaPtr& chunkMeta)
{
    YT_VERIFY(State_.exchange(EState::Closing) == EState::Ready);

    auto metaFileName = FileName_ + ChunkMetaSuffix;
    return IOEngine_->Close({std::move(DataFile_), DataSize_, SyncOnClose_})
        .Apply(BIND([=, _this = MakeStrong(this)] {
            YT_VERIFY(State_.load() == EState::Closing);

            if (!chunkMeta->IsFinalized()) {
                chunkMeta->Finalize();
            }
            ChunkMeta_->CopyFrom(*chunkMeta);
            SetProtoExtension(ChunkMeta_->mutable_extensions(), BlocksExt_);

            return IOEngine_->Open({metaFileName + NFS::TempFileSuffix, FileMode});
        }))
        .Apply(BIND([=, _this = MakeStrong(this)] (const std::shared_ptr<TFileHandle>& chunkMetaFile) {
            YT_VERIFY(State_.load() == EState::Closing);

            auto metaData = SerializeProtoToRefWithEnvelope(*ChunkMeta_);

            TChunkMetaHeader_2 header;
            header.Signature = header.ExpectedSignature;
            header.Checksum = GetChecksum(metaData);
            header.ChunkId = ChunkId_;

            MetaDataSize_ = metaData.Size() + sizeof(header);

            struct TMetaBufferTag
            { };

            auto buffer = TSharedMutableRef::Allocate<TMetaBufferTag>(MetaDataSize_, false);
            ::memcpy(buffer.Begin(), &header, sizeof(header));
            ::memcpy(buffer.Begin() + sizeof(header), metaData.Begin(), metaData.Size());

            return
                IOEngine_->Write({
                    *chunkMetaFile,
                    0,
                    {std::move(buffer)}
                })
                .Apply(BIND(&IIOEngine::Close, IOEngine_, IIOEngine::TCloseRequest{
                    std::move(chunkMetaFile),
                    MetaDataSize_,
                    SyncOnClose_
                },
                IIOEngine::DefaultPriority));
        }))
        .Apply(BIND([=, _this = MakeStrong(this)] () {
            YT_VERIFY(State_.load() == EState::Closing);

            NFS::Rename(metaFileName + NFS::TempFileSuffix, metaFileName);
            NFS::Rename(FileName_ + NFS::TempFileSuffix, FileName_);

            if (!SyncOnClose_) {
                return VoidFuture;
            }

            return IOEngine_->FlushDirectory({NFS::GetDirectoryName(FileName_)});
        }).AsyncVia(IOEngine_->GetAuxPoolInvoker()))
        .Apply(BIND([this, _this = MakeStrong(this)] (const TError& error) {
            YT_VERIFY(State_.load() == EState::Closing);

            if (!error.IsOK()) {
                State_.store(EState::Failed);
                THROW_ERROR_EXCEPTION("Failed to close chunk data file %v",
                    FileName_)
                    << error;
            }

            ChunkInfo_.set_disk_space(DataSize_ + MetaDataSize_);
            State_.store(EState::Closed);
        }));
}

i64 TChunkFileWriter::GetDataSize() const
{
    return DataSize_;
}

const TString& TChunkFileWriter::GetFileName() const
{
    return FileName_;
}

TFuture<void> TChunkFileWriter::Abort()
{
    auto state = State_.exchange(EState::Aborting);
    YT_VERIFY(
        state != EState::Opening &&
        state != EState::WritingBlocks &&
        state != EState::Closing);

    return
        BIND([=, _this = MakeStrong(this)] {
            YT_VERIFY(State_.load() == EState::Aborting);

            DataFile_.reset();

            auto removeIfExists = [] (const TString& path) {
                if (NFS::Exists(path)) {
                    NFS::Remove(path);
                }
            };
            removeIfExists(FileName_ + NFS::TempFileSuffix);
            removeIfExists(FileName_ + ChunkMetaSuffix + NFS::TempFileSuffix);

            State_.store(EState::Aborted);
        })
        .AsyncVia(IOEngine_->GetAuxPoolInvoker())
        .Run();
}

const TChunkInfo& TChunkFileWriter::GetChunkInfo() const
{
    YT_VERIFY(State_.load() == EState::Closed);

    return ChunkInfo_;
}

const TDataStatistics& TChunkFileWriter::GetDataStatistics() const
{
    YT_VERIFY(State_.load() == EState::Closed);

    YT_ABORT();
}

const TRefCountedChunkMetaPtr& TChunkFileWriter::GetChunkMeta() const
{
    YT_VERIFY(State_.load() == EState::Closed);

    return ChunkMeta_;
}

TChunkReplicaWithMediumList TChunkFileWriter::GetWrittenChunkReplicas() const
{
    YT_UNIMPLEMENTED();
}

TChunkId TChunkFileWriter::GetChunkId() const
{
    return ChunkId_;
}

NErasure::ECodec TChunkFileWriter::GetErasureCodecId() const
{
    return NErasure::ECodec::None;
}

bool TChunkFileWriter::IsCloseDemanded() const
{
    YT_UNIMPLEMENTED();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NIO
