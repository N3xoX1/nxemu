# Scalar JIT round-trips (V14-C)

The x64 backend keeps a low U32 in an XMM register when extracting vector lane zero or extending an XMM-resident U32. It clears all upper lanes: scalar FP post-processing can otherwise observe unrelated signaling NaNs and change FPSR. SSE4.1 is detected before using INSERTPS; extraction retains a SSE2 mask fallback and extension retains its original GPR fallback.

The common constant-propagation pass replaces extraction of lane zero from `ZeroExtendLongToQuad(ZeroExtendWordToLong(x))` with the original U32. This is an integer bit identity; it does not replace or reassociate FP arithmetic. Both A64 backends run this pass. The x64 changes do not apply to ARM hosts.

`run.ps1` links the actual JIT objects from an unmodified reference and the candidate. In a Visual Studio developer PowerShell, provide `-ReferenceLibrary`, `-CandidateLibrary` and `-DependencyDirectory` (containing fmt/mcl/zydis/Zycore static libraries). The two JIT static libraries should be assembled from the CPU project's objects in separate, otherwise identical Release builds. The fixture uses no game files or captured code. It executes 32,768 deterministic cases per accuracy and compares all 32 vector registers, 31 GPRs, FPCR/FPSR/PSTATE and RAM writes, including random bit patterns, NaN payloads, signaling NaNs, subnormals, signed zero, rounding/FZ/DN, shared vector consumers and every lane. It checks terminal SVC execution to avoid comparing empty runs after a stale code-cache reset.

Additional pre-commit validation used an expanded captured-instruction fixture: 167,936 cases per Safe/Auto setting, with complete state identical to the original backend. The common pass cross-compiled for AArch64 GNU/Linux; no ARM hardware performance claim is made. Mario Cappy F9 reference/V13-AB means were 163.63/195.81 FPS; two subsequent C runs were 198.44/199.02 versus CD 196.53 in their common post-five-second window. These are separate measurements, not a universal FPS guarantee.

The commit includes V13-AB plus C only. It excludes CD's extra scalar reuse, other screened strategies, benchmark instrumentation and game-specific patches.
