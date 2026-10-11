#include "TracyImGui.hpp"
#include "TracyTimelineContext.hpp"
#include "TracyTimelineItemWaveSlots.hpp"
#include "TracyView.hpp"
#include "TracyWorker.hpp"

namespace tracy
{

TimelineItemWaveSlots::TimelineItemWaveSlots( View& view, Worker& worker, void* key )
    : TimelineItem( view, worker, key, false )
{
}

bool TimelineItemWaveSlots::IsEmpty() const
{
    return !m_worker.HasGpuWaveSlots();
}

void TimelineItemWaveSlots::HeaderTooltip( const char* label ) const
{
    ImGui::BeginTooltip();
    ImGui::TextUnformatted( "Hardware thread-trace wave slot occupancy across Compute Units and SIMD pipelines." );
    ImGui::Text( "Total Compute Units: %zu", m_worker.GetGpuComputeUnits().size() );
    ImGui::EndTooltip();
}

int64_t TimelineItemWaveSlots::RangeBegin() const
{
    int64_t t = std::numeric_limits<int64_t>::max();
    for( const auto& cu : m_worker.GetGpuComputeUnits() )
    {
        for( int s = 0; s < 4; s++ )
        {
            for( const auto& slot : cu.simds[s].slots )
            {
                if( !slot.waves.empty() )
                {
                    t = (std::min)( t, slot.waves.front().start.Val() );
                }
            }
        }
    }
    return ( t == std::numeric_limits<int64_t>::max() ) ? 0 : t;
}

int64_t TimelineItemWaveSlots::RangeEnd() const
{
    int64_t t = 0;
    for( const auto& cu : m_worker.GetGpuComputeUnits() )
    {
        for( int s = 0; s < 4; s++ )
        {
            for( const auto& slot : cu.simds[s].slots )
            {
                if( !slot.waves.empty() )
                {
                    t = (std::max)( t, slot.waves.back().end.Val() );
                }
            }
        }
    }
    return t;
}

bool TimelineItemWaveSlots::DrawContents( const TimelineContext& ctx, int& offset )
{
    return m_view.DrawGpuWaveSlots( ctx, offset );
}

}
