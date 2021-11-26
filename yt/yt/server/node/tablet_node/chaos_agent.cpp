#include "chaos_agent.h"

#include "private.h"
#include "tablet.h"
#include "tablet_slot.h"

#include <yt/yt/server/lib/tablet_node/config.h>

#include <yt/yt/ytlib/api/native/client.h>
#include <yt/yt/ytlib/api/native/connection.h>

#include <yt/yt/core/concurrency/delayed_executor.h>

#include <yt/yt/client/chaos_client/replication_card_cache.h>
#include <yt/yt/client/chaos_client/replication_card_serialization.h>

#include <util/generic/cast.h>

namespace NYT::NTabletNode {

using namespace NApi;
using namespace NChaosClient;
using namespace NTransactionClient;
using namespace NYTree;
using namespace NObjectClient;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

class TChaosAgent
    : public IChaosAgent
{
public:
    TChaosAgent(
        TTablet* tablet,
        ITabletSlotPtr slot,
        const TReplicationCardToken& replicationCardToken,
        NNative::IConnectionPtr localConnection)
        : Tablet_(tablet)
        , Slot_(std::move(slot))
        , MountConfig_(tablet->GetSettings().MountConfig)
        , ReplicationCardToken_(replicationCardToken)
        , Connection_(std::move(localConnection))
        , Logger(TabletNodeLogger
            .WithTag("%v", tablet->GetLoggingTag())
            .WithTag("ReplicationCardToken", replicationCardToken))
    { }

    void Enable() override
    {
        FiberFuture_ = BIND(&TChaosAgent::FiberMain, MakeWeak(this))
            .AsyncVia(Tablet_->GetEpochAutomatonInvoker())
            .Run();

        YT_LOG_INFO("Chaos agent fiber started");
    }

    void Disable() override
    {
        if (FiberFuture_) {
            FiberFuture_.Cancel(TError("Chaos agent disabled"));
            YT_LOG_INFO("Chaos agent fiber stopped");
        }
        FiberFuture_.Reset();
    }

private:
    TTablet* const Tablet_;
    const ITabletSlotPtr Slot_;
    const TTableMountConfigPtr MountConfig_;
    const TReplicationCardToken ReplicationCardToken_;
    const NNative::IConnectionPtr Connection_;

    TReplicationCardPtr ReplicationCard_;

    const NLogging::TLogger Logger;

    TFuture<void> FiberFuture_;

    void FiberMain()
    {
        while (true) {
            NProfiling::TWallTimer timer;
            FiberIteration();
            TDelayedExecutor::WaitForDuration(MountConfig_->ReplicationTickPeriod - timer.GetElapsedTime());
        }
    }

    void FiberIteration()
    {
        UpdateReplicationCard();
        ReconfigureTabletWriteMode();
        ReportUpdatedReplicationProgress();
    }

    void UpdateReplicationCard()
    {
        try {
            YT_LOG_DEBUG("Updating tablet replication card");

            const auto& replicationCardCache = Connection_->GetReplicationCardCache();

            ReplicationCard_ = WaitFor(replicationCardCache->GetReplicationCard({
                    .Token = ReplicationCardToken_,
                    .RequestHistory = true,
                    .RequestProgress = true
                })).ValueOrThrow();

            Tablet_->ReplicationCard() = ReplicationCard_;

            YT_LOG_DEBUG("Tablet replication card updated (ReplicationCard: %v)",
                *ReplicationCard_);
        } catch (std::exception& ex) {
            YT_LOG_DEBUG(ex, "Failed to update tablet replication card");
        }
    }

    void ReconfigureTabletWriteMode()
    {
        auto* selfReplica = ReplicationCard_->FindReplica(Tablet_->GetUpstreamReplicaId());
        if (!selfReplica) {
            YT_LOG_DEBUG("Could not find self replica in replication card");

            Tablet_->RuntimeData()->WriteMode = ETabletWriteMode::Pull;
            Tablet_->RuntimeData()->ReplicationEra = ReplicationCard_->Era;
            return;
        }

        ETabletWriteMode writeMode = ETabletWriteMode::Pull;
        auto progress = Tablet_->RuntimeData()->ReplicationProgress.Load();

        YT_LOG_DEBUG("Checking self write mode (ReplicationProgress: %v, LastHistoryItemTimestamp: %llx, IsProgressGreaterThanTimestamp: %llx)",
            static_cast<TReplicationProgress>(*progress),
            selfReplica->History.back().Timestamp,
            IsReplicationProgressGreaterOrEqual(*progress, selfReplica->History.back().Timestamp));

        if (IsReplicaReallySync(selfReplica->Mode, selfReplica->State) &&
            IsReplicationProgressGreaterOrEqual(*progress, selfReplica->History.back().Timestamp))
        {
            writeMode = ETabletWriteMode::Direct;
        }

        Tablet_->RuntimeData()->WriteMode = writeMode;
        Tablet_->RuntimeData()->ReplicationEra = ReplicationCard_->Era;

        YT_LOG_DEBUG("Updated tablet write mode (WriteMode: %v, ReplicationEra: %v)",
            writeMode,
            ReplicationCard_->Era);

        auto forwardReplicationProgress = [&] (int historyItemIndex) {
            YT_VERIFY(historyItemIndex  < std::ssize(selfReplica->History));
            *progress = AdvanceReplicationProgress(
                *progress,
                selfReplica->History[historyItemIndex].Timestamp);
            Tablet_->RuntimeData()->ReplicationProgress.Store(progress);

            YT_LOG_DEBUG("Advance replication progress to next era (Era: %v, Timestamp: %llx, ReplicationProgress: %v)",
                selfReplica->History[historyItemIndex].Era,
                selfReplica->History[historyItemIndex].Timestamp,
                static_cast<NChaosClient::TReplicationProgress>(*progress));
        };

        if (writeMode == ETabletWriteMode::Pull) {
            auto oldestTimestamp = GetReplicationProgressMinTimestamp(*progress);
            auto historyItemIndex = selfReplica->FindHistoryItemIndex(oldestTimestamp);

            YT_LOG_DEBUG("Rplica is in pulling mode, consider jumping (ReplicaMode: %v, OldestTimestmap: %llx, HistoryItemIndex: %v)",
                ETabletWriteMode::Pull,
                oldestTimestamp,
                historyItemIndex);

            if (historyItemIndex == -1) {
                if (selfReplica->History[0].Era == InitialReplicationEra) {
                    forwardReplicationProgress(0);
                } else {
                    YT_LOG_WARNING("Invalid replication card: replica history does not cover its progress (ReplicationProgress: %v, Replica: %v, Timestamp: %llx)",
                        static_cast<NChaosClient::TReplicationProgress>(*progress),
                        *selfReplica,
                        oldestTimestamp);
                }
            } else if (const auto& item = selfReplica->History[historyItemIndex]; IsReplicaReallySync(item.Mode, item.State)) {
                forwardReplicationProgress(historyItemIndex + 1);
            }
        }

        auto barrierTimestamp = Slot_->GetRuntimeData()->BarrierTimestamp.load();
        if (writeMode == ETabletWriteMode::Direct &&
            !IsReplicationProgressGreaterOrEqual(*progress, barrierTimestamp))
        {
            *progress = AdvanceReplicationProgress(*progress, barrierTimestamp);
            Tablet_->RuntimeData()->ReplicationProgress.Store(progress);

            YT_LOG_DEBUG("Advance replication progress to barrier (BarrierTimestamp: %llx, ReplicationProgress: %v)",
                barrierTimestamp,
                static_cast<NChaosClient::TReplicationProgress>(*progress));
        }
    }

    void ReportUpdatedReplicationProgress()
    {
        auto client = Connection_->CreateNativeClient(TClientOptions::FromUser(NSecurityClient::ReplicatorUserName));

        auto options = TUpdateReplicationProgressOptions{
            .Progress = *Tablet_->RuntimeData()->ReplicationProgress.Load()
        };
        auto future = client->UpdateReplicationProgress(
            ReplicationCardToken_,
            Tablet_->GetUpstreamReplicaId(),
            options);
        auto resultOrError = WaitFor(future);

        if (resultOrError.IsOK()) {
            YT_LOG_DEBUG("Replication progress updated successfully (ReplicationProgress: %v)",
                options.Progress);
        } else {
            YT_LOG_ERROR(resultOrError, "Failed to update replication progress");
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

IChaosAgentPtr CreateChaosAgent(
    TTablet* tablet,
    ITabletSlotPtr slot,
    const TReplicationCardToken& replicationCardToken,
    NNative::IConnectionPtr localConnection)
{
    return New<TChaosAgent>(
        tablet,
        std::move(slot),
        replicationCardToken,
        std::move(localConnection));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletNode