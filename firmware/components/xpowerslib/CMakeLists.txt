# XPowersLib v0.2.7 by Lewis He — vendored unmodified, MIT (see LICENSE).
# https://github.com/lewisxhe/XPowersLib — only the AXP2101 driver is used.
idf_component_register(
    SRCS "XPowersLibInterface.cpp"
    INCLUDE_DIRS "." "REG"
    REQUIRES driver)
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    XPOWERS_CHIP_AXP2101
    CONFIG_XPOWERS_ESP_IDF_NEW_API)
