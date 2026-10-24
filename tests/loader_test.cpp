#include <gtest/gtest.h>
#include <sstream>
#include <vector>
#include <cstring>
#include "tinyarmsim/state.hpp"
#include "tinyarmsim/memory_bus.hpp"
#include "tinyarmsim/loader.hpp"

using namespace tinyarmsim;

// Synthetic ELF32 Header generator helper
static std::vector<uint8_t> create_synthetic_elf32(uint32_t entry_point,
                                                   uint32_t load_vaddr,
                                                   const std::vector<uint8_t>& payload,
                                                   uint32_t mem_size) {
    std::vector<uint8_t> elf(52 + 32 + payload.size(), 0);

    // 1. ELF Header (52 bytes)
    elf[0] = 0x7F; elf[1] = 'E'; elf[2] = 'L'; elf[3] = 'F'; // Magic
    elf[4] = 1; // 32-bit
    elf[5] = 1; // Little endian
    elf[6] = 1; // Current version

    uint16_t e_type = 2; // ET_EXEC
    uint16_t e_machine = 40; // EM_ARM
    uint32_t e_version = 1;
    uint32_t e_entry = entry_point;
    uint32_t e_phoff = 52; // Program headers immediately after ELF header
    uint16_t e_ehsize = 52;
    uint16_t e_phentsize = 32;
    uint16_t e_phnum = 1;

    std::memcpy(&elf[16], &e_type, 2);
    std::memcpy(&elf[18], &e_machine, 2);
    std::memcpy(&elf[20], &e_version, 4);
    std::memcpy(&elf[24], &e_entry, 4);
    std::memcpy(&elf[28], &e_phoff, 4);
    std::memcpy(&elf[40], &e_ehsize, 2);
    std::memcpy(&elf[42], &e_phentsize, 2);
    std::memcpy(&elf[44], &e_phnum, 2);

    // 2. Program Header (32 bytes) for PT_LOAD
    uint32_t p_type = 1; // PT_LOAD
    uint32_t p_offset = 52 + 32; // Offset in file
    uint32_t p_vaddr = load_vaddr;
    uint32_t p_paddr = load_vaddr;
    uint32_t p_filesz = static_cast<uint32_t>(payload.size());
    uint32_t p_memsz = mem_size;
    uint32_t p_flags = 7; // RWX
    uint32_t p_align = 4;

    size_t ph_idx = 52;
    std::memcpy(&elf[ph_idx + 0], &p_type, 4);
    std::memcpy(&elf[ph_idx + 4], &p_offset, 4);
    std::memcpy(&elf[ph_idx + 8], &p_vaddr, 4);
    std::memcpy(&elf[ph_idx + 12], &p_paddr, 4);
    std::memcpy(&elf[ph_idx + 16], &p_filesz, 4);
    std::memcpy(&elf[ph_idx + 20], &p_memsz, 4);
    std::memcpy(&elf[ph_idx + 24], &p_flags, 4);
    std::memcpy(&elf[ph_idx + 28], &p_align, 4);

    // 3. Payload
    std::memcpy(&elf[52 + 32], payload.data(), payload.size());

    return elf;
}

TEST(LoaderTest, LoadRawBinaryToAddress) {
    MemoryBus bus{1024 * 1024};
    std::vector<uint8_t> raw_data = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    std::stringstream stream(std::string(raw_data.begin(), raw_data.end()));

    Loader::load_raw(stream, bus, 0x1000);

    EXPECT_EQ(bus.read8(0x1000), 0x11u);
    EXPECT_EQ(bus.read8(0x1001), 0x22u);
    EXPECT_EQ(bus.read8(0x1002), 0x33u);
    EXPECT_EQ(bus.read8(0x1003), 0x44u);
    EXPECT_EQ(bus.read8(0x1004), 0x55u);
    EXPECT_EQ(bus.read8(0x1005), 0x66u);
}

TEST(LoaderTest, LoadElf32StaticExecutableAndSetPc) {
    MemoryBus bus{256 * 1024 * 1024}; // 256MB
    ArchitecturalState state;

    std::vector<uint8_t> code = {
        0x20, 0x2A, // MOVS R0, #42
        0x00, 0xDF  // SVC #0
    };

    uint32_t entry = 0x08000000;
    uint32_t vaddr = 0x08000000;
    auto elf_bytes = create_synthetic_elf32(entry, vaddr, code, 8); // memsz 8 (4 bytes code + 4 zero bss)

    std::stringstream stream(std::string(elf_bytes.begin(), elf_bytes.end()));
    Loader::load_elf(stream, bus, state);

    EXPECT_EQ(state.get_pc(), 0x08000000u);
    EXPECT_EQ(bus.read16(0x08000000), 0x2A20u); // Little-endian check
    EXPECT_EQ(bus.read16(0x08000002), 0xDF00u);
    // Verify BSS zeroing:
    EXPECT_EQ(bus.read32(0x08000004), 0x00000000u);
}

TEST(LoaderTest, CorruptElfThrowsLoaderException) {
    MemoryBus bus{1024 * 1024};
    ArchitecturalState state;

    std::vector<uint8_t> bad_elf = {0x00, 'E', 'L', 'F', 1, 1, 1};
    std::stringstream stream(std::string(bad_elf.begin(), bad_elf.end()));

    EXPECT_THROW(Loader::load_elf(stream, bus, state), LoaderException);
}

TEST(LoaderTest, FirmwareElfPreservesFlashLmaAndDefersRamInitialization) {
    std::vector<uint8_t> elf(52 + 32 + 8, 0);
    elf[0] = 0x7f; elf[1] = 'E'; elf[2] = 'L'; elf[3] = 'F';
    elf[4] = 1; elf[5] = 1; elf[6] = 1;
    uint16_t machine = 40, phentsize = 32, phnum = 1;
    uint32_t entry = 0x401, phoff = 52;
    std::memcpy(&elf[18], &machine, 2); std::memcpy(&elf[24], &entry, 4);
    std::memcpy(&elf[28], &phoff, 4); std::memcpy(&elf[42], &phentsize, 2);
    std::memcpy(&elf[44], &phnum, 2);
    const auto put32 = [&](size_t at, uint32_t value) { std::memcpy(&elf[at], &value, 4); };
    const size_t data = 52;
    put32(data, 1); put32(data + 4, 84); put32(data + 8, 0x800); put32(data + 12, 0x400);
    put32(data + 16, 4); put32(data + 20, 8);
    elf[84] = 0x44; elf[85] = 0x33; elf[86] = 0x22; elf[87] = 0x11;

    MemoryBus firmware_bus{1024 * 1024};
    ArchitecturalState firmware_state;
    firmware_bus.write32(0x800, 0xaaaaaaaa);
    firmware_bus.write32(0x804, 0xbbbbbbbb);
    std::stringstream firmware_stream(std::string(elf.begin(), elf.end()));
    Loader::load_elf(firmware_stream, firmware_bus, firmware_state, Loader::ElfLoadMode::Firmware);
    EXPECT_EQ(firmware_state.get_pc(), 0x400u);
    EXPECT_EQ(firmware_bus.read32(0x400), 0x11223344u);
    EXPECT_EQ(firmware_bus.read32(0x404), 0u);
    EXPECT_EQ(firmware_bus.read32(0x800), 0xaaaaaaaau);
    EXPECT_EQ(firmware_bus.read32(0x804), 0xbbbbbbbbu);

    MemoryBus default_bus{1024 * 1024};
    ArchitecturalState default_state;
    default_bus.write32(0x400, 0xcccccccc);
    default_bus.write32(0x800, 0xdddddddd);
    default_bus.write32(0x804, 0xeeeeeeee);
    std::stringstream default_stream(std::string(elf.begin(), elf.end()));
    Loader::load_elf(default_stream, default_bus, default_state);
    EXPECT_EQ(default_state.get_pc(), 0x400u);
    EXPECT_EQ(default_bus.read32(0x400), 0xccccccccu);
    EXPECT_EQ(default_bus.read32(0x800), 0x11223344u);
    EXPECT_EQ(default_bus.read32(0x804), 0u);
}
