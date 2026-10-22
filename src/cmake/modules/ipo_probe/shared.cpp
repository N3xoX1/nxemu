int nxemu_ipo_cxx();
#ifdef NXEMU_PROBE_OBJCXX
int nxemu_ipo_objcxx();
#endif
int nxemu_ipo_shared() {
#ifdef NXEMU_PROBE_OBJCXX
    if (nxemu_ipo_objcxx() != 42) return 0;
#endif
    return nxemu_ipo_cxx();
}
