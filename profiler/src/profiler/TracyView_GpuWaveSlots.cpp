#include <algorithm>
#include <inttypes.h>

#include "TracyColor.hpp"
#include "TracyImGui.hpp"
#include "TracyMouse.hpp"
#include "TracyPrint.hpp"
#include "TracyTimelineContext.hpp"
#include "TracyUtility.hpp"
#include "TracyView.hpp"
#include "TracyWorker.hpp"
#include "../Fonts.hpp"

namespace tracy
{

void View::ShowGpuShader( uint64_t hash, uint32_t waveId, uint32_t wgX, uint32_t wgY, uint32_t wgZ )
{
    if( hash == 0 && !m_worker.GetGpuShaders().empty() )
    {
        m_selectedGpuShader = m_worker.GetGpuShaders().begin()->first;
    }
    else
    {
        m_selectedGpuShader = hash;
    }
    m_selectedGpuWaveId = waveId;
    m_selectedGpuWgX = wgX;
    m_selectedGpuWgY = wgY;
    m_selectedGpuWgZ = wgZ;
    m_showGpuShaderWindow = true;
    m_focusGpuShaderWindow = true;
}

bool View::DrawGpuWaveSlots( const TimelineContext& ctx, int& offset )
{
    const auto& cus = m_worker.GetGpuComputeUnits();
    if( cus.empty() ) return false;

    const auto w = ctx.w;
    const auto ty = ctx.ty;
    const auto pxns = ctx.pxns;
    const auto nspx = ctx.nspx;
    const auto& wpos = ctx.wpos;
    const auto dpos = wpos + ImVec2( 0.5f, 0.5f );
    const auto hover = ctx.hover;
    const auto yMin = ctx.yMin;
    const auto yMax = ctx.yMax;
    const auto slotHeight = std::max( 4.0f, ty * 0.6f );
    const auto step = slotHeight + 1.0f;

    auto draw = ImGui::GetWindowDrawList();

    ImGui::PushFont( g_fonts.normal, FontSmall );
    const auto sty = ImGui::GetTextLineHeight();
    const auto sstep = sty + 2.0f;
    ImGui::PopFont();

    const int64_t vBegin = m_vd.zvStart;
    const int64_t vEnd   = m_vd.zvEnd;

    for( size_t cuIdx = 0; cuIdx < cus.size(); cuIdx++ )
    {
        const auto& cu = cus[cuIdx];

        // Only skip CUs that have no waves in the entire trace
        bool cuHasAnyWaves = false;
        for( int s = 0; s < 4; s++ )
        {
            for( const auto& slot : cu.simds[s].slots )
            {
                if( !slot.waves.empty() )
                {
                    cuHasAnyWaves = true;
                    break;
                }
            }
            if( cuHasAnyWaves ) break;
        }

        if( !cuHasAnyWaves ) continue;

        // Draw CU Header
        char cuBuf[64];
        snprintf( cuBuf, sizeof( cuBuf ), "Compute Unit %u", cu.cuId );
        ImGui::PushFont( g_fonts.normal, FontSmall );
        DrawTextContrast( draw, wpos + ImVec2( ty, offset ), 0xFF55DDFF, cuBuf );
        DrawLine( draw, dpos + ImVec2( 0, offset + sty ), dpos + ImVec2( w, offset + sty ), 0x2255DDFF );
        ImGui::PopFont();
        offset += static_cast<int>( sstep );

        // Draw SIMD Wave Slots
        for( int s = 0; s < 4; s++ )
        {
            const auto& simd = cu.simds[s];

            for( size_t slIdx = 0; slIdx < simd.slots.size(); slIdx++ )
            {
                const auto& slot = simd.slots[slIdx];
                if( slot.waves.empty() ) continue;

                // Binary search visible wave slice
                auto it = std::lower_bound( slot.waves.begin(), slot.waves.end(), vBegin, []( const auto& wave, int64_t t ) {
                    return wave.end.Val() < t;
                } );

                if( it == slot.waves.end() || it->start.Val() > vEnd ) continue;

                const float y0 = offset;
                const float y1 = y0 + slotHeight;

                if( y1 >= yMin && y0 <= yMax )
                {
                    // Slot track background guideline
                    DrawLine( draw, dpos + ImVec2( 0, y1 ), dpos + ImVec2( w, y1 ), 0x11FFFFFF );

                    while( it != slot.waves.end() && it->start.Val() <= vEnd )
                    {
                        const auto& wave = *it;
                        const float px0 = std::max( 0.0, ( wave.start.Val() - vBegin ) * pxns );
                        const float px1 = std::min( static_cast<double>( w ), ( wave.end.Val() - vBegin ) * pxns );

                        if( px1 > px0 )
                        {
                            const ImVec2 minPt = wpos + ImVec2( px0, y0 );
                            const ImVec2 maxPt = wpos + ImVec2( std::max( px0 + 1.0f, px1 ), y1 );

                            // Base wave bar (Dark Cyan/Blue)
                            uint32_t waveColor = 0xFF4488AA;
                            draw->AddRectFilled( minPt, maxPt, waveColor );
                            draw->AddRect( minPt, maxPt, 0x55FFFFFF, 0.0f, 0, 1.0f );

                            // Hover Tooltip and Inspection Click
                            // Generous hit box for hover and click (at least 4 pixels wide)
                            const float hitW = (std::max)( 4.0f, px1 - px0 );
                            const ImVec2 hitMin = wpos + ImVec2( px0, y0 );
                            const ImVec2 hitMax = wpos + ImVec2( px0 + hitW, y1 );

                            // Hover Tooltip and Inspection Click
                            if( hover && ImGui::IsMouseHoveringRect( hitMin, hitMax ) )
                            {
                                ImGui::BeginTooltip();
                                if( wave.waveId > 0 )
                                {
                                    ImGui::TextColored( ImVec4( 0.4f, 0.8f, 1.0f, 1.0f ), "Wave ID: #%u", wave.waveId );
                                }
                                ImGui::Text( "CU %u, SIMD %d, WaveSlot %zu", cu.cuId, s, slIdx );
                                ImGui::Text( "Workgroup: (%u, %u, %u)", wave.workgroupX, wave.workgroupY, wave.workgroupZ );
                                const int64_t durNs = wave.end.Val() - wave.start.Val();
                                ImGui::Text( "Duration: %s", TimeToString( durNs ) );
                                if( wave.stateCount > 0 )
                                {
                                    ImGui::Text( "Execution Slices: %u", wave.stateCount );
                                }
                                ImGui::Separator();
                                ImGui::TextDisabled( "Click to inspect shader disassembly & instruction timing" );
                                ImGui::EndTooltip();

                                if( IsMouseClicked( ImGuiMouseButton_Left ) || IsMouseClickReleased( ImGuiMouseButton_Left ) || ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left ) )
                                {
                                    ShowGpuShader( wave.pipelineHash, wave.waveId, wave.workgroupX, wave.workgroupY, wave.workgroupZ );
                                }
                            }
                        }

                        ++it;
                    }
                }

                offset += static_cast<int>( step );
            }
        }
    }

