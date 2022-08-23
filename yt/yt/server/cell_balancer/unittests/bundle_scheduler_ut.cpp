#include <yt/yt/core/test_framework/framework.h>

#include <yt/yt/server/cell_balancer/bundle_scheduler.h>
#include <yt/yt/server/cell_balancer/config.h>

#include <yt/yt/core/ytree/yson_serializable.h>

#include <library/cpp/yt/memory/new.h>

namespace NYT::NCellBalancer {
namespace {

////////////////////////////////////////////////////////////////////////////////

TString GetPodIdForInstance(const TString& name)
{
    auto endPos = name.find(".");
    YT_VERIFY(endPos != TString::npos);

    return name.substr(0, endPos);
}

////////////////////////////////////////////////////////////////////////////////

TBundleInfoPtr SetBundleInfo(
    TSchedulerInputState& input,
    const TString& bundleName,
    int nodeCount,
    int writeThreadCount = 0,
    int proxyCount = 0)
{
    auto bundleInfo = New<TBundleInfo>();
    input.Bundles[bundleName] = bundleInfo;
    bundleInfo->Health = NTabletClient::ETabletCellHealth::Good;
    bundleInfo->Zone = "default-zone";
    bundleInfo->NodeTagFilter = "default-zone/" + bundleName;
    bundleInfo->EnableBundleController = true;
    bundleInfo->EnableTabletCellManagement = true;

    auto config = New<TBundleConfig>();
    bundleInfo->TargetConfig = config;
    config->TabletNodeCount = nodeCount;
    config->RpcProxyCount = proxyCount;
    config->TabletNodeResourceGuarantee = New<TInstanceResources>();
    config->TabletNodeResourceGuarantee->Vcpu = 9999;
    config->TabletNodeResourceGuarantee->Memory = 88_GB;
    config->RpcProxyResourceGuarantee->Vcpu = 1111;
    config->RpcProxyResourceGuarantee->Memory = 18_GB;
    config->CpuLimits->WriteThreadPoolSize = writeThreadCount;

    return bundleInfo;
}

////////////////////////////////////////////////////////////////////////////////

TSchedulerInputState GenerateSimpleInputContext(int nodeCount, int writeThreadCount = 0, int proxyCount = 0)
{
    TSchedulerInputState input;
    input.Config = New<TBundleControllerConfig>();
    input.Config->Cluster = "default-cluster";

    {
        auto zoneInfo = New<TZoneInfo>();
        input.Zones["default-zone"] = zoneInfo;
        zoneInfo->YPCluster = "pre-pre";
        zoneInfo->TabletNodeNannyService = "nanny-bunny-tablet-nodes";
        zoneInfo->RpcProxyNannyService = "nanny-bunny-rpc-proxies";
    }

    SetBundleInfo(input, "default-bundle", nodeCount, writeThreadCount, proxyCount);

    return input;
}

void VerifyNodeAllocationRequests(const TSchedulerMutations& mutations, int expectedCount)
{
    EXPECT_EQ(expectedCount, std::ssize(mutations.NewAllocations));

    for (const auto& [id, request] : mutations.NewAllocations) {
        EXPECT_FALSE(id.empty());
        
        auto spec = request->Spec;
        EXPECT_TRUE(static_cast<bool>(spec));
        EXPECT_EQ(spec->YPCluster, "pre-pre");
        EXPECT_EQ(spec->NannyService, "nanny-bunny-tablet-nodes");
        EXPECT_FALSE(spec->PodIdTemplate.empty());
        EXPECT_TRUE(spec->InstanceRole == YTRoleTypeTabNode);
        EXPECT_EQ(spec->ResourceRequest->Vcpu, 9999);
        EXPECT_EQ(spec->ResourceRequest->MemoryMb, static_cast<i64>(88_GB / 1_MB));
    }
}

void VerifyProxyAllocationRequests(const TSchedulerMutations& mutations, int expectedCount)
{
    EXPECT_EQ(expectedCount, std::ssize(mutations.NewAllocations));

    for (const auto& [id, request] : mutations.NewAllocations) {
        EXPECT_FALSE(id.empty());
        
        auto spec = request->Spec;
        EXPECT_TRUE(static_cast<bool>(spec));
        EXPECT_EQ(spec->YPCluster, "pre-pre");
        EXPECT_EQ(spec->NannyService, "nanny-bunny-rpc-proxies");
        EXPECT_FALSE(spec->PodIdTemplate.empty());
        EXPECT_TRUE(spec->InstanceRole == YTRoleTypeRpcProxy);
        EXPECT_EQ(spec->ResourceRequest->Vcpu, 1111);
        EXPECT_EQ(spec->ResourceRequest->MemoryMb, static_cast<i64>(18_GB / 1_MB));
    }
}

void VerifyNodeDeallocationRequests(
    const TSchedulerMutations& mutations,
    TBundleControllerStatePtr& bundleState,
    int expectedCount)
{
    EXPECT_EQ(expectedCount, std::ssize(mutations.NewDeallocations));

    for (const auto& [id, request] : mutations.NewDeallocations) {
        EXPECT_FALSE(id.empty());

        auto spec = request->Spec;
        EXPECT_TRUE(static_cast<bool>(spec));
        EXPECT_EQ(spec->YPCluster, "pre-pre");
        EXPECT_FALSE(spec->PodId.empty());

        EXPECT_TRUE(spec->InstanceRole == YTRoleTypeTabNode);

        EXPECT_FALSE(bundleState->NodeDeallocations[id]->InstanceName.empty());
    }
}

void VerifyProxyDeallocationRequests(
    const TSchedulerMutations& mutations,
    TBundleControllerStatePtr& bundleState,
    int expectedCount)
{
    EXPECT_EQ(expectedCount, std::ssize(mutations.NewDeallocations));

    for (const auto& [id, request] : mutations.NewDeallocations) {
        EXPECT_FALSE(id.empty());

        auto spec = request->Spec;
        EXPECT_TRUE(static_cast<bool>(spec));
        EXPECT_EQ(spec->YPCluster, "pre-pre");
        EXPECT_FALSE(spec->PodId.empty());

        EXPECT_TRUE(spec->InstanceRole == YTRoleTypeRpcProxy);

        EXPECT_FALSE(bundleState->ProxyDeallocations[id]->InstanceName.empty());
    }
}

THashSet<TString> GenerateNodesForBundle(
    TSchedulerInputState& inputState,
    const TString& bundleName,
    int nodeCount,
    bool setFilterTag = false,
    int slotCount = 5)
{
    THashSet<TString> result;

    for (int index = 0; index < nodeCount; ++index) {
        int nodeIndex = std::ssize(inputState.TabletNodes);
        auto nodeId = Format("seneca-ayt-%v-%v-aa-tab-node-%v.search.yandex.net",
            nodeIndex,
            bundleName,
            inputState.Config->Cluster);
        auto nodeInfo = New<TTabletNodeInfo>();
        nodeInfo->Banned = false;
        nodeInfo->Decommissioned = false;
        nodeInfo->Host = Format("seneca-ayt-%v.search.yandex.net", nodeIndex);
        nodeInfo->State = "online";
        nodeInfo->Annotations->Allocated = true;
        nodeInfo->Annotations->NannyService = "nanny-bunny-tablet-nodes";
        nodeInfo->Annotations->YPCluster = "pre-pre";
        nodeInfo->Annotations->AllocatedForBundle = bundleName;

        for (int index = 0; index < slotCount; ++index) {
            nodeInfo->TabletSlots.push_back(New<TTabletSlot>());
        }

        if (setFilterTag) {
            nodeInfo->UserTags.insert(GetOrCrash(inputState.Bundles, bundleName)->NodeTagFilter);
        }

        inputState.TabletNodes[nodeId] = nodeInfo;
        result.insert(nodeId);
    }

    return result;
}

THashSet<TString> GenerateProxiesForBundle(
    TSchedulerInputState& inputState,
    const TString& bundleName,
    int proxyCount,
    bool setRole = false)
{
    THashSet<TString> result;

    for (int index = 0; index < proxyCount; ++index) {
        int proxyIndex = std::ssize(inputState.RpcProxies);
        auto proxyName = Format("seneca-ayt-%v-%v-aa-proxy-%v.search.yandex.net",
            proxyIndex,
            bundleName,
            inputState.Config->Cluster);
        auto proxyInfo = New<TRpcProxyInfo>();
        // nodeInfo->Host = Format("seneca-ayt-%v.search.yandex.net", nodeIndex);
        proxyInfo->Alive = New<TRpcProxyAlive>();
        proxyInfo->Annotations->Allocated = true;
        proxyInfo->Annotations->NannyService = "nanny-bunny-rpc-proxies";
        proxyInfo->Annotations->YPCluster = "pre-pre";
        proxyInfo->Annotations->AllocatedForBundle = bundleName;

        if (setRole) {
            proxyInfo->Role = bundleName;
        }

        inputState.RpcProxies[proxyName] = proxyInfo;
        result.insert(proxyName);
    }

    return result;
}

void SetTabletSlotsState(TSchedulerInputState& inputState, const TString& nodeName, const TString& state)
{
    const auto& nodeInfo = GetOrCrash(inputState.TabletNodes, nodeName);

    for (const auto& slot : nodeInfo->TabletSlots) {
        slot->State = state;
    }
}

void GenerateNodeAllocationsForBundle(TSchedulerInputState& inputState, const TString& bundleName, int count)
{
    auto& state = inputState.BundleStates[bundleName];
    if (!state) {
        state = New<TBundleControllerState>();
    }

    for (int index = 0; index < count; ++index) {
        auto requestId = Format("alloc-%v", state->NodeAllocations.size());
        state->NodeAllocations[requestId] = New<TAllocationRequestState>();
        state->NodeAllocations[requestId]->CreationTime = TInstant::Now();
        inputState.AllocationRequests[requestId] = New<TAllocationRequest>();
        auto& spec = inputState.AllocationRequests[requestId]->Spec;
        spec->NannyService = "nanny-bunny-tablet-nodes";
        spec->YPCluster = "pre-pre";
        spec->ResourceRequest->Vcpu = 9999;
        spec->ResourceRequest->MemoryMb = 88_GB / 1_MB;
    }
}

void GenerateProxyAllocationsForBundle(TSchedulerInputState& inputState, const TString& bundleName, int count)
{
    auto& state = inputState.BundleStates[bundleName];
    if (!state) {
        state = New<TBundleControllerState>();
    }

    for (int index = 0; index < count; ++index) {
        auto requestId = Format("proxy-alloc-%v", state->ProxyAllocations.size());
        state->ProxyAllocations[requestId] = New<TAllocationRequestState>();
        state->ProxyAllocations[requestId]->CreationTime = TInstant::Now();
        inputState.AllocationRequests[requestId] = New<TAllocationRequest>();
        auto& spec = inputState.AllocationRequests[requestId]->Spec;
        spec->NannyService = "nanny-bunny-rpc-proxies";
        spec->YPCluster = "pre-pre";
        spec->ResourceRequest->Vcpu = 1111;
        spec->ResourceRequest->MemoryMb = 18_GB / 1_MB;
    }
}

void GenerateTabletCellsForBundle(
    TSchedulerInputState& inputState,
    const TString& bundleName,
    int cellCount,
    int peerCount = 1)
{
    auto bundleInfo = GetOrCrash(inputState.Bundles, bundleName);

    for (int index = 0; index < cellCount; ++index) {
        auto cellId = Format("tablet-cell-%v-%v", bundleName, bundleInfo->TabletCellIds.size());
        auto cellInfo = New<TTabletCellInfo>();
        cellInfo->TabletCount = 2;
        cellInfo->TabletCellBundle = bundleName;
        cellInfo->Peers.resize(peerCount, New<TTabletCellPeer>());
        bundleInfo->TabletCellIds.push_back(cellId);
        inputState.TabletCells[cellId] = cellInfo;
    }
}

void GenerateNodeDeallocationsForBundle(
    TSchedulerInputState& inputState, 
    const TString& bundleName,
    const std::vector<TString>& nodeNames)
{
    auto& state = inputState.BundleStates[bundleName];
    if (!state) {
        state = New<TBundleControllerState>();
    }

    for (const auto& nodeName : nodeNames) {
        const auto& nodeInfo = GetOrCrash(inputState.TabletNodes, nodeName);
        nodeInfo->Decommissioned = true;
        SetTabletSlotsState(inputState, nodeName, TabletSlotStateEmpty);

        auto requestId = Format("dealloc-%v", state->NodeAllocations.size());

        auto deallocationState = New<TDeallocationRequestState>();
        state->NodeDeallocations[requestId] = deallocationState;
        deallocationState->CreationTime = TInstant::Now();
        deallocationState->InstanceName = nodeName;
        deallocationState->HulkRequestCreated = true;

        inputState.DeallocationRequests[requestId] = New<TDeallocationRequest>();
        auto& spec = inputState.DeallocationRequests[requestId]->Spec;
        spec->YPCluster = "pre-pre";
        spec->PodId = "random_pod_id";
    }
}

void GenerateProxyDeallocationsForBundle(
    TSchedulerInputState& inputState, 
    const TString& bundleName,
    const std::vector<TString>& proxyNames)
{
    auto& state = inputState.BundleStates[bundleName];
    if (!state) {
        state = New<TBundleControllerState>();
    }

    for (const auto& proxyName : proxyNames) {
        auto requestId = Format("proxy-dealloc-%v", state->ProxyDeallocations.size());

        auto deallocationState = New<TDeallocationRequestState>();
        state->ProxyDeallocations[requestId] = deallocationState;
        deallocationState->CreationTime = TInstant::Now();
        deallocationState->InstanceName = proxyName;
        deallocationState->HulkRequestCreated = true;

        inputState.DeallocationRequests[requestId] = New<TDeallocationRequest>();
        auto& spec = inputState.DeallocationRequests[requestId]->Spec;
        spec->YPCluster = "pre-pre";
        spec->PodId = "random_pod_id";
    }
}

void SetNodeAnnotations(const TString& nodeId, const TString& bundleName, const TSchedulerInputState& input)
{
    auto& annotation = GetOrCrash(input.TabletNodes, nodeId)->Annotations;
    annotation->YPCluster = "pre-pre";
    annotation->AllocatedForBundle = bundleName;
    annotation->Allocated = true;
}

void SetProxyAnnotations(const TString& nodeId, const TString& bundleName, const TSchedulerInputState& input)
{
    auto& annotation = GetOrCrash(input.RpcProxies, nodeId)->Annotations;
    annotation->YPCluster = "pre-pre";
    annotation->AllocatedForBundle = bundleName;
    annotation->Allocated = true;
}

////////////////////////////////////////////////////////////////////////////////

TEST(TBundleSchedulerTest, AllocationCreated)
{
    auto input = GenerateSimpleInputContext(5);
    TSchedulerMutations mutations;

    GenerateNodesForBundle(input, "default-bundle", 1);
    GenerateNodeAllocationsForBundle(input, "default-bundle", 1);

    ScheduleBundles(input, &mutations);

    EXPECT_EQ(0, std::ssize(mutations.AlertsToFire));
    EXPECT_EQ(0, std::ssize(mutations.NewDeallocations));
    VerifyNodeAllocationRequests(mutations, 3);

    EXPECT_EQ(4, std::ssize(mutations.ChangedStates["default-bundle"]->NodeAllocations));
}

TEST(TBundleSchedulerTest, AllocationProgressTrackCompleted)
{
    auto input = GenerateSimpleInputContext(2);

    GenerateNodesForBundle(input, "default-bundle", 2);
    GenerateNodeAllocationsForBundle(input, "default-bundle", 1);

    const TString nodeId = input.TabletNodes.begin()->first;
    GetOrCrash(input.TabletNodes, nodeId)->Annotations = New<TInstanceAnnotations>();

    {
        auto& request = input.AllocationRequests.begin()->second;
        auto status = New<TAllocationRequestStatus>();
        request->Status = status;
        status->NodeId = GetOrCrash(input.TabletNodes, nodeId)->Host;
        status->PodId = GetPodIdForInstance(nodeId);
        status->State = "COMPLETED";
    }

    // Check Setting node attributes
    {
        TSchedulerMutations mutations;
        ScheduleBundles(input, &mutations);

        EXPECT_EQ(0, std::ssize(mutations.AlertsToFire));
        EXPECT_EQ(0, std::ssize(mutations.NewDeallocations));
        VerifyNodeAllocationRequests(mutations, 0);
        EXPECT_EQ(1, std::ssize(input.BundleStates["default-bundle"]->NodeAllocations));

        EXPECT_EQ(1, std::ssize(mutations.ChangeNodeAnnotations));
        const auto& annotations = GetOrCrash(mutations.ChangeNodeAnnotations, nodeId);
        EXPECT_EQ(annotations->YPCluster, "pre-pre");
        EXPECT_EQ(annotations->AllocatedForBundle, "default-bundle");
        EXPECT_EQ(annotations->NannyService, "nanny-bunny-tablet-nodes");
        EXPECT_EQ(annotations->Resource->Vcpu, 9999);
        EXPECT_EQ(annotations->Resource->Memory, static_cast<i64>(88_GB));
        EXPECT_TRUE(annotations->Allocated);

        input.TabletNodes[nodeId]->Annotations = annotations;
    }

    // Schedule one more time with annotation tags set
    TSchedulerMutations mutations;
    ScheduleBundles(input, &mutations);

    EXPECT_EQ(0, std::ssize(mutations.AlertsToFire));
    EXPECT_EQ(0, std::ssize(mutations.NewDeallocations));
    EXPECT_EQ(0, std::ssize(mutations.ChangedStates["default-bundle"]->NodeAllocations));
    EXPECT_EQ(0, std::ssize(mutations.ChangeNodeAnnotations));
    VerifyNodeAllocationRequests(mutations, 0);
}

TEST(TBundleSchedulerTest, AllocationProgressTrackFailed)
{
    auto input = GenerateSimpleInputContext(2);
    TSchedulerMutations mutations;

    GenerateNodesForBundle(input, "default-bundle", 2);
    GenerateNodeAllocationsForBundle(input, "default-bundle", 1);

    {
        auto& request = input.AllocationRequests.begin()->second;
        auto status = New<TAllocationRequestStatus>();
        request->Status = status;
        status->State = "FAILED";
    }

    ScheduleBundles(input, &mutations);

    EXPECT_EQ(0, std::ssize(mutations.NewDeallocations));
    VerifyNodeAllocationRequests(mutations, 0);
    EXPECT_EQ(0, std::ssize(mutations.ChangedStates["default-bundle"]->NodeAllocations));

    EXPECT_EQ(1, std::ssize(mutations.AlertsToFire));
    // TODO(capone212): use constants instead of inline strings
    EXPECT_EQ(mutations.AlertsToFire.front().Id, "instance_allocation_failed");
}

TEST(TBundleSchedulerTest, AllocationProgressTrackCompletedButNoNode)
{
    auto input = GenerateSimpleInputContext(2);
    TSchedulerMutations mutations;

    GenerateNodesForBundle(input, "default-bundle", 2);
    GenerateNodeAllocationsForBundle(input, "default-bundle", 1);

    {
        auto& request = input.AllocationRequests.begin()->second;
        auto status = New<TAllocationRequestStatus>();
        request->Status = status;
        status->NodeId = "non-existing-node";
        status->State = "COMPLETED";
    }

    ScheduleBundles(input, &mutations);

    EXPECT_EQ(0, std::ssize(mutations.AlertsToFire));
    EXPECT_EQ(0, std::ssize(mutations.NewDeallocations));
    VerifyNodeAllocationRequests(mutations, 0);
    EXPECT_EQ(1, std::ssize(input.BundleStates["default-bundle"]->NodeAllocations));
}

TEST(TBundleSchedulerTest, AllocationProgressTrackStaledAllocation)
{
    auto input = GenerateSimpleInputContext(2);
    TSchedulerMutations mutations;

    GenerateNodesForBundle(input, "default-bundle", 2);
    GenerateNodeAllocationsForBundle(input, "default-bundle", 1);

    {
        auto& allocState = input.BundleStates["default-bundle"]->NodeAllocations.begin()->second;
        allocState->CreationTime = TInstant::Now() - TDuration::Days(1);
    }

    {
        auto& request = input.AllocationRequests.begin()->second;
        auto status = New<TAllocationRequestStatus>();
        request->Status = status;
        status->NodeId = "non-existing-node";
        status->State = "COMPLETED";
    }

    ScheduleBundles(input, &mutations);

    EXPECT_EQ(0, std::ssize(mutations.NewDeallocations));
    VerifyNodeAllocationRequests(mutations, 0);
    EXPECT_EQ(0, std::ssize(mutations.ChangedStates["default-bundle"]->NodeAllocations));

    EXPECT_EQ(1, std::ssize(mutations.AlertsToFire));
    EXPECT_EQ(mutations.AlertsToFire.front().Id, "stuck_instance_allocation");
}

TEST(TBundleSchedulerTest, DoNotCreateNewDeallocationsWhileInProgress)
{
    auto input = GenerateSimpleInputContext(2);
    auto nodes = GenerateNodesForBundle(input, "default-bundle", 5);
    GenerateNodeDeallocationsForBundle(input, "default-bundle", { *nodes.begin()});

    for (const auto& [nodeId, _] : input.TabletNodes) {
        SetNodeAnnotations(nodeId, "default-bundle", input);
    }

    TSchedulerMutations mutations;

    ScheduleBundles(input, &mutations);

    EXPECT_EQ(0, std::ssize(mutations.AlertsToFire));
    EXPECT_EQ(0, std::ssize(mutations.NewAllocations));

    auto& bundleState = mutations.ChangedStates["default-bundle"];
    EXPECT_EQ(1, std::ssize(mutations.ChangedStates["default-bundle"]->NodeDeallocations));
    VerifyNodeDeallocationRequests(mutations, bundleState, 0);
}

TEST(TBundleSchedulerTest, CreateNewDeallocations)
{
    auto input = GenerateSimpleInputContext(2);
    GenerateNodesForBundle(input, "default-bundle", 5);

    for (auto& [nodeId, _] : input.TabletNodes) {
        SetNodeAnnotations(nodeId, "default-bundle", input);
    }

    TSchedulerMutations mutations;

    ScheduleBundles(input, &mutations);

    EXPECT_EQ(0, std::ssize(mutations.AlertsToFire));
    EXPECT_EQ(0, std::ssize(mutations.NewAllocations));
    EXPECT_EQ(0, std::ssize(mutations.NewDeallocations));
    EXPECT_EQ(3, std::ssize(mutations.ChangedStates["default-bundle"]->NodeDeallocations));

    input.BundleStates = mutations.ChangedStates;
    mutations = TSchedulerMutations{};
    ScheduleBundles(input, &mutations);

    EXPECT_EQ(0, std::ssize(mutations.NewDeallocations));
    EXPECT_EQ(3, std::ssize(mutations.ChangedDecommissionedFlag));

    std::vector<TString> nodesToRemove;
    for (auto& [nodeName, decommissioned] : mutations.ChangedDecommissionedFlag) {
        GetOrCrash(input.TabletNodes, nodeName)->Decommissioned = decommissioned;
        EXPECT_TRUE(decommissioned);
        nodesToRemove.push_back(nodeName);

        SetTabletSlotsState(input, nodeName, PeerStateLeading);
    }

    input.BundleStates = mutations.ChangedStates;
    mutations = TSchedulerMutations{};
    ScheduleBundles(input, &mutations);

    // Node are decommissioned but tablet slots have to be empty.
    EXPECT_EQ(0, std::ssize(mutations.NewDeallocations));
    EXPECT_EQ(0, std::ssize(mutations.ChangedDecommissionedFlag));

    for (const auto& nodeName : nodesToRemove) {
        SetTabletSlotsState(input, nodeName, TabletSlotStateEmpty);
    }

    input.BundleStates = mutations.ChangedStates;
    mutations = TSchedulerMutations{};
    ScheduleBundles(input, &mutations);

    // Hulk deallocation requests are finally created.
    auto& bundleState = mutations.ChangedStates["default-bundle"];
    VerifyNodeDeallocationRequests(mutations, bundleState, 3);
    EXPECT_EQ(0, std::ssize(mutations.ChangedDecommissionedFlag));
}

TEST(TBundleSchedulerTest, DeallocationProgressTrackFailed)
{
    auto input = GenerateSimpleInputContext(1);
    TSchedulerMutations mutations;

    auto bundleNodes = GenerateNodesForBundle(input, "default-bundle", 2);
    GenerateNodeDeallocationsForBundle(input, "default-bundle", { *bundleNodes.begin()});

    {
        auto& request = input.DeallocationRequests.begin()->second;
        request->Status->State = "FAILED";
    }

    ScheduleBundles(input, &mutations);

    EXPECT_EQ(0, std::ssize(mutations.NewDeallocations));
    VerifyNodeAllocationRequests(mutations, 0);
    EXPECT_EQ(0, std::ssize(mutations.ChangedStates["default-bundle"]->NodeDeallocations));

    EXPECT_EQ(1, std::ssize(mutations.AlertsToFire));
    // TODO(capone212): use constants instead of inline strings
    EXPECT_EQ(mutations.AlertsToFire.front().Id, "instance_deallocation_failed");
}

TEST(TBundleSchedulerTest, DeallocationProgressTrackCompleted)
{
    auto input = GenerateSimpleInputContext(1);

    auto bundleNodes = GenerateNodesForBundle(input, "default-bundle", 2);
    const TString nodeId = *bundleNodes.begin();

    GenerateNodeDeallocationsForBundle(input, "default-bundle", {nodeId});

    {
        auto& request = input.DeallocationRequests.begin()->second;
        auto& status = request->Status;
        status->State = "COMPLETED";
    }

    // Check Setting node attributes
    {
        TSchedulerMutations mutations;
        ScheduleBundles(input, &mutations);

        EXPECT_EQ(0, std::ssize(mutations.AlertsToFire));
        EXPECT_EQ(0, std::ssize(mutations.NewAllocations));
        EXPECT_EQ(0, std::ssize(mutations.NewDeallocations));
        EXPECT_EQ(1, std::ssize(input.BundleStates["default-bundle"]->NodeDeallocations));

        EXPECT_EQ(1, std::ssize(mutations.ChangeNodeAnnotations));
        const auto& annotations = GetOrCrash(mutations.ChangeNodeAnnotations, nodeId);
        EXPECT_TRUE(annotations->YPCluster.empty());
        EXPECT_TRUE(annotations->AllocatedForBundle.empty());
        EXPECT_TRUE(annotations->NannyService.empty());
        EXPECT_FALSE(annotations->Allocated);

        input.TabletNodes[nodeId]->Annotations = annotations;
    }

    // Schedule one more time with annotation tags set
    TSchedulerMutations mutations;
    ScheduleBundles(input, &mutations);

    EXPECT_EQ(0, std::ssize(mutations.AlertsToFire));
    EXPECT_EQ(0, std::ssize(mutations.NewDeallocations));
    EXPECT_EQ(0, std::ssize(mutations.ChangedStates["default-bundle"]->NodeAllocations));
    EXPECT_EQ(0, std::ssize(mutations.ChangeNodeAnnotations));
    VerifyNodeAllocationRequests(mutations, 0);
}

TEST(TBundleSchedulerTest, DeallocationProgressTrackStaledAllocation)
{
    auto input = GenerateSimpleInputContext(1);
    TSchedulerMutations mutations;

    auto bundleNodes = GenerateNodesForBundle(input, "default-bundle", 2);
    const TString nodeId = *bundleNodes.begin();

    GenerateNodeDeallocationsForBundle(input, "default-bundle", {nodeId});

    {
        auto& allocState = input.BundleStates["default-bundle"]->NodeDeallocations.begin()->second;
        allocState->CreationTime = TInstant::Now() - TDuration::Days(1);
    }

    ScheduleBundles(input, &mutations);

    EXPECT_EQ(0, std::ssize(mutations.NewAllocations));
    EXPECT_EQ(0, std::ssize(mutations.NewDeallocations));

    EXPECT_EQ(1, std::ssize(mutations.AlertsToFire));
    EXPECT_EQ(mutations.AlertsToFire.front().Id, "stuck_instance_deallocation");
}

TEST(TBundleSchedulerTest, CreateNewCellsCreation)
{
    auto input = GenerateSimpleInputContext(2, 5);
    TSchedulerMutations mutations;

    GenerateNodesForBundle(input, "default-bundle", 2);
    GenerateTabletCellsForBundle(input, "default-bundle", 3);

    ScheduleBundles(input, &mutations);

    EXPECT_EQ(0, std::ssize(mutations.AlertsToFire));
    EXPECT_EQ(0, std::ssize(mutations.NewAllocations));
    EXPECT_EQ(0, std::ssize(mutations.NewDeallocations));
    EXPECT_EQ(0, std::ssize(mutations.CellsToRemove));
    EXPECT_EQ(1, std::ssize(mutations.CellsToCreate));

    EXPECT_EQ(7, mutations.CellsToCreate.at("default-bundle"));
}

TEST(TBundleSchedulerTest, CreateNewCellsNoRemoveNoCreate)
{
    auto input = GenerateSimpleInputContext(2, 5);
    TSchedulerMutations mutations;

    GenerateNodesForBundle(input, "default-bundle", 2);
    GenerateTabletCellsForBundle(input, "default-bundle", 10);

    ScheduleBundles(input, &mutations);

    EXPECT_EQ(0, std::ssize(mutations.AlertsToFire));
    EXPECT_EQ(0, std::ssize(mutations.NewAllocations));
    EXPECT_EQ(0, std::ssize(mutations.NewDeallocations));
    EXPECT_EQ(0, std::ssize(mutations.CellsToCreate));
    EXPECT_EQ(0, std::ssize(mutations.CellsToRemove));
}

TEST(TBundleSchedulerTest, CreateNewCellsRemove)
{
    auto input = GenerateSimpleInputContext(2, 5);
    TSchedulerMutations mutations;

    GenerateNodesForBundle(input, "default-bundle", 2);
    GenerateTabletCellsForBundle(input, "default-bundle", 13);

    ScheduleBundles(input, &mutations);

    EXPECT_EQ(0, std::ssize(mutations.AlertsToFire));
    EXPECT_EQ(0, std::ssize(mutations.NewAllocations));
    EXPECT_EQ(0, std::ssize(mutations.NewDeallocations));
    EXPECT_EQ(0, std::ssize(mutations.CellsToCreate));
    EXPECT_EQ(3, std::ssize(mutations.CellsToRemove));
}

TEST(TBundleSchedulerTest, PeekRightCellToRemove)
{
    auto input = GenerateSimpleInputContext(2, 5);
    TSchedulerMutations mutations;

    GenerateNodesForBundle(input, "default-bundle", 2);
    GenerateTabletCellsForBundle(input, "default-bundle", 11);

    auto cellId = input.Bundles["default-bundle"]->TabletCellIds[RandomNumber<ui32>(11)];
    input.TabletCells[cellId]->TabletCount = 0;

    ScheduleBundles(input, &mutations);

    EXPECT_EQ(0, std::ssize(mutations.AlertsToFire));
    EXPECT_EQ(0, std::ssize(mutations.NewAllocations));
    EXPECT_EQ(0, std::ssize(mutations.NewDeallocations));
    EXPECT_EQ(0, std::ssize(mutations.CellsToCreate));
    EXPECT_EQ(1, std::ssize(mutations.CellsToRemove));

    EXPECT_EQ(cellId, mutations.CellsToRemove.front());
}

TEST(TBundleSchedulerTest, TestSpareNodesAllocate)
{
    auto input = GenerateSimpleInputContext(0);
    auto zoneInfo = input.Zones["default-zone"];
    zoneInfo->SpareTargetConfig->TabletNodeCount = 3;

    TSchedulerMutations mutations;

    ScheduleBundles(input, &mutations);

    EXPECT_EQ(0, std::ssize(mutations.AlertsToFire));
    EXPECT_EQ(0, std::ssize(mutations.NewDeallocations));
    EXPECT_EQ(0, std::ssize(mutations.CellsToCreate));
    EXPECT_EQ(0, std::ssize(mutations.CellsToRemove));
    EXPECT_EQ(3, std::ssize(mutations.NewAllocations));
}

TEST(TBundleSchedulerTest, TestSpareNodesDeallocate)
{
    auto input = GenerateSimpleInputContext(0);
    auto zoneInfo = input.Zones["default-zone"];

    zoneInfo->SpareTargetConfig->TabletNodeCount = 2;
    GenerateNodesForBundle(input, "spare", 3);

    TSchedulerMutations mutations;

    ScheduleBundles(input, &mutations);

    EXPECT_EQ(0, std::ssize(mutations.AlertsToFire));
    EXPECT_EQ(0, std::ssize(mutations.CellsToCreate));
    EXPECT_EQ(0, std::ssize(mutations.CellsToRemove));
    EXPECT_EQ(0, std::ssize(mutations.NewAllocations));
    EXPECT_EQ(1, std::ssize(mutations.ChangedStates["spare"]->NodeDeallocations));
}

////////////////////////////////////////////////////////////////////////////////

void CheckEmptyAlerts(const TSchedulerMutations& mutations)
{
    EXPECT_EQ(0, std::ssize(mutations.AlertsToFire));

    for (const auto& alert : mutations.AlertsToFire) {
        EXPECT_EQ("", alert.Id);
        EXPECT_EQ("", alert.Description);
    }
}

////////////////////////////////////////////////////////////////////////////////

TEST(TNodeTagsFilterManager, TestBundleWithNoTagFilter)
{
    auto input = GenerateSimpleInputContext(2, 5);
    input.Bundles["default-bundle"]->EnableNodeTagFilterManagement = true;
    input.Bundles["default-bundle"]->NodeTagFilter = {};

    GenerateNodesForBundle(input, "default-bundle", 2);
    GenerateTabletCellsForBundle(input, "default-bundle", 10);

    TSchedulerMutations mutations;
    ScheduleBundles(input, &mutations);

    EXPECT_EQ(1, std::ssize(mutations.AlertsToFire));
    EXPECT_EQ("bundle_with_no_tag_filter", mutations.AlertsToFire.front().Id);

    EXPECT_EQ(0, std::ssize(mutations.ChangedNodeUserTags));
    EXPECT_EQ(0, std::ssize(mutations.ChangedDecommissionedFlag));
}

TEST(TNodeTagsFilterManager, TestBundleNodeTagsAssigned)
{
    auto input = GenerateSimpleInputContext(2, 5);
    input.Bundles["default-bundle"]->EnableNodeTagFilterManagement = true;

    GenerateNodesForBundle(input, "default-bundle", 2);
    GenerateTabletCellsForBundle(input, "default-bundle", 10);

    TSchedulerMutations mutations;
    ScheduleBundles(input, &mutations);

    CheckEmptyAlerts(mutations);

    EXPECT_EQ(0, std::ssize(mutations.ChangedDecommissionedFlag));
    EXPECT_EQ(2, std::ssize(mutations.ChangedNodeUserTags));

    for (const auto& [nodeName, tags] : mutations.ChangedNodeUserTags) {
        input.TabletNodes[nodeName]->UserTags = tags;
    }

    mutations = TSchedulerMutations{};
    ScheduleBundles(input, &mutations);

    CheckEmptyAlerts(mutations);

    EXPECT_EQ(0, std::ssize(mutations.ChangedDecommissionedFlag));
    EXPECT_EQ(0, std::ssize(mutations.ChangedNodeUserTags));
}

TEST(TNodeTagsFilterManager, TestBundleNodesWithSpare)
{
    const bool SetNodeFilterTag = true; 
    const int SlotCount = 5;

    auto input = GenerateSimpleInputContext(2, SlotCount);
    input.Bundles["default-bundle"]->EnableNodeTagFilterManagement = true;

    GenerateNodesForBundle(input, "default-bundle", 1, SetNodeFilterTag, SlotCount);
    GenerateTabletCellsForBundle(input, "default-bundle", 15);

    // Generate Spare nodes
    auto zoneInfo = input.Zones["default-zone"];
    zoneInfo->SpareTargetConfig->TabletNodeCount = 3;
    auto spareNodes = GenerateNodesForBundle(input, "spare", 3, false, SlotCount);

    TSchedulerMutations mutations;
    ScheduleBundles(input, &mutations);

    CheckEmptyAlerts(mutations);
    EXPECT_EQ(2, std::ssize(mutations.ChangedDecommissionedFlag));
    EXPECT_EQ(2, std::ssize(mutations.ChangedNodeUserTags));

    const TString BundleNodeTagFilter = input.Bundles["default-bundle"]->NodeTagFilter;

    THashSet<TString> usedSpare;

    for (auto& [nodeName, tags] : mutations.ChangedNodeUserTags) {
        EXPECT_FALSE(mutations.ChangedDecommissionedFlag.at(nodeName));
        EXPECT_TRUE(tags.find(BundleNodeTagFilter) != tags.end());
        EXPECT_TRUE(spareNodes.find(nodeName) != spareNodes.end());

        usedSpare.insert(nodeName);
        input.TabletNodes[nodeName]->UserTags = tags;
    }

    EXPECT_EQ(2, std::ssize(usedSpare));

    // Populate slots with cell peers.
    for (const auto& spareNode : usedSpare) {
        SetTabletSlotsState(input, spareNode, PeerStateLeading);
    }

    mutations = TSchedulerMutations{};
    ScheduleBundles(input, &mutations);

    CheckEmptyAlerts(mutations);
    EXPECT_EQ(0, std::ssize(mutations.ChangedDecommissionedFlag));
    EXPECT_EQ(0, std::ssize(mutations.ChangedNodeUserTags));

    // Add new node to bundle
    auto newNodes = GenerateNodesForBundle(input, "default-bundle", 1, SetNodeFilterTag, SlotCount);

    mutations = TSchedulerMutations{};
    ScheduleBundles(input, &mutations);

    CheckEmptyAlerts(mutations);
    EXPECT_EQ(1, std::ssize(mutations.ChangedDecommissionedFlag));
    EXPECT_EQ(0, std::ssize(mutations.ChangedNodeUserTags));

    TString spareNodeToRelease;

    for (const auto& [nodeName, decommission] : mutations.ChangedDecommissionedFlag) {
        EXPECT_TRUE(usedSpare.count(nodeName) != 0);
        EXPECT_TRUE(decommission);
        input.TabletNodes[nodeName]->Decommissioned = decommission;
        spareNodeToRelease = nodeName;
    }

    mutations = TSchedulerMutations{};
    ScheduleBundles(input, &mutations);

    CheckEmptyAlerts(mutations);
    EXPECT_EQ(0, std::ssize(mutations.ChangedDecommissionedFlag));
    EXPECT_EQ(0, std::ssize(mutations.ChangedNodeUserTags));

    // Populate slots with cell peers.
    SetTabletSlotsState(input, spareNodeToRelease, TabletSlotStateEmpty);

    mutations = TSchedulerMutations{};
    ScheduleBundles(input, &mutations);

    CheckEmptyAlerts(mutations);
    EXPECT_EQ(0, std::ssize(mutations.ChangedDecommissionedFlag));
    EXPECT_EQ(1, std::ssize(mutations.ChangedNodeUserTags));

    for (auto& [nodeName, tags] : mutations.ChangedNodeUserTags) {
        EXPECT_EQ(spareNodeToRelease, nodeName);
        EXPECT_TRUE(tags.count(BundleNodeTagFilter) == 0);
        input.TabletNodes[nodeName]->UserTags = tags;
    }

    mutations = TSchedulerMutations{};
    ScheduleBundles(input, &mutations);

    CheckEmptyAlerts(mutations);
    EXPECT_EQ(0, std::ssize(mutations.ChangedDecommissionedFlag));
    EXPECT_EQ(0, std::ssize(mutations.ChangedNodeUserTags));
}

////////////////////////////////////////////////////////////////////////////////

TEST(TBundleSchedulerTest, CheckDisruptedState)
{
    auto input = GenerateSimpleInputContext(5);
    TSchedulerMutations mutations;

    auto zoneInfo = input.Zones["default-zone"];
    zoneInfo->SpareTargetConfig->TabletNodeCount = 3;
    GenerateNodesForBundle(input, "spare", 3);
    GenerateNodesForBundle(input, "default-bundle", 4);

    for (auto& [_, nodeInfo] : input.TabletNodes) {
        nodeInfo->State = InstanceStateOffline;
    }

    ScheduleBundles(input, &mutations);

    EXPECT_EQ(0, std::ssize(mutations.AlertsToFire));
    EXPECT_EQ(0, std::ssize(mutations.NewDeallocations));
    EXPECT_EQ(0, std::ssize(mutations.NewAllocations));
}

TEST(TBundleSchedulerTest, CheckAllocationLimit)
{
    auto input = GenerateSimpleInputContext(5);
    TSchedulerMutations mutations;

    auto zoneInfo = input.Zones["default-zone"];
    zoneInfo->SpareTargetConfig->TabletNodeCount = 3;
    GenerateNodesForBundle(input, "spare", 3);
    GenerateNodesForBundle(input, "default-bundle", 4);

    zoneInfo->MaxTabletNodeCount = 5;

    ScheduleBundles(input, &mutations);

    EXPECT_EQ(0, std::ssize(mutations.NewDeallocations));
    EXPECT_EQ(0, std::ssize(mutations.NewAllocations));

    EXPECT_EQ(1, std::ssize(mutations.AlertsToFire));
}

TEST(TBundleSchedulerTest, CheckDynamicConfig)
{
    auto input = GenerateSimpleInputContext(5, 5);
    input.Bundles["default-bundle"]->EnableTabletNodeDynamicConfig = true;

    auto zoneInfo = input.Zones["default-zone"];
    zoneInfo->SpareTargetConfig->TabletNodeCount = 3;
    GenerateNodesForBundle(input, "spare", 3);
    GenerateNodesForBundle(input, "default-bundle", 5);

    TSchedulerMutations mutations;
    ScheduleBundles(input, &mutations);

    // Check that new dynamic config is set for bundles.
    EXPECT_TRUE(mutations.DynamicConfig);

    input.DynamicConfig = *mutations.DynamicConfig;
    mutations = TSchedulerMutations{};
    ScheduleBundles(input, &mutations);

    // Dynamic config did not change.
    EXPECT_FALSE(mutations.DynamicConfig);

    mutations = TSchedulerMutations{};
    ScheduleBundles(input, &mutations);

    input.Bundles["default-bundle"]->TargetConfig->CpuLimits->WriteThreadPoolSize = 212;
    mutations = TSchedulerMutations{};
    ScheduleBundles(input, &mutations);

    // Dynamic config is changed.
    EXPECT_TRUE(mutations.DynamicConfig);
}

////////////////////////////////////////////////////////////////////////////////

struct TFooBarStruct
    : public TYsonStructAttributes<TFooBarStruct>
{
    TString Foo;
    int Bar;

    REGISTER_YSON_STRUCT(TFooBarStruct);

    static void Register(TRegistrar registrar)
    {
        RegisterAttribute(registrar, "foo", &TThis::Foo)
            .Default();
        RegisterAttribute(registrar, "bar", &TThis::Bar)
            .Default(0);
    }
};

TEST(TBundleSchedulerTest, CheckCypressBindings)
{
    EXPECT_EQ(TFooBarStruct::GetAttributes().size(), 2u);
}

////////////////////////////////////////////////////////////////////////////////

const int DefaultNodeCount = 0;
const int DefaultCellCount = 0;

////////////////////////////////////////////////////////////////////////////////

TEST(TBundleSchedulerTest, ProxyAllocationCreated)
{
    auto input = GenerateSimpleInputContext(DefaultNodeCount, DefaultCellCount, 5);
    TSchedulerMutations mutations;

    GenerateProxiesForBundle(input, "default-bundle", 1);
    GenerateProxyAllocationsForBundle(input, "default-bundle", 1);

    ScheduleBundles(input, &mutations);

    EXPECT_EQ(0, std::ssize(mutations.AlertsToFire));
    EXPECT_EQ(0, std::ssize(mutations.NewDeallocations));
    VerifyProxyAllocationRequests(mutations, 3);

    EXPECT_EQ(0, std::ssize(mutations.ChangedStates["default-bundle"]->NodeAllocations));
    EXPECT_EQ(4, std::ssize(mutations.ChangedStates["default-bundle"]->ProxyAllocations));
}

TEST(TBundleSchedulerTest, ProxyAllocationProgressTrackCompleted)
{
    auto input = GenerateSimpleInputContext(DefaultNodeCount, DefaultCellCount, 2);

    GenerateProxiesForBundle(input, "default-bundle", 2);
    GenerateProxyAllocationsForBundle(input, "default-bundle", 1);

    const TString proxyName = input.RpcProxies.begin()->first;
    GetOrCrash(input.RpcProxies, proxyName)->Annotations = New<TInstanceAnnotations>();

    {
        auto& request = input.AllocationRequests.begin()->second;
        auto status = New<TAllocationRequestStatus>();
        request->Status = status;
        status->PodId = GetPodIdForInstance(proxyName);
        status->State = "COMPLETED";
    }

    // Check Setting proxy attributes
    {
        TSchedulerMutations mutations;
        ScheduleBundles(input, &mutations);

        EXPECT_EQ(0, std::ssize(mutations.AlertsToFire));
        EXPECT_EQ(0, std::ssize(mutations.NewDeallocations));
        EXPECT_EQ(0, std::ssize(mutations.NewAllocations));
        EXPECT_EQ(1, std::ssize(input.BundleStates["default-bundle"]->ProxyAllocations));

        EXPECT_EQ(1, std::ssize(mutations.ChangedProxyAnnotations));
        const auto& annotations = GetOrCrash(mutations.ChangedProxyAnnotations, proxyName);
        EXPECT_EQ(annotations->YPCluster, "pre-pre");
        EXPECT_EQ(annotations->AllocatedForBundle, "default-bundle");
        EXPECT_EQ(annotations->NannyService, "nanny-bunny-rpc-proxies");
        EXPECT_EQ(annotations->Resource->Vcpu, 1111);
        EXPECT_EQ(annotations->Resource->Memory, static_cast<i64>(18_GB));
        EXPECT_TRUE(annotations->Allocated);

        input.RpcProxies[proxyName]->Annotations = annotations;
    }

    // Schedule one more time with annotation tags set
    TSchedulerMutations mutations;
    ScheduleBundles(input, &mutations);

    EXPECT_EQ(0, std::ssize(mutations.AlertsToFire));
    EXPECT_EQ(0, std::ssize(mutations.NewDeallocations));
    EXPECT_EQ(0, std::ssize(mutations.ChangedStates["default-bundle"]->ProxyAllocations));
    EXPECT_EQ(0, std::ssize(mutations.ChangedProxyAnnotations));
    EXPECT_EQ(0, std::ssize(mutations.NewAllocations));
}

TEST(TBundleSchedulerTest, ProxyAllocationProgressTrackFailed)
{
    auto input = GenerateSimpleInputContext(DefaultNodeCount, DefaultCellCount, 2);
    TSchedulerMutations mutations;

    GenerateProxiesForBundle(input, "default-bundle", 2);
    GenerateProxyAllocationsForBundle(input, "default-bundle", 1);

    {
        auto& request = input.AllocationRequests.begin()->second;
        auto status = New<TAllocationRequestStatus>();
        request->Status = status;
        status->State = "FAILED";
    }

    ScheduleBundles(input, &mutations);

    EXPECT_EQ(0, std::ssize(mutations.NewDeallocations));
    EXPECT_EQ(0, std::ssize(mutations.NewAllocations));

    EXPECT_EQ(0, std::ssize(mutations.ChangedStates["default-bundle"]->ProxyAllocations));

    EXPECT_EQ(1, std::ssize(mutations.AlertsToFire));
    EXPECT_EQ(mutations.AlertsToFire.front().Id, "instance_allocation_failed");
}

TEST(TBundleSchedulerTest, ProxyAllocationProgressTrackCompletedButNoProxy)
{
    auto input = GenerateSimpleInputContext(DefaultNodeCount, DefaultCellCount, 2);
    TSchedulerMutations mutations;

    GenerateProxiesForBundle(input, "default-bundle", 2);
    GenerateProxyAllocationsForBundle(input, "default-bundle", 1);

    {
        auto& request = input.AllocationRequests.begin()->second;
        auto status = New<TAllocationRequestStatus>();
        request->Status = status;
        status->PodId = "non-existing-pod";
        status->State = "COMPLETED";
    }

    ScheduleBundles(input, &mutations);

    EXPECT_EQ(0, std::ssize(mutations.AlertsToFire));
    EXPECT_EQ(0, std::ssize(mutations.NewDeallocations));
    EXPECT_EQ(0, std::ssize(mutations.NewAllocations));
    EXPECT_EQ(1, std::ssize(input.BundleStates["default-bundle"]->ProxyAllocations));
}

TEST(TBundleSchedulerTest, ProxyAllocationProgressTrackStaledAllocation)
{
    auto input = GenerateSimpleInputContext(DefaultNodeCount, DefaultCellCount, 2);
    TSchedulerMutations mutations;

    GenerateProxiesForBundle(input, "default-bundle", 2);
    GenerateProxyAllocationsForBundle(input, "default-bundle", 1);

    {
        auto& allocState = input.BundleStates["default-bundle"]->ProxyAllocations.begin()->second;
        allocState->CreationTime = TInstant::Now() - TDuration::Days(1);
    }

    {
        auto& request = input.AllocationRequests.begin()->second;
        auto status = New<TAllocationRequestStatus>();
        request->Status = status;
        status->PodId = "non-existing-pod";
        status->State = "COMPLETED";
    }

    ScheduleBundles(input, &mutations);

    EXPECT_EQ(0, std::ssize(mutations.NewDeallocations));
    EXPECT_EQ(0, std::ssize(mutations.NewAllocations));
    EXPECT_EQ(0, std::ssize(mutations.ChangedStates["default-bundle"]->ProxyAllocations));

    EXPECT_EQ(1, std::ssize(mutations.AlertsToFire));
    EXPECT_EQ(mutations.AlertsToFire.front().Id, "stuck_instance_allocation");
}

TEST(TBundleSchedulerTest, ProxyCreateNewDeallocations)
{
    auto input = GenerateSimpleInputContext(DefaultNodeCount, DefaultCellCount, 2);
    GenerateProxiesForBundle(input, "default-bundle", 5);

    for (auto& [proxyName, _] : input.BundleProxies) {
        SetProxyAnnotations(proxyName, "default-bundle", input);
    }

    TSchedulerMutations mutations;
    ScheduleBundles(input, &mutations);

    EXPECT_EQ(0, std::ssize(mutations.AlertsToFire));
    EXPECT_EQ(0, std::ssize(mutations.NewAllocations));
    EXPECT_EQ(0, std::ssize(mutations.NewDeallocations));
    EXPECT_EQ(3, std::ssize(mutations.ChangedStates["default-bundle"]->ProxyDeallocations));

    input.BundleStates = mutations.ChangedStates;
    mutations = TSchedulerMutations{};
    ScheduleBundles(input, &mutations);

    // Hulk deallocation requests are created.
    auto& bundleState = mutations.ChangedStates["default-bundle"];
    VerifyProxyDeallocationRequests(mutations, bundleState, 3);
}

TEST(TBundleSchedulerTest, ProxyDeallocationProgressTrackFailed)
{
    auto input = GenerateSimpleInputContext(DefaultNodeCount, DefaultCellCount, 1);
    TSchedulerMutations mutations;

    auto bundleProxies = GenerateProxiesForBundle(input, "default-bundle", 1);
    GenerateProxyDeallocationsForBundle(input, "default-bundle", { *bundleProxies.begin()});

    {
        auto& request = input.DeallocationRequests.begin()->second;
        request->Status->State = "FAILED";
    }

    ScheduleBundles(input, &mutations);

    EXPECT_EQ(0, std::ssize(mutations.NewDeallocations));
    EXPECT_EQ(0, std::ssize(mutations.NewAllocations));
    EXPECT_EQ(0, std::ssize(mutations.ChangedStates["default-bundle"]->ProxyDeallocations));

    EXPECT_EQ(1, std::ssize(mutations.AlertsToFire));
    // TODO(capone212): use constants instead of inline strings
    EXPECT_EQ(mutations.AlertsToFire.front().Id, "instance_deallocation_failed");
}

TEST(TBundleSchedulerTest, ProxyDeallocationProgressTrackCompleted)
{
    auto input = GenerateSimpleInputContext(DefaultNodeCount, DefaultCellCount, 1);

    auto bundleProxies = GenerateProxiesForBundle(input, "default-bundle", 2);
    const TString proxyName = *bundleProxies.begin();

    GenerateProxyDeallocationsForBundle(input, "default-bundle", {proxyName});

    {
        auto& request = input.DeallocationRequests.begin()->second;
        auto& status = request->Status;
        status->State = "COMPLETED";
    }

    // Check Setting proxy attributes
    {
        TSchedulerMutations mutations;
        ScheduleBundles(input, &mutations);

        EXPECT_EQ(0, std::ssize(mutations.AlertsToFire));
        EXPECT_EQ(0, std::ssize(mutations.NewAllocations));
        EXPECT_EQ(0, std::ssize(mutations.NewDeallocations));
        EXPECT_EQ(1, std::ssize(input.BundleStates["default-bundle"]->ProxyDeallocations));

        EXPECT_EQ(1, std::ssize(mutations.ChangedProxyAnnotations));
        const auto& annotations = GetOrCrash(mutations.ChangedProxyAnnotations, proxyName);
        EXPECT_TRUE(annotations->YPCluster.empty());
        EXPECT_TRUE(annotations->AllocatedForBundle.empty());
        EXPECT_TRUE(annotations->NannyService.empty());
        EXPECT_FALSE(annotations->Allocated);

        input.RpcProxies[proxyName]->Annotations = annotations;
    }

    // Schedule one more time with annotation tags set
    TSchedulerMutations mutations;
    ScheduleBundles(input, &mutations);

    EXPECT_EQ(0, std::ssize(mutations.AlertsToFire));
    EXPECT_EQ(0, std::ssize(mutations.NewDeallocations));
    EXPECT_EQ(0, std::ssize(mutations.ChangedStates["default-bundle"]->ProxyDeallocations));
    EXPECT_EQ(0, std::ssize(mutations.ChangedProxyAnnotations));
}

TEST(TBundleSchedulerTest, ProxyDeallocationProgressTrackStaledAllocation)
{
    auto input = GenerateSimpleInputContext(DefaultNodeCount, DefaultCellCount, 1);
    TSchedulerMutations mutations;

    auto bundleProxies = GenerateProxiesForBundle(input, "default-bundle", 2);
    const TString proxyName = *bundleProxies.begin();

    GenerateProxyDeallocationsForBundle(input, "default-bundle", {proxyName});

    {
        auto& allocState = input.BundleStates["default-bundle"]->ProxyDeallocations.begin()->second;
        allocState->CreationTime = TInstant::Now() - TDuration::Days(1);
    }

    ScheduleBundles(input, &mutations);

    EXPECT_EQ(0, std::ssize(mutations.NewAllocations));
    EXPECT_EQ(0, std::ssize(mutations.NewDeallocations));

    EXPECT_EQ(1, std::ssize(mutations.AlertsToFire));
    EXPECT_EQ(mutations.AlertsToFire.front().Id, "stuck_instance_deallocation");
}

TEST(TBundleSchedulerTest, TestSpareProxiesAllocate)
{
    auto input = GenerateSimpleInputContext(0);
    auto zoneInfo = input.Zones["default-zone"];
    zoneInfo->SpareTargetConfig->RpcProxyCount = 3;

    TSchedulerMutations mutations;

    ScheduleBundles(input, &mutations);

    EXPECT_EQ(0, std::ssize(mutations.AlertsToFire));
    EXPECT_EQ(0, std::ssize(mutations.NewDeallocations));
    EXPECT_EQ(0, std::ssize(mutations.CellsToCreate));
    EXPECT_EQ(0, std::ssize(mutations.CellsToRemove));
    EXPECT_EQ(3, std::ssize(mutations.NewAllocations));
    EXPECT_EQ(3, std::ssize(mutations.ChangedStates["spare"]->ProxyAllocations));
}

TEST(TBundleSchedulerTest, TestSpareProxyDeallocate)
{
    auto input = GenerateSimpleInputContext(0);
    auto zoneInfo = input.Zones["default-zone"];

    zoneInfo->SpareTargetConfig->RpcProxyCount = 2;
    GenerateProxiesForBundle(input, "spare", 3);

    TSchedulerMutations mutations;

    ScheduleBundles(input, &mutations);

    EXPECT_EQ(0, std::ssize(mutations.AlertsToFire));
    EXPECT_EQ(0, std::ssize(mutations.CellsToCreate));
    EXPECT_EQ(0, std::ssize(mutations.CellsToRemove));
    EXPECT_EQ(0, std::ssize(mutations.NewAllocations));
    EXPECT_EQ(1, std::ssize(mutations.ChangedStates["spare"]->ProxyDeallocations));
}

TEST(TBundleSchedulerTest, CheckProxyZoneDisruptedState)
{
    auto input = GenerateSimpleInputContext(DefaultNodeCount, DefaultCellCount, 5);
    TSchedulerMutations mutations;

    auto zoneInfo = input.Zones["default-zone"];
    zoneInfo->SpareTargetConfig->RpcProxyCount = 3;
    GenerateProxiesForBundle(input, "spare", 3);
    GenerateProxiesForBundle(input, "default-bundle", 4);

    for (auto& [_, proxyInfo] : input.RpcProxies) {
        proxyInfo->Alive.Reset();
    }

    ScheduleBundles(input, &mutations);

    EXPECT_EQ(0, std::ssize(mutations.AlertsToFire));
    EXPECT_EQ(0, std::ssize(mutations.NewDeallocations));
    EXPECT_EQ(0, std::ssize(mutations.NewAllocations));
}

TEST(TBundleSchedulerTest, ProxyCheckAllocationLimit)
{
    auto input = GenerateSimpleInputContext(DefaultNodeCount, DefaultCellCount, 5);
    TSchedulerMutations mutations;

    auto zoneInfo = input.Zones["default-zone"];
    zoneInfo->SpareTargetConfig->RpcProxyCount = 3;
    GenerateProxiesForBundle(input, "spare", 3);
    GenerateProxiesForBundle(input, "default-bundle", 4);

    zoneInfo->MaxRpcProxyCount = 5;

    ScheduleBundles(input, &mutations);

    EXPECT_EQ(0, std::ssize(mutations.NewDeallocations));
    EXPECT_EQ(0, std::ssize(mutations.NewAllocations));

    EXPECT_EQ(1, std::ssize(mutations.AlertsToFire));
}

////////////////////////////////////////////////////////////////////////////////

TEST(TProxyRoleManagement, TestBundleProxyRolesAssigned)
{
    auto input = GenerateSimpleInputContext(DefaultNodeCount, DefaultCellCount, 2);
    input.Bundles["default-bundle"]->EnableRpcProxyManagement = true;

    GenerateProxiesForBundle(input, "default-bundle", 2);

    TSchedulerMutations mutations;
    ScheduleBundles(input, &mutations);

    CheckEmptyAlerts(mutations);

    EXPECT_EQ(2, std::ssize(mutations.ChangedProxyRole));

    for (const auto& [proxyName, role] : mutations.ChangedProxyRole) {
        ASSERT_EQ(role, "default-bundle");
        input.RpcProxies[proxyName]->Role = role;
    }

    mutations = TSchedulerMutations{};
    ScheduleBundles(input, &mutations);

    CheckEmptyAlerts(mutations);

    EXPECT_EQ(0, std::ssize(mutations.ChangedDecommissionedFlag));
    EXPECT_EQ(0, std::ssize(mutations.ChangedProxyRole));
}

TEST(TProxyRoleManagement, TestBundleProxyBanned)
{
    const bool SetProxyRole = true; 

    auto input = GenerateSimpleInputContext(DefaultNodeCount, DefaultCellCount, 3);
    input.Bundles["default-bundle"]->EnableRpcProxyManagement = true;

    auto bundleProxies = GenerateProxiesForBundle(input, "default-bundle", 3, SetProxyRole);

    // Generate Spare proxies
    auto zoneInfo = input.Zones["default-zone"];
    zoneInfo->SpareTargetConfig->RpcProxyCount = 3;
    auto spareProxies = GenerateProxiesForBundle(input, "spare", 3);

    TSchedulerMutations mutations;
    ScheduleBundles(input, &mutations);

    CheckEmptyAlerts(mutations);
    EXPECT_EQ(0, std::ssize(mutations.ChangedProxyRole));
    EXPECT_EQ(0, std::ssize(mutations.NewAllocations));

    // Ban bundle proxy
    {
        auto& proxy = GetOrCrash(input.RpcProxies, *bundleProxies.begin());
        proxy->Banned = true;
    }

    mutations = TSchedulerMutations{};
    ScheduleBundles(input, &mutations);

    CheckEmptyAlerts(mutations);
    EXPECT_EQ(1, std::ssize(mutations.ChangedProxyRole));
    EXPECT_EQ(1, std::ssize(mutations.NewAllocations));

    for (auto& [proxyName, role] : mutations.ChangedProxyRole) {
        EXPECT_EQ(role, "default-bundle");
        EXPECT_TRUE(spareProxies.find(proxyName) != spareProxies.end());
    }
}

TEST(TProxyRoleManagement, TestBundleProxyRolesWithSpare)
{
    const bool SetProxyRole = true; 

    auto input = GenerateSimpleInputContext(DefaultNodeCount, DefaultCellCount, 3);
    input.Bundles["default-bundle"]->EnableRpcProxyManagement = true;

    GenerateProxiesForBundle(input, "default-bundle", 1, SetProxyRole);

    // Generate Spare proxies
    auto zoneInfo = input.Zones["default-zone"];
    zoneInfo->SpareTargetConfig->RpcProxyCount = 3;
    auto spareProxies = GenerateProxiesForBundle(input, "spare", 3);

    TSchedulerMutations mutations;
    ScheduleBundles(input, &mutations);

    CheckEmptyAlerts(mutations);
    EXPECT_EQ(2, std::ssize(mutations.ChangedProxyRole));

    THashSet<TString> usedSpare;

    for (auto& [proxyName, role] : mutations.ChangedProxyRole) {
        EXPECT_EQ(role, "default-bundle");
        EXPECT_TRUE(spareProxies.find(proxyName) != spareProxies.end());

        usedSpare.insert(proxyName);
        input.RpcProxies[proxyName]->Role = role;
    }

    EXPECT_EQ(2, std::ssize(usedSpare));

    mutations = TSchedulerMutations{};
    ScheduleBundles(input, &mutations);

    CheckEmptyAlerts(mutations);
    EXPECT_EQ(0, std::ssize(mutations.ChangedProxyRole));

    // Add new proxies to bundle
    auto newProxies = GenerateProxiesForBundle(input, "default-bundle", 1, SetProxyRole);;

    mutations = TSchedulerMutations{};
    ScheduleBundles(input, &mutations);

    CheckEmptyAlerts(mutations);

    for (const auto& [proxyName, role] : mutations.ChangedProxyRole) {
        EXPECT_TRUE(usedSpare.count(proxyName) != 0);
        input.RpcProxies[proxyName]->Role = role;
    }
    EXPECT_EQ(1, std::ssize(mutations.ChangedProxyRole));

    // Check no more changes
    mutations = TSchedulerMutations{};
    ScheduleBundles(input, &mutations);

    CheckEmptyAlerts(mutations);
    EXPECT_EQ(0, std::ssize(mutations.ChangedProxyRole));
}

////////////////////////////////////////////////////////////////////////////////

struct TExpectedLimits
{
    i64 Nodes = 0;
    i64 Chunks = 0;
    i64 SsdBlobs = 0;
    i64 Default = 0;
    i64 SsdJournal = 0;
};

void CheckLimits(const TExpectedLimits& limits, const TAccountResourcesPtr& resource)
{
    ASSERT_EQ(limits.Chunks, resource->ChunkCount);
    ASSERT_EQ(limits.Nodes, resource->NodeCount);
    ASSERT_EQ(limits.SsdJournal, resource->DiskSpacePerMedium["ssd_journal"]);
    ASSERT_EQ(limits.Default, resource->DiskSpacePerMedium["default"]);
    ASSERT_EQ(limits.SsdBlobs, resource->DiskSpacePerMedium["ssd_blobs"]);
}

TEST(TBundleSchedulerTest, CheckSystemAccountLimit)
{
    auto input = GenerateSimpleInputContext(2, 5);

    input.RootSystemAccount = New<TSystemAccount>();
    auto& bundleInfo1 = input.Bundles["default-bundle"];

    bundleInfo1->Options->ChangelogAccount = "default-bundle-account";
    bundleInfo1->Options->SnapshotAccount = "default-bundle-account";
    bundleInfo1->Options->ChangelogPrimaryMedium = "ssd_journal";
    bundleInfo1->Options->SnapshotPrimaryMedium = "default";
    bundleInfo1->EnableSystemAccountManagement = true;

    input.SystemAccounts["default-bundle-account"] = New<TSystemAccount>();

    input.Config->QuotaMultiplier = 1.5;
    input.Config->ChunkCountPerCell = 2;
    input.Config->NodeCountPerCell = 3;
    input.Config->JournalDiskSpacePerCell = 5_MB;
    input.Config->SnapshotDiskSpacePerCell = 7_MB;
    input.Config->MinNodeCount = 9;
    input.Config->MinChunkCount = 7;

    GenerateNodesForBundle(input, "default-bundle", 2);
    {
        auto& limits = input.RootSystemAccount->ResourceLimits;
        limits->NodeCount = 1000;
        limits->ChunkCount = 2000;
        limits->DiskSpacePerMedium["default"] = 1_MB;
    }

    TSchedulerMutations mutations;
    ScheduleBundles(input, &mutations);
    EXPECT_EQ(1, std::ssize(mutations.ChangedSystemAccountLimit));

    CheckLimits(
        TExpectedLimits{
            .Nodes = 45,
            .Chunks = 30,
            .Default = 105_MB,
            .SsdJournal = 75_MB,
        },
        mutations.ChangedSystemAccountLimit["default-bundle-account"]);

    CheckLimits(
        TExpectedLimits{
            .Nodes = 1045,
            .Chunks = 2030,
            .Default = 106_MB,
            .SsdJournal = 75_MB
        },
        mutations.ChangedRootSystemAccountLimit);

    SetBundleInfo(input, "default-bundle2", 10, 20);
    auto& bundleInfo2 = input.Bundles["default-bundle2"];
    bundleInfo2->EnableSystemAccountManagement = true;
    bundleInfo2->Options->ChangelogAccount = "default-bundle2-account";
    bundleInfo2->Options->SnapshotAccount = "default-bundle2-account";
    bundleInfo2->Options->ChangelogPrimaryMedium = "ssd_journal";
    bundleInfo2->Options->SnapshotPrimaryMedium = "ssd_blobs";
    input.SystemAccounts["default-bundle2-account"] = New<TSystemAccount>();

    mutations = TSchedulerMutations{};
    ScheduleBundles(input, &mutations);
    EXPECT_EQ(2, std::ssize(mutations.ChangedSystemAccountLimit));

    CheckLimits(
        TExpectedLimits{
            .Nodes = 45,
            .Chunks = 30,
            .Default = 105_MB,
            .SsdJournal = 75_MB
        },
        mutations.ChangedSystemAccountLimit["default-bundle-account"]);

    CheckLimits(
        TExpectedLimits{
            .Nodes = 900,
            .Chunks = 600,
            .SsdBlobs = 2100_MB,
            .SsdJournal = 1500_MB
        },
        mutations.ChangedSystemAccountLimit["default-bundle2-account"]);

    CheckLimits(
        TExpectedLimits{
            .Nodes = 1945,
            .Chunks = 2630,
            .SsdBlobs = 2100_MB,
            .Default = 106_MB,
            .SsdJournal = 1575_MB
        },
        mutations.ChangedRootSystemAccountLimit);

    // Test account actual cells count
    GenerateTabletCellsForBundle(input, "default-bundle2", 300);
    mutations = TSchedulerMutations{};
    ScheduleBundles(input, &mutations);

    CheckLimits(
        TExpectedLimits{
            .Nodes = 1350,
            .Chunks = 900,
            .SsdBlobs = 3150_MB,
            .SsdJournal = 2250_MB
        },
        mutations.ChangedSystemAccountLimit["default-bundle2-account"]);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // NYT::NCellBalancer