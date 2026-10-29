"""VP2S_Recomp's tools/wrap_elf.py for JC2's code overlays: the same ELF (.modhdr, .text, .data, .init for the static
initializers, .ctors, .bss), but .text ends at the header's text size + 0x40, where JC2's code ends (as in
tools/overlay_functions.py). wrap_elf.py looks for the end past the text size, which in a module with a constructor
table runs into the static initializers just before it (SC_A005: .text then overlapped .init).
  python -I tools/wrap_overlay.py <overlay>.bin <out>.elf [--flags-from <game ELF>]   (JC2_WRAP_ELF: wrap_elf.py)
"""
import importlib.util
import os
import sys

path = os.environ.get('JC2_WRAP_ELF', 'D:/VP2S_Recomp/tools/wrap_elf.py')
spec = importlib.util.spec_from_file_location('wrap_elf', path)
wrap_elf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wrap_elf)
wrap_elf.text_end = lambda module, text: min(text + 0x40, len(module) & ~3)
sys.argv[0] = path
wrap_elf.main()
