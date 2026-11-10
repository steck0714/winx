#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build the ARM64 GPU backend objects and run its host format tests.

Builds the static backend and optional render-only or KMDF probe packages.
Use LLVM, an external mesa-switch checkout and official WDK/SDK Include roots.
"""
import argparse
from pathlib import Path
import shutil
import subprocess

MESA_REVISION = "0ff52079ca9e9e837ed27eb95a8d239f439a1864"
root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--mesa", required=True, type=Path)
parser.add_argument("--wdk-include", required=True, type=Path)
parser.add_argument("--sdk-include", required=True, type=Path)
parser.add_argument("--out", required=True, type=Path)
parser.add_argument("--cc", default="clang")
parser.add_argument("--clang-cl", default="clang-cl")
parser.add_argument("--lib", default="llvm-lib")
parser.add_argument("--link", default="lld-link")
parser.add_argument("--dlltool", default="llvm-dlltool")
parser.add_argument("--probe", action="store_true", help="also build the opt-in KMDF probe and its SwitchFan bridge")
parser.add_argument("--render", action="store_true", help="build the opt-in WDDM render-only package")
parser.add_argument("--gr-firmware", type=Path, help="external NVIDIA tegra21x NETB_img.bin/fecs.bin/gpccs.bin directory")
parser.add_argument("--acr-firmware", type=Path, help="external NVIDIA 32.7.6 ACR/PMU/FECS files")
parser.add_argument("--keep-fan", action="store_true", help="with --probe, preserve the existing signed SwitchFan package")
parser.add_argument("--windows-tests", action="store_true", help="build the real-WDK paging test executable for ARM64 Windows")
args = parser.parse_args()
if args.keep_fan and not args.probe:
    parser.error("--keep-fan requires --probe")
gr_names=("NETB_img.bin", "fecs.bin", "gpccs.bin")
if (args.probe or args.render) and (not args.gr_firmware or
    any(not (args.gr_firmware / n).is_file() for n in gr_names)):
    parser.error("--probe and --render require --gr-firmware with the three external NVIDIA GR files")
acr_names=("nv_acr_ucode_prod.bin", "pmu_bl.bin", "gpmu_ucode_image.bin",
           "gpmu_ucode_desc.bin", "pmu_sig.bin", "fecs_sig.bin")
if (args.probe or args.render) and (not args.acr_firmware or
    any(not (args.acr_firmware / n).is_file() for n in acr_names)):
    parser.error("--probe and --render require --acr-firmware with all six external NVIDIA ACR files")
revision = subprocess.check_output(
    ["git", "-C", str(args.mesa), "rev-parse", "HEAD"], text=True).strip()
if revision != MESA_REVISION:
    parser.error(f"mesa-switch must be at {MESA_REVISION}; got {revision}")
args.out.mkdir(parents=True, exist_ok=True)
mesa_src = args.mesa / "src"
mesa_includes = [mesa_src, mesa_src / "nouveau/headers",
                 mesa_src / "nouveau/headers/nvidia/classes"]
encoder = mesa_src / "nouveau/horizon/nouveau_horizon_gm20b.c"
core = [root / "core/gm20b_mmu.c", root / "core/report.c", root / "core/gpfifo.c", root / "core/fault.c",
        root / "core/execute.c", root / "core/submission.c", root / "core/channel.c",
        root / "core/command_stream.c", root / "core/paging_dma.c",
        root / "platform/t214_mmu.c", root / "platform/t214_frontend.c",
        root / "platform/t214_gr.c", root / "platform/t214_acr.c", root / "platform/t214_pmu.c",
        root / "core/gr_firmware.c", root / "core/acr_firmware.c", root / "core/gr_image.c",
        root / "core/gr_submit.c", root / "core/gr_clear.c", root / "core/gr_draw.c",
        root / "platform/t214_gr_context.c", root / "platform/t214_interrupt.c", encoder]
host = args.out / "test-gm20b-core"
subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                *[f"-I{p}" for p in mesa_includes], *map(str, core), str(root / "tests/test_core.c"),
                str(root / "tests/test_execute_init.c"),
                str(root / "tests/test_boot_bar1.c"),
                "-o", str(host)], check=True)
subprocess.run([str(host.resolve())], check=True)
stream_test=args.out / "test-command-stream"
subprocess.run([args.cc,"-std=c11","-O2","-Wall","-Wextra","-Werror",
                *[f"-I{p}" for p in mesa_includes],str(root / "core/command_stream.c"),
                str(root / "tests/test_command_stream.c"),"-o",str(stream_test)],check=True)
subprocess.run([str(stream_test.resolve())],check=True)
power = root / "platform/t214_power.c"
power_test = args.out / "test-t214-power"
subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                str(power), str(root / "tests/test_power.c"), "-o", str(power_test)], check=True)
subprocess.run([str(power_test.resolve())], check=True)
clock = root / "platform/t214_clock.c"
clock_test = args.out / "test-t214-clock"
subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                str(clock), str(root / "tests/test_clock.c"), "-o", str(clock_test)], check=True)
subprocess.run([str(clock_test.resolve())], check=True)
frontend_test = args.out / "test-t214-frontend"
subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                str(root / "platform/t214_frontend.c"), str(root / "tests/test_frontend.c"),
                "-o", str(frontend_test)], check=True)
subprocess.run([str(frontend_test.resolve())], check=True)
interrupt_test=args.out / "test-interrupt"
subprocess.run([args.cc,"-std=c11","-O2","-Wall","-Wextra","-Werror",
                str(root / "platform/t214_interrupt.c"),str(root / "tests/test_interrupt.c"),
                "-o",str(interrupt_test)],check=True)
subprocess.run([str(interrupt_test.resolve())],check=True)
tlb = root / "platform/t214_mmu.c"
tlb_test = args.out / "test-tlb"
subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                str(tlb), str(root / "tests/test_tlb.c"), "-o", str(tlb_test)], check=True)
subprocess.run([str(tlb_test.resolve())], check=True)
channel_test=args.out / "test-channel"
subprocess.run([args.cc,"-std=c11","-O2","-Wall","-Wextra","-Werror",
                str(root / "core/channel.c"),str(root / "core/fault.c"),str(tlb),
                str(root / "platform/t214_frontend.c"),str(root / "tests/test_channel.c"),
                "-o",str(channel_test)],check=True)
subprocess.run([str(channel_test.resolve())],check=True)
gr_firmware = root / "core/gr_firmware.c"
gr_firmware_test = args.out / "test-gr-firmware"
subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                str(gr_firmware), str(root / "tests/test_gr_firmware.c"),
                "-o", str(gr_firmware_test)], check=True)
subprocess.run([str(gr_firmware_test.resolve())], check=True)
acr_test = args.out / "test-acr-firmware"
subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                str(root / "core/acr_firmware.c"), str(gr_firmware),
                str(root / "tests/test_acr_firmware.c"), "-o", str(acr_test)], check=True)
subprocess.run([str(acr_test.resolve()), *([str(args.acr_firmware.resolve())] if args.acr_firmware else [])], check=True)
acr_boot_test = args.out / "test-acr-boot"
subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                str(root / "platform/t214_acr.c"), str(root / "tests/test_acr_boot.c"),
                "-o", str(acr_boot_test)], check=True)
subprocess.run([str(acr_boot_test.resolve())], check=True)
pmu_test = args.out / "test-pmu-boot"
subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                str(root / "platform/t214_pmu.c"), str(root / "tests/test_pmu_boot.c"),
                "-o", str(pmu_test)], check=True)
subprocess.run([str(pmu_test.resolve())], check=True)
gr = root / "platform/t214_gr.c"
gr_test = args.out / "test-gr-boot"
subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                str(gr), str(gr_firmware), str(root / "platform/t214_frontend.c"),
                str(root / "tests/test_gr_boot.c"), "-o", str(gr_test)], check=True)
subprocess.run([str(gr_test.resolve())], check=True)
gr_image_test = args.out / "test-gr-image"
subprocess.run([args.cc,"-std=c11","-O2","-Wall","-Wextra","-Werror",
                str(root / "core/gr_image.c"),str(root / "tests/test_gr_image.c"),
                "-o",str(gr_image_test)],check=True)
subprocess.run([str(gr_image_test.resolve())],check=True)
gr_clear_test = args.out / "test-gr-clear"
subprocess.run([args.cc,"-std=c11","-O2","-Wall","-Wextra","-Werror",
                *[f"-I{p}" for p in mesa_includes],
                str(root / "core/gr_clear.c"),str(root / "core/report.c"),str(root / "core/command_stream.c"),str(encoder),
                str(root / "tests/test_gr_clear.c"),"-o",str(gr_clear_test)],check=True)
subprocess.run([str(gr_clear_test.resolve())],check=True)
gr_draw_test=args.out / "test-gr-draw"
subprocess.run([args.cc,"-std=c11","-O2","-Wall","-Wextra","-Werror",
                *[f"-I{p}" for p in mesa_includes],
                str(root / "core/gr_draw.c"),str(root / "core/report.c"),str(root / "core/command_stream.c"),str(encoder),
                str(root / "tests/test_gr_draw.c"),"-o",str(gr_draw_test)],check=True)
subprocess.run([str(gr_draw_test.resolve())],check=True)
gr_context_test = args.out / "test-gr-context"
subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                str(root / "platform/t214_gr_context.c"),str(root / "core/gr_image.c"), str(gr), str(gr_firmware),
                str(root / "platform/t214_frontend.c"), str(root / "core/gm20b_mmu.c"),
                str(root / "tests/test_gr_context.c"), "-o", str(gr_context_test)], check=True)
subprocess.run([str(gr_context_test.resolve()),
                *([str((args.gr_firmware / "NETB_img.bin").resolve())] if args.gr_firmware else [])], check=True)

includes = [args.wdk_include / "km/crt", args.wdk_include / "km",
            args.wdk_include / "shared", args.sdk_include / "shared",
            args.sdk_include / "ucrt"]
flags = ["--target=aarch64-pc-windows-msvc", "/nologo", "/c", "/kernel",
         "/GS-", "/Gy", "/Zl", "/O2", "/W4", "/WX", "/std:c11",
         "/clang:-ffreestanding", "-Wno-microsoft-anon-tag", "-Wno-pragma-pack",
         "-Wno-ignored-pragma-intrinsic", "-Wno-nonportable-include-path",
         "/D_ARM64_", "/DARM64", "/D_WIN64", "/D_WIN32_WINNT=0x0A00",
         "/DNTDDI_VERSION=0x0A00000C", "/DNDEBUG",
         "/DDXGKDDI_INTERFACE_VERSION=DXGKDDI_INTERFACE_VERSION_WDDM2_6",
         *[f"/I{p}" for p in mesa_includes], *[f"/imsvc{p}" for p in includes]]
objects = []
for source in [*core, power, clock, root / "windows/resources.c",
               root / "windows/private_memory.c", root / "windows/paging.c",
               root / "windows/table_segment.c", root / "windows/root_table.c",
               root / "windows/paging_buffer.c",root / "windows/paging_dispatch.c",
               root / "windows/paging_submit.c",root / "windows/context.c",
               root / "windows/internal_va.c", root / "windows/submit_worker.c",
               root / "windows/graphics.c",root / "windows/hardware.c",
               root / "windows/work_queue.c", root / "windows/allocation.c",
               root / "windows/query.c", root / "windows/adapter.c",
               root / "windows/submit_ddi.c",root / "windows/command_snapshot.c",root / "windows/recovery.c",
               root / "windows/platform_io.c",root / "windows/worker.c",
               root / "windows/platform_adapter.c",root / "windows/gr_firmware_io.c",
               root / "windows/reset_backend.c",root / "windows/adapter_reset.c"]:
    obj = args.out / (source.stem + ".obj")
    subprocess.run([args.clang_cl, *flags, f"/Fo{obj}", "--", str(source)], check=True)
    objects.append(obj)
library = args.out / "TegraGpuCore.lib"
subprocess.run([args.lib, "/nologo", "/machine:arm64", f"/out:{library}",
                *map(str, objects)], check=True)
print(f"ARM64 kernel library: {library} (hardware execution not tested)")

if args.windows_tests:
    test_obj = args.out / "test_paging_windows.obj"
    subprocess.run([args.clang_cl, *flags, "/clang:-funwind-tables", f"/Fo{test_obj}", "--",
                    str(root / "tests/test_paging_windows.c")], check=True)
    # The SDK header NuGet does not ship import libraries. Use the same LLVM
    # import-library construction as the existing performance tool.
    exports = args.out / "paging-kernel32.def"
    exports.write_text("LIBRARY kernel32.dll\nEXPORTS\nExitProcess\nGetStdHandle\nWriteFile\n")
    sdk_lib = args.out / "paging-kernel32.lib"
    subprocess.run([args.dlltool, "-m", "arm64", "-d", str(exports), "-l", str(sdk_lib)], check=True)
    subprocess.run([args.link, "/nologo", "/machine:arm64", "/subsystem:console,10.0",
                    "/entry:start", "/include:_load_config_used", "/nodefaultlib", "/release", "/opt:ref", "/timestamp:0",
                    f"/out:{args.out / 'test-paging.exe'}", str(test_obj),
                    str(args.out / "paging.obj"), str(args.out / "gm20b_mmu.obj"),
                    str(args.out / "table_segment.obj"), str(args.out / "private_memory.obj"),
                    str(args.out / "root_table.obj"), str(args.out / "t214_mmu.obj"),
                    str(args.out / "paging_buffer.obj"), str(args.out / "paging_dma.obj"),
                    str(args.out / "resources.obj"),
                    str(sdk_lib)], check=True)
    context_test = args.out / "test_context_windows.obj"
    subprocess.run([args.clang_cl, *flags, "/clang:-funwind-tables", f"/Fo{context_test}", "--",
                    str(root / "tests/test_context_windows.c")], check=True)
    subprocess.run([args.link, "/nologo", "/machine:arm64", "/subsystem:console,10.0",
                    "/entry:start", "/include:_load_config_used", "/nodefaultlib", "/release", "/opt:ref", "/timestamp:0",
                    f"/out:{args.out / 'test-context.exe'}", str(context_test),
                    str(library), str(sdk_lib)], check=True)
    hardware_test=args.out / "test_hardware_windows.obj"
    subprocess.run([args.clang_cl,*flags,"/clang:-funwind-tables",f"/Fo{hardware_test}","--",
                    str(root / "tests/test_hardware_windows.c")],check=True)
    subprocess.run([args.link,"/nologo","/machine:arm64","/subsystem:console,10.0","/entry:start",
                    "/include:_load_config_used","/nodefaultlib","/release","/opt:ref","/timestamp:0",
                    f"/out:{args.out / 'test-hardware.exe'}",str(hardware_test),str(args.out / "hardware.obj"),
                    str(args.out / "reset_backend.obj"),str(args.out / "private_memory.obj"),str(sdk_lib)],check=True)
    platform_test=args.out / "test_platform_windows.obj"
    subprocess.run([args.clang_cl,*flags,"/clang:-funwind-tables",f"/Fo{platform_test}","--",
                    str(root / "tests/test_platform_windows.c")],check=True)
    subprocess.run([args.link,"/nologo","/machine:arm64","/subsystem:console,10.0","/entry:start",
                    "/include:_load_config_used","/nodefaultlib","/release","/opt:ref","/timestamp:0",
                    f"/out:{args.out / 'test-platform.exe'}",str(platform_test),
                    str(args.out / "platform_io.obj"),str(sdk_lib)],check=True)
    worker_test=args.out / "test_worker_windows.obj"
    subprocess.run([args.clang_cl,*flags,"/clang:-funwind-tables",f"/Fo{worker_test}","--",
                    str(root / "tests/test_worker_windows.c")],check=True)
    subprocess.run([args.link,"/nologo","/machine:arm64","/subsystem:console,10.0","/entry:start",
                    "/include:_load_config_used","/nodefaultlib","/release","/opt:ref","/timestamp:0",
                    f"/out:{args.out / 'test-worker.exe'}",str(worker_test),
                    str(args.out / "worker.obj"),str(sdk_lib)],check=True)
    diagnostic_test=args.out / "test_diagnostic_windows.obj"
    subprocess.run([args.clang_cl,*flags,"/clang:-funwind-tables",f"/Fo{diagnostic_test}","--",
                    str(root / "tests/test_diagnostic_windows.c")],check=True)
    subprocess.run([args.link,"/nologo","/machine:arm64","/subsystem:console,10.0","/entry:start",
                    "/include:_load_config_used","/nodefaultlib","/release","/opt:ref","/timestamp:0",
                    f"/out:{args.out / 'test-diagnostic.exe'}",str(diagnostic_test),str(sdk_lib)],check=True)

def copy_firmware(dest):
    for filename in gr_names:
        shutil.copyfile(args.gr_firmware / filename, dest / filename)
    for filename in acr_names:
        shutil.copyfile(args.acr_firmware / filename, dest / filename)

if args.render:
    dest = args.out / "kit/tegragpurender"
    dest.mkdir(parents=True, exist_ok=True)
    obj = args.out / "render_miniport.obj"
    km_lib = args.wdk_include.parent.parent / "Lib" / args.wdk_include.name / "km/ARM64"
    subprocess.run([args.clang_cl, *flags, "/GS", f"/Fo{obj}", "--",
                    str(root / "windows/render_miniport.c")], check=True)
    subprocess.run([args.link, "/nologo", "/driver", "/subsystem:native,10.0", "/machine:arm64",
                    "/entry:GsDriverEntry", "/include:_load_config_used", "/nodefaultlib", "/release",
                    "/opt:ref", "/tsaware:no", "/section:.text,ERP", "/section:.rdata,RP",
                    "/section:.data,RWP", "/section:.pdata,RP", "/section:INIT,ERD", "/timestamp:0",
                    f"/out:{dest / 'tegragpurender.sys'}", str(obj), str(library),
                    *[str(km_lib / name) for name in ("displib.lib", "ntoskrnl.lib", "hal.lib",
                                                    "bufferoverflowfastfailk.lib", "libcntpr.lib")]], check=True)
    umd = args.out / "no_d3d.obj"
    umd_includes = [args.sdk_include / "um", args.wdk_include / "km/crt",
                    args.wdk_include / "shared", args.sdk_include / "shared",
                    args.sdk_include / "ucrt"]
    subprocess.run([args.clang_cl, "--target=aarch64-pc-windows-msvc", "/nologo", "/c",
                    "/O2", "/GS-", "/Zl", "/W4", "/WX", "/std:c11",
                    "-Wno-nonportable-include-path", "/D_ARM64_",
                    *[f"/imsvc{p}" for p in umd_includes],
                    f"/Fo{umd}", "--", str(root / "windows/no_d3d.c")], check=True)
    subprocess.run([args.link, "/nologo", "/dll", "/noentry", "/machine:arm64", "/nodefaultlib",
                    "/release", "/opt:ref", "/timestamp:0", f"/out:{dest / 'tegragpuno3d.dll'}",
                    f"/implib:{args.out / 'tegragpuno3d.lib'}", str(umd)], check=True)
    shutil.copyfile(root / "windows/tegragpurender.inf", dest / "tegragpurender.inf")
    copy_firmware(dest)
    print(f"Unsigned MCDM package: {dest}; requires catalog generation and signing.")

if args.probe:
    kit = args.out / "kit"
    wdk_root = args.wdk_include.parent.parent
    wdf_inc = args.wdk_include.parent / "wdf/kmdf/1.15"
    wdf_lib = wdk_root / "Lib/wdf/kmdf/ARM64/1.15"
    km_lib = wdk_root / "Lib" / args.wdk_include.name / "km/ARM64"
    repo = root.parents[1]
    kflags = [*flags, "/DKMDF_VERSION_MAJOR=1", "/DKMDF_VERSION_MINOR=15",
              "/DPOOL_NX_OPTIN=1", "/DSWITCH_LITE_GPU=1", "-Wno-unused-parameter",
              "-Wno-invalid-token-paste", "-Wno-unknown-pragmas", "-Wno-ignored-attributes",
              f"/imsvc{wdf_inc}", f"/imsvc{args.wdk_include / 'km/spb/1.0'}"]
    support = args.out / "kmdf_support.obj"
    subprocess.run([args.clang_cl, *kflags, f"/Fo{support}", "--", str(repo / "tools/kmdf_support.c")], check=True)
    packages = [("tegragpuprobe", root / "windows/probe.c", root / "windows/tegragpuprobe.inf")]
    if not args.keep_fan:
        packages.append(("switchfan", repo / "drivers/SwitchFan/src/switchfan.c", repo / "drivers/SwitchFan/switchfan.inf"))
    for name, source, inf in packages:
        dest = kit / name
        dest.mkdir(parents=True, exist_ok=True)
        obj = args.out / (name + ".obj")
        subprocess.run([args.clang_cl, *kflags, f"/Fo{obj}", "--", str(source)], check=True)
        libs = [wdf_lib / "wdfdriverentry.lib", wdf_lib / "wdfldr.lib",
                km_lib / "ntoskrnl.lib", km_lib / "hal.lib", km_lib / "ntstrsafe.lib",
                km_lib / "bufferoverflowfastfailk.lib"]
        if name == "tegragpuprobe":
            diagnostic = args.out / "diagnostic_backend.obj"
            subprocess.run([args.clang_cl, *kflags, f"/Fo{diagnostic}", "--",
                            str(root / "windows/diagnostic_backend.c")], check=True)
            libs.append(diagnostic)
            libs.append(library)
            copy_firmware(dest)
        subprocess.run([args.link, "/nologo", "/driver", "/subsystem:native,10.0", "/machine:arm64",
                        "/entry:FxDriverEntry", "/nodefaultlib", "/release", "/opt:ref", "/opt:icf",
                        "/tsaware:no", "/section:.text,ERP", "/section:.rdata,RP",
                        "/section:.data,RWP", "/section:.pdata,RP", "/section:INIT,ERD",
                        "/timestamp:0", f"/out:{dest / (name + '.sys')}",
                        str(obj), str(support), *map(str, libs)], check=True)
        shutil.copyfile(inf, dest / inf.name)
    built = "probe (existing SwitchFan package preserved)" if args.keep_fan else "probe/bridge"
    print(f"Unsigned experimental {built}: {kit}; requires catalog generation and signing.")
