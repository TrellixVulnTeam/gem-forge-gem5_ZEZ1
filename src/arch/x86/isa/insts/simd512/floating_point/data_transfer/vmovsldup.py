microcode = '''
def macroop VMOVSLDUP_XMM_XMM {
    mmovsdup dest=xmm0, src1=xmm0m, size=4, ext=0
    mmovsdup dest=xmm1, src1=xmm1m, size=4, ext=0
    vclear dest=xmm2, destVL=16
};

def macroop VMOVSLDUP_XMM_M {
    ldfp ufp1, seg, sib, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, sib, "DISPLACEMENT + 8", dataSize=8
    mmovsdup dest=xmm0, src1=ufp1, size=4, ext=0
    mmovsdup dest=xmm1, src1=ufp2, size=4, ext=0
    vclear dest=xmm2, destVL=16
};

def macroop VMOVSLDUP_XMM_P {
    rdip t7
    ldfp ufp1, seg, riprel, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, riprel, "DISPLACEMENT + 8", dataSize=8
    mmovsdup dest=xmm0, src1=ufp1, size=4, ext=0
    mmovsdup dest=xmm1, src1=ufp2, size=4, ext=0
    vclear dest=xmm2, destVL=16
};

def macroop VMOVSLDUP_YMM_YMM {
    mmovsdup dest=xmm0, src1=xmm0m, size=4, ext=0
    mmovsdup dest=xmm1, src1=xmm1m, size=4, ext=0
    mmovsdup dest=xmm2, src1=xmm2m, size=4, ext=0
    mmovsdup dest=xmm3, src1=xmm3m, size=4, ext=0
    vclear dest=xmm4, destVL=32
};

def macroop VMOVSLDUP_YMM_M {
    ldfp ufp1, seg, sib, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, sib, "DISPLACEMENT + 8", dataSize=8
    ldfp ufp3, seg, sib, "DISPLACEMENT + 16", dataSize=8
    ldfp ufp4, seg, sib, "DISPLACEMENT + 24", dataSize=8
    mmovsdup dest=xmm0, src1=ufp1, size=4, ext=0
    mmovsdup dest=xmm1, src1=ufp2, size=4, ext=0
    mmovsdup dest=xmm2, src1=ufp3, size=4, ext=0
    mmovsdup dest=xmm3, src1=ufp4, size=4, ext=0
    vclear dest=xmm4, destVL=32
};

def macroop VMOVSLDUP_YMM_P {
    rdip t7
    ldfp ufp1, seg, riprel, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, riprel, "DISPLACEMENT + 8", dataSize=8
    ldfp ufp3, seg, riprel, "DISPLACEMENT + 16", dataSize=8
    ldfp ufp4, seg, riprel, "DISPLACEMENT + 24", dataSize=8
    mmovsdup dest=xmm0, src1=ufp1, size=4, ext=0
    mmovsdup dest=xmm1, src1=ufp2, size=4, ext=0
    mmovsdup dest=xmm2, src1=ufp3, size=4, ext=0
    mmovsdup dest=xmm3, src1=ufp4, size=4, ext=0
    vclear dest=xmm4, destVL=32
};

def macroop VMOVSLDUP_ZMM_ZMM {
    mmovsdup dest=xmm0, src1=xmm0m, size=4, ext=0
    mmovsdup dest=xmm1, src1=xmm1m, size=4, ext=0
    mmovsdup dest=xmm2, src1=xmm2m, size=4, ext=0
    mmovsdup dest=xmm3, src1=xmm3m, size=4, ext=0
    mmovsdup dest=xmm4, src1=xmm4m, size=4, ext=0
    mmovsdup dest=xmm5, src1=xmm5m, size=4, ext=0
    mmovsdup dest=xmm6, src1=xmm6m, size=4, ext=0
    mmovsdup dest=xmm7, src1=xmm7m, size=4, ext=0
};

def macroop VMOVSLDUP_ZMM_M {
    ldfp ufp1, seg, sib, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, sib, "DISPLACEMENT + 8", dataSize=8
    ldfp ufp3, seg, sib, "DISPLACEMENT + 16", dataSize=8
    ldfp ufp4, seg, sib, "DISPLACEMENT + 24", dataSize=8
    ldfp ufp5, seg, sib, "DISPLACEMENT + 32", dataSize=8
    ldfp ufp6, seg, sib, "DISPLACEMENT + 40", dataSize=8
    ldfp ufp7, seg, sib, "DISPLACEMENT + 48", dataSize=8
    ldfp ufp8, seg, sib, "DISPLACEMENT + 56", dataSize=8
    mmovsdup dest=xmm0, src1=ufp1, size=4, ext=0
    mmovsdup dest=xmm1, src1=ufp2, size=4, ext=0
    mmovsdup dest=xmm2, src1=ufp3, size=4, ext=0
    mmovsdup dest=xmm3, src1=ufp4, size=4, ext=0
    mmovsdup dest=xmm4, src1=ufp5, size=4, ext=0
    mmovsdup dest=xmm5, src1=ufp6, size=4, ext=0
    mmovsdup dest=xmm6, src1=ufp7, size=4, ext=0
    mmovsdup dest=xmm7, src1=ufp8, size=4, ext=0
};

def macroop VMOVSLDUP_ZMM_P {
    rdip t7
    ldfp ufp1, seg, riprel, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, riprel, "DISPLACEMENT + 8", dataSize=8
    ldfp ufp3, seg, riprel, "DISPLACEMENT + 16", dataSize=8
    ldfp ufp4, seg, riprel, "DISPLACEMENT + 24", dataSize=8
    ldfp ufp5, seg, riprel, "DISPLACEMENT + 32", dataSize=8
    ldfp ufp6, seg, riprel, "DISPLACEMENT + 40", dataSize=8
    ldfp ufp7, seg, riprel, "DISPLACEMENT + 48", dataSize=8
    ldfp ufp8, seg, riprel, "DISPLACEMENT + 56", dataSize=8
    mmovsdup dest=xmm0, src1=ufp1, size=4, ext=0
    mmovsdup dest=xmm1, src1=ufp2, size=4, ext=0
    mmovsdup dest=xmm2, src1=ufp3, size=4, ext=0
    mmovsdup dest=xmm3, src1=ufp4, size=4, ext=0
    mmovsdup dest=xmm4, src1=ufp5, size=4, ext=0
    mmovsdup dest=xmm5, src1=ufp6, size=4, ext=0
    mmovsdup dest=xmm6, src1=ufp7, size=4, ext=0
    mmovsdup dest=xmm7, src1=ufp8, size=4, ext=0
};
'''