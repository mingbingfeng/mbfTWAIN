# TWAIN Data Source Discovery

TWAIN applications do not load a scanner Data Source directly in the normal
path. The application loads the TWAIN Data Source Manager (DSM), opens the DSM,
enumerates available Data Sources, then asks the DSM to open one selected
source. The DSM loads the Data Source module in-process and calls its exported
`DS_Entry` function.

For this project, the virtual scanner is a TWAIN Data Source module, not a
kernel driver. Installing it means placing the built `.ds` module in a
DSM-discoverable Data Source folder with the expected bitness:

```text
32-bit host process -> 32-bit TWAIN .ds module
64-bit host process -> 64-bit TWAIN .ds module
```

The TWAIN DSM 2.x Windows packages conventionally search `C:\Windows\twain_32`
or `C:\Windows\twain_64` and load `.ds` modules. For development, use a test
TWAIN application or TWAIN sample DSM and configure it to discover the built
module explicitly where possible. For packaging, ship both Win32 and x64 builds
and install them into the DSM's conventional Data Source locations for the
target environment.

During enumeration/opening, the DSM expects the DS to provide a stable
`TW_IDENTITY`, including protocol version, supported data groups, manufacturer,
product family, and product name. Phase 1 implements:

- `DG_CONTROL / DAT_IDENTITY / MSG_GET`
- `DG_CONTROL / DAT_IDENTITY / MSG_OPENDS`
- `DG_CONTROL / DAT_IDENTITY / MSG_CLOSEDS`
- `DG_CONTROL / DAT_STATUS / MSG_GET`

All image and capability triplets intentionally return failure until later
phases implement them.
