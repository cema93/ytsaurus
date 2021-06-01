#include "chaos_slot.h"

#include "automaton.h"
#include "private.h"
#include "serialize.h"
#include "slot_manager.h"
#include "chaos_manager.h"
#include "transaction_manager.h"

#include <yt/yt/server/lib/cellar_agent/automaton_invoker_hood.h>
#include <yt/yt/server/lib/cellar_agent/occupant.h>

#include <yt/yt/server/lib/hive/public.h>

#include <yt/yt/server/lib/hydra/public.h>
#include <yt/yt/server/lib/hydra/distributed_hydra_manager.h>

#include <yt/yt/server/lib/chaos_node/config.h>

#include <yt/yt/server/node/cluster_node/bootstrap.h>

#include <yt/yt/ytlib/api/public.h>

#include <yt/yt/core/concurrency/fair_share_action_queue.h>
#include <yt/yt/core/concurrency/thread_affinity.h>

#include <yt/yt/core/ytree/virtual.h>
#include <yt/yt/core/ytree/helpers.h>

namespace NYT::NChaosNode {

using namespace NCellarAgent;
using namespace NCellarClient;
using namespace NClusterNode;
using namespace NConcurrency;
using namespace NHiveClient;
using namespace NHiveServer;
using namespace NHydra;
using namespace NObjectClient;
using namespace NYTree;
using namespace NYson;

using NHydra::EPeerState;

////////////////////////////////////////////////////////////////////////////////

class TChaosSlot
    : public IChaosSlot
    , public TAutomatonInvokerHood<EAutomatonThreadQueue>
{
    using THood = TAutomatonInvokerHood<EAutomatonThreadQueue>;

public:
    TChaosSlot(
        int slotIndex,
        TChaosNodeConfigPtr config,
        TBootstrap* bootstrap)
        : THood(Format("ChaosSlot:%v", slotIndex))
        , Config_(config)
        , Bootstrap_(bootstrap)
        , SnapshotQueue_(New<TActionQueue>(
            Format("ChaosSnap:%v", slotIndex)))
        , Logger(ChaosNodeLogger)
    {
        VERIFY_INVOKER_THREAD_AFFINITY(GetAutomatonInvoker(), AutomatonThread);

        ResetEpochInvokers();
        ResetGuardedInvokers();
    }

    virtual void SetOccupant(ICellarOccupantPtr occupant) override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        YT_VERIFY(!Occupant_);

        Occupant_ = std::move(occupant);
        Logger.AddTag("CellId: %v, PeerId: %v",
            Occupant_->GetCellId(),
            Occupant_->GetPeerId());
    }

