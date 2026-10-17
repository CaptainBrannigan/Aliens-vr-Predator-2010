#pragma once
// Per-object shadow vtables: the CEG-safe way to hook virtual functions.
//
// Steam CEG checksums the exe's .text and .rdata, so neither code nor the vtables themselves may be modified. Objects
// live on the heap, which CEG can't check. For each registered class this module builds a copy of the class's vtable
// in mod memory, replaces the entries that have a shadow trace stub (trace_hooks.h, TraceShadowStub), and a
// background thread periodically scans committed private memory for objects whose vtable pointer is the original,
// validates each candidate with a class-specific check, then atomically points the object at the shadow copy.
// Virtual calls on that object then go through the stub; non-virtual and devirtualized calls are not seen.
//
// Log: avpvr_shadow.log (scans, candidates, validated, swapped objects).

namespace avp {

// Builds the shadow vtables and starts the scanner thread. Trace hooks must already be installed.
void StartVmtShadow();

} // namespace avp
