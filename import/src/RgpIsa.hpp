#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tracy::rgp_isa
{

struct IsaInst
{
    uint64_t offset = 0;
    uint8_t size = 4;
    std::string mnemonic;
    std::string operands;
};

bool DisassembleCodeObject( const uint8_t* elf, size_t elfSize, const char* mcpu, const char* llvmObjdump,
    std::vector<IsaInst>& instructions, std::string& error );

}