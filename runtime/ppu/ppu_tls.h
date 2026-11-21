#ifndef PS3RECOMP_PPU_TLS_H
#define PS3RECOMP_PPU_TLS_H

#ifndef PPU_THREAD_LOCAL
#  if defined(__cpp_constinit)
#    define PPU_TLS_CONSTINIT constinit
#  else
#    define PPU_TLS_CONSTINIT
#  endif
#  define PPU_THREAD_LOCAL PPU_TLS_CONSTINIT __thread
#endif

#endif
