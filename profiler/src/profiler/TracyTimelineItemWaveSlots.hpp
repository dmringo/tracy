#ifndef __TRACYTIMELINEITEMWAVESLOTS_HPP__
#define __TRACYTIMELINEITEMWAVESLOTS_HPP__

#include "TracyTimelineItem.hpp"

namespace tracy
{

class TimelineItemWaveSlots final : public TimelineItem
{
public:
    TimelineItemWaveSlots( View& view, Worker& worker, void* key );

protected:
    uint32_t HeaderColor() const override { return 0xFF55DDFF; }
    uint32_t HeaderColorInactive() const override { return 0xFF337788; }
    uint32_t HeaderLineColor() const override { return 0x3355DDFF; }
    const char* HeaderLabel() const override { return "GPU Wave Slots (SIMD Execution)"; }

    int64_t RangeBegin() const override;
    int64_t RangeEnd() const override;

    void HeaderTooltip( const char* label ) const override;
    bool DrawContents( const TimelineContext& ctx, int& offset ) override;
    bool IsEmpty() const override;
};

}

#endif
