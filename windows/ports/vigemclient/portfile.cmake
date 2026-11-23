vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO nefarius/ViGEmClient
    REF b66d02d57e32cc8595369c53418b843e958649b4
    SHA512 448780245fe928e91da88ad1263c8c5d9bcc573b646e2892366f6fd247ec310f794dfba2e53cf65221b319573dc31b451b9a6e5ad4e3122fb7ca2498da235579
)
vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}" OPTIONS -DViGEmClient_DLL=OFF)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME ViGEmClient CONFIG_PATH lib/cmake/ViGEmClient)
vcpkg_fixup_pkgconfig()
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
