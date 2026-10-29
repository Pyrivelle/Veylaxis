file(MAKE_DIRECTORY "${APP_DIR}/config")
foreach(component Qt5Core Qt5Gui)
    file(COPY "${QT_ROOT}/bin/${component}.dll" DESTINATION "${APP_DIR}")
endforeach()
file(COPY "${SDK_ROOT}/SilverLining/resources/VC14/x64/SilverLiningOpenGL-MT-DLL.dll" DESTINATION "${APP_DIR}")
# Keep shaders/textures/configs; use Triton's built-in FFTSS CPU backend rather
# than shipping unrelated legacy CUDA/OpenCL/IPP binaries (hundreds of MB).
file(COPY "${SDK_ROOT}/SilverLining/resources/" DESTINATION "${APP_DIR}/resource/SilverLining/resources"
    PATTERN "*.dll" EXCLUDE PATTERN "*.lib" EXCLUDE PATTERN "*.pdb" EXCLUDE)
# SilverLining resolves its backend relative to ResourceLoader, not the normal
# Windows executable DLL search path. Preserve the SDK's VC14/x64 layout.
file(COPY "${SDK_ROOT}/SilverLining/resources/VC14/x64/SilverLiningOpenGL-MT-DLL.dll"
    DESTINATION "${APP_DIR}/resource/SilverLining/resources/VC14/x64")
file(COPY "${SDK_ROOT}/Triton/Resources/" DESTINATION "${APP_DIR}/resource/Triton/Resources"
    PATTERN "dll64" EXCLUDE PATTERN "vc14" EXCLUDE PATTERN "*.dll" EXCLUDE PATTERN "*.lib" EXCLUDE PATTERN "*.pdb" EXCLUDE)
set(triton_config "${APP_DIR}/resource/Triton/Resources/Triton.config")
file(READ "${triton_config}" contents)
string(REPLACE "fft-force-cpu = no" "fft-force-cpu = yes" contents "${contents}")
file(WRITE "${triton_config}" "${contents}")
