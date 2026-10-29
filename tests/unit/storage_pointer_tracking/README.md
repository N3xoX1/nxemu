# Loop-invariant storage pointers

The Mario Party fragment shader `62efd8a44c967bf4` keeps a storage pointer through
an identity-only loop Phi. The recursive provenance tracker treated the back
edge as an unknown pointer, leaving GlobalAtomicIAdd32 on the unimplemented
SPIR-V global atomic path. The same translation failure occurred at 1x and 2x.

Only incoming values resolving to that exact Phi through Identity instructions
are ignored. All remaining incoming paths must resolve to the same descriptor,
and at least one such path is required. Arithmetic on the recurrence, mutually
recursive Phis, different descriptors and pointers loaded from memory retain
the conservative fallback. This does not add general global atomic support.

The regression runs the production pass under ASan/UBSan. Before the change,
three checks fail (direct invariant loop, identity back edge, outer merge).
Afterwards all nine cases pass, including the rejection cases. Offline translation
of the dumped shader at 1x and 2x now emits StorageAtomicIAdd32 and completes SPIR-V
generation. The replay environment substitutes cbuf/texture metadata; this is not
an in-game rendering test. Full spirv-val validation of that replay still reports
a missing StorageImageExtendedFormats capability for an Rg32ui image declaration;
that separate issue is not addressed by this pointer change.

SR_ORDERING_TICKET (special register 15) still logs its existing stub and returns
zero. No unverified ordering emulation or error suppression is added. The stencil
cache fix 362dff4b is already an ancestor of this branch and remains unchanged.
