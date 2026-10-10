# Carrier for idf_component.yml: an empty component whose only job is to pull
# vroland/epdiy into the build and hand its headers to whoever requires it.
idf_component_register(REQUIRES epdiy)

# epdiy and this firmware's panel facade (src/epd_driver.c) both define
# epd_init() and epd_clear(). epdiy's two are renamed inside its own sources,
# consistently, so its internal callers follow; epdiy_gray.c, the one file that
# calls them, applies the same names before including epdiy.h.
idf_component_get_property(epdiy_lib vroland__epdiy COMPONENT_LIB)
target_compile_definitions(${epdiy_lib} PRIVATE epd_init=epdiy_epd_init epd_clear=epdiy_epd_clear)
