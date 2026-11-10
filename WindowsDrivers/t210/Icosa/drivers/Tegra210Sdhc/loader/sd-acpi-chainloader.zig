const std = @import("std");
const uefi = std.os.uefi;
const LoadedImage = uefi.protocol.LoadedImage;
const SimpleFileSystem = uefi.protocol.SimpleFileSystem;
const File = uefi.protocol.File;

const AcpiTableProtocol = extern struct {
    install_acpi_table: *const fn (*AcpiTableProtocol, *const anyopaque, usize, *usize) callconv(uefi.cc) uefi.Status,
    uninstall_acpi_table: *const fn (*AcpiTableProtocol, usize) callconv(uefi.cc) uefi.Status,

    pub const guid align(8) = uefi.Guid{
        .time_low = 0xffe06bdd,
        .time_mid = 0x6107,
        .time_high_and_version = 0x46a6,
        .clock_seq_high_and_reserved = 0x7b,
        .clock_seq_low = 0xb2,
        .node = [_]u8{ 0x5a, 0x9c, 0x7e, 0xc5, 0x27, 0x5c },
    };
};

// Compiled from SDH0-Test.asl. It exposes Tegra210 SDMMC1 to Windows as
// ACPI\NVDA0101 with MMIO 0x700B0000..0x700B01FF and GSI 46.
const sdh0_ssdt = [_]u8{
    0x53,0x53,0x44,0x54,0x8c,0x00,0x00,0x00,0x02,0xf6,0x4e,0x56,0x49,0x44,0x49,0x41,
    0x54,0x32,0x31,0x30,0x53,0x44,0x48,0x30,0x01,0x00,0x00,0x00,0x49,0x4e,0x54,0x4c,
    0x08,0x04,0x26,0x20,0x10,0x47,0x06,0x5f,0x53,0x42,0x5f,0x5b,0x82,0x4f,0x05,0x53,
    0x44,0x48,0x30,0x08,0x5f,0x48,0x49,0x44,0x0d,0x4e,0x56,0x44,0x41,0x30,0x31,0x30,
    0x31,0x00,0x08,0x5f,0x43,0x49,0x44,0x0d,0x50,0x4e,0x50,0x30,0x44,0x34,0x30,0x00,
    0x08,0x5f,0x55,0x49,0x44,0x00,0x08,0x5f,0x43,0x43,0x41,0x00,0x08,0x5f,0x52,0x4d,
    0x56,0x01,0x08,0x5f,0x43,0x52,0x53,0x11,0x1a,0x0a,0x17,0x86,0x09,0x00,0x01,0x00,
    0x00,0x0b,0x70,0x00,0x02,0x00,0x00,0x89,0x06,0x00,0x01,0x01,0x2e,0x00,0x00,0x00,
    0x79,0x00,0x14,0x09,0x5f,0x53,0x54,0x41,0x00,0xa4,0x0a,0x0f,
};

fn print(text: []const u8) void {
    const out = uefi.system_table.con_out orelse return;
    var wide: [256:0]u16 = undefined;
    const len = @min(text.len, wide.len - 1);
    for (text[0..len], 0..) |char, index| wide[index] = char;
    wide[len] = 0;
    _ = out.outputString(&wide) catch {};
}

fn fail(text: []const u8) noreturn {
    print("\r\nFAIL: ");
    print(text);
    print("\r\nHold POWER for 12-15 seconds.\r\n");
    const bs = uefi.system_table.boot_services;
    while (true) if (bs) |services| services.stall(1_000_000) catch {} else asm volatile ("wfe");
}

pub fn main() void {
    print("\r\n=== Tegra210 SD ACPI chainloader ===\r\n");
    const bs = uefi.system_table.boot_services orelse fail("Boot Services unavailable.");
    bs.setWatchdogTimer(0, 0, null) catch {};

    const acpi = (bs.locateProtocol(AcpiTableProtocol, null) catch null) orelse
        fail("EFI_ACPI_TABLE_PROTOCOL not found.");
    var table_key: usize = 0;
    if (acpi.install_acpi_table(acpi, &sdh0_ssdt, sdh0_ssdt.len, &table_key) != .success)
        fail("Could not install SDH0 SSDT.");
    print("PASS: SDH0 SSDT installed.\r\n");

    const loaded = (bs.handleProtocol(LoadedImage, uefi.handle) catch null) orelse
        fail("LoadedImage protocol unavailable.");
    const device = loaded.device_handle orelse fail("Source device unavailable.");
    const fs = (bs.handleProtocol(SimpleFileSystem, device) catch null) orelse
        fail("SimpleFileSystem protocol unavailable.");
    const root = fs.openVolume() catch fail("Could not open the microSD filesystem.");
    defer root.close() catch {};

    // Existing Switch WinPE media uses WIN10AA64.EFI. Also accept the two
    // common bootmgfw locations so the loader remains usable with rebuilt media.
    const switch_path = std.unicode.utf8ToUtf16LeStringLiteral("\\EFI\\BOOT\\WIN10AA64.EFI");
    const standard_path = std.unicode.utf8ToUtf16LeStringLiteral("\\EFI\\Microsoft\\Boot\\bootmgfw.efi");
    const root_path = std.unicode.utf8ToUtf16LeStringLiteral("\\bootmgfw.efi");
    const file = root.open(switch_path, .read, .{}) catch
        root.open(standard_path, .read, .{}) catch
        root.open(root_path, .read, .{}) catch
        fail("WIN10AA64.EFI / bootmgfw.efi not found.");
    defer file.close() catch {};

    const info_size = file.getInfoSize(.file) catch fail("Could not query bootmgfw.efi size.");
    const info_buffer = uefi.pool_allocator.alignedAlloc(u8, .of(File.Info.File), info_size) catch
        fail("Out of memory for file information.");
    defer uefi.pool_allocator.free(info_buffer);
    const info = file.getInfo(.file, info_buffer) catch fail("Could not read bootmgfw.efi information.");
    if (info.file_size == 0 or info.file_size > 64 * 1024 * 1024)
        fail("bootmgfw.efi has an invalid size.");

    const image_buffer = uefi.pool_allocator.alloc(u8, @intCast(info.file_size)) catch
        fail("Out of memory for Windows Boot Manager.");
    defer uefi.pool_allocator.free(image_buffer);
    const bytes_read = file.read(image_buffer) catch fail("Could not read bootmgfw.efi.");
    if (bytes_read != image_buffer.len) fail("Short read of bootmgfw.efi.");

    print("PASS: Windows Boot Manager loaded. Starting WinPE...\r\n");
    const child = bs.loadImage(false, uefi.handle, .{ .buffer = image_buffer }) catch
        fail("UEFI LoadImage rejected bootmgfw.efi.");
    // A buffer-loaded image has no source device by default. Windows Boot
    // Manager uses this handle to locate EFI\Microsoft\Boot\BCD.
    const child_loaded = (bs.handleProtocol(LoadedImage, child) catch null) orelse
        fail("LoadedImage protocol missing on Windows Boot Manager.");
    child_loaded.device_handle = device;
    _ = bs.startImage(child) catch fail("Windows Boot Manager returned an error.");
    fail("Windows Boot Manager unexpectedly returned.");
}