    return true;
}

void View::DrawGpuShaderWindow()
{
    const auto scale = GetScale();
    ImGui::SetNextWindowSize( ImVec2( 850 * scale, 550 * scale ), ImGuiCond_FirstUseEver );

    if( m_focusGpuShaderWindow )
    {
        ImGui::SetNextWindowFocus();
        m_focusGpuShaderWindow = false;
    }

    if( !ImGui::Begin( "GPU Shader Disassembly & Instruction Timing###GpuShaderWin", &m_showGpuShaderWindow ) )
    {
        ImGui::End();
        return;
    }

    const auto& shaders = m_worker.GetGpuShaders();
    if( shaders.empty() )
    {
        ImGui::TextUnformatted( "No shader disassembly data available." );
        ImGui::End();
        return;
    }

    auto it = shaders.find( m_selectedGpuShader );
    if( it == shaders.end() )
    {
        it = shaders.begin();
        m_selectedGpuShader = it->first;
    }
    const GpuShaderData* pShader = &it->second;

    // Dropdown combo to select any shader
    char curShaderName[128];
    snprintf( curShaderName, sizeof( curShaderName ), "Shader 0x%" PRIx64 " (%zu instructions)", pShader->pipelineHash, pShader->isaLinePcs.size() );
    ImGui::SetNextItemWidth( 350 * scale );
    if( ImGui::BeginCombo( "Shader", curShaderName ) )
    {
        for( const auto& sh : shaders )
        {
            char optName[128];
            snprintf( optName, sizeof( optName ), "Shader 0x%" PRIx64 " (%zu instructions)", sh.first, sh.second.isaLinePcs.size() );
            const bool isSelected = ( sh.first == m_selectedGpuShader );
            if( ImGui::Selectable( optName, isSelected ) )
            {
                m_selectedGpuShader = sh.first;
            }
            if( isSelected ) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled( "(%zu shaders in capture)", shaders.size() );

    // Header info: pipeline hash, registers, stage, occupancy
    if( m_selectedGpuWaveId > 0 )
    {
        ImGui::TextColored( ImVec4( 0.4f, 0.8f, 1.0f, 1.0f ), "Selected Wave: #%u", m_selectedGpuWaveId );
        ImGui::SameLine( 0, 15 * scale );
        ImGui::Text( "Workgroup: (%u, %u, %u)", m_selectedGpuWgX, m_selectedGpuWgY, m_selectedGpuWgZ );
        ImGui::SameLine( 0, 15 * scale );
    }
    ImGui::Text( "Pipeline: 0x%016" PRIx64, pShader->pipelineHash );
    ImGui::SameLine( 0, 15 * scale );
    ImGui::Text( "Stage: %s", pShader->shaderStage == 6 ? "Compute" : ( pShader->shaderStage == 4 ? "Pixel" : "Vertex" ) );
    ImGui::SameLine( 0, 15 * scale );
    ImGui::Text( "VGPRs: %u / 256", pShader->vgprCount );
    ImGui::SameLine( 0, 15 * scale );
    ImGui::Text( "SGPRs: %u / 106", pShader->sgprCount );
    ImGui::SameLine( 0, 15 * scale );
    ImGui::Text( "Max Waves/SIMD: %u", pShader->maxOccupancy );

    ImGui::Separator();

    // Disassembly and Stall Heatmap Table
    const auto flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
    if( ImGui::BeginTable( "ShaderAsmTable", 5, flags ) )
    {
        ImGui::TableSetupColumn( "Offset", ImGuiTableColumnFlags_WidthFixed, 65 * scale );
        ImGui::TableSetupColumn( "Instruction", ImGuiTableColumnFlags_WidthStretch );
        ImGui::TableSetupColumn( "Issue Count", ImGuiTableColumnFlags_WidthFixed, 90 * scale );
        ImGui::TableSetupColumn( "Active Lanes", ImGuiTableColumnFlags_WidthFixed, 95 * scale );
        ImGui::TableSetupColumn( "Stall / Latency Breakdown", ImGuiTableColumnFlags_WidthFixed, 220 * scale );
        ImGui::TableHeadersRow();

        for( size_t i = 0; i < pShader->isaLinePcs.size(); i++ )
        {
            const uint64_t pc = pShader->isaLinePcs[i];
            ImGui::TableNextRow();

            // Offset
            ImGui::TableNextColumn();
            ImGui::Text( "0x%04" PRIx64, pc );

            // Instruction ISA Text
            ImGui::TableNextColumn();
            if( i < pShader->isaLines.size() )
            {
                ImGui::TextUnformatted( m_worker.GetString( pShader->isaLines[i] ) );
            }
            else
            {
                ImGui::TextDisabled( "isa_inst" );
            }

            // Stats lookup
            uint32_t issues = 0;
            uint64_t activeLanes = 0;
            auto sIt = pShader->instructionStats.find( pc );
            if( sIt != pShader->instructionStats.end() )
            {
                issues = sIt->second.issueCount;
                activeLanes = sIt->second.totalActiveLanes;
            }

            // Issue Count
            ImGui::TableNextColumn();
            if( issues > 0 )
            {
                ImGui::Text( "%u", issues );
            }
            else
            {
                ImGui::TextDisabled( "—" );
            }

            // Active Lanes (SIMD Divergence)
            ImGui::TableNextColumn();
            if( issues > 0 )
            {
                const double avgLanes = static_cast<double>( activeLanes ) / issues;
                if( avgLanes < 32.0 )
                {
                    ImGui::TextColored( ImVec4( 1.0f, 0.65f, 0.2f, 1.0f ), "%.1f / 32", avgLanes );
                }
                else
                {
                    ImGui::Text( "%.1f / 32", avgLanes );
                }
            }
            else
            {
                ImGui::TextDisabled( "—" );
            }

            // Stall Heatmap / Latency Breakdown
            ImGui::TableNextColumn();
            const auto cat = ( sIt != pShader->instructionStats.end() ) ? sIt->second.category : 0;
            switch( cat )
            {
            case 6: // Wait
                ImGui::TextColored( ImVec4( 1.0f, 0.85f, 0.2f, 1.0f ), "[WAIT: %uc Memory Sync]", sIt != pShader->instructionStats.end() ? sIt->second.aluStallCycles : 48 );
                break;
            case 5: // Barrier
                ImGui::TextColored( ImVec4( 1.0f, 0.6f, 0.1f, 1.0f ), "[BARRIER: %uc Group Sync]", sIt != pShader->instructionStats.end() ? sIt->second.ldsStallCycles : 64 );
                break;
            case 2: // VMEM
                ImGui::TextColored( ImVec4( 1.0f, 0.35f, 0.35f, 1.0f ), "[VMEM: ~%uc Latency]", sIt != pShader->instructionStats.end() ? sIt->second.vmemStallCycles : 220 );
                break;
            case 3: // SMEM
                ImGui::TextColored( ImVec4( 0.4f, 0.7f, 1.0f, 1.0f ), "[SMEM: %uc Load Latency]", sIt != pShader->instructionStats.end() ? sIt->second.smemStallCycles : 40 );
                break;
            case 4: // LDS
                ImGui::TextColored( ImVec4( 1.0f, 0.75f, 0.3f, 1.0f ), "[LDS: %uc Shared Memory]", sIt != pShader->instructionStats.end() ? sIt->second.ldsStallCycles : 16 );
                break;
            case 7: // Branch
                ImGui::TextColored( ImVec4( 0.8f, 0.5f, 1.0f, 1.0f ), "[BRANCH: 4c Control Flow]" );
                break;
            case 1: // SALU
                ImGui::TextColored( ImVec4( 0.5f, 0.85f, 1.0f, 1.0f ), "[SALU: 1c Active]" );
                break;
            case 0: // VALU
            default:
                ImGui::TextColored( ImVec4( 0.4f, 0.9f, 0.4f, 1.0f ), "[VALU: 4c Active]" );
                break;
            }
        }

        ImGui::EndTable();
    }

    ImGui::End();
}

}
