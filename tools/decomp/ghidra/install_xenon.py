"""Adds the Xenon language (PowerPC:BE:64:Xenon: 64-bit AltiVec with VMX128 and 32-bit addresses) to a Ghidra
install and compiles it.

  install_xenon.py [ghidra dir]        default D:/ghidra/ghidra_12.2_PUBLIC

Copies ppc_64_xenon_be.slaspec and vmx128.sinc (regenerated from XenonRecomp's table first) into the PowerPC
languages folder, adds the language to ppc.ldefs (once), and runs Ghidra's sleigh compiler on it.
"""
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LANG = '''  <language processor="PowerPC"
            endian="big"
            size="32"
            variant="Xenon"
            version="1.7"
            slafile="ppc_64_xenon_be.sla"
            processorspec="ppc_64.pspec"
            manualindexfile="../manuals/PowerISA.idx"
            id="PowerPC:BE:64:Xenon">
    <description>Xbox 360 Xenon: Power ISA 64-bit Big Endian w/Altivec, VMX128 and 32-bit Addressing</description>
    <truncate_space space="ram" size="4"/>
    <compiler name="default" spec="ppc_64_32.cspec" id="default"/>
    <external_name tool="DWARF.register.mapping.file" name="ppc.dwarf"/>
  </language>
'''


def main():
    ghidra = sys.argv[1] if len(sys.argv) > 1 else 'D:/ghidra/ghidra_12.2_PUBLIC'
    langs = os.path.join(ghidra, 'Ghidra', 'Processors', 'PowerPC', 'data', 'languages')
    root = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
    subprocess.check_call([sys.executable, '-I', os.path.join(HERE, 'vmx128.py'), 'sleigh',
                           os.path.join(HERE, 'vmx128.sinc')], cwd=root)
    # vmx128.sinc's token fields go into ppc_common's instr token (SLEIGH can't combine fields of two tokens in
    # one pattern), so the Xenon spec includes a copy of ppc_common with them spliced in
    vmx = open(os.path.join(HERE, 'vmx128.sinc'), encoding='utf-8').read()
    a, b = vmx.index('#@fields'), vmx.index('#@end')
    fields = '\n'.join('\t' + l.strip() for l in vmx[a:b].splitlines()[1:] if l.strip())
    common = open(os.path.join(langs, 'ppc_common.sinc'), encoding='utf-8').read()
    common = common.replace('define token instr(32)\n', 'define token instr(32)\n' + fields + '\n', 1)
    open(os.path.join(langs, 'ppc_common_xenon.sinc'), 'w', encoding='utf-8', newline='\n').write(common)
    open(os.path.join(langs, 'vmx128.sinc'), 'w', encoding='utf-8', newline='\n').write(vmx[:a] + vmx[b + len('#@end'):])
    # Power ISA 3.0's maddhd/maddhdu/maddld (opcode 4) are more specific than vsldoi128 and would win: the 360
    # doesn't have them, so the Xenon spec uses a copy of ppc_isa without them
    # (and likewise AltiVec's Power ISA 2.07 additions that shadow lvx128 and vsldoi128 for some registers)
    for src, dst, names in (('ppc_isa.sinc', 'ppc_isa_xenon.sinc', ('maddhd', 'maddhdu', 'maddld')),
                            ('altivec.sinc', 'altivec_xenon.sinc', ('vcmpequd', 'vcmpequd.', 'vaddeuqm', 'vaddecuq',
                                                                    'vsubeuqm', 'vsubecuq'))):
        text = open(os.path.join(langs, src), encoding='utf-8').read()
        for name in names:
            a = text.index('\n:%s ' % name)
            b = text.index('\n}', a) + 2
            text = text[:a] + '\n# (%s left out: the 360 lacks it, and it shares an encoding with VMX128)' % name + text[b:]
        open(os.path.join(langs, dst), 'w', encoding='utf-8', newline='\n').write(text)
    shutil.copy(os.path.join(HERE, 'ppc_64_xenon_be.slaspec'), langs)
    ldefs = os.path.join(langs, 'ppc.ldefs')
    text = open(ldefs, encoding='utf-8').read()
    if 'PowerPC:BE:64:Xenon' not in text:
        text = text.replace('</language_definitions>', LANG + '</language_definitions>')
        open(ldefs, 'w', encoding='utf-8', newline='\n').write(text)
    env = dict(os.environ, GHIDRA=ghidra)
    r = subprocess.run(['cmd', '/c', os.path.join(HERE, 'sleigh_xenon.cmd')], capture_output=True, text=True,
                       stdin=subprocess.DEVNULL, env=env)
    print(r.stdout[-4000:], r.stderr[-4000:])
    return r.returncode


if __name__ == '__main__':
    sys.exit(main())
