# Force Android-container Mesa's KGSL DRI driver for OpenGL applications.
# Vulkan continues to use the Freedreno/Turnip ICD selected by the session.
export MESA_LOADER_DRIVER_OVERRIDE=kgsl
export FD_KGSL_ENABLE_DMABUF=1
unset LIBGL_ALWAYS_SOFTWARE
