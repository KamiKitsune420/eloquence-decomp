r"""pushes - the stack traffic of an ENU.SYN function: each push with its offset below the entry esp, each
call with where its return address goes (linear, may run past the first ret).  python tools/pushes.py ADDR..."""
# linear esp tracking: every push of a register, with its offset below the entry esp
import sys; sys.path.insert(0,'tools'); import x2c, re
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
img=x2c.Image('pkg/ENU.SYN'); md=Cs(CS_ARCH_X86,CS_MODE_32)
for arg in sys.argv[1:]:
    a=int(arg,16); o=img.off(a); sp=0; out=[]
    for i in md.disasm(img.d[o:o+0x300],a):
        m,ops=i.mnemonic,i.op_str
        if m=='push': sp+=4; out.append('%x:push %s@-%x'%(i.address,ops,sp))
        elif m=='pop': sp-=4
        elif m=='sub' and ops.startswith('esp, '): sp+=int(ops[5:],0)
        elif m=='add' and ops.startswith('esp, '): sp-=int(ops[5:],0)
        elif m=='call': out.append('%x:call %s ret@-%x'%(i.address,ops,sp+4))
        elif m=='ret': out.append('ret(sp=%x)'%sp); 
        if m=='ret' and i.address> a+0x200: break
        if m in ('int3',): break
    print(arg,' '.join(out[:40]))
