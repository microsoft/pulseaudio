librdp_sources = [
  'module-rdp-sink.c',
  'module-rdp-source.c',
]

librdp_headers = [
]

librdp = shared_library('rdp',
  librdp_sources,
  librdp_headers,
  c_args : [pa_c_args, server_c_args],
  link_args : [nodelete_link_args],
  include_directories : [configinc, topinc],
  dependencies : [libpulse_dep, libpulsecommon_dep, libpulsecore_dep, libatomic_ops_dep, gst_dep, gstapp_dep, gstrtp_dep, gio_dep],
  install : true,
  install_rpath : privlibdir,
  install_dir : modlibexecdir,
)

librdp_dep = declare_dependency(link_with: librdp)
