#pragma once

#include <cstdint>
#include <istream>
#include <vector>
#include <cstring>
#include <stdexcept>
#include "tinyarmsim/state.hpp"
#include "tinyarmsim/memory_bus.hpp"

namespace tinyarmsim {

class LoaderException : public std::runtime_error {
public:
    explicit LoaderException(const std::string& message)
        : std::runtime_error("Loader error: " + message) {}
};

class Loader {
public:
    enum class ElfLoadMode { Default, Firmware };
    static void load_raw(std::istream& stream, MemoryBus& bus, uint32_t base_address) {
        std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(stream)),
                                     std::istreambuf_iterator<char>());
        for (size_t i = 0; i < buffer.size(); ++i) {
            bus.write8(base_address + static_cast<uint32_t>(i), buffer[i]);
        }
    }

    static void load_elf(std::istream& stream, MemoryBus& bus, ArchitecturalState& state) {
        load_elf(stream, bus, state, ElfLoadMode::Default);
    }

    static void load_elf(std::istream& stream, MemoryBus& bus, ArchitecturalState& state,
                         ElfLoadMode mode) {
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(stream)),
                                  std::istreambuf_iterator<char>());

        if (data.size() < 52) {
            throw LoaderException("File too small to be an ELF32 executable");
        }

        // Validate Magic: 0x7F 'E' 'L' 'F'
        if (data[0] != 0x7F || data[1] != 'E' || data[2] != 'L' || data[3] != 'F') {
            throw LoaderException("Invalid ELF magic number");
        }

        // Validate 32-bit (EI_CLASS == 1) and Little Endian (EI_DATA == 1)
        if (data[4] != 1) {
            throw LoaderException("Only 32-bit ELF binaries are supported");
        }
        if (data[5] != 1) {
            throw LoaderException("Only Little-Endian ELF binaries are supported");
        }

        uint16_t e_machine;
        std::memcpy(&e_machine, &data[18], 2);
        if (e_machine != 40) { // EM_ARM = 40 (0x28)
            throw LoaderException("ELF architecture is not ARM (EM_ARM)");
        }

        uint32_t e_entry;
        uint32_t e_phoff;
        uint16_t e_phentsize;
        uint16_t e_phnum;

        std::memcpy(&e_entry, &data[24], 4);
        std::memcpy(&e_phoff, &data[28], 4);
        std::memcpy(&e_phentsize, &data[42], 2);
        std::memcpy(&e_phnum, &data[44], 2);

        constexpr size_t minimum_program_header_size = 32;
        if (e_phnum != 0 && e_phentsize < minimum_program_header_size) {
            throw LoaderException("Program header entry is too small");
        }
        if (e_phoff > data.size() ||
            (e_phnum != 0 && static_cast<size_t>(e_phnum) >
                (data.size() - e_phoff) / e_phentsize)) {
            throw LoaderException("Program header table exceeds file size");
        }

        // Load segments
        for (uint16_t i = 0; i < e_phnum; ++i) {
            const size_t ph_offset = static_cast<size_t>(e_phoff) +
                static_cast<size_t>(i) * e_phentsize;

            uint32_t p_type;
            uint32_t p_offset;
            uint32_t p_vaddr;
            uint32_t p_paddr;
            uint32_t p_filesz;
            uint32_t p_memsz;

            std::memcpy(&p_type, &data[ph_offset + 0], 4);
            std::memcpy(&p_offset, &data[ph_offset + 4], 4);
            std::memcpy(&p_vaddr, &data[ph_offset + 8], 4);
            std::memcpy(&p_paddr, &data[ph_offset + 12], 4);
            std::memcpy(&p_filesz, &data[ph_offset + 16], 4);
            std::memcpy(&p_memsz, &data[ph_offset + 20], 4);

            if (p_type == 1) { // PT_LOAD
                if (p_offset > data.size() ||
                    static_cast<size_t>(p_filesz) > data.size() - p_offset) {
                    throw LoaderException("PT_LOAD segment data exceeds file size");
                }

                const uint32_t load_address = mode == ElfLoadMode::Firmware ? p_paddr : p_vaddr;
                if (p_memsz < p_filesz ||
                    static_cast<uint64_t>(load_address) + p_filesz >
                        static_cast<uint64_t>(UINT32_MAX) + 1u ||
                    static_cast<uint64_t>(p_vaddr) + p_memsz >
                        static_cast<uint64_t>(UINT32_MAX) + 1u) {
                    throw LoaderException("PT_LOAD segment memory range is invalid");
                }
                for (uint32_t b = 0; b < p_filesz; ++b) {
                    bus.write8(load_address + b, data[p_offset + b]);
                }

                if (mode == ElfLoadMode::Default || load_address == p_vaddr) {
                    for (uint32_t b = p_filesz; b < p_memsz; ++b) {
                        bus.write8(p_vaddr + b, 0);
                    }
                }
            }
        }

        // Set entry point (mask out bit 0 for Thumb entry addresses)
        state.set_pc(e_entry & ~1u);
    }

    // Cortex-M hardware reset sequence: loads initial MSP from VTOR+0 and Reset_Handler PC from VTOR+4
    static void boot_cortex_m(const MemoryBus& bus, ArchitecturalState& state, uint32_t vtor_base = 0x00000000) {
        uint32_t initial_msp = bus.read32(vtor_base);
        uint32_t reset_vector = bus.read32(vtor_base + 4u);
        state.set_sp(initial_msp);
        state.set_pc(reset_vector & ~1u);
    }
};

} // namespace tinyarmsim
