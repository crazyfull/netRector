---
name: code-conventions
description: Naming and commenting conventions used in socketManager source files
metadata:
  type: project
---

- File names use a `cls` prefix for classes (`clsTCPSocket.cpp`), while the class itself has no prefix (`TCPSocket`).
- Members are `m_camelCase`; pointer members are `m_pName`.
- Comments are a mix of Finglish (Persian written in Latin letters), Persian script, and English. Match whichever the surrounding code uses.
- `#mohem` ("important") in a comment marks a spot the author considers risky or unresolved. Search for it before changing nearby code.
- Notes to self and TODOs for the whole project are kept as comments at the top of `netRector.pro`.
- Debug output is plain `printf`, often left in hot paths; there is no logging abstraction.
- Hot paths prefer C function pointers plus a `void*` argument over `std::function`.

Related: [[architecture-map]].
