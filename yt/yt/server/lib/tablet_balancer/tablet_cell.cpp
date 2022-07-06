#include "tablet_cell.h"

#include <yt/yt/core/ytree/node.h>
#include <yt/yt/core/ytree/convert.h>

namespace NYT::NTabletBalancer {

using namespace NTabletClient;

////////////////////////////////////////////////////////////////////////////////

void Deserialize(TTabletCellStatus& value, NYTree::INodePtr node)
{
    auto mapNode = node->AsMap();
    value.Health = ConvertTo<ETabletCellHealth>(mapNode->FindChild("health"));
    value.Decommissioned = mapNode->FindChild("decommissioned")->AsBoolean()->GetValue();
}

////////////////////////////////////////////////////////////////////////////////

void Deserialize(TTabletCellStatistics& value, NYTree::INodePtr node)
{
    auto mapNode = node->AsMap();
    value.MemorySize = mapNode->FindChild("memory_size")->AsInt64()->GetValue();
}

////////////////////////////////////////////////////////////////////////////////

TTabletCell::TTabletCell(
    TTabletCellId cellId,
    const TTabletCellStatistics& statistics,
    const TTabletCellStatus& status)
    : Id(cellId)
    , Statistics(statistics)
    , Status(status)
{ }

bool TTabletCell::IsAlive() const
{
    return Status.Health == ETabletCellHealth::Good && !Status.Decommissioned;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletBalancer