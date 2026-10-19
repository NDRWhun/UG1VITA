# Third-party code and licensing

NFSUG1VITA's own code is licensed under the MIT License ([LICENSE](LICENSE)). The parts below come from other
projects and keep their own terms.

## pefile

`tools/recompiler/disasm/pefile.py` and `tools/recompiler/disasm/ordlookup/`, from the pefile project (Ero Carrera,
version 2018.8.8), MIT License. The license text is in `tools/recompiler/disasm/LICENSE.pefile`.

## Capstone

The recompiler disassembles with Capstone (BSD 3-Clause License), version pinned in
`tools/recompiler/requirements.txt`. It is not part of the repository: pip or `tools/recompiler/bootstrap.py`
installs it into `.pydeps/`.

## Need for Speed Underground

Game code, binaries and assets are the property of Electronic Arts Inc. and its licensors and are not part of this
repository. Users supply their own copy of the game; the code generated from its executable (`src/nfsu/generated/`)
is not tracked.
