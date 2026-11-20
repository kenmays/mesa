# SPDX-License-Identifier: MIT

libswpipe = shared_library(
  'swpipe',
  files('SoftwareRenderer.cpp'),
  include_directories : [
    inc_include, inc_src, inc_glapi, inc_mesa, inc_gallium, inc_gallium_aux,
    inc_util, inc_gallium_drivers, inc_gallium_winsys,
    include_directories('../../frontends/hgl'),
    include_directories('../../winsys/sw/hgl'),
    include_directories('/boot/system/develop/headers/os/opengl'),
  ],
  gnu_symbol_visibility : 'hidden',
  link_args : [ld_args_bsymbolic, ld_args_gc_sections],
  link_with : [
    libglapi, libswhgl, libsthgl, libcompiler,
    libmesa, libglsl, libgalliumvl, libgallium,
  ],
  dependencies : [
    driver_swrast, cpp.find_library('be'), dep_unwind,
    idep_mesautil, idep_nir,
  ],
  install : true,
)
