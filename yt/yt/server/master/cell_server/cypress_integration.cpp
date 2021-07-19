#include "cypress_integration.h"

#include "area.h"
#include "cell_base.h"
#include "tamed_cell_manager.h"

#include <yt/yt/server/master/cell_master/bootstrap.h>

#include <yt/yt/server/master/cypress_server/node_detail.h>
#include <yt/yt/server/master/cypress_server/node_proxy_detail.h>
#include <yt/yt/server/master/cypress_server/virtual.h>

#include <yt/yt/server/master/object_server/object.h>

#include <yt/yt/server/lib/misc/object_helpers.h>

#include <yt/yt/client/object_client/helpers.h>

namespace NYT::NCellServer {

using namespace NYPath;
using namespace NRpc;
using namespace NYson;
using namespace NYTree;
using namespace NObjectClient;
using namespace NCypressServer;
using namespace NTransactionServer;
using namespace NObjectServer;
using namespace NCellMaster;
using namespace NCellarClient;

////////////////////////////////////////////////////////////////////////////////

class TVirtualAreaMap
    : public TVirtualMapBase
{
public:
    explicit TVirtualAreaMap(TBootstrap* bootstrap)
        : Bootstrap_(bootstrap)
    { }

private:
    TBootstrap* const Bootstrap_;

    virtual std::vector<TString> GetKeys(i64 sizeLimit) const override
    {
        const auto& cellManager = Bootstrap_->GetTamedCellManager();
        return ConvertToStrings(GetValues(cellManager->Areas(), sizeLimit), TObjectIdFormatter());
    }

    virtual bool IsValid(TObject* object) const
    {
        return object->GetType() == EObjectType::Area;
    }

    virtual i64 GetSize() const override
    {
        const auto& cellManager = Bootstrap_->GetTamedCellManager();
        return std::ssize(cellManager->Areas());
    }

    virtual IYPathServicePtr FindItemService(TStringBuf key) const override
    {
        const auto& cellManager = Bootstrap_->GetTamedCellManager();
        auto* area = cellManager->Areas().Find(TAreaId::FromString(key));
        if (!IsObjectAlive(area)) {
            return nullptr;
        }

        const auto& objectManager = Bootstrap_->GetObjectManager();
        return objectManager->GetProxy(area);
    }
};

INodeTypeHandlerPtr CreateAreaMapTypeHandler(TBootstrap* bootstrap)
{
    YT_VERIFY(bootstrap);

    return CreateVirtualTypeHandler(
        bootstrap,
        EObjectType::AreaMap,
        BIND([=] (INodePtr /*owningNode*/) -> IYPathServicePtr {
            return New<TVirtualAreaMap>(bootstrap);
        }),
        EVirtualNodeOptions::RedirectSelf);
}

////////////////////////////////////////////////////////////////////////////////

class TCellNodeProxy
    : public TMapNodeProxy
{
public:
    using TMapNodeProxy::TMapNodeProxy;

    virtual TResolveResult ResolveSelf(
        const TYPath& path,
        const IServiceContextPtr& context) override
    {
        const auto& method = context->GetMethod();
        if (method == "Remove") {
            return TResolveResultThere{GetTargetProxy(), path};
        } else {
            return TMapNodeProxy::ResolveSelf(path, context);
        }
    }

    virtual IYPathService::TResolveResult ResolveAttributes(
        const TYPath& path,
        const IServiceContextPtr& /*context*/) override
    {
        return TResolveResultThere{GetTargetProxy(), "/@" + path};
    }

    virtual void DoWriteAttributesFragment(
        IAsyncYsonConsumer* consumer,
        const std::optional<std::vector<TString>>& attributeKeys,
        bool stable) override
    {
        GetTargetProxy()->WriteAttributesFragment(consumer, attributeKeys, stable);
    }

private:
    IObjectProxyPtr GetTargetProxy() const
    {
        auto key = GetParent()->AsMap()->GetChildKeyOrThrow(this);
        auto id = TCellId::FromString(key);

        const auto& cellManager = Bootstrap_->GetTamedCellManager();
        auto* cell = cellManager->GetCellOrThrow(id);

        const auto& objectManager = Bootstrap_->GetObjectManager();
        return objectManager->GetProxy(cell, nullptr);
    }
};

////////////////////////////////////////////////////////////////////////////////

class TCellNodeTypeHandler
    : public TMapNodeTypeHandler
{
public:
    using TMapNodeTypeHandler::TMapNodeTypeHandlerImpl;

    virtual EObjectType GetObjectType() const override
    {
        return EObjectType::TabletCellNode;
    }

private:
    virtual ICypressNodeProxyPtr DoGetProxy(
        TMapNode* trunkNode,
        TTransaction* transaction) override
    {
        return New<TCellNodeProxy>(
            Bootstrap_,
            &Metadata_,
            transaction,
            trunkNode);
    }
};

INodeTypeHandlerPtr CreateCellNodeTypeHandler(TBootstrap* bootstrap)
{
    return New<TCellNodeTypeHandler>(bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

class TVirtualCellBundleMap
    : public TVirtualMapBase
{
public:
    TVirtualCellBundleMap(TBootstrap* bootstrap, ECellarType cellarType)
        : Bootstrap_(bootstrap)
        , CellarType_(cellarType)
    { }

private:
    TBootstrap* const Bootstrap_;
    const ECellarType CellarType_;

    virtual std::vector<TString> GetKeys(i64 sizeLimit) const override
    {
        const auto& cellManager = Bootstrap_->GetTamedCellManager();
        return ToNames(GetItems(cellManager->CellBundles(CellarType_), sizeLimit));
    }

    virtual i64 GetSize() const override
    {
        const auto& cellManager = Bootstrap_->GetTamedCellManager();
        return std::ssize(cellManager->CellBundles(CellarType_));
    }

    virtual IYPathServicePtr FindItemService(TStringBuf key) const override
    {
        const auto& cellManager = Bootstrap_->GetTamedCellManager();
        auto* cellBundle = cellManager->FindCellBundleByName(TString(key), CellarType_, false /*activeLifeStageOnly*/);
        if (!IsObjectAlive(cellBundle)) {
            return nullptr;
        }

        const auto& objectManager = Bootstrap_->GetObjectManager();
        return objectManager->GetProxy(cellBundle);
    }
};

INodeTypeHandlerPtr CreateCellBundleMapTypeHandler(
    TBootstrap* bootstrap,
    ECellarType cellarType,
    EObjectType cellBundleMapType)
{
    YT_VERIFY(bootstrap);

    return CreateVirtualTypeHandler(
        bootstrap,
        cellBundleMapType,
        BIND([=] (INodePtr /*owningNode*/) -> IYPathServicePtr {
            return New<TVirtualCellBundleMap>(bootstrap, cellarType);
        }),
        EVirtualNodeOptions::RedirectSelf);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCellServer