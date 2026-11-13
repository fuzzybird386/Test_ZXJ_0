"""Compile the actual protocol.c for ARM and execute golden/fuzz tests in Unicorn.
No PCB/phone access is involved. Install requirements into an isolated directory.
"""
import argparse
import pathlib
import subprocess
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_PC, UC_CPU_ARM_CORTEX_M3

parser = argparse.ArgumentParser()
parser.add_argument('--gcc', default=r'C:\ncs\toolchains\5f5ef0cea0\opt\zephyr-sdk\gnu\arm-zephyr-eabi\bin\arm-zephyr-eabi-gcc.exe')
parser.add_argument('--methods', action='store_true', help='Run real methods.c with deterministic AFE/BLE stubs')
parser.add_argument('--ble', action='store_true', help='Run real ble_link.c against a Bluetooth lifecycle model')
parser.add_argument('--boot', action='store_true', help='Run real main.c startup with injected failures')
parser.add_argument('--board', action='store_true', help='Run real board_io.c LED and power timing')
parser.add_argument('--range', action='store_true', help='Run real AD5941 gain switching against register model')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parents[1]
out = root / 'tests' / 'build'
out.mkdir(exist_ok=True)
if sum((args.methods, args.ble, args.boot, args.board, args.range)) > 1:
    parser.error('Select only one test suite')
test_name = 'range_tests' if args.range else ('board_tests' if args.board else ('ble_tests' if args.ble else ('boot_tests' if args.boot else ('method_tests' if args.methods else 'protocol_tests'))))
elf = out / (test_name + '.elf')
subprocess.run([args.gcc, '-mcpu=cortex-m3', '-mthumb', '-O1', '-g', '-ffreestanding', '-fno-builtin',
                '-nostdlib', '-ffunction-sections', '-fdata-sections',
                *(['-I' + str(root / 'tests/ble_stubs')] if args.ble or args.board else []),
                '-I' + str(root / 'tests/stubs'), '-I' + str(root / 'include'),
                *(['-I' + str(root / 'vendor/ad5940')] if args.range else []),
                *([str(root / 'src/protocol.c')] if not args.boot else []),
                str(root / ('tests/' + test_name + '.c')), '-T' + str(root / 'tests/test.ld'),
                '-Wl,--gc-sections', '-lm', '-lc', '-lgcc', '-o', str(elf)], check=True)
uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
uc.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M3)
uc.mem_map(0x10000, 0x100000)
uc.mem_map(0x20000000, 0x100000)
with elf.open('rb') as f:
    image = ELFFile(f)
    for segment in image.iter_segments():
        if segment['p_type'] == 'PT_LOAD':
            uc.mem_write(segment['p_vaddr'], segment.data())
    entry = image.header['e_entry']
    symbols = image.get_section_by_name('.symtab')
    checks_addr = symbols.get_symbol_by_name('checks')[0]['st_value']
uc.reg_write(UC_ARM_REG_SP, 0x200ff000)
uc.reg_write(UC_ARM_REG_LR, 0x100001)
uc.emu_start(entry | 1, 0x100000, count=200000000)
if uc.reg_read(UC_ARM_REG_PC) != 0x100000:
    raise SystemExit('FAIL: emulator instruction limit reached before test returned')
failed_line = uc.reg_read(UC_ARM_REG_R0)
checks = int.from_bytes(uc.mem_read(checks_addr, 4), 'little')
if failed_line:
    raise SystemExit(f'FAIL at {test_name}.c:{failed_line}; checks={checks}')
print(f'PASS: {test_name}: {checks} assertions on actual ARM-compiled firmware C code')
if args.methods:
    import csv
    with elf.open('rb') as f:
        sym = ELFFile(f).get_section_by_name('.symtab')
        trace_addr = sym.get_symbol_by_name('user_trace')[0]['st_value']
        count_addr = sym.get_symbol_by_name('user_trace_count')[0]['st_value']
    count = int.from_bytes(uc.mem_read(count_addr, 4), 'little')
    trace = bytes(uc.mem_read(trace_addr, count * 14))
    with (out / 'user_cv_model_trace.csv').open('w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['potential_mV', 'current_nA', 'cycle'])
        for i in range(count):
            frame = trace[i*14:(i+1)*14]
            writer.writerow([int.from_bytes(frame[8:10], 'little', signed=True),
                             int.from_bytes(frame[10:14], 'little', signed=True), 1])
    print('Model trace saved: tests/build/user_cv_model_trace.csv (synthetic currents, actual C scan/frame order)')


