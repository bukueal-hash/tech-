Place MemProcFS / LeechCore binaries here for x64 builds:

  vmm.lib
  leechcore.lib
  vmm.dll
  leechcore.dll
  FTD3XX.dll
  leechcore_driver.dll  (if required by your device)

Symbol support (must sit next to vmm.dll at runtime — post-build copies these to Build\):

  info.db
  dbghelp.dll
  symsrv.dll
  libpdbcrust.dll       (rename from pdbcrust.dll in pdbcrust v1.0 release if needed)
  vmmyara.dll           (optional)

Copy from your DMA kit or MemProcFS release folder.
info.db: https://github.com/ufrisk/MemProcFS/releases/latest
libpdbcrust.dll: https://github.com/ufrisk/pdbcrust/releases/download/v1.0/pdbcrust.dll (save as libpdbcrust.dll)
