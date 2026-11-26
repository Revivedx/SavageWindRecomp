# NSP reader

Adapted from HereOrCode's MIT-licensed `tools/nxpatch` at commit
`7ef602fe8e7c46e357febb897ad8b06c2892260c`. Savage Wind uses it as a
subprocess to inventory NSPs and extract selected base, update and DLC content.
It matches each NCA to its CNMT entry and supports several content records in
one NSP. An NSP patch requires the original base NSP. DLC can be read by itself.

`make package-helper` publishes the helper with the .NET 10 runtime.
The helper also uses LibHac 0.19.0. The corresponding license notices are
included with release builds.