    virtual TCellId GetCellId() const override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return Occupant_->GetCellId();
    }

    virtual EPeerState GetAutomatonState() const override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto hydraManager = GetHydraManager();
        return hydraManager ? hydraManager->GetAutomatonState() : EPeerState::None;
    }

    virtual IDistributedHydraManagerPtr GetHydraManager() const override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return Occupant_->GetHydraManager();
    }

    virtual const TCompositeAutomatonPtr& GetAutomaton() const override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return Occupant_->GetAutomaton();
    }

    virtual const THiveManagerPtr& GetHiveManager() const override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return Occupant_->GetHiveManager();
    }

    virtual TMailbox* GetMasterMailbox() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        return Occupant_->GetMasterMailbox();
    }

    virtual ITransactionManagerPtr GetTransactionManager() const override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return TransactionManager_;
    }

    virtual NHiveServer::ITransactionManagerPtr GetOccupierTransactionManager() override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return TransactionManager_;
    }

    virtual const ITransactionSupervisorPtr& GetTransactionSupervisor() const override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return Occupant_->GetTransactionSupervisor();
    }

    virtual const IChaosManagerPtr& GetChaosManager() const override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return ChaosManager_;
    }

    virtual TObjectId GenerateId(EObjectType type) override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        return Occupant_->GenerateId(type);
    }

    virtual TCompositeAutomatonPtr CreateAutomaton() override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return New<TChaosAutomaton>(
            this,
            SnapshotQueue_->GetInvoker());
    }

    virtual void Configure(IDistributedHydraManagerPtr hydraManager) override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        hydraManager->SubscribeStartLeading(BIND(&TChaosSlot::OnStartEpoch, MakeWeak(this)));
        hydraManager->SubscribeStartFollowing(BIND(&TChaosSlot::OnStartEpoch, MakeWeak(this)));

        hydraManager->SubscribeStopLeading(BIND(&TChaosSlot::OnStopEpoch, MakeWeak(this)));
        hydraManager->SubscribeStopFollowing(BIND(&TChaosSlot::OnStopEpoch, MakeWeak(this)));

        InitGuardedInvokers(hydraManager);

        ChaosManager_ = CreateChaosManager(
            Config_->ChaosManager,
            this,
            Bootstrap_);

        TransactionManager_ = CreateTransactionManager(
            Config_->TransactionManager,
            this,
            Bootstrap_);
    }

    virtual void Initialize() override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        ChaosManager_->Initialize();
    }

    virtual void RegisterRpcServices() override
    { }

    virtual void Stop() override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        ResetEpochInvokers();
        ResetGuardedInvokers();
    }

    virtual void Finalize() override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        ChaosManager_.Reset();

        TransactionManager_.Reset();

        if (ChaosService_) {
            const auto& rpcServer = Bootstrap_->GetRpcServer();
            rpcServer->UnregisterService(ChaosService_);
        }
        ChaosService_.Reset();

        if (CoordinatorService_) {
            const auto& rpcServer = Bootstrap_->GetRpcServer();
            rpcServer->UnregisterService(CoordinatorService_);
        }
        CoordinatorService_.Reset();
    }

    virtual TCompositeMapServicePtr PopulateOrchidService(TCompositeMapServicePtr orchid) override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return orchid
            ->AddChild("transactions", TransactionManager_->GetOrchidService())
            ->AddChild("chaos", ChaosManager_->GetOrchidService());
    }

    virtual NProfiling::TRegistry GetProfiler() override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return ChaosNodeProfiler;
    }

    virtual IInvokerPtr GetAutomatonInvoker(EAutomatonThreadQueue queue = EAutomatonThreadQueue::Default) const override
    {
        return TAutomatonInvokerHood<EAutomatonThreadQueue>::GetAutomatonInvoker(queue);
    }

    virtual IInvokerPtr GetEpochAutomatonInvoker(EAutomatonThreadQueue queue = EAutomatonThreadQueue::Default) const override
    {
        return TAutomatonInvokerHood<EAutomatonThreadQueue>::GetEpochAutomatonInvoker(queue);
    }

    virtual IInvokerPtr GetGuardedAutomatonInvoker(EAutomatonThreadQueue queue = EAutomatonThreadQueue::Default) const override
    {
        return TAutomatonInvokerHood<EAutomatonThreadQueue>::GetGuardedAutomatonInvoker(queue);
    }

    virtual IInvokerPtr GetOccupierAutomatonInvoker() override
    {
        return GetAutomatonInvoker();
    }

    virtual IInvokerPtr GetMutationAutomatonInvoker() override
    {
        return GetAutomatonInvoker(EAutomatonThreadQueue::Mutation);
    }

    virtual ECellarType GetCellarType() override
    {
        return IChaosSlot::CellarType;
    }

private:
    const TChaosNodeConfigPtr Config_;
    TBootstrap* const Bootstrap_;

    ICellarOccupantPtr Occupant_;

    const TActionQueuePtr SnapshotQueue_;

    TCellDescriptor CellDescriptor_;

    const NProfiling::TTagIdList ProfilingTagIds_;

    IChaosManagerPtr ChaosManager_;

    ITransactionManagerPtr TransactionManager_;

    NRpc::IServicePtr ChaosService_;
    NRpc::IServicePtr CoordinatorService_;

    IYPathServicePtr OrchidService_;

    NLogging::TLogger Logger;


    void OnStartEpoch()
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto hydraManager = GetHydraManager();
        if (!hydraManager) {
            return;
        }

        InitEpochInvokers(hydraManager);
    }

    void OnStopEpoch()
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        ResetEpochInvokers();
    }

    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);
    DECLARE_THREAD_AFFINITY_SLOT(AutomatonThread);
};

////////////////////////////////////////////////////////////////////////////////

IChaosSlotPtr CreateChaosSlot(
    int slotIndex,
    TChaosNodeConfigPtr config,
    TBootstrap* bootstrap)
{
    return New<TChaosSlot>(
        slotIndex,
        config,
        bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChaosNode