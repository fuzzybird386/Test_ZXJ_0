"""ARM instruction-level tests. pip install unicorn pyelftools; no board needed."""
import os, sys, subprocess, pathlib, struct
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR
from elftools.elf.elffile import ELFFile
root=pathlib.Path(__file__).resolve().parents[1]
tc=pathlib.Path(os.environ.get('ECW_TOOLCHAIN','C:/ncs/toolchains/5f5ef0cea0'))
gcc=tc/'opt/zephyr-sdk/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc.exe'
out=root/'tests/out';out.mkdir(exist_ok=True)
elf=out/'proxies.elf'
cmd=[str(gcc),'-mcpu=cortex-m3','-mthumb','-Os','-g','-ffunction-sections','-fdata-sections',
     '-DCHIPSEL_594X=1','-D__packed=__attribute__((packed))','-I'+str(root/'tests/stubs'),'-I'+str(root),
     str(root/'tests/proxy_harness.c'),str(root/'src/method/proxy/ec_proxy_cv.c'),
     str(root/'src/method/proxy/ec_proxy_dpv.c'),'-nostartfiles','-Wl,--gc-sections',
     '-T'+str(root/'tests/test.ld'),'-Wl,-e,test_entry','-o',str(elf),'-lm','-lc','-lgcc']
subprocess.run(cmd,check=True)
u=Uc(UC_ARCH_ARM,UC_MODE_THUMB|UC_MODE_MCLASS)
u.mem_map(0x10000,0x100000);u.mem_map(0x20000000,0x100000)
with elf.open('rb') as f:
 e=ELFFile(f)
 for seg in e.iter_segments():
  if seg['p_type']=='PT_LOAD' and seg['p_filesz']: u.mem_write(seg['p_vaddr'],seg.data())
 symbols={s.name:s['st_value'] for s in e.get_section_by_name('.symtab').iter_symbols()}
u.reg_write(UC_ARM_REG_SP,0x200ff000);u.reg_write(UC_ARM_REG_LR,0x100001)
u.emu_start(symbols['test_entry']|1,0x100000,count=10000000)
result=struct.unpack('<i',u.mem_read(symbols['test_result'],4))[0]
count=struct.unpack('<i',u.mem_read(symbols['test_count'],4))[0]
assert result==-1,f'FAIL at harness line {result}, checks={count}'
print(f'PASS: {count} assertions on compiled CV/DPV ARM code')
